// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct archive;

namespace filepan {

struct CreateArchiveOptions {
    // libarchive's ARCHIVE_FORMAT_* and ARCHIVE_FILTER_* codes.
    int format;
    int filter;
    // Zip only; encrypts with AES-256 when non-empty.
    std::string password;
    // For compressors that can use several threads; 0 picks the number of CPUs.
    unsigned threads = 0;
    // The compressor's level in its own scale (zip, xz and 7z 0-9, zstd 1-22); -1 keeps the
    // default.
    int level = -1;
};

struct CreateArchiveSource {
    std::string path;
    // Name of the source in the archive; its descendants are named relative to it.
    std::string entryName;
};

class CreateArchiveCallbacks {
public:
    virtual ~CreateArchiveCallbacks() = default;

    // Reports bytes read from sources and entries finished since the last call. Returns false to
    // abort.
    virtual bool onProgress(std::uint64_t readSize, std::uint64_t entryCount,
                            std::string_view currentPath) = 0;

    // Reports a source that couldn't be read. Returns true to skip it, or false to abort.
    virtual bool onSourceError(std::string_view path, int error) = 0;
};

struct CreateArchiveResult {
    // 0, ECANCELED, or an error writing the archive, with libarchive's message if any.
    int error = 0;
    std::string message;
};

// Applies the options to a new libarchive write handle. Returns false on failure, with the error
// set on the handle.
bool configureArchiveWrite(archive *a, const CreateArchiveOptions &options);

// Writes the sources, recursively and without following symbolic links, to a new archive file at
// target. A partial archive is left for the caller to remove.
CreateArchiveResult createArchive(const std::string &target, const CreateArchiveOptions &options,
                                  const std::vector<CreateArchiveSource> &sources,
                                  CreateArchiveCallbacks &callbacks);

}  // namespace filepan
