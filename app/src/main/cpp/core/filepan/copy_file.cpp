// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/copy_file.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "filepan/eintr.hpp"
#include "filepan/unique_fd.hpp"

namespace filepan {

namespace {

// Bytes moved per kernel copy call, between progress checks: a few MiB keeps cancellation quick.
constexpr std::size_t kChunkSize = 8 * 1024 * 1024;
constexpr std::size_t kBufferSize = 256 * 1024;

CopyFileResult fail(CopyFileStage stage, int error) { return {stage, error}; }

// Buffers progress so that onProgress() runs at most once per interval.
class Progress {
public:
    Progress(CopyFileCallbacks &callbacks, std::chrono::milliseconds interval)
        : callbacks_(callbacks), interval_(interval), last_(std::chrono::steady_clock::now()) {}

    // Returns false when the callback asked to abort.
    bool add(std::uint64_t size) {
        pending_ += size;
        if (interval_.count() > 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_ < interval_) {
                return true;
            }
            last_ = now;
        }
        return flush();
    }

    bool flush() {
        if (pending_ == 0) {
            return true;
        }
        return callbacks_.onProgress(std::exchange(pending_, 0));
    }

private:
    CopyFileCallbacks &callbacks_;
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point last_;
    std::uint64_t pending_ = 0;
};

bool isFallbackError(int error) {
    return error == EXDEV || error == EINVAL || error == EOPNOTSUPP || error == ENOTSUP ||
           error == ENOSYS;
}

// copy_file_range also fails with EPERM (immutable files, some FUSE and LSM policies) and EBADF
// (descriptors it can't use) where sendfile or read and write still work.
bool isCopyFileRangeFallbackError(int error) {
    return isFallbackError(error) || error == EPERM || error == EBADF;
}

enum class CopyStatus { kDone, kFallback, kError, kAborted };

// Copies with a kernel-side call until end of file. kFallback leaves both offsets where they are,
// so the next method carries on from there.
template <typename Call, typename IsFallback>
CopyStatus copyInKernel(Call call, IsFallback isFallback, off_t sourceSize, off_t &copied,
                        Progress &progress, int &error) {
    bool first = true;
    while (true) {
        const ssize_t n = FILEPAN_RETRY(call());
        if (n < 0) {
            error = errno;
            return isFallback(error) ? CopyStatus::kFallback : CopyStatus::kError;
        }
        if (n == 0) {
            // Some file systems report nothing copied rather than an error.
            return first && copied < sourceSize ? CopyStatus::kFallback : CopyStatus::kDone;
        }
        first = false;
        copied += n;
        if (!progress.add(static_cast<std::uint64_t>(n))) {
            return CopyStatus::kAborted;
        }
    }
}

CopyStatus copyWithBuffer(int source, int target, Progress &progress, int &error) {
    const auto buffer = std::make_unique_for_overwrite<std::byte[]>(kBufferSize);
    while (true) {
        const ssize_t n = FILEPAN_RETRY(::read(source, buffer.get(), kBufferSize));
        if (n < 0) {
            error = errno;
            return CopyStatus::kError;
        }
        if (n == 0) {
            return CopyStatus::kDone;
        }
        for (ssize_t written = 0; written < n;) {
            const ssize_t w = FILEPAN_RETRY(::write(target, buffer.get() + written, n - written));
            if (w < 0) {
                error = errno;
                return CopyStatus::kError;
            }
            if (w == 0) {
                error = EIO;
                return CopyStatus::kError;
            }
            written += w;
        }
        if (!progress.add(static_cast<std::uint64_t>(n))) {
            return CopyStatus::kAborted;
        }
    }
}

CopyStatus copyData(int source, int target, off_t sourceSize, Progress &progress, int &error) {
    off_t copied = 0;
    CopyStatus status = copyInKernel(
        [&] { return ::copy_file_range(source, nullptr, target, nullptr, kChunkSize, 0); },
        isCopyFileRangeFallbackError, sourceSize, copied, progress, error);
    if (status == CopyStatus::kFallback) {
        status = copyInKernel([&] { return ::sendfile(target, source, nullptr, kChunkSize); },
                              isFallbackError, sourceSize, copied, progress, error);
    }
    if (status == CopyStatus::kFallback) {
        status = copyWithBuffer(source, target, progress, error);
    }
    return status;
}

// An attribute target or source: an open descriptor, or a path for links and directories.
struct Node {
    int fd = -1;
    const char *path = nullptr;
    bool follow = false;

