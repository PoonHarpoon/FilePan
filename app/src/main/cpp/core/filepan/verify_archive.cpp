// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/verify_archive.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <map>
#include <memory>
#include <optional>

#include <archive.h>
#include <archive_entry.h>
#include <zlib.h>

namespace filepan {

namespace {

constexpr std::size_t kReadBlockSize = 1024 * 1024;
// Enough for libarchive to bid on the filters and formats, before a seekable source is reread.
constexpr std::size_t kProbeBlockSize = 64 * 1024;
constexpr auto kProgressInterval = std::chrono::milliseconds(200);
// Header failures in a row before giving up on finding the next good one.
constexpr int kMaxHeaderFailures = 16;

struct ArchiveDeleter {
    void operator()(archive *a) const { archive_read_free(a); }
};
using ArchivePtr = std::unique_ptr<archive, ArchiveDeleter>;

std::string errorString(archive *a) {
    const char *string = archive_error_string(a);
    return string ? string : "";
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// libarchive says so in words only: "Passphrase required for this entry", "Incorrect passphrase".
bool isPasswordMessage(std::string_view message) {
    return contains(message, "assphrase") || contains(message, "assword");
}

// "The file is encrypted, but currently not supported" (7z), "RAR encryption support
// unavailable": libarchive can't decrypt these at all, so they can't be checked.
bool isUnsupportedEncryption(std::string_view message) {
    return contains(message, "ncrypt")
           && (contains(message, "support") || contains(message, "unavailable"));
}

// Inflates gzip members from the archive's own bytes as they're read, only to check each
// member's CRC-32 and length, which libarchive's gzip filter reads past unchecked.
class GzipCheck {
public:
    GzipCheck() { ok_ = inflateInit2(&stream_, 15 + 16) == Z_OK; }
    ~GzipCheck() {
        if (ok_) {
            inflateEnd(&stream_);
        }
    }
    GzipCheck(const GzipCheck &) = delete;
    GzipCheck &operator=(const GzipCheck &) = delete;

    void feed(const std::uint8_t *data, std::size_t size) {
        while (ok_ && size > 0 && !done_) {
            if (between_) {
                // Another member, or trailing data, which gzip and libarchive ignore.
                inflateReset(&stream_);
                between_ = false;
            }
            stream_.next_in = const_cast<Bytef *>(data);
            stream_.avail_in = static_cast<uInt>(size);
            int status;
            do {
                stream_.next_out = out_;
                stream_.avail_out = sizeof(out_);
                status = inflate(&stream_, Z_NO_FLUSH);
            } while (status == Z_OK && (stream_.avail_in > 0 || stream_.avail_out == 0));
            const std::size_t used = size - stream_.avail_in;
            data += used;
            size -= used;
            if (status == Z_STREAM_END) {
                ++members_;
                between_ = true;
                continue;
            }
            if (status == Z_OK || status == Z_BUF_ERROR) {
                continue;
            }
            // Bytes after the last member that don't start another one are trailing data.
            if (members_ > 0 && status == Z_DATA_ERROR && stream_.total_in <= 2) {
                done_ = true;
                return;
            }
            error_ = std::string("gzip: ") + (stream_.msg ? stream_.msg : "damaged data");
            done_ = true;
        }
    }

    // The damage found, once the stream has been read to its end; empty for none.
    [[nodiscard]] std::string finish() const {
        if (!ok_ || done_ || between_) {
            return error_;
        }
        // Inside a member: the stream ended early. A reset with nothing read since is fine.
        return members_ == 0 || stream_.total_in > 0 ? "gzip: unexpected end of data" : "";
    }

private:
    z_stream stream_{};
    bool ok_ = false;
    bool between_ = false;
    bool done_ = false;
    std::uint64_t members_ = 0;
    std::string error_;
    Bytef out_[64 * 1024];
};

class Verifier {
public:
    Verifier(ArchiveSource &source, const std::vector<std::string> &passwords,
             VerifyArchiveCallbacks &callbacks)
        : source_(source),
          passwords_(passwords),
          callbacks_(callbacks),
          buffer_(std::make_unique<std::uint8_t[]>(kReadBlockSize)) {}

    VerifyArchiveResult run(bool isSeekable) {
        // First through the filters only, as one raw stream: whether it's compressed decides
        // how the archive in it is read.
        outer_.reset(archive_read_new());
        if (!outer_) {
            return outOfMemory();
        }
        archive_read_support_filter_all(outer_.get());
        archive_read_support_format_raw(outer_.get());
        gzip_.emplace();
        blockSize_ = kProbeBlockSize;
        probing_ = true;
        setSourceCallbacks(outer_.get(), false);
        archive_entry *entry;
        if (archive_read_open1(outer_.get()) != ARCHIVE_OK
            || archive_read_next_header(outer_.get(), &entry) != ARCHIVE_OK) {
            archiveFailure(outer_.get(), "");
            return finish();
        }
        probing_ = false;
        blockSize_ = kReadBlockSize;
        const int filters = archive_filter_count(outer_.get());
        const bool compressed = filters > 1;
        if (!compressed && isSeekable) {
            // Read straight from the source instead, which lets zip and 7z seek.
            outer_.reset();
            gzip_.reset();
            pendingProgress_ = 0;
            covered_.clear();
            position_ = 0;
            if (source_.seek(0, SEEK_SET) != 0) {
                sourceFailed_ = true;
                return finish();
            }
            ArchivePtr direct(newFormatArchive());
            if (!direct) {
                return outOfMemory();
            }
            seekable_ = true;
            setSourceCallbacks(direct.get(), true);
            if (archive_read_open1(direct.get()) == ARCHIVE_OK) {
                verifyEntries(direct.get());
            } else {
                archiveFailure(direct.get(), "");
            }
            return finish();
        }
        // The raw bytes must be gzip for its check to apply; filter 0 is the outermost.
        if (archive_filter_code(outer_.get(), filters - 2) != ARCHIVE_FILTER_GZIP) {
            gzip_.reset();
        }

        // The archive inside, read from the decompressed stream.
        ArchivePtr inner(newFormatArchive());
        if (!inner) {
            return outOfMemory();
        }
        archive_read_set_callback_data(inner.get(), this);
        archive_read_set_read_callback(
            inner.get(), [](archive *a, void *data, const void **buffer) -> la_ssize_t {
                return static_cast<Verifier *>(data)->readOuter(a, buffer);
            });
        if (archive_read_open1(inner.get()) == ARCHIVE_OK) {
            verifyEntries(inner.get());
            if (stopped() || outerFailed_ || fatal_) {
                return finish();
            }
        } else if (stopped() || outerFailed_ || !compressed
                   || archive_errno(inner.get()) == ENOMEM) {
            archiveFailure(inner.get(), "");
            return finish();
        }
        // Otherwise it isn't an archive but a plain compressed file, verified as its one stream.

        // What follows the archive's end marker, up to the stream's end: a compressor's trailer
        // and checks are only read there.
        const void *block;
        size_t size;
        la_int64_t offset;
        int status;
        while ((status = archive_read_data_block(outer_.get(), &block, &size, &offset))
               == ARCHIVE_OK) {
        }
        if (status != ARCHIVE_EOF) {
            archiveFailure(outer_.get(), "");
        } else if (gzip_) {
            if (std::string damage = gzip_->finish(); !damage.empty()) {
                addProblem("", std::move(damage));
            }
        }
        return finish();
    }

private:
    archive *newFormatArchive() {
        archive *a = archive_read_new();
        if (!a) {
            return nullptr;
        }
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        for (const auto &password : passwords_) {
            archive_read_add_passphrase(a, password.c_str());
        }
        return a;
    }

    void setSourceCallbacks(archive *a, bool seekable) {
        archive_read_set_callback_data(a, this);
        archive_read_set_read_callback(
            a, [](archive *a, void *data, const void **buffer) -> la_ssize_t {
                return static_cast<Verifier *>(data)->readSource(a, buffer);
            });
        if (!seekable) {
            return;
        }
        archive_read_set_seek_callback(
            a, [](archive *a, void *data, la_int64_t offset, int whence) -> la_int64_t {
                auto *self = static_cast<Verifier *>(data);
                const std::int64_t position = self->source_.seek(offset, whence);
                if (position < 0) {
                    self->sourceFailed_ = true;
                    archive_set_error(a, EIO, "Can't seek archive");
                    return ARCHIVE_FATAL;
                }
                self->position_ = static_cast<std::uint64_t>(position);
                return position;
            });
        archive_read_set_skip_callback(
            a, [](archive *a, void *data, la_int64_t request) -> la_int64_t {
                auto *self = static_cast<Verifier *>(data);
                const std::int64_t position = self->source_.seek(request, SEEK_CUR);
                if (position < 0) {
                    self->sourceFailed_ = true;
                    archive_set_error(a, EIO, "Can't seek archive");
                    return ARCHIVE_FATAL;
                }
                self->position_ = static_cast<std::uint64_t>(position);
                return request;
            });
    }

    la_ssize_t readSource(archive *a, const void **buffer) {
        if (!flushProgress(false)) {
            aborted_ = true;
            archive_set_error(a, ECANCELED, "Cancelled");
            return ARCHIVE_FATAL;
        }
        const std::int64_t size = source_.read(buffer_.get(), blockSize_);
        if (size < 0) {
            sourceFailed_ = true;
            archive_set_error(a, EIO, "Can't read archive");
            return ARCHIVE_FATAL;
        }
        if (gzip_) {
            gzip_->feed(buffer_.get(), static_cast<std::size_t>(size));
        }
        pendingProgress_ += cover(position_, static_cast<std::uint64_t>(size));
        position_ += static_cast<std::uint64_t>(size);
        *buffer = buffer_.get();
        return static_cast<la_ssize_t>(size);
    }

    // The inner archive's reads: the outer one's decompressed stream, without copying it.
    la_ssize_t readOuter(archive *a, const void **buffer) {
        size_t size;
        la_int64_t offset;
        const int status = archive_read_data_block(outer_.get(), buffer, &size, &offset);
        if (status == ARCHIVE_EOF) {
            return 0;
        }
        if (status != ARCHIVE_OK) {
            outerFailed_ = true;
            const int code = archive_errno(outer_.get());
            // -1 is libarchive's own "misc" errno.
            archive_set_error(a, code ? code : -1, "%s",
                              errorString(outer_.get()).c_str());
            return ARCHIVE_FATAL;
        }
        return static_cast<la_ssize_t>(size);
    }

    void verifyEntries(archive *a) {
        int failures = 0;
        while (true) {
            if (problems_.size() >= kMaxArchiveProblems) {
                fatal_ = true;
                return;
            }
            archive_entry *entry;
            const int status = archive_read_next_header(a, &entry);
            if (status == ARCHIVE_EOF) {
                return;
            }
            // A warning is e.g. a name that can't be converted, which still has its stored form.
            if (status < ARCHIVE_WARN) {
                if (archiveFailure(a, "") || status == ARCHIVE_FATAL
                    || ++failures >= kMaxHeaderFailures) {
                    fatal_ = true;
                    return;
                }
                continue;
            }
            failures = 0;
            ++entryCount_;
            const char *utf8 = archive_entry_pathname_utf8(entry);
            const char *stored = archive_entry_pathname(entry);
            currentPath_ = utf8 ? utf8 : stored ? stored : "";
            // Reading every block runs the format's and compressor's checks.
            const void *block;
            size_t size;
            la_int64_t offset;
            int data;
            while ((data = archive_read_data_block(a, &block, &size, &offset)) == ARCHIVE_OK) {
            }
            if (data == ARCHIVE_EOF) {
                continue;
            }
            if (archiveFailure(a, currentPath_) || data == ARCHIVE_FATAL) {
                fatal_ = true;
                return;
            }
        }
    }

    // Records a's failure as damage at path (empty for the archive), unless it's an abort, a
    // source failure, a lack of memory or encryption. Returns true if verifying must stop.
    bool archiveFailure(archive *a, const std::string &path) {
        if (stopped()) {
            return true;
        }
        std::string message = errorString(a);
        if (archive_errno(a) == ENOMEM) {
            stop(ENOMEM, "Out of memory");
            return true;
        }
        // From the decompressed stream, it's the stream's damage, whatever it says.
        if (!outerFailed_ && isPasswordMessage(message)) {
            stop(EACCES, std::move(message));
            return true;
        }
        if (!outerFailed_ && isUnsupportedEncryption(message)) {
            stop(ENOTSUP, std::move(message));
            return true;
        }
        // 7z reads its header from the end: a stream that can't seek (an archive in an archive)
        // can't be checked, which isn't damage.
        if (!outerFailed_ && !seekable_
            && (contains(message, "Seek error") || contains(message, "Cannot seek"))) {
            stop(ENOTSUP, "Can't check this archive without random access");
            return true;
        }
        addProblem(path, message.empty() ? "Not a readable archive" : std::move(message));
        return false;
    }

    void addProblem(std::string path, std::string message) {
        problems_.push_back({std::move(path), std::move(message)});
    }

    void stop(int error, std::string message) {
        error_ = error;
        message_ = std::move(message);
    }

    VerifyArchiveResult outOfMemory() {
        stop(ENOMEM, "Out of memory");
        return finish();
    }

    [[nodiscard]] bool stopped() const { return aborted_ || sourceFailed_ || error_ != 0; }

    VerifyArchiveResult finish() {
        VerifyArchiveResult result;
        if (aborted_) {
            result.error = ECANCELED;
        } else if (sourceFailed_) {
            result.error = EIO;
            result.message = "Can't read archive";
        } else if (error_) {
            result.error = error_;
            result.message = std::move(message_);
        } else if (!flushProgress(true)) {
            result.error = ECANCELED;
        }
        result.entryCount = entryCount_;
        if (!result.error) {
            result.problems = std::move(problems_);
        }
        return result;
    }

    // Marks [start, start + size) as read and returns how much of it is new: zip and 7z read
    // their directory at the end and some blocks twice, but progress counts each byte once.
    std::uint64_t cover(std::uint64_t start, std::uint64_t size) {
        const std::uint64_t end = start + size;
        std::uint64_t fresh = size;
        std::uint64_t mergedStart = start;
        std::uint64_t mergedEnd = end;
        auto it = covered_.upper_bound(start);
        if (it != covered_.begin() && std::prev(it)->second >= start) {
            --it;
        }
        // The ranges are disjoint; merge every one that overlaps or touches this one.
        while (it != covered_.end() && it->first <= end) {
            const std::uint64_t low = std::max(start, it->first);
            const std::uint64_t high = std::min(end, it->second);
            if (high > low) {
                fresh -= high - low;
            }
            mergedStart = std::min(mergedStart, it->first);
            mergedEnd = std::max(mergedEnd, it->second);
            it = covered_.erase(it);
        }
        covered_.emplace(mergedStart, mergedEnd);
        return fresh;
    }

    bool flushProgress(bool force) {
        // A seekable source may be reread from the start after probing; only that read counts.
        if (probing_ || pendingProgress_ == 0) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastProgress_ < kProgressInterval) {
            return true;
        }
        lastProgress_ = now;
        const auto size = pendingProgress_;
        pendingProgress_ = 0;
        return callbacks_.onProgress(size, currentPath_);
    }

    ArchiveSource &source_;
    const std::vector<std::string> &passwords_;
    VerifyArchiveCallbacks &callbacks_;
    std::unique_ptr<std::uint8_t[]> buffer_;
    std::size_t blockSize_ = kReadBlockSize;
    bool probing_ = false;
    ArchivePtr outer_;
    std::optional<GzipCheck> gzip_;
    bool aborted_ = false;
    bool sourceFailed_ = false;
    // The decompressed stream failed under the inner archive, whose error says so.
    bool outerFailed_ = false;
    // Reading stopped at damage it can't read past.
    bool fatal_ = false;
    // The archive is read with seeking.
    bool seekable_ = false;
    int error_ = 0;
    std::string message_;
    std::uint64_t entryCount_ = 0;
    std::string currentPath_;
    std::vector<ArchiveProblem> problems_;
    std::uint64_t pendingProgress_ = 0;
    // Where the source is, and the ranges of it read so far (start to end).
    std::uint64_t position_ = 0;
    std::map<std::uint64_t, std::uint64_t> covered_;
    std::chrono::steady_clock::time_point lastProgress_ = std::chrono::steady_clock::now();
};

}  // namespace

VerifyArchiveResult verifyArchive(ArchiveSource &source, bool isSeekable,
                                  const std::vector<std::string> &passwords,
                                  VerifyArchiveCallbacks &callbacks) {
    return Verifier(source, passwords, callbacks).run(isSeekable);
}

}  // namespace filepan
