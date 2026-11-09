// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <chrono>
#include <cstdint>

namespace filepan {

struct CopyFileOptions {
    bool replaceExisting = false;
    bool noFollowLinks = false;
    // Ownership, access time and all extended attributes; otherwise only the mode, modification
    // time and user.* extended attributes are copied.
    bool copyAttributes = false;
    std::chrono::milliseconds progressInterval{200};
};

class CopyFileCallbacks {
public:
    virtual ~CopyFileCallbacks() = default;
    // Reports bytes copied since the last call. Returns false to abort.
    virtual bool onProgress(std::uint64_t copiedSize) = 0;
};

// Which step failed, so the caller can attribute the error to the source or the target.
enum class CopyFileStage : std::int32_t {
    kNone = 0,
    kStatSource,
    kStatTarget,
    kTargetExists,  // Not an errno failure; the target exists and replaceExisting is false.
    kRemoveTarget,
    kOpenSource,
    kOpenTarget,
    kCopyData,
    kCloseTarget,
    kMakeDirectory,
    kReadLink,
    kCreateLink,
    kUnsupportedType,  // error holds st_mode instead of an errno.
    kAborted,
};

struct CopyFileResult {
    CopyFileStage stage = CopyFileStage::kNone;
    int error = 0;
};

// Copies one file, directory (without its contents) or symbolic link, like
// java.nio.file.Files.copy(). A partially written target is removed on failure. A target that
// is the source itself is left alone, and a regular file's size is reported as copied.
CopyFileResult copyFile(const char *source, const char *target, const CopyFileOptions &options,
                        CopyFileCallbacks &callbacks);

}  // namespace filepan