    ssize_t list(char *buffer, std::size_t size) const {
        if (fd >= 0) return ::flistxattr(fd, buffer, size);
        return follow ? ::listxattr(path, buffer, size) : ::llistxattr(path, buffer, size);
    }
    ssize_t get(const char *name, void *buffer, std::size_t size) const {
        if (fd >= 0) return ::fgetxattr(fd, name, buffer, size);
        return follow ? ::getxattr(path, name, buffer, size)
                      : ::lgetxattr(path, name, buffer, size);
    }
    int set(const char *name, const void *value, std::size_t size) const {
        if (fd >= 0) return ::fsetxattr(fd, name, value, size, 0);
        return ::lsetxattr(path, name, value, size, 0);
    }
    int chown(uid_t uid, gid_t gid) const {
        if (fd >= 0) return ::fchown(fd, uid, gid);
        return ::fchownat(AT_FDCWD, path, uid, gid, AT_SYMLINK_NOFOLLOW);
    }
    int chmod(mode_t mode) const {
        if (fd >= 0) return ::fchmod(fd, mode);
        return ::fchmodat(AT_FDCWD, path, mode, 0);
    }
    int setTimes(const timespec (&times)[2]) const {
        if (fd >= 0) return ::futimens(fd, times);
        return ::utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW);
    }
};

// Calls read(buffer, size) until the buffer is large enough, as sizes may change in between.
template <typename Read>
bool readSized(std::string &buffer, Read read) {
    while (true) {
        const ssize_t size = FILEPAN_RETRY(read(nullptr, 0));
        if (size < 0) {
            return false;
        }
        buffer.resize(static_cast<std::size_t>(size));
        const ssize_t n = FILEPAN_RETRY(read(buffer.data(), buffer.size()));
        if (n >= 0) {
            buffer.resize(static_cast<std::size_t>(n));
            return true;
        }
        if (errno != ERANGE) {
            return false;
        }
    }
}

void copyXattrs(const Node &source, const Node &target, bool all) {
    std::string names;
    if (!readSized(names, [&](char *b, std::size_t s) { return source.list(b, s); })) {
        return;
    }
    std::string value;
    for (std::size_t i = 0; i < names.size();) {
        const char *name = names.c_str() + i;
        const std::string_view view(name);
        i += view.size() + 1;
        if (all ? view == "security.selinux" : !view.starts_with("user.")) {
            // The label comes from the policy for the target's location.
            continue;
        }
        if (!readSized(value, [&](char *b, std::size_t s) { return source.get(name, b, s); })) {
            continue;
        }
        FILEPAN_RETRY(target.set(name, value.data(), value.size()));
    }
}

// Best effort: no failure here fails the copy. Owner first (chown clears setuid/setgid and
// security.capability), then xattrs while the target is still writable to us, then the mode
// (consistent with an ACL the xattrs carried), then the times, after every other change.
void copyAttributes(const struct stat &st, const Node &source, const Node &target,
                    const CopyFileOptions &options) {
    const bool isLink = S_ISLNK(st.st_mode);
    bool ownerKept = false;
    if (options.copyAttributes) {
        if (FILEPAN_RETRY(target.chown(st.st_uid, st.st_gid)) == 0) {
            ownerKept = true;
        } else {
            // The group alone may still work for a group we belong to.
            FILEPAN_RETRY(target.chown(static_cast<uid_t>(-1), st.st_gid));
        }
    }
    copyXattrs(source, target, options.copyAttributes);
    if (!isLink) {
        mode_t mode = st.st_mode & 07777;
        if (!ownerKept && S_ISREG(st.st_mode) && (mode & (S_ISUID | S_ISGID)) != 0) {
            // Like cp -p: never hand someone else's setuid/setgid bits to a different owner.
            struct stat now {};
            const bool known = target.fd >= 0 && ::fstat(target.fd, &now) == 0;
            if (!known || now.st_uid != st.st_uid) {
                mode &= ~S_ISUID;
            }
            if (!known || now.st_gid != st.st_gid) {
                mode &= ~S_ISGID;
            }
        }
        FILEPAN_RETRY(target.chmod(mode));
    }
    timespec times[2] = {st.st_atim, st.st_mtim};
    if (!options.copyAttributes) {
        times[0].tv_nsec = UTIME_OMIT;
    }
    FILEPAN_RETRY(target.setTimes(times));
}

CopyFileResult removeTarget(const char *target, const struct stat &st) {
    const int rc = S_ISDIR(st.st_mode) ? ::rmdir(target) : ::unlink(target);
    // ENOENT: it went away since the lstat, which is what we wanted.
    if (rc != 0 && errno != ENOENT) {
        // Some systems report a non-empty directory as EEXIST.
        return fail(CopyFileStage::kRemoveTarget, errno == EEXIST ? ENOTEMPTY : errno);
    }
    return {};
}

