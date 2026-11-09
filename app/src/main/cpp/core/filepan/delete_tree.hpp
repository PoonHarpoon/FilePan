// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace filepan {

// Receives progress and errors from deleteTree(). Both are called on the deleting thread and may
// block, e.g. to ask the user what to do about an error.
class DeleteTreeCallbacks {
public:
    enum class ErrorKind {
        kOpenDirectory,  // The directory's contents couldn't be listed.
        kRemoveFile,
        kRemoveDirectory,
    };

    virtual ~DeleteTreeCallbacks() = default;

    // Reports entries deleted since the last call. Returns false to abort.
    virtual bool onProgress(std::uint64_t deletedCount, std::string_view currentPath) = 0;

    // Lets the caller handle a failure, e.g. by retrying through another mechanism or skipping
    // the entry. Afterwards the entry counts as deleted if it no longer exists, and as skipped
    // otherwise; directories containing skipped entries are skipped silently. Returns false to
    // abort.
    virtual bool onError(std::string_view path, ErrorKind kind, int error) = 0;
};

// The system calls deleteTree() uses, replaceable for fault injection in tests.
class DeleteTreeOperations {
public:
    virtual ~DeleteTreeOperations() = default;
    virtual int openDirectoryAt(int dirFd, const char *name);
    virtual int unlinkAt(int dirFd, const char *name, int flags);
};

struct DeleteTreeResult {
    // 0, ECANCELED if a callback aborted, or the errno from inspecting the root.
    int error = 0;
    std::uint64_t deletedCount = 0;
    std::uint64_t skippedCount = 0;
};

// Recursively deletes path without following symbolic links. The deleted count includes
// directories and the root itself, matching what a scan counts.
DeleteTreeResult deleteTree(const std::string &path, DeleteTreeCallbacks &callbacks,
                            DeleteTreeOperations &operations);
DeleteTreeResult deleteTree(const std::string &path, DeleteTreeCallbacks &callbacks);

}  // namespace filepan
