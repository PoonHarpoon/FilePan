// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

#include "filepan/delete_tree.hpp"
#include "test_util.hpp"

using filepan::DeleteTreeCallbacks;
using filepan::DeleteTreeOperations;
using filepan::test::TempDir;
namespace fs = std::filesystem;

namespace {

class RecordingCallbacks : public DeleteTreeCallbacks {
public:
    std::uint64_t progress = 0;
    int progressCalls = 0;
    std::vector<std::pair<std::string, ErrorKind>> errors;
    // Returning false from onProgress after this many calls.
    int abortAfterProgressCalls = -1;
    // Called from onError, e.g. to delete the entry another way.
    std::function<bool(const std::string &)> errorHandler = [](const std::string &) {
        return true;
    };

    bool onProgress(std::uint64_t deletedCount, std::string_view) override {
        progress += deletedCount;
        ++progressCalls;
        return abortAfterProgressCalls < 0 || progressCalls < abortAfterProgressCalls;
    }

    bool onError(std::string_view path, ErrorKind kind, int) override {
        errors.emplace_back(std::string(path), kind);
        return errorHandler(std::string(path));
    }
};

// Fails operations on names in the given sets.
class FaultyOperations : public DeleteTreeOperations {
public:
    std::set<std::string> failOpen;
    std::set<std::string> failUnlink;

    int openDirectoryAt(int dirFd, const char *name) override {
        if (failOpen.contains(name)) {
            errno = EACCES;
            return -1;
        }
        return DeleteTreeOperations::openDirectoryAt(dirFd, name);
    }

