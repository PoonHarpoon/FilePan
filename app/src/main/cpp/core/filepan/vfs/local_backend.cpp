// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/vfs/local_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "filepan/delete_tree.hpp"
#include "filepan/directory_reader.hpp"
#include "filepan/eintr.hpp"
#include "filepan/move.hpp"
#include "filepan/vfs/directory_watcher.hpp"

namespace filepan::vfs {

namespace {

constexpr unsigned kStatxMask = STATX_BASIC_STATS | STATX_BTIME;
// As DirectoryReader: a listing shouldn't force a round trip to refresh cached attributes.
constexpr int kListStatxFlags = AT_STATX_DONT_SYNC;

EntryType typeFromMode(std::uint32_t mode) {
    if (S_ISREG(mode)) return EntryType::kFile;
    if (S_ISDIR(mode)) return EntryType::kDirectory;
    if (S_ISLNK(mode)) return EntryType::kSymlink;
    return EntryType::kOther;
}

EntryType typeFromDirentType(unsigned char type) {
    switch (type) {
        case DT_REG: return EntryType::kFile;
        case DT_DIR: return EntryType::kDirectory;
        case DT_LNK: return EntryType::kSymlink;
        default: return EntryType::kOther;
    }
}

Timestamp toTimestamp(const struct statx_timestamp &time) {
    return {time.tv_sec, time.tv_nsec};
}

void fillStat(Entry &entry, const struct statx &st) {
    entry.hasStat = true;
    entry.type = typeFromMode(st.stx_mode);
    entry.targetType = entry.type;
    entry.mode = st.stx_mode;
    entry.uid = st.stx_uid;
    entry.gid = st.stx_gid;
    entry.linkCount = st.stx_nlink;
    entry.size = st.stx_size;
    entry.accessTime = toTimestamp(st.stx_atime);
    entry.modifiedTime = toTimestamp(st.stx_mtime);
    entry.hasBirthTime = st.stx_mask & STATX_BTIME;
    entry.birthTime = entry.hasBirthTime ? toTimestamp(st.stx_btime) : Timestamp{};
    entry.device = makedev(st.stx_dev_major, st.stx_dev_minor);
    entry.inode = st.stx_ino;
}

// A symlink's resolved target: its type, and its size, which is what a user wants to see.
void fillTarget(Entry &entry, const struct statx *targetStat) {
    if (targetStat) {
        entry.targetType = typeFromMode(targetStat->stx_mode);
        entry.size = targetStat->stx_size;
    } else {
        entry.targetType = EntryType::kOther;
        entry.brokenLink = true;
    }
}

// Builds Entries straight from DirectoryReader's statx results.
class EntrySink : public DirectoryEntrySink {
public:
    void onEntry(const DirectoryEntryView &view) override {
        Entry &entry = batch.emplace_back();
        entry.name.assign(view.name);
        entry.hidden = Entry::isHiddenName(view.name);
        if (view.stat) {
            fillStat(entry, *view.stat);
        } else {
            entry.type = typeFromDirentType(view.type);
            entry.targetType = entry.type;
        }
        if (entry.type == EntryType::kSymlink) {
            entry.targetType = EntryType::kOther;
            if (view.hasLinkTarget) {
                entry.linkTarget.assign(view.linkTarget);
                fillTarget(entry, view.targetStat);
            }
        }
    }

