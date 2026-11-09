// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/move.hpp"

#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "filepan/copy_file.hpp"
#include "filepan/delete_tree.hpp"
#include "filepan/directory_reader.hpp"
#include "filepan/eintr.hpp"

namespace filepan {

namespace {

// RENAME_NOREPLACE from <linux/fs.h>, which clashes with other headers on some libcs.
constexpr unsigned kRenameNoReplace = 1u << 0;

class NameCollector : public DirectoryEntrySink {
public:
    void onEntry(const DirectoryEntryView &entry) override { names.emplace_back(entry.name); }
    std::vector<std::string> names;
};

int readNames(const std::string &path, std::vector<std::string> &names) {
    const int fd = DirectoryReader::open(path.c_str());
    if (fd < 0) {
        return -fd;
    }
    DirectoryReader reader(fd);
    NameCollector collector;
    while (!reader.atEnd()) {
        if (const int error = reader.readEntries(false, collector)) {
            return error;
        }
    }
    names = std::move(collector.names);
    return 0;
}

class QuietDelete : public DeleteTreeCallbacks {
public:
    bool onProgress(std::uint64_t, std::string_view) override { return true; }
    bool onError(std::string_view path, ErrorKind, int error) override {
        if (firstError == 0) {
            firstError = error;
            firstErrorPath = path;
        }
        return true;
    }

    int firstError = 0;
    std::string firstErrorPath;
};

const char *describe(CopyFileStage stage) {
    switch (stage) {
        case CopyFileStage::kStatSource: return "stat source";
        case CopyFileStage::kStatTarget: return "stat target";
        case CopyFileStage::kTargetExists: return "target exists";
        case CopyFileStage::kRemoveTarget: return "remove target";
        case CopyFileStage::kOpenSource: return "open source";
        case CopyFileStage::kOpenTarget: return "open target";
        case CopyFileStage::kCopyData: return "copy data";
        case CopyFileStage::kCloseTarget: return "close target";
        case CopyFileStage::kMakeDirectory: return "make directory";
        case CopyFileStage::kReadLink: return "read link";
        case CopyFileStage::kCreateLink: return "create link";
        case CopyFileStage::kUnsupportedType: return "unsupported file type";
        case CopyFileStage::kNone:
        case CopyFileStage::kAborted: break;
    }
    return "copy";
}

// Copies a tree with copyFile(), reporting throttled progress.
class TreeCopier : public CopyFileCallbacks {
public:
    TreeCopier(const MoveOptions &options, MoveCallbacks &callbacks)
        : options_(options), callbacks_(callbacks) {}

    bool onProgress(std::uint64_t copiedSize) override {
        // copyFile() also reports a directory's or link's st_size; only file data counts.
        if (countBytes_) {
            pendingBytes_ += copiedSize;
        }
        return flush(false);
    }

    Result<void> copy(const std::string &source, const std::string &target) {
        struct stat st {};
        if (FILEPAN_RETRY(lstat(source.c_str(), &st)) == -1) {
            return failErrno(source);
        }
        current_ = source;
        countBytes_ = S_ISREG(st.st_mode);
        const CopyFileOptions copyOptions{
            .replaceExisting = false,
            .noFollowLinks = true,
            .copyAttributes = true,
            .progressInterval = options_.progressInterval,
        };
        const CopyFileResult result = copyFile(source.c_str(), target.c_str(), copyOptions, *this);
        if (result.stage == CopyFileStage::kAborted) {
            return fail(ECANCELED);
        }
        if (result.stage != CopyFileStage::kNone) {
            const int code = result.stage == CopyFileStage::kUnsupportedType ? ENOTSUP
                                                                              : result.error;
            return fail(code, std::string(describe(result.stage)) + ": " + source);
        }
        if (S_ISDIR(st.st_mode)) {
            if (auto copied = copyChildren(source, target, st); !copied) {
                return copied;
            }
        }
        ++pendingCount_;
        return flush(false) ? Result<void>() : fail(ECANCELED);
    }

    bool flush(bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastProgress_ < options_.progressInterval) {
            return true;
        }
        if (pendingBytes_ == 0 && pendingCount_ == 0 && !force) {
            return true;
        }
        lastProgress_ = now;
        const auto bytes = pendingBytes_;
        const auto count = pendingCount_;
        pendingBytes_ = 0;
        pendingCount_ = 0;
        return callbacks_.onProgress(bytes, count, current_);
    }

private:
    Result<void> copyChildren(const std::string &source, const std::string &target,
                              const struct stat &st) {
        // copyFile() already applied the source's mode; a read-only directory still needs to
        // take its children.
        const mode_t mode = st.st_mode & 07777;
        const bool restoreMode = (mode & 0700) != 0700;
        if (restoreMode) {
            FILEPAN_RETRY(chmod(target.c_str(), mode | 0700));
        }
        std::vector<std::string> names;
        if (const int error = readNames(source, names)) {
            return fail(error, source);
        }
        for (const auto &name : names) {
            if (auto copied = copy(source + "/" + name, target + "/" + name); !copied) {
                return copied;
            }
        }
        if (restoreMode) {
            FILEPAN_RETRY(chmod(target.c_str(), mode));
        }
        // Adding the children bumped the modification time.
        const struct timespec times[2] = {st.st_atim, st.st_mtim};
        FILEPAN_RETRY(utimensat(AT_FDCWD, target.c_str(), times, AT_SYMLINK_NOFOLLOW));
        return {};
    }