    int unlinkAt(int dirFd, const char *name, int flags) override {
        if (failUnlink.contains(name)) {
            errno = EPERM;
            return -1;
        }
        return DeleteTreeOperations::unlinkAt(dirFd, name, flags);
    }
};

void touch(const fs::path &path) { std::ofstream(path) << "x"; }

void testDeletesTreeWithoutFollowingLinks() {
    TempDir dir;
    const auto root = dir.path() / "root";
    fs::create_directories(root / "a/b/c");
    touch(root / "a/file");
    touch(root / "a/b/c/deep");
    // A link to a directory outside the tree must be removed, not followed.
    const auto outside = dir.path() / "outside";
    fs::create_directory(outside);
    touch(outside / "keep");
    fs::create_directory_symlink(outside, root / "a/link");
    fs::create_symlink("missing", root / "dangling");

    RecordingCallbacks callbacks;
    const auto result = filepan::deleteTree(root.string(), callbacks);
    EXPECT(result.error == 0);
    EXPECT(!fs::exists(root));
    EXPECT(fs::exists(outside / "keep"));
    // root, a, a/b, a/b/c, a/file, a/b/c/deep, a/link, dangling
    EXPECT(result.deletedCount == 8);
    EXPECT(result.skippedCount == 0);
    EXPECT(callbacks.progress == 8);
    EXPECT(callbacks.errors.empty());
}

void testDeletesSingleFileAndLinkToDirectory() {
    TempDir dir;
    touch(dir.path() / "file");
    RecordingCallbacks callbacks;
    EXPECT(filepan::deleteTree((dir.path() / "file").string(), callbacks).deletedCount == 1);
    EXPECT(!fs::exists(dir.path() / "file"));

    fs::create_directory(dir.path() / "target");
    touch(dir.path() / "target/keep");
    fs::create_directory_symlink(dir.path() / "target", dir.path() / "link");
    EXPECT(filepan::deleteTree((dir.path() / "link").string(), callbacks).deletedCount == 1);
    EXPECT(!fs::is_symlink(dir.path() / "link"));
    EXPECT(fs::exists(dir.path() / "target/keep"));
}

void testMissingRoot() {
    RecordingCallbacks callbacks;
    EXPECT(filepan::deleteTree("/nonexistent/filepan", callbacks).error == ENOENT);
}

void testManyFilesReportProgressInBatches() {
    TempDir dir;
    const auto root = dir.path() / "root";
    fs::create_directory(root);
    constexpr int kCount = 3000;
    for (int i = 0; i < kCount; ++i) {
        touch(root / std::to_string(i));
    }
    RecordingCallbacks callbacks;
    const auto result = filepan::deleteTree(root.string(), callbacks);
    EXPECT(result.error == 0);
    EXPECT(result.deletedCount == kCount + 1);
    EXPECT(callbacks.progress == kCount + 1);
    // Throttled, not one call per file.
    EXPECT(callbacks.progressCalls < kCount / 32);
}

void testAbortFromProgress() {
    TempDir dir;
    const auto root = dir.path() / "root";
    fs::create_directory(root);
    for (int i = 0; i < 1000; ++i) {
        touch(root / std::to_string(i));
    }
    RecordingCallbacks callbacks;
    callbacks.abortAfterProgressCalls = 1;
    const auto result = filepan::deleteTree(root.string(), callbacks);
    EXPECT(result.error == ECANCELED);
    EXPECT(fs::exists(root));
    EXPECT(result.deletedCount < 1000);
}

void testSkippedFileKeepsAncestors() {
    TempDir dir;
    const auto root = dir.path() / "root";
    fs::create_directories(root / "a/b");
    touch(root / "a/b/stuck");
    touch(root / "a/b/other");
    touch(root / "sibling");
    FaultyOperations operations;
    operations.failUnlink = {"stuck"};
    RecordingCallbacks callbacks;
    const auto result = filepan::deleteTree(root.string(), callbacks, operations);
    EXPECT(result.error == 0);
    EXPECT(callbacks.errors.size() == 1);
    EXPECT(callbacks.errors[0].first == (root / "a/b/stuck").string());
    EXPECT(callbacks.errors[0].second == DeleteTreeCallbacks::ErrorKind::kRemoveFile);
    // Everything else went; the skipped file's ancestors stay without further errors.
    EXPECT(fs::exists(root / "a/b/stuck"));
    EXPECT(!fs::exists(root / "a/b/other"));
    EXPECT(!fs::exists(root / "sibling"));
    EXPECT(result.deletedCount == 2);
    // stuck, b, a, root
    EXPECT(result.skippedCount == 4);
}

void testErrorHandledByCallbackCountsAsDeleted() {
    TempDir dir;
    const auto root = dir.path() / "root";
    fs::create_directories(root / "locked");
    touch(root / "locked/inside");
    FaultyOperations operations;
    operations.failOpen = {"locked"};
    RecordingCallbacks callbacks;
    // Like the Kotlin side falling back to deleting the subtree through the root service.
    callbacks.errorHandler = [](const std::string &path) {
        fs::remove_all(path);
        return true;
    };
    const auto result = filepan::deleteTree(root.string(), callbacks, operations);
    EXPECT(result.error == 0);
    EXPECT(callbacks.errors.size() == 1);
    EXPECT(callbacks.errors[0].second == DeleteTreeCallbacks::ErrorKind::kOpenDirectory);
    EXPECT(!fs::exists(root));
    EXPECT(result.skippedCount == 0);
}

void testAbortFromError() {
    TempDir dir;
    const auto root = dir.path() / "root";
    fs::create_directory(root);
    touch(root / "stuck");
    FaultyOperations operations;
    operations.failUnlink = {"stuck"};
    RecordingCallbacks callbacks;
    callbacks.errorHandler = [](const std::string &) { return false; };
    EXPECT(filepan::deleteTree(root.string(), callbacks, operations).error == ECANCELED);
    EXPECT(fs::exists(root / "stuck"));
}

}  // namespace

int main() {
    testDeletesTreeWithoutFollowingLinks();
    testDeletesSingleFileAndLinkToDirectory();
    testMissingRoot();
    testManyFilesReportProgressInBatches();
    testAbortFromProgress();
    testSkippedFileKeepsAncestors();
    testErrorHandledByCallbackCountsAsDeleted();
    testAbortFromError();
    std::puts("delete_tree_test: OK");
    return 0;
}
