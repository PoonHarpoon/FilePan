// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "filepan/archive_stream.hpp"

namespace filepan {

// Damage found while verifying an archive.
struct ArchiveProblem {
    // The entry's path as stored; empty for the archive as a whole, e.g. a compressed tar's
    // stream or a header that couldn't be read.
    std::string path;
    std::string message;
};

class VerifyArchiveCallbacks {
public:
    virtual ~VerifyArchiveCallbacks() = default;

    // Reports archive bytes read since the last call, and the entry being read. Returns false to
    // abort.
    virtual bool onProgress(std::uint64_t readSize, std::string_view currentPath) = 0;
};

struct VerifyArchiveResult {
    // 0 once the archive was read through, damaged or not. ECANCELED if a callback aborted,
    // EACCES when an encrypted entry or header needs a password passwords lacks, ENOMEM, or EIO
    // when the source failed (it has its own error).
    int error = 0;
    std::string message;
    // Headers read.
    std::uint64_t entryCount = 0;
    // Empty for an intact archive. Reading stops after kMaxArchiveProblems.
    std::vector<ArchiveProblem> problems;
};

inline constexpr std::size_t kMaxArchiveProblems = 100;

// Reads the whole archive, decompressing every entry, so that each format's and compressor's own
// checks run: zip, 7z and rar CRCs, tar header checksums, xz, bzip2, zstd and lz4 checks. A
// compressed stream is read to its end, past the archive's end marker, and a gzip stream's CRC-32
// and length, which libarchive doesn't check, are checked here. A plain compressed file (a .gz
// that isn't a tar) is verified as one stream.
//
// A seekable source lets zip and 7z read their central directory; it's read from the start twice
// for those, the first time only far enough to tell that it isn't compressed.
VerifyArchiveResult verifyArchive(ArchiveSource &source, bool isSeekable,
                                  const std::vector<std::string> &passwords,
                                  VerifyArchiveCallbacks &callbacks);

}  // namespace filepan