CopyFileResult copyRegular(const char *source, const char *target,
                           const struct stat *existing, const CopyFileOptions &options,
                           CopyFileCallbacks &callbacks) {
    // Opened before the target is replaced, so an unreadable source leaves it alone. O_NONBLOCK
    // keeps a FIFO swapped in after the stat from blocking the open.
    const int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK | (options.noFollowLinks ? O_NOFOLLOW : 0);
    UniqueFd in(FILEPAN_RETRY(::open(source, flags)));
    if (!in) {
        return fail(CopyFileStage::kOpenSource, errno);
    }
    struct stat st {};
    if (::fstat(in.get(), &st) != 0) {
        return fail(CopyFileStage::kOpenSource, errno);
    }
    if (!S_ISREG(st.st_mode)) {
        return fail(CopyFileStage::kUnsupportedType, static_cast<int>(st.st_mode));
    }
    if (existing != nullptr) {
        if (const CopyFileResult result = removeTarget(target, *existing);
            result.stage != CopyFileStage::kNone) {
            return result;
        }
    }
    UniqueFd out(FILEPAN_RETRY(
        ::open(target, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)));
    if (!out) {
        return fail(CopyFileStage::kOpenTarget, errno);
    }

    Progress progress(callbacks, options.progressInterval);
    int error = 0;
    CopyFileResult result;
    switch (copyData(in.get(), out.get(), st.st_size, progress, error)) {
        case CopyStatus::kDone:
            if (!progress.flush()) {
                result = fail(CopyFileStage::kAborted, ECANCELED);
            }
            break;
        case CopyStatus::kAborted:
            result = fail(CopyFileStage::kAborted, ECANCELED);
            break;
        case CopyStatus::kFallback:
        case CopyStatus::kError:
            result = fail(CopyFileStage::kCopyData, error);
            break;
    }
    if (result.stage == CopyFileStage::kNone) {
        copyAttributes(st, Node{.fd = in.get()}, Node{.fd = out.get()}, options);
        // EINTR still closes the descriptor on Linux and reports no data loss.
        if (::close(out.release()) != 0 && errno != EINTR) {
            result = fail(CopyFileStage::kCloseTarget, errno);
        }
    }
    if (result.stage != CopyFileStage::kNone) {
        out.reset();
        ::unlink(target);
    }
    return result;
}

CopyFileResult copyDirectory(const char *source, const char *target, const struct stat &st,
                             const struct stat *existing, const CopyFileOptions &options) {
    if (existing != nullptr) {
        if (const CopyFileResult result = removeTarget(target, *existing);
            result.stage != CopyFileStage::kNone) {
            return result;
        }
    }
    // Private until the attributes are in place; the final mode is set explicitly.
    if (::mkdir(target, 0700) != 0) {
        return fail(CopyFileStage::kMakeDirectory, errno);
    }
    const UniqueFd fd(
        FILEPAN_RETRY(::open(target, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)));
    const Node targetNode = fd ? Node{.fd = fd.get()} : Node{.path = target};
    copyAttributes(st, Node{.path = source, .follow = !options.noFollowLinks}, targetNode,
                   options);
    return {};
}

CopyFileResult copyLink(const char *source, const char *target, const struct stat &st,
                        const struct stat *existing, const CopyFileOptions &options) {
    // st_size may be 0 (some file systems) or stale; grow until the text fits.
    std::string text(st.st_size > 0 ? static_cast<std::size_t>(st.st_size) + 1 : 256, '\0');
    while (true) {
        const ssize_t n = ::readlink(source, text.data(), text.size());
        if (n < 0) {
            return fail(CopyFileStage::kReadLink, errno);
        }
        if (static_cast<std::size_t>(n) < text.size()) {
            text.resize(static_cast<std::size_t>(n));
            break;
        }
        text.resize(text.size() * 2);
    }
    if (existing != nullptr) {
        if (const CopyFileResult result = removeTarget(target, *existing);
            result.stage != CopyFileStage::kNone) {
            return result;
        }
    }
    if (::symlink(text.c_str(), target) != 0) {
        return fail(CopyFileStage::kCreateLink, errno);
    }
    copyAttributes(st, Node{.path = source}, Node{.path = target}, options);
    return {};
}

}  // namespace

CopyFileResult copyFile(const char *source, const char *target, const CopyFileOptions &options,
                        CopyFileCallbacks &callbacks) {
    struct stat st {};
    if ((options.noFollowLinks ? ::lstat(source, &st) : ::stat(source, &st)) != 0) {
        return fail(CopyFileStage::kStatSource, errno);
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        return fail(CopyFileStage::kUnsupportedType, static_cast<int>(st.st_mode));
    }

    struct stat targetSt {};
    const struct stat *existing = nullptr;
    if (::lstat(target, &targetSt) == 0) {
        if (targetSt.st_dev == st.st_dev && targetSt.st_ino == st.st_ino) {
            // Already there. A file's size still counts as copied, so progress totals complete.
            if (S_ISREG(st.st_mode) && st.st_size > 0
                && !callbacks.onProgress(static_cast<std::uint64_t>(st.st_size))) {
                return fail(CopyFileStage::kAborted, ECANCELED);
            }
            return {};
        }
        if (!options.replaceExisting) {
            return fail(CopyFileStage::kTargetExists, EEXIST);
        }
        existing = &targetSt;
    } else if (errno != ENOENT) {
        return fail(CopyFileStage::kStatTarget, errno);
    }

    if (S_ISREG(st.st_mode)) {
        return copyRegular(source, target, existing, options, callbacks);
    }
    if (S_ISDIR(st.st_mode)) {
        return copyDirectory(source, target, st, existing, options);
    }
    return copyLink(source, target, st, existing, options);
}

}  // namespace filepan