    std::vector<Entry> batch;
};

// The same as DirectoryReader does with statx, for an entry listed without it.
void statEntry(int dirFd, Entry &entry) {
    struct statx st {};
    if (FILEPAN_RETRY(statx(dirFd, entry.name.c_str(), AT_SYMLINK_NOFOLLOW | kListStatxFlags,
                            kStatxMask, &st))
        == -1) {
        return;  // Keep the type from the dirent, without stat.
    }
    fillStat(entry, st);
    if (entry.type != EntryType::kSymlink) {
        return;
    }
    entry.targetType = EntryType::kOther;
    char target[PATH_MAX];
    const ssize_t length =
        FILEPAN_RETRY(readlinkat(dirFd, entry.name.c_str(), target, sizeof(target)));
    if (length < 0) {
        return;
    }
    entry.linkTarget.assign(target, static_cast<std::size_t>(length));
    struct statx targetSt {};
    const bool resolved = FILEPAN_RETRY(statx(dirFd, entry.name.c_str(), kListStatxFlags,
                                              kStatxMask, &targetSt))
                          == 0;
    fillTarget(entry, resolved ? &targetSt : nullptr);
}

// Stats a listing's entries on several threads. Each statx is a system call that may wait on a
// slow file system (FUSE on Android's shared storage), so overlapping them shortens the listing.
// The workers live for one listing and are only started for a large enough directory.
class ParallelStat {
public:
    ParallelStat(int dirFd, unsigned threads) : dirFd_(dirFd), threadCount_(threads) {}

    ~ParallelStat() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        workAvailable_.notify_all();
        for (auto &thread : threads_) {
            thread.join();
        }
    }

    void run(std::span<Entry> entries) {
        if (entries.size() < kMinParallelEntries || threadCount_ <= 1) {
            for (auto &entry : entries) {
                statEntry(dirFd_, entry);
            }
            return;
        }
        if (threads_.empty()) {
            for (unsigned i = 1; i < threadCount_; ++i) {
                threads_.emplace_back([this] { workerLoop(); });
            }
        }
        {
            std::lock_guard lock(mutex_);
            entries_ = entries;
            next_.store(0, std::memory_order_relaxed);
            active_ = threads_.size();
            ++generation_;
        }
        workAvailable_.notify_all();
        work(entries);
        std::unique_lock lock(mutex_);
        workDone_.wait(lock, [this] { return active_ == 0; });
    }

private:
    static constexpr std::size_t kMinParallelEntries = 128;
    // Entries claimed at a time, to keep the shared counter cool.
    static constexpr std::size_t kChunk = 16;

    void work(std::span<Entry> entries) {
        while (true) {
            const std::size_t start = next_.fetch_add(kChunk, std::memory_order_relaxed);
            if (start >= entries.size()) {
                return;
            }
            const std::size_t end = std::min(start + kChunk, entries.size());
            for (std::size_t i = start; i < end; ++i) {
                statEntry(dirFd_, entries[i]);
            }
        }
    }

    void workerLoop() {
        std::uint64_t seen = 0;
        while (true) {
            std::span<Entry> entries;
            {
                std::unique_lock lock(mutex_);
                workAvailable_.wait(lock, [&] { return stopping_ || generation_ != seen; });
                if (stopping_) {
                    return;
                }
                seen = generation_;
                entries = entries_;
            }
            work(entries);
            std::lock_guard lock(mutex_);
            if (--active_ == 0) {
                workDone_.notify_one();
            }
        }
    }

    int dirFd_;
    unsigned threadCount_;
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable workDone_;
    bool stopping_ = false;
    std::uint64_t generation_ = 0;
    std::size_t active_ = 0;
    std::span<Entry> entries_;
    std::atomic<std::size_t> next_ = 0;
};

Result<Entry> statPath(const Location &location, bool follow) {
    const char *path = location.path().c_str();
    struct statx st {};
    if (FILEPAN_RETRY(statx(AT_FDCWD, path, follow ? 0 : AT_SYMLINK_NOFOLLOW, kStatxMask, &st))
        == -1) {
        return failErrno(location.path());
    }
    Entry entry;
    entry.name.assign(location.name());
    entry.hidden = Entry::isHiddenName(entry.name);
    fillStat(entry, st);
    if (entry.type == EntryType::kSymlink) {
        char target[PATH_MAX];
        const ssize_t length = FILEPAN_RETRY(readlink(path, target, sizeof(target)));
        entry.targetType = EntryType::kOther;
        if (length >= 0) {
            entry.linkTarget.assign(target, static_cast<std::size_t>(length));
            struct statx targetSt {};
            const bool resolved =
                FILEPAN_RETRY(statx(AT_FDCWD, path, 0, kStatxMask, &targetSt)) == 0;
            fillTarget(entry, resolved ? &targetSt : nullptr);
        }
    }
    return entry;
}

class LocalInputStream final : public InputStream {
public:
    explicit LocalInputStream(int fd) : fd_(fd) {}
    ~LocalInputStream() override { ::close(fd_); }

