// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/extract_archive.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filepan/eintr.hpp"

namespace filepan {

namespace {

using Stage = ExtractCallbacks::Stage;

constexpr std::size_t kReadBlockSize = 1024 * 1024;
constexpr auto kProgressInterval = std::chrono::milliseconds(200);

struct ArchiveDeleter {
    void operator()(archive *a) const { archive_read_free(a); }
};

std::string errorString(archive *a) {
    const char *string = archive_error_string(a);
    return string ? string : "";
}

class Extractor {
public:
    Extractor(const std::vector<ExtractItem> &items, ExtractCallbacks &callbacks)
        : items_(items), callbacks_(callbacks) {}

    ExtractResult run(const std::string &archivePath, const std::vector<std::string> &passwords) {
        std::unique_ptr<archive, ArchiveDeleter> a(archive_read_new());
        archive_read_support_filter_all(a.get());
        archive_read_support_format_all(a.get());
        // Raw streams have no headers, so the index would be meaningless; the old reader didn't
        // enable the raw format either.
        for (const auto &password : passwords) {
            archive_read_add_passphrase(a.get(), password.c_str());
        }
        if (archive_read_open_filename(a.get(), archivePath.c_str(), kReadBlockSize)
            != ARCHIVE_OK) {
            return archiveFailure(Stage::kOpenArchive, a.get());
        }
        auto next = items_.begin();
        for (std::uint64_t index = 0; next != items_.end(); ++index) {
            archive_entry *entry;
            const int status = archive_read_next_header(a.get(), &entry);
            if (status == ARCHIVE_EOF) {
                callbacks_.onError(&*next, Stage::kMissingEntry, ENOENT, "");
                result_.error = EIO;
                return result_;
            }
            if (status < ARCHIVE_WARN) {
                return archiveFailure(Stage::kReadArchive, a.get());
            }
            if (next->index != index) {
                continue;
            }
            const ExtractItem &item = *next++;
            if (!extractItem(a.get(), entry, item)) {
                result_.error = ECANCELED;
                return result_;
            }
        }
        if (!flushProgress(true)) {
            result_.error = ECANCELED;
        }
        return result_;
    }

private:
    ExtractResult archiveFailure(Stage stage, archive *a) {
        const int error = archive_errno(a);
        callbacks_.onError(nullptr, stage, error ? error : EIO, errorString(a));
        result_.error = EIO;
        return result_;
    }

    // Returns false to abort.
    bool fail(const ExtractItem &item, Stage stage, int error, const std::string &message = "") {
        unlink(item.target.c_str());
        return flushProgress(true) && callbacks_.onError(&item, stage, error, message);
    }

    bool extractItem(archive *a, archive_entry *entry, const ExtractItem &item) {
        const mode_t type = archive_entry_filetype(entry);
        const bool typeMatches = item.type == ExtractItem::Type::kSymbolicLink
                                     ? type == AE_IFLNK
                                     : type == AE_IFREG || archive_entry_hardlink(entry);
        if (!typeMatches
            || (item.size >= 0 && archive_entry_size_is_set(entry)
                && archive_entry_size(entry) != item.size)) {
            return fail(item, Stage::kEntryMismatch, EIO);
        }
        if (item.type == ExtractItem::Type::kSymbolicLink) {
            return extractSymbolicLink(entry, item);
        }
        if (const char *hardlink = archive_entry_hardlink(entry);
            hardlink && archive_entry_size(entry) == 0) {
            if (const auto original = extractedHardlinkTargets_.find(hardlink);
                original != extractedHardlinkTargets_.end()) {
                return copyExtracted(original->second, item);
            }
        }
        return extractFile(a, entry, item);
    }

    bool extractSymbolicLink(archive_entry *entry, const ExtractItem &item) {
        const char *linkTarget = archive_entry_symlink(entry);
        if (!linkTarget) {
            return fail(item, Stage::kEntryMismatch, EIO);
        }
        unlink(item.target.c_str());
        if (FILEPAN_RETRY(symlink(linkTarget, item.target.c_str())) == -1) {
            return fail(item, Stage::kCreateTarget, errno);
        }
        ++result_.extractedCount;
        return true;
    }

