// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/create_archive.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <memory>
#include <thread>

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "filepan/eintr.hpp"

namespace filepan {

namespace {

constexpr std::size_t kReadBufferSize = 1024 * 1024;
constexpr auto kProgressInterval = std::chrono::milliseconds(200);

bool hasNoError(archive *a) { return archive_errno(a) == 0; }

struct WriteDeleter {
    void operator()(archive *a) const { archive_write_free(a); }
};
struct EntryDeleter {
    void operator()(archive_entry *entry) const { archive_entry_free(entry); }
};

class FdCloser {
public:
    explicit FdCloser(int fd) : fd_(fd) {}
    ~FdCloser() { ::close(fd_); }
    FdCloser(const FdCloser &) = delete;
    FdCloser &operator=(const FdCloser &) = delete;

private:
    int fd_;
};

std::vector<std::string> listNames(int fd, int &error) {
    struct LinuxDirent64 {
        std::uint64_t d_ino;
        std::int64_t d_off;
        unsigned short d_reclen;
        unsigned char d_type;
        char d_name[];
    };
    std::vector<std::string> names;
    alignas(8) std::uint8_t buffer[32 * 1024];
    while (true) {
        const long length = FILEPAN_RETRY(syscall(SYS_getdents64, fd, buffer, sizeof(buffer)));
        if (length <= 0) {
            error = length == -1 ? errno : 0;
            break;
        }
        for (long offset = 0; offset < length;) {
            const auto &dirent = *reinterpret_cast<const LinuxDirent64 *>(buffer + offset);
            offset += dirent.d_reclen;
            const char *name = dirent.d_name;
            if (!(name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))) {
                names.emplace_back(name);
            }
        }
    }
    // Stable, predictable archive contents regardless of directory order.
    std::sort(names.begin(), names.end());
    return names;
}

class ArchiveCreator {
public:
    ArchiveCreator(archive *a, CreateArchiveCallbacks &callbacks)
        : archive_(a), callbacks_(callbacks), buffer_(new char[kReadBufferSize]) {}

    // Returns false to stop, with result() describing why.
    bool addSource(const CreateArchiveSource &source) {
        path_ = source.path;
        return addEntry(AT_FDCWD, source.path, source.entryName);
    }

    bool finish() { return flushProgress(true); }