    Result<std::size_t> read(std::span<std::uint8_t> buffer) override {
        const ssize_t length = FILEPAN_RETRY(::read(fd_, buffer.data(), buffer.size()));
        return length == -1 ? Result<std::size_t>(failErrno())
                            : static_cast<std::size_t>(length);
    }

    Result<std::uint64_t> seek(std::int64_t offset, SeekOrigin origin) override {
        return seekFd(fd_, offset, origin);
    }

    Result<std::uint64_t> size() override { return sizeOfFd(fd_); }

    static Result<std::uint64_t> seekFd(int fd, std::int64_t offset, SeekOrigin origin) {
        const int whence = origin == SeekOrigin::kBegin     ? SEEK_SET
                           : origin == SeekOrigin::kCurrent ? SEEK_CUR
                                                            : SEEK_END;
        const off_t position = lseek(fd, offset, whence);
        return position == -1 ? Result<std::uint64_t>(failErrno())
                              : static_cast<std::uint64_t>(position);
    }

    static Result<std::uint64_t> sizeOfFd(int fd) {
        struct stat st {};
        return fstat(fd, &st) == -1 ? Result<std::uint64_t>(failErrno())
                                    : static_cast<std::uint64_t>(st.st_size);
    }

private:
    int fd_;
};

class LocalOutputStream final : public OutputStream {
public:
    explicit LocalOutputStream(int fd) : fd_(fd) {}
    ~LocalOutputStream() override {
        if (fd_ != -1) {
            ::close(fd_);
        }
    }

    Result<void> write(std::span<const std::uint8_t> buffer) override {
        while (!buffer.empty()) {
            const ssize_t length = FILEPAN_RETRY(::write(fd_, buffer.data(), buffer.size()));
            if (length == -1) {
                return failErrno();
            }
            buffer = buffer.subspan(static_cast<std::size_t>(length));
        }
        return {};
    }

    Result<std::uint64_t> seek(std::int64_t offset, SeekOrigin origin) override {
        return LocalInputStream::seekFd(fd_, offset, origin);
    }

    Result<std::uint64_t> size() override { return LocalInputStream::sizeOfFd(fd_); }

    Result<void> close() override {
        // close() must not be retried on EINTR on Linux; it reports deferred write errors.
        const int fd = std::exchange(fd_, -1);
        return fd == -1 || ::close(fd) == 0 ? Result<void>() : failErrno();
    }

private:
    int fd_;
};

// Adapts deleteTree() to RemoveCallbacks and a cancellation token.
class RemoveAdapter : public DeleteTreeCallbacks {
public:
    RemoveAdapter(RemoveCallbacks *callbacks, const CancellationToken &cancel)
        : callbacks_(callbacks), cancel_(cancel) {}

    bool onProgress(std::uint64_t deletedCount, std::string_view currentPath) override {
        if (cancel_.isCancelled()) {
            return false;
        }
        return !callbacks_ || callbacks_->onProgress(deletedCount, currentPath);
    }

    bool onError(std::string_view path, ErrorKind kind, int error) override {
        while (true) {
            if (!callbacks_ || cancel_.isCancelled()) {
                firstError_ = firstError_ ? firstError_ : error;
                return false;
            }
            const auto action = callbacks_->onError(path, Error{error, std::string(path)});
            if (action == RemoveCallbacks::ErrorAction::kSkip) {
                return true;
            }
            if (action == RemoveCallbacks::ErrorAction::kAbort) {
                firstError_ = firstError_ ? firstError_ : error;
                return false;
            }
            const std::string pathString(path);
            int result;
            switch (kind) {
                case ErrorKind::kRemoveFile:
                    result = FILEPAN_RETRY(unlink(pathString.c_str()));
                    break;
                case ErrorKind::kRemoveDirectory:
                    result = FILEPAN_RETRY(rmdir(pathString.c_str()));
                    break;
                case ErrorKind::kOpenDirectory: {
                    // Retry the whole subtree; deleteTree() then sees whether it is gone.
                    const auto nested = deleteTree(pathString, *this);
                    if (nested.error == ECANCELED) {
                        return false;
                    }
                    result = nested.error ? -1 : 0;
                    errno = nested.error;
                    break;
                }
            }
            if (result == 0 || errno == ENOENT) {
                return true;
            }
            error = errno;
        }
    }

