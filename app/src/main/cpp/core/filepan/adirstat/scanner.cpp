// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/adirstat/scanner.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "filepan/directory_reader.hpp"
#include "filepan/eintr.hpp"

namespace filepan::adirstat {
namespace {

#ifdef STATX_ATTR_MOUNT_ROOT
constexpr std::uint64_t kMountRoot = STATX_ATTR_MOUNT_ROOT;
#else
constexpr std::uint64_t kMountRoot = 0x00002000;
#endif

std::uint64_t deviceOf(const struct statx &st) { return makedev(st.stx_dev_major, st.stx_dev_minor); }

NodeKind kindOf(unsigned mode) {
    if (S_ISDIR(mode)) {
        return NodeKind::kDirectory;
    }
    if (S_ISREG(mode)) {
        return NodeKind::kFile;
    }
    if (S_ISLNK(mode)) {
        return NodeKind::kSymlink;
    }
    return NodeKind::kOther;
}

NodeKind kindOfDirent(unsigned char type) {
    switch (type) {
        case DT_DIR:
            return NodeKind::kDirectory;
        case DT_REG:
            return NodeKind::kFile;
        case DT_LNK:
            return NodeKind::kSymlink;
        default:
            return NodeKind::kOther;
    }
}

// One entry read from a directory, before it goes into the tree.
struct Pending {
    std::string name;
    NodeKind kind;
    std::uint64_t size;
    std::uint64_t allocated;
    std::int64_t modified;
    std::uint64_t device;
    std::uint64_t inode;
    std::uint32_t links;
    bool otherFileSystem;
};

class Collector final : public DirectoryEntrySink {
public:
    Collector(std::vector<Pending> &out, std::uint64_t rootDevice) : out_(out), rootDevice_(rootDevice) {}

    void onEntry(const DirectoryEntryView &entry) override {
        Pending p{.name = std::string(entry.name),
                  .kind = kindOfDirent(entry.type),
                  .size = 0,
                  .allocated = 0,
                  .modified = 0,
                  .device = 0,
                  .inode = 0,
                  .links = 1,
                  .otherFileSystem = false};
        if (const struct statx *st = entry.stat) {
            p.kind = kindOf(st->stx_mode);
            p.size = st->stx_size;
            p.allocated = st->stx_blocks * 512;
            p.modified = st->stx_mtime.tv_sec;
            p.device = deviceOf(*st);
            p.inode = st->stx_ino;
            p.links = st->stx_nlink;
            p.otherFileSystem = isMountBoundary(rootDevice_, *st);
        }
        out_.push_back(std::move(p));
    }

private:
    std::vector<Pending> &out_;
    std::uint64_t rootDevice_;
};

class Scan {
public:
    Scan(std::string rootPath, const ScanOptions &options, const CancellationToken &cancel, ScanProgress *progress)
        : tree_(std::make_unique<Tree>(std::move(rootPath))),
          options_(options),
          cancel_(cancel),
          progress_(progress),
          threads_(std::max(1, options.threads)) {}

    Result<std::unique_ptr<Tree>> run() {
        const std::string &rootPath = tree_->rootPath();
        const int fd = DirectoryReader::open(rootPath.c_str());
        if (fd < 0) {
            return fail(-fd, rootPath);
        }
        struct statx st {};
        if (FILEPAN_RETRY(statx(fd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_BASIC_STATS, &st)) == -1) {
            const int error = errno;
            ::close(fd);
            return fail(error, rootPath);
        }
        rootDevice_ = deviceOf(st);
        Node &root = tree_->mutableNode(tree_->root());
        root.ownSize = st.stx_size;
        root.ownAllocated = st.stx_blocks * 512;
        root.modified = st.stx_mtime.tv_sec;

        busy_ = 1;
        std::vector<std::thread> workers;
        for (int i = 1; i < threads_; ++i) {
            workers.emplace_back([this] { work(); });
        }
        std::string path = rootPath;
        readDirectory(tree_->root(), fd, path);
        finishItem();
        work();
        for (std::thread &worker : workers) {
            worker.join();
        }
        if (cancel_.isCancelled()) {
            return fail(ECANCELED);
        }
        tree_->finalize();
        return std::move(tree_);
    }

private:
    struct Work {
        NodeId dir;
        std::string path;
    };

    void work() {
        while (true) {
            Work item;
            {
                std::unique_lock lock(queueMutex_);
                queueReady_.wait(lock, [&] { return !queue_.empty() || busy_ == 0; });
                if (queue_.empty()) {
                    return;
                }
                item = std::move(queue_.front());
                queue_.pop_front();
                ++busy_;
            }
            if (!cancel_.isCancelled()) {
                const int fd = DirectoryReader::open(item.path.c_str());
                if (fd < 0) {
                    setError(item.dir, -fd);
                } else {
                    readDirectory(item.dir, fd, item.path);
                }
            }
            finishItem();
        }
    }