    CreateArchiveResult &result() { return result_; }

private:
    // path_ holds the entry's full path while it is being added.
    bool addEntry(int dirFd, const std::string &name, const std::string &entryName) {
        struct stat st {};
        if (FILEPAN_RETRY(fstatat(dirFd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW)) == -1) {
            return sourceError(errno);
        }
        std::unique_ptr<archive_entry, EntryDeleter> entry(archive_entry_new());
        archive_entry_copy_stat(entry.get(), &st);
        archive_entry_set_pathname_utf8(entry.get(), entryName.c_str());

        int fd = -1;
        if (S_ISREG(st.st_mode)) {
            fd = FILEPAN_RETRY(openat(dirFd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
            if (fd == -1) {
                return sourceError(errno);
            }
        } else if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            const ssize_t length =
                FILEPAN_RETRY(readlinkat(dirFd, name.c_str(), target, sizeof(target) - 1));
            if (length == -1) {
                return sourceError(errno);
            }
            target[length] = '\0';
            archive_entry_set_symlink_utf8(entry.get(), target);
            archive_entry_set_size(entry.get(), 0);
        } else if (S_ISDIR(st.st_mode)) {
            archive_entry_set_size(entry.get(), 0);
        } else {
            // Devices, FIFOs and sockets can't be archived meaningfully on Android.
            return sourceError(ENOTSUP);
        }
        std::unique_ptr<FdCloser> closer(fd != -1 ? new FdCloser(fd) : nullptr);

        if (archive_write_header(archive_, entry.get()) < ARCHIVE_WARN) {
            return archiveError();
        }
        if (fd != -1 && !writeData(fd)) {
            return false;
        }
        ++pendingEntries_;
        if (!flushProgress(false)) {
            return cancel();
        }
        if (!S_ISDIR(st.st_mode)) {
            return true;
        }

        const int childDirFd = FILEPAN_RETRY(
            openat(dirFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (childDirFd == -1) {
            return sourceError(errno);
        }
        FdCloser childCloser(childDirFd);
        int listError = 0;
        const auto children = listNames(childDirFd, listError);
        if (listError) {
            return sourceError(listError);
        }
        const auto pathLength = path_.size();
        for (const auto &child : children) {
            path_.append("/").append(child);
            const bool shouldContinue = addEntry(childDirFd, child, entryName + "/" + child);
            path_.resize(pathLength);
            if (!shouldContinue) {
                return false;
            }
        }
        return true;
    }

    bool writeData(int fd) {
        while (true) {
            const ssize_t length = FILEPAN_RETRY(read(fd, buffer_.get(), kReadBufferSize));
            if (length == -1) {
                // The header is already written, so the archive can't skip this entry anymore.
                const int error = errno;
                callbacks_.onSourceError(path_, error);
                result_.error = error;
                return false;
            }
            if (length == 0) {
                return true;
            }
            if (archive_write_data(archive_, buffer_.get(), static_cast<size_t>(length))
                != length) {
                return archiveError();
            }
            pendingSize_ += static_cast<std::uint64_t>(length);
            if (!flushProgress(false)) {
                return cancel();
            }
        }
    }

    bool sourceError(int error) {
        if (!flushProgress(true)) {
            return cancel();
        }
        if (callbacks_.onSourceError(path_, error)) {
            return true;
        }
        return cancel();
    }

    bool archiveError() {
        const int error = archive_errno(archive_);
        result_.error = error > 0 ? error : EIO;
        if (const char *message = archive_error_string(archive_)) {
            result_.message = message;
        }
        return false;
    }

    bool cancel() {
        result_.error = ECANCELED;
        return false;
    }

    bool flushProgress(bool force) {
        if (pendingSize_ == 0 && pendingEntries_ == 0) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastProgress_ < kProgressInterval) {
            return true;
        }
        lastProgress_ = now;
        const auto size = pendingSize_;
        const auto entries = pendingEntries_;
        pendingSize_ = 0;
        pendingEntries_ = 0;
        return callbacks_.onProgress(size, entries, path_);
    }

    archive *archive_;
    CreateArchiveCallbacks &callbacks_;
    std::unique_ptr<char[]> buffer_;
    CreateArchiveResult result_;
    std::string path_;
    std::uint64_t pendingSize_ = 0;
    std::uint64_t pendingEntries_ = 0;
    std::chrono::steady_clock::time_point lastProgress_ = std::chrono::steady_clock::now();
};

}  // namespace

bool configureArchiveWrite(archive *a, const CreateArchiveOptions &options) {
    if (archive_write_set_format(a, options.format) != ARCHIVE_OK
        || archive_write_add_filter(a, options.filter) != ARCHIVE_OK) {
        return false;
    }
    const unsigned threads =
        options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency());
    const std::string threadsValue = std::to_string(threads);
    // Options for a filter or format that isn't in use are ignored.
    archive_write_set_filter_option(a, "xz", "threads", threadsValue.c_str());
    archive_write_set_filter_option(a, "zstd", "threads", threadsValue.c_str());
    if (options.level >= 0) {
        const std::string level = std::to_string(options.level);
        archive_write_set_filter_option(a, nullptr, "compression-level", level.c_str());
        archive_write_set_format_option(a, nullptr, "compression-level", level.c_str());
    }
    if (!options.password.empty()) {
        if (archive_write_set_passphrase(a, options.password.c_str()) != ARCHIVE_OK
            || archive_write_set_format_option(a, "zip", "encryption", "aes256")
                   != ARCHIVE_OK) {
            return false;
        }
    }
    // Compressed output is written in whole blocks; don't pad the last one.
    archive_write_set_bytes_in_last_block(a, 1);
    return true;
}

CreateArchiveResult createArchive(const std::string &target, const CreateArchiveOptions &options,
                                  const std::vector<CreateArchiveSource> &sources,
                                  CreateArchiveCallbacks &callbacks) {
    CreateArchiveResult result;
    std::unique_ptr<archive, WriteDeleter> a(archive_write_new());
    auto failWithArchiveError = [&] {
        result.error = archive_errno(a.get()) > 0 ? archive_errno(a.get()) : EIO;
        if (const char *message = archive_error_string(a.get())) {
            result.message = message;
        }
        return result;
    };
    if (!configureArchiveWrite(a.get(), options)) {
        return failWithArchiveError();
    }
    const int fd = FILEPAN_RETRY(
        open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666));
    if (fd == -1) {
        result.error = errno;
        return result;
    }
    FdCloser closer(fd);
    if (archive_write_open_fd(a.get(), fd) != ARCHIVE_OK) {
        return failWithArchiveError();
    }

    ArchiveCreator creator(a.get(), callbacks);
    // Closing (rather than archive_write_fail()) lets libarchive release the state of the entry
    // in progress; the caller removes the partial archive anyway.
    for (const auto &source : sources) {
        if (!creator.addSource(source)) {
            if (hasNoError(a.get())) {
                archive_write_close(a.get());
            }
            return creator.result();
        }
    }
    if (!creator.finish()) {
        archive_write_close(a.get());
        result.error = ECANCELED;
        return result;
    }
    if (archive_write_close(a.get()) != ARCHIVE_OK) {
        return failWithArchiveError();
    }
    return result;
}

}  // namespace filepan
