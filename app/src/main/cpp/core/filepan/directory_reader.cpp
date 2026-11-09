// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/directory_reader.hpp"

#include <cerrno>
#include <climits>
#include <cstring>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "filepan/eintr.hpp"

namespace filepan {

namespace {

// Matches the kernel's struct linux_dirent64; glibc and bionic disagree on exposing it.
struct LinuxDirent64 {
    std::uint64_t d_ino;
    std::int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

constexpr unsigned kStatxMask = STATX_BASIC_STATS | STATX_BTIME;
// Don't force a round trip to a remote or FUSE backend just to refresh cached attributes.
constexpr int kStatxFlags = AT_STATX_DONT_SYNC;

void putStat(ByteWriter &out, const struct statx &st) {
    out.put<std::uint32_t>(st.stx_mode);
    out.put<std::uint32_t>(st.stx_uid);
    out.put<std::uint32_t>(st.stx_gid);
    out.put<std::uint32_t>(st.stx_nlink);
    out.put<std::int64_t>(static_cast<std::int64_t>(st.stx_size));
    out.put<std::int64_t>(st.stx_atime.tv_sec);
    out.put<std::uint32_t>(st.stx_atime.tv_nsec);
    out.put<std::int64_t>(st.stx_mtime.tv_sec);
    out.put<std::uint32_t>(st.stx_mtime.tv_nsec);
    if (st.stx_mask & STATX_BTIME) {
        out.put<std::int64_t>(st.stx_btime.tv_sec);
        out.put<std::uint32_t>(st.stx_btime.tv_nsec);
    } else {
        out.put<std::int64_t>(DirectoryReader::kNoBirthTime);
        out.put<std::uint32_t>(0);
    }
    out.put<std::uint64_t>(makedev(st.stx_dev_major, st.stx_dev_minor));
    out.put<std::uint64_t>(st.stx_ino);
}

bool isDotOrDotDot(const char *name) {
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

// Stats one dirent and hands the result to sink.
void visitEntry(int dirFd, const LinuxDirent64 &dirent, bool withStat, DirectoryEntrySink &sink) {
    struct statx st {};
    int statErrno = 0;
    if (withStat
        && FILEPAN_RETRY(statx(dirFd, dirent.d_name, AT_SYMLINK_NOFOLLOW | kStatxFlags, kStatxMask,
                              &st)) == -1) {
        statErrno = errno;
    }
    const bool statOk = withStat && statErrno == 0;
    const bool isLink = statOk ? S_ISLNK(st.stx_mode) : dirent.d_type == DT_LNK;

    char target[PATH_MAX];
    ssize_t targetLength = -1;
    if (withStat && isLink) {
        targetLength = FILEPAN_RETRY(readlinkat(dirFd, dirent.d_name, target, sizeof(target)));
    }
    const bool hasTarget = targetLength >= 0;

    struct statx targetSt {};
    int targetErrno = 0;
    if (hasTarget
        && FILEPAN_RETRY(statx(dirFd, dirent.d_name, kStatxFlags, kStatxMask, &targetSt)) == -1) {
        targetErrno = errno;
    }

    sink.onEntry({
        .name = std::string_view(dirent.d_name),
        .type = dirent.d_type,
        .stat = statOk ? &st : nullptr,
        .statError = statErrno,
        .hasLinkTarget = hasTarget,
        .linkTarget = hasTarget ? std::string_view(target, static_cast<std::size_t>(targetLength))
                                : std::string_view(),
        .targetStat = hasTarget && targetErrno == 0 ? &targetSt : nullptr,
        .targetStatError = targetErrno,
    });
}

// Serializes entries in the format described in directory_reader.hpp.
class ByteWriterSink : public DirectoryEntrySink {
public:
    ByteWriterSink(ByteWriter &out, bool withStat) : out_(out), withStat_(withStat) {}

    void onEntry(const DirectoryEntryView &entry) override {
        std::uint8_t flags = 0;
        if (entry.stat) flags |= DirectoryReader::kStatOk;
        if (entry.hasLinkTarget) flags |= DirectoryReader::kHasLinkTarget;
        if (entry.targetStat) flags |= DirectoryReader::kTargetStatOk;
        out_.put<std::uint8_t>(entry.type);
        out_.put<std::uint8_t>(flags);
        out_.put<std::uint16_t>(static_cast<std::uint16_t>(entry.name.size()));
        out_.putBytes(entry.name);
        if (entry.stat) {
            putStat(out_, *entry.stat);
        } else if (withStat_) {
            out_.put<std::int32_t>(entry.statError);
        }
        if (entry.hasLinkTarget) {
            out_.put<std::uint16_t>(static_cast<std::uint16_t>(entry.linkTarget.size()));
            out_.putBytes(entry.linkTarget);
            if (entry.targetStat) {
                putStat(out_, *entry.targetStat);
            } else {
                out_.put<std::int32_t>(entry.targetStatError);
            }
        }
    }

private:
    ByteWriter &out_;
    bool withStat_;
};

}  // namespace

int DirectoryReader::open(const char *path) {
    const int fd = FILEPAN_RETRY(::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    return fd == -1 ? -errno : fd;
}

DirectoryReader::~DirectoryReader() {
    // close() must not be retried on EINTR on Linux.
    ::close(fd_);
}

int DirectoryReader::readEntries(bool withStat, DirectoryEntrySink &sink) {
    if (eof_) {
        return 0;
    }
    const long length = FILEPAN_RETRY(syscall(SYS_getdents64, fd_, buffer_, sizeof(buffer_)));
    if (length == -1) {
        return errno;
    }
    if (length == 0) {
        eof_ = true;
        return 0;
    }
    for (long offset = 0; offset < length;) {
        const auto &dirent = *reinterpret_cast<const LinuxDirent64 *>(buffer_ + offset);
        offset += dirent.d_reclen;
        if (!isDotOrDotDot(dirent.d_name)) {
            visitEntry(fd_, dirent, withStat, sink);
        }
    }
    return 0;
}

int DirectoryReader::readBatch(bool withStat, ByteWriter &out) {
    out.clear();
    out.reserve(sizeof(buffer_) * (withStat ? 4 : 1));
    ByteWriterSink sink(out, withStat);
    // Keep reading until something is emitted, so that a buffer of only "." and ".." isn't
    // mistaken for the end of the directory.
    while (out.empty() && !eof_) {
        if (const int error = readEntries(withStat, sink)) {
            return error;
        }
    }
    return 0;
}

}  // namespace filepan