    void finishItem() {
        std::lock_guard lock(queueMutex_);
        if (--busy_ == 0 && queue_.empty()) {
            queueReady_.notify_all();
        }
    }

    // Hands a directory to an idle thread, if the queue is short enough that one may be waiting.
    bool share(NodeId dir, const std::string &path) {
        if (threads_ == 1) {
            return false;
        }
        std::lock_guard lock(queueMutex_);
        if (queue_.size() >= static_cast<std::size_t>(threads_)) {
            return false;
        }
        queue_.push_back({dir, path});
        queueReady_.notify_one();
        return true;
    }

    void setError(NodeId dir, int error) {
        std::lock_guard lock(treeMutex_);
        tree_->mutableNode(dir).error = error;
    }

    // Reads the directory open at fd (taking it over), adds its entries, then recurses into the
    // subdirectories, sharing some with idle threads. path is the directory's and is restored.
    void readDirectory(NodeId dir, int fd, std::string &path) {
        if (cancel_.isCancelled()) {
            ::close(fd);
            return;
        }
        // On the heap: the reader's buffer is large and this recurses.
        auto reader = std::make_unique<DirectoryReader>(fd);
        std::vector<Pending> entries;
        Collector collector(entries, rootDevice_);
        int error = 0;
        while (!reader->atEnd()) {
            if ((error = reader->readEntries(true, collector)) != 0) {
                break;
            }
        }

        std::vector<std::pair<NodeId, std::size_t>> subdirs;
        std::uint64_t files = 0;
        std::uint64_t bytes = 0;
        {
            std::lock_guard lock(treeMutex_);
            if (error != 0) {
                tree_->mutableNode(dir).error = error;
            }
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const Pending &p = entries[i];
                NodeKind kind = p.kind;
                if (kind == NodeKind::kDirectory && p.otherFileSystem && !options_.crossMounts) {
                    kind = NodeKind::kMountPoint;
                }
                const NodeId id = tree_->add(dir, p.name, kind);
                Node &node = tree_->mutableNode(id);
                node.ownSize = p.size;
                node.ownAllocated = p.allocated;
                node.modified = p.modified;
                if (kind != NodeKind::kDirectory && kind != NodeKind::kMountPoint && p.links > 1
                    && !hardLinks_.emplace(p.device, p.inode).second) {
                    node.flags |= kHardLinkDuplicate;
                }
                if (kind == NodeKind::kDirectory) {
                    subdirs.emplace_back(id, i);
                } else if (!(node.flags & kHardLinkDuplicate)) {
                    ++files;
                    bytes += p.size;
                }
            }
        }
        if (progress_ != nullptr) {
            progress_->dirs.fetch_add(1, std::memory_order_relaxed);
            progress_->files.fetch_add(files, std::memory_order_relaxed);
            progress_->bytes.fetch_add(bytes, std::memory_order_relaxed);
            progress_->setCurrent(path);
        }

        for (const auto &[id, index] : subdirs) {
            if (cancel_.isCancelled()) {
                return;
            }
            const std::string &name = entries[index].name;
            const std::size_t length = path.size();
            if (path.back() != '/') {
                path += '/';
            }
            path += name;
            if (!share(id, path)) {
                const int child = FILEPAN_RETRY(
                        openat(reader->fd(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (child < 0) {
                    setError(id, errno);
                } else {
                    readDirectory(id, child, path);
                }
            }
            path.resize(length);
        }
    }

    std::unique_ptr<Tree> tree_;
    const ScanOptions &options_;
    const CancellationToken &cancel_;
    ScanProgress *progress_;
    const int threads_;
    std::uint64_t rootDevice_ = 0;

    std::mutex treeMutex_;
    std::set<std::pair<std::uint64_t, std::uint64_t>> hardLinks_;  // Guarded by treeMutex_.

    std::mutex queueMutex_;
    std::condition_variable queueReady_;
    std::deque<Work> queue_;
    int busy_ = 0;  // Items being read, plus the queued ones' producers still running.
};

}  // namespace

bool isMountBoundary(std::uint64_t rootDevice, const struct statx &st) {
    return deviceOf(st) != rootDevice
           || ((st.stx_attributes_mask & kMountRoot) && (st.stx_attributes & kMountRoot));
}

Result<std::unique_ptr<Tree>> scan(std::string_view rootPath, const ScanOptions &options,
                                   const CancellationToken &cancel, ScanProgress *progress) {
    std::string path(rootPath);
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    if (path.empty() || path.front() != '/') {
        return fail(EINVAL, path);
    }
    return Scan(std::move(path), options, cancel, progress).run();
}

}  // namespace filepan::adirstat
