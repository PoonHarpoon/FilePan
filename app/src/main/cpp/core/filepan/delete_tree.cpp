// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/delete_tree.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "filepan/eintr.hpp"

namespace filepan {

int DeleteTreeOperations::openDirectoryAt(int dirFd, const char *name) {
    return FILEPAN_RETRY(
        openat(dirFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
}

int DeleteTreeOperations::unlinkAt(int dirFd, const char *name, int flags) {
    return FILEPAN_RETRY(unlinkat(dirFd, name, flags));
}

namespace {

struct LinuxDirent64 {
    std::uint64_t d_ino;
    std::int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

struct Child {
    std::string name;
    unsigned char type;
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

// Reads all names up front: deleting entries while iterating can make some file systems (FUSE
// in particular) skip entries.
int readChildren(int fd, std::vector<Child> &children) {
    alignas(8) std::uint8_t buffer[32 * 1024];
    while (true) {
        const long length = FILEPAN_RETRY(syscall(SYS_getdents64, fd, buffer, sizeof(buffer)));
        if (length == -1) {
            return errno;
        }
        if (length == 0) {
            return 0;
        }
        for (long offset = 0; offset < length;) {
            const auto &dirent = *reinterpret_cast<const LinuxDirent64 *>(buffer + offset);
            offset += dirent.d_reclen;
            const char *name = dirent.d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
                continue;
            }
            children.push_back({name, dirent.d_type});
        }
    }
}

class TreeDeleter {
public:
    TreeDeleter(DeleteTreeCallbacks &callbacks, DeleteTreeOperations &operations)
        : callbacks_(callbacks), operations_(operations) {}

    DeleteTreeResult run(const std::string &path) {
        struct stat st {};
        if (FILEPAN_RETRY(lstat(path.c_str(), &st)) == -1) {
            result_.error = errno;
            return result_;
        }
        path_ = path;
        // Resolve the parent so that the root is removed relative to a directory fd like
        // everything else.
        const auto slash = path_.find_last_of('/');
        const std::string parent = slash == std::string::npos ? "."
                                   : slash == 0               ? "/"
                                                              : path_.substr(0, slash);
        const std::string name = slash == std::string::npos ? path_ : path_.substr(slash + 1);
        const int parentFd = FILEPAN_RETRY(
            open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (parentFd == -1) {
            result_.error = errno;
            return result_;
        }
        FdCloser closer(parentFd);
        const Outcome outcome = deleteEntry(parentFd, name, S_ISDIR(st.st_mode) ? DT_DIR : DT_REG);
        if (outcome == Outcome::kAborted) {
            result_.error = ECANCELED;
        } else if (!flushProgress(true)) {
            result_.error = ECANCELED;
        }
        return result_;
    }

private:
    enum class Outcome { kDeleted, kSkipped, kAborted };

    static constexpr std::uint64_t kProgressEntries = 64;
    static constexpr auto kProgressInterval = std::chrono::milliseconds(100);

    // path_ holds the entry's full path while it is being deleted.
    Outcome deleteEntry(int dirFd, const std::string &name, unsigned char type) {
        if (type == DT_UNKNOWN) {
            struct stat st {};
            type = FILEPAN_RETRY(fstatat(dirFd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW)) == 0
                           && S_ISDIR(st.st_mode)
                       ? DT_DIR
                       : DT_REG;
        }
        if (type != DT_DIR) {
            if (operations_.unlinkAt(dirFd, name.c_str(), 0) == 0) {
                return onDeleted();
            }
            return handleError(dirFd, name, DeleteTreeCallbacks::ErrorKind::kRemoveFile, errno);
        }

        bool hasSkipped = false;
        const int fd = operations_.openDirectoryAt(dirFd, name.c_str());
        if (fd == -1) {
            return handleError(dirFd, name, DeleteTreeCallbacks::ErrorKind::kOpenDirectory, errno);
        }
        {
            FdCloser closer(fd);
            std::vector<Child> children;
            if (const int error = readChildren(fd, children)) {
                return handleError(dirFd, name, DeleteTreeCallbacks::ErrorKind::kOpenDirectory,
                                   error);
            }
            const auto pathLength = path_.size();
            for (const auto &child : children) {
                path_.append("/").append(child.name);
                const Outcome outcome = deleteEntry(fd, child.name, child.type);
                path_.resize(pathLength);
                if (outcome == Outcome::kAborted) {
                    return outcome;
                }
                hasSkipped |= outcome == Outcome::kSkipped;
            }
        }
        if (hasSkipped) {
            // The directory can't be empty; it is kept along with what was skipped inside it.
            ++result_.skippedCount;
            return Outcome::kSkipped;
        }
        if (operations_.unlinkAt(dirFd, name.c_str(), AT_REMOVEDIR) == 0) {
            return onDeleted();
        }
        return handleError(dirFd, name, DeleteTreeCallbacks::ErrorKind::kRemoveDirectory, errno);
    }

    Outcome onDeleted() {
        ++result_.deletedCount;
        ++pendingProgress_;
        return flushProgress(false) ? Outcome::kDeleted : Outcome::kAborted;
    }

    Outcome handleError(int dirFd, const std::string &name, DeleteTreeCallbacks::ErrorKind kind,
                        int error) {
        if (!flushProgress(true) || !callbacks_.onError(path_, kind, error)) {
            return Outcome::kAborted;
        }
        struct stat st {};
        if (FILEPAN_RETRY(fstatat(dirFd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW)) == -1
            && errno == ENOENT) {
            // The callback dealt with it.
            return onDeleted();
        }
        ++result_.skippedCount;
        return Outcome::kSkipped;
    }

    bool flushProgress(bool force) {
        if (pendingProgress_ == 0) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && pendingProgress_ < kProgressEntries && now - lastProgress_ < kProgressInterval) {
            return true;
        }
        lastProgress_ = now;
        const auto count = pendingProgress_;
        pendingProgress_ = 0;
        return callbacks_.onProgress(count, path_);
    }

    DeleteTreeCallbacks &callbacks_;
    DeleteTreeOperations &operations_;
    DeleteTreeResult result_;
    std::string path_;
    std::uint64_t pendingProgress_ = 0;
    std::chrono::steady_clock::time_point lastProgress_ = std::chrono::steady_clock::now();
};

}  // namespace

DeleteTreeResult deleteTree(const std::string &path, DeleteTreeCallbacks &callbacks,
                            DeleteTreeOperations &operations) {
    return TreeDeleter(callbacks, operations).run(path);
}

DeleteTreeResult deleteTree(const std::string &path, DeleteTreeCallbacks &callbacks) {
    DeleteTreeOperations operations;
    return deleteTree(path, callbacks, operations);
}

}  // namespace filepan