    bool extractFile(archive *a, archive_entry *entry, const ExtractItem &item) {
        // Replace rather than write through whatever is there, e.g. a symbolic link.
        unlink(item.target.c_str());
        const mode_t mode = archive_entry_perm(entry) ? (archive_entry_perm(entry) & 0777) : 0666;
        const int fd = FILEPAN_RETRY(open(item.target.c_str(),
                                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                          mode));
        if (fd == -1) {
            return fail(item, Stage::kCreateTarget, errno);
        }
        const void *block;
        size_t blockSize;
        la_int64_t offset;
        la_int64_t end = 0;
        int status;
        while ((status = archive_read_data_block(a, &block, &blockSize, &offset)) == ARCHIVE_OK) {
            // Offsets can skip ahead in sparse entries; pwrite leaves the hole unallocated.
            for (size_t written = 0; written < blockSize;) {
                const ssize_t count = FILEPAN_RETRY(
                    pwrite(fd, static_cast<const char *>(block) + written, blockSize - written,
                           offset + static_cast<la_int64_t>(written)));
                if (count == -1) {
                    const int error = errno;
                    ::close(fd);
                    return fail(item, Stage::kWriteTarget, error);
                }
                written += static_cast<size_t>(count);
            }
            end = offset + static_cast<la_int64_t>(blockSize);
            pendingProgress_ += blockSize;
            if (!flushProgress(false)) {
                ::close(fd);
                unlink(item.target.c_str());
                return false;
            }
        }
        if (status != ARCHIVE_EOF) {
            const int error = archive_errno(a);
            const std::string message = errorString(a);
            ::close(fd);
            const bool needsPassword = archive_entry_is_encrypted(entry) && error != ENOMEM;
            return fail(item, needsPassword ? Stage::kPasswordRequired : Stage::kReadArchive,
                        error ? error : EIO, message);
        }
        // A sparse file may end in a hole that pwrite never reached.
        if (archive_entry_size_is_set(entry) && archive_entry_size(entry) > end) {
            if (FILEPAN_RETRY(ftruncate(fd, archive_entry_size(entry))) == -1) {
                const int error = errno;
                ::close(fd);
                return fail(item, Stage::kWriteTarget, error);
            }
        }
        if (archive_entry_mtime_is_set(entry)) {
            const struct timespec times[2] = {
                {0, UTIME_OMIT},
                {archive_entry_mtime(entry), archive_entry_mtime_nsec(entry)},
            };
            futimens(fd, times);
        }
        if (::close(fd) == -1) {
            return fail(item, Stage::kWriteTarget, errno);
        }
        if (const char *pathname = archive_entry_pathname(entry)) {
            extractedHardlinkTargets_.emplace(pathname, item.target);
        }
        ++result_.extractedCount;
        return true;
    }

    bool copyExtracted(const std::string &source, const ExtractItem &item) {
        unlink(item.target.c_str());
        const int sourceFd = FILEPAN_RETRY(open(source.c_str(), O_RDONLY | O_CLOEXEC));
        if (sourceFd == -1) {
            return fail(item, Stage::kCreateTarget, errno);
        }
        struct stat st {};
        fstat(sourceFd, &st);
        const int targetFd = FILEPAN_RETRY(open(
            item.target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
            st.st_mode & 0777));
        if (targetFd == -1) {
            const int error = errno;
            ::close(sourceFd);
            return fail(item, Stage::kCreateTarget, error);
        }
        ssize_t copied;
        while ((copied = FILEPAN_RETRY(copy_file_range(sourceFd, nullptr, targetFd, nullptr,
                                                       kReadBlockSize, 0))) > 0) {
            pendingProgress_ += static_cast<std::uint64_t>(copied);
        }
        const int error = copied == -1 ? errno : 0;
        ::close(sourceFd);
        if (::close(targetFd) == -1 || error) {
            return fail(item, Stage::kWriteTarget, error ? error : EIO);
        }
        ++result_.extractedCount;
        return flushProgress(false);
    }

    bool flushProgress(bool force) {
        if (pendingProgress_ == 0) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastProgress_ < kProgressInterval) {
            return true;
        }
        lastProgress_ = now;
        const auto size = pendingProgress_;
        pendingProgress_ = 0;
        return callbacks_.onProgress(size);
    }

    const std::vector<ExtractItem> &items_;
    ExtractCallbacks &callbacks_;
    ExtractResult result_;
    std::uint64_t pendingProgress_ = 0;
    std::chrono::steady_clock::time_point lastProgress_ = std::chrono::steady_clock::now();
    std::map<std::string, std::string> extractedHardlinkTargets_;
};

}  // namespace

ExtractResult extractArchive(const std::string &archivePath,
                             const std::vector<std::string> &passwords,
                             const std::vector<ExtractItem> &items, ExtractCallbacks &callbacks) {
    return Extractor(items, callbacks).run(archivePath, passwords);
}

}  // namespace filepan