    [[nodiscard]] int firstError() const { return firstError_; }

private:
    RemoveCallbacks *callbacks_;
    const CancellationToken &cancel_;
    int firstError_ = 0;
};

}  // namespace

LocalBackend::LocalBackend(std::shared_ptr<DirectoryWatcher> watcher)
    : watcher_(std::move(watcher)) {}

LocalBackend::~LocalBackend() = default;

Capabilities LocalBackend::capabilities() const {
    return {Capability::kWrite,        Capability::kSetTimes,     Capability::kChmod,
            Capability::kSymlinks,     Capability::kWatch,        Capability::kSeekableRead,
            Capability::kSeekableWrite, Capability::kBirthTime};
}

Result<void> LocalBackend::list(const Location &directory, const ListOptions &options,
                                const ListCallback &onBatch, const CancellationToken &cancel) {
    const int fd = DirectoryReader::open(directory.path().c_str());
    if (fd < 0) {
        return fail(-fd, directory.path());
    }
    // The reader's getdents64 buffer is too large for some threads' stacks.
    const auto reader = std::make_unique<DirectoryReader>(fd);
    EntrySink sink;
    sink.batch.reserve(options.minBatchSize);
    // With several threads, the reader only lists names and types and the entries of each
    // getdents64 buffer are stat'ed in parallel; otherwise the reader stats them as it goes.
    const bool parallel = options.withStat && options.statThreads > 1;
    std::optional<ParallelStat> parallelStat;
    if (parallel) {
        parallelStat.emplace(reader->fd(), options.statThreads);
    }
    while (!reader->atEnd()) {
        if (cancel.isCancelled()) {
            return fail(ECANCELED);
        }
        const std::size_t start = sink.batch.size();
        if (const int error = reader->readEntries(options.withStat && !parallel, sink)) {
            return fail(error, directory.path());
        }
        if (parallel) {
            parallelStat->run(std::span(sink.batch).subspan(start));
        }
        if (sink.batch.size() >= options.minBatchSize) {
            onBatch(std::exchange(sink.batch, {}));
            sink.batch.reserve(options.minBatchSize);
        }
    }
    if (!sink.batch.empty()) {
        onBatch(std::move(sink.batch));
    }
    return {};
}

Result<Entry> LocalBackend::stat(const Location &location) { return statPath(location, true); }

Result<Entry> LocalBackend::lstat(const Location &location) { return statPath(location, false); }

Result<void> LocalBackend::makeDirectory(const Location &location, std::uint32_t mode) {
    if (FILEPAN_RETRY(mkdir(location.path().c_str(), mode)) == -1) {
        return failErrno(location.path());
    }
    return {};
}

Result<void> LocalBackend::createFile(const Location &location, std::uint32_t mode) {
    const int fd = FILEPAN_RETRY(
        open(location.path().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode));
    if (fd == -1) {
        return failErrno(location.path());
    }
    ::close(fd);
    return {};
}

Result<void> LocalBackend::rename(const Location &source, const Location &target, bool replace) {
    MoveOperations operations;
    if (operations.rename(source.path().c_str(), target.path().c_str(), replace) == -1) {
        return failErrno(source.path());
    }
    return {};
}

