// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "filepan/error.hpp"

namespace filepan {

struct MoveOptions {
    // Replace an existing target the way rename(2) does: a file replaces a file, a directory an
    // empty directory. Otherwise an existing target fails with EEXIST.
    bool replaceExisting = false;
    std::chrono::milliseconds progressInterval{100};
};

class MoveCallbacks {
public:
    virtual ~MoveCallbacks() = default;
    // Reports bytes and entries copied since the last call, only when falling back to copying.
    // Returns false to abort, which removes the partial copy and keeps the source.
    virtual bool onProgress(std::uint64_t copiedSize, std::uint64_t copiedCount,
                            std::string_view currentPath) = 0;
};

// The rename calls movePath() makes, replaceable to simulate EXDEV in tests.
class MoveOperations {
public:
    virtual ~MoveOperations() = default;
    // renameat2(RENAME_NOREPLACE), or plain rename() when replace is true. Returns 0 or -1 with
    // errno set.
    virtual int rename(const char *source, const char *target, bool replace);
};

enum class MoveMethod {
    kRenamed,
    // The source was on another file system: it was copied, then deleted.
    kCopied,
};

// Moves a file, symbolic link or directory tree. Renames when possible; on EXDEV copies with
// copyFile() (preserving attributes, never following links) and then deletes the source with
// deleteTree(). On a failed or cancelled copy the partial target is removed and the source left
// intact; if only deleting the source fails, the target is complete and the error says so.
Result<MoveMethod> movePath(const std::string &source, const std::string &target,
                            const MoveOptions &options, MoveCallbacks &callbacks,
                            MoveOperations &operations);
Result<MoveMethod> movePath(const std::string &source, const std::string &target,
                            const MoveOptions &options, MoveCallbacks &callbacks);

}  // namespace filepan
