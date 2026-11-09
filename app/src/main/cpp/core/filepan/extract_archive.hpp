// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace filepan {

// One archive entry to extract, identified by its position among the archive's headers. The
// caller has already resolved conflicts: target either doesn't exist or may be replaced.
struct ExtractItem {
    enum class Type : std::int32_t { kFile = 0, kSymbolicLink = 1 };

    std::uint64_t index;
    Type type;
    // Expected size, checked so that a different reading of the archive can't write the wrong
    // entry's data; -1 to skip the check.
    std::int64_t size;
    std::string target;
};

class ExtractCallbacks {
public:
    enum class Stage : std::int32_t {
        kOpenArchive = 0,
        kReadArchive,
        kEntryMismatch,  // The entry at the planned index isn't what was planned.
        kCreateTarget,
        kWriteTarget,
        kPasswordRequired,
        kMissingEntry,  // The archive ended before the planned index.
    };

    virtual ~ExtractCallbacks() = default;

    // Reports bytes written since the last call. Returns false to abort.
    virtual bool onProgress(std::uint64_t writtenSize) = 0;

    // Reports an item that couldn't be extracted; its partial target is removed. Returns true to
    // skip it and continue, or false to abort. Archive-level failures abort regardless.
    virtual bool onError(const ExtractItem *item, Stage stage, int error,
                         const std::string &message) = 0;
};

struct ExtractResult {
    // 0, ECANCELED if a callback aborted, or EIO for an archive-level failure (already reported
    // through onError).
    int error = 0;
    std::uint64_t extractedCount = 0;
};

// Reads the archive once from start to end, writing the given items (sorted by index).
ExtractResult extractArchive(const std::string &archivePath,
                             const std::vector<std::string> &passwords,
                             const std::vector<ExtractItem> &items, ExtractCallbacks &callbacks);

}  // namespace filepan