    const MoveOptions &options_;
    MoveCallbacks &callbacks_;
    std::string current_;
    bool countBytes_ = false;
    std::uint64_t pendingBytes_ = 0;
    std::uint64_t pendingCount_ = 0;
    std::chrono::steady_clock::time_point lastProgress_ = std::chrono::steady_clock::now();
};

bool isEmptyDirectory(const std::string &path) {
    std::vector<std::string> names;
    return readNames(path, names) == 0 && names.empty();
}

// Makes room for the copy the way rename(2) would replace the target.
Result<void> removeTargetForReplace(const std::string &source, const std::string &target) {
    struct stat sourceSt {};
    struct stat targetSt {};
    if (FILEPAN_RETRY(lstat(source.c_str(), &sourceSt)) == -1) {
        return failErrno(source);
    }
    if (FILEPAN_RETRY(lstat(target.c_str(), &targetSt)) == -1) {
        return errno == ENOENT ? Result<void>() : failErrno(target);
    }
    const bool sourceIsDir = S_ISDIR(sourceSt.st_mode);
    const bool targetIsDir = S_ISDIR(targetSt.st_mode);
    if (sourceIsDir != targetIsDir) {
        return fail(sourceIsDir ? ENOTDIR : EISDIR, target);
    }
    if (targetIsDir) {
        if (!isEmptyDirectory(target)) {
            return fail(ENOTEMPTY, target);
        }
        return FILEPAN_RETRY(rmdir(target.c_str())) == 0 ? Result<void>() : failErrno(target);
    }
    return FILEPAN_RETRY(unlink(target.c_str())) == 0 ? Result<void>() : failErrno(target);
}

}  // namespace

int MoveOperations::rename(const char *source, const char *target, bool replace) {
    if (replace) {
        return FILEPAN_RETRY(::rename(source, target));
    }
    const long result = FILEPAN_RETRY(
        syscall(SYS_renameat2, AT_FDCWD, source, AT_FDCWD, target, kRenameNoReplace));
    if (result == 0 || errno != EINVAL) {
        return static_cast<int>(result);
    }
    // Some file systems (older FUSE ones in particular) reject the flag. This check races with
    // other writers, which RENAME_NOREPLACE wouldn't.
    struct stat st {};
    if (FILEPAN_RETRY(lstat(target, &st)) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) {
        return -1;
    }
    return FILEPAN_RETRY(::rename(source, target));
}

Result<MoveMethod> movePath(const std::string &source, const std::string &target,
                            const MoveOptions &options, MoveCallbacks &callbacks,
                            MoveOperations &operations) {
    if (operations.rename(source.c_str(), target.c_str(), options.replaceExisting) == 0) {
        return MoveMethod::kRenamed;
    }
    if (errno != EXDEV) {
        return failErrno(source);
    }

    struct stat st {};
    if (FILEPAN_RETRY(lstat(target.c_str(), &st)) == 0) {
        if (!options.replaceExisting) {
            return fail(EEXIST, target);
        }
        if (auto removed = removeTargetForReplace(source, target); !removed) {
            return std::unexpected(removed.error());
        }
    } else if (errno != ENOENT) {
        return failErrno(target);
    }

    TreeCopier copier(options, callbacks);
    auto copied = copier.copy(source, target);
    if (copied && !copier.flush(true)) {
        copied = fail(ECANCELED);
    }
    if (!copied) {
        QuietDelete cleanup;
        if (FILEPAN_RETRY(lstat(target.c_str(), &st)) == 0) {
            deleteTree(target, cleanup);
        }
        return std::unexpected(copied.error());
    }

    QuietDelete deleter;
    const DeleteTreeResult deleted = deleteTree(source, deleter);
    if (deleted.error || deleter.firstError) {
        const int error = deleted.error ? deleted.error : deleter.firstError;
        return fail(error, "copied, but couldn't delete the source: "
                               + (deleter.firstErrorPath.empty() ? source
                                                                 : deleter.firstErrorPath));
    }
    return MoveMethod::kCopied;
}

Result<MoveMethod> movePath(const std::string &source, const std::string &target,
                            const MoveOptions &options, MoveCallbacks &callbacks) {
    MoveOperations operations;
    return movePath(source, target, options, callbacks, operations);
}

}  // namespace filepan