Result<RemoveStats> LocalBackend::remove(const Location &location, bool recursive,
                                         RemoveCallbacks *callbacks,
                                         const CancellationToken &cancel) {
    const char *path = location.path().c_str();
    if (!recursive) {
        if (FILEPAN_RETRY(unlink(path)) == 0
            || ((errno == EISDIR || errno == EPERM) && FILEPAN_RETRY(rmdir(path)) == 0)) {
            return RemoveStats{1, 0};
        }
        return failErrno(location.path());
    }
    RemoveAdapter adapter(callbacks, cancel);
    const DeleteTreeResult result = deleteTree(location.path(), adapter);
    if (result.error == ECANCELED) {
        return cancel.isCancelled() || !adapter.firstError()
                   ? fail(ECANCELED)
                   : fail(adapter.firstError(), location.path());
    }
    if (result.error) {
        return fail(result.error, location.path());
    }
    return RemoveStats{result.deletedCount, result.skippedCount};
}

Result<std::unique_ptr<InputStream>> LocalBackend::openRead(const Location &location) {
    const int fd = FILEPAN_RETRY(open(location.path().c_str(), O_RDONLY | O_CLOEXEC));
    if (fd == -1) {
        return failErrno(location.path());
    }
    return std::make_unique<LocalInputStream>(fd);
}

Result<std::unique_ptr<OutputStream>> LocalBackend::openWrite(const Location &location,
                                                             const OpenWriteOptions &options) {
    int flags = O_WRONLY | O_CLOEXEC;
    switch (options.mode) {
        case WriteMode::kCreateNew: flags |= O_CREAT | O_EXCL; break;
        case WriteMode::kReplace: flags |= O_CREAT | O_TRUNC; break;
        case WriteMode::kExisting: break;
    }
    const int fd = FILEPAN_RETRY(open(location.path().c_str(), flags, options.permissions));
    if (fd == -1) {
        return failErrno(location.path());
    }
    return std::make_unique<LocalOutputStream>(fd);
}

Result<std::string> LocalBackend::readLink(const Location &location) {
    char target[PATH_MAX];
    const ssize_t length = FILEPAN_RETRY(readlink(location.path().c_str(), target, sizeof(target)));
    if (length == -1) {
        return failErrno(location.path());
    }
    return std::string(target, static_cast<std::size_t>(length));
}

Result<void> LocalBackend::createSymlink(const Location &link, std::string_view target) {
    if (FILEPAN_RETRY(symlink(std::string(target).c_str(), link.path().c_str())) == -1) {
        return failErrno(link.path());
    }
    return {};
}

Result<void> LocalBackend::setTimes(const Location &location, std::optional<Timestamp> accessTime,
                                    std::optional<Timestamp> modifiedTime) {
    const auto toTimespec = [](const std::optional<Timestamp> &time) {
        return time ? timespec{static_cast<time_t>(time->seconds),
                               static_cast<long>(time->nanoseconds)}
                    : timespec{0, UTIME_OMIT};
    };
    const struct timespec times[2] = {toTimespec(accessTime), toTimespec(modifiedTime)};
    if (FILEPAN_RETRY(utimensat(AT_FDCWD, location.path().c_str(), times, AT_SYMLINK_NOFOLLOW))
        == -1) {
        return failErrno(location.path());
    }
    return {};
}

Result<void> LocalBackend::chmod(const Location &location, std::uint32_t mode) {
    if (FILEPAN_RETRY(::chmod(location.path().c_str(), mode & 07777)) == -1) {
        return failErrno(location.path());
    }
    return {};
}

Result<std::unique_ptr<WatchHandle>> LocalBackend::watch(const Location &directory,
                                                         WatchCallback callback) {
    std::shared_ptr<DirectoryWatcher> watcher;
    {
        std::lock_guard lock(watcherMutex_);
        if (!watcher_) {
            auto created = DirectoryWatcher::create();
            if (!created) {
                return std::unexpected(created.error());
            }
            watcher_ = std::move(*created);
        }
        watcher = watcher_;
    }
    const auto id = watcher->watch(directory.path(), std::move(callback));
    if (!id) {
        return std::unexpected(id.error());
    }
    return DirectoryWatcher::makeHandle(std::move(watcher), *id);
}

std::unique_ptr<InputStream> fdInputStream(int fd) { return std::make_unique<LocalInputStream>(fd); }

std::unique_ptr<OutputStream> fdOutputStream(int fd) { return std::make_unique<LocalOutputStream>(fd); }

}  // namespace filepan::vfs
