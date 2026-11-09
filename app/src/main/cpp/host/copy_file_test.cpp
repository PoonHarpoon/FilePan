// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "filepan/copy_file.hpp"
#include "test_util.hpp"

using filepan::CopyFileCallbacks;
using filepan::CopyFileOptions;
using filepan::CopyFileStage;
using filepan::test::TempDir;
namespace fs = std::filesystem;

namespace {

class Progress : public CopyFileCallbacks {
public:
    std::uint64_t total = 0;
    int calls = 0;
    int abortAfterCalls = -1;

    bool onProgress(std::uint64_t copiedSize) override {
        total += copiedSize;
        ++calls;
        return abortAfterCalls < 0 || calls < abortAfterCalls;
    }
};

std::string read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void write(const fs::path &path, const std::string &content) {
    std::ofstream(path, std::ios::binary) << content;
}

std::string randomBytes(std::size_t size) {
    std::mt19937_64 random(42);
    std::string bytes(size, '\0');
    for (auto &byte : bytes) {
        byte = static_cast<char>(random());
    }
    return bytes;
}

void testCopiesLargeFileWithAttributes() {
    TempDir dir;
    const auto source = dir.path() / "source";
    // Spans several chunks.
    const std::string content = randomBytes(20 * 1024 * 1024 + 123);
    write(source, content);
    chmod(source.c_str(), 0640);
    const struct timespec times[2] = {{1'600'000'000, 0}, {1'700'000'000, 500}};
    utimensat(AT_FDCWD, source.c_str(), times, 0);
    const bool hasXattrs = setxattr(source.c_str(), "user.filepan", "v", 1, 0) == 0;

    const auto target = dir.path() / "target";
    Progress progress;
    const auto result = filepan::copyFile(source.c_str(), target.c_str(), {}, progress);
    EXPECT(result.stage == CopyFileStage::kNone);
    EXPECT(read(target) == content);
    EXPECT(progress.total == content.size());
    struct stat st {};
    EXPECT(stat(target.c_str(), &st) == 0);
    EXPECT((st.st_mode & 07777) == 0640);
    EXPECT(st.st_mtim.tv_sec == 1'700'000'000);
    EXPECT(st.st_mtim.tv_nsec == 500);
    if (hasXattrs) {
        // The attribute must come from the source, not the target.
        char value[8] = {};
        EXPECT(getxattr(target.c_str(), "user.filepan", value, sizeof(value)) == 1);
        EXPECT(value[0] == 'v');
    } else {
        std::puts("copy_file_test: user xattrs unsupported here, skipping that check");
    }
}

void testExistingTarget() {
    TempDir dir;
    write(dir.path() / "source", "new");
    write(dir.path() / "target", "old");
    Progress progress;
    const auto source = (dir.path() / "source").string();
    const auto target = (dir.path() / "target").string();
    auto result = filepan::copyFile(source.c_str(), target.c_str(), {}, progress);
    EXPECT(result.stage == CopyFileStage::kTargetExists);
    EXPECT(read(target) == "old");

    CopyFileOptions replace;
    replace.replaceExisting = true;
    result = filepan::copyFile(source.c_str(), target.c_str(), replace, progress);
    EXPECT(result.stage == CopyFileStage::kNone);
    EXPECT(read(target) == "new");

    // Copying a file onto itself is a no-op, but its size counts as copied.
    progress.total = 0;
    progress.calls = 0;
    result = filepan::copyFile(source.c_str(), source.c_str(), {}, progress);
    EXPECT(result.stage == CopyFileStage::kNone);
    EXPECT(read(source) == "new");
    EXPECT(progress.total == 3 && progress.calls == 1);
    // A hard link is the same file too, and the callback can still abort.
    fs::create_hard_link(source, dir.path() / "hardlink");
    progress.abortAfterCalls = progress.calls + 1;
    result = filepan::copyFile(source.c_str(), (dir.path() / "hardlink").c_str(), {}, progress);
    EXPECT(result.stage == CopyFileStage::kAborted);
    EXPECT(read(source) == "new");
}

void testLinksAndDirectories() {
    TempDir dir;
    write(dir.path() / "file", "content");
    fs::create_symlink("file", dir.path() / "link");
    Progress progress;

    CopyFileOptions noFollow;
    noFollow.noFollowLinks = true;
    auto result = filepan::copyFile((dir.path() / "link").c_str(),
                                    (dir.path() / "link-copy").c_str(), noFollow, progress);
    EXPECT(result.stage == CopyFileStage::kNone);
    EXPECT(fs::is_symlink(dir.path() / "link-copy"));
    EXPECT(fs::read_symlink(dir.path() / "link-copy") == "file");

    result = filepan::copyFile((dir.path() / "link").c_str(),
                               (dir.path() / "followed").c_str(), {}, progress);
    EXPECT(result.stage == CopyFileStage::kNone);
    EXPECT(!fs::is_symlink(dir.path() / "followed"));
    EXPECT(read(dir.path() / "followed") == "content");

    fs::create_directory(dir.path() / "directory");
    write(dir.path() / "directory/child", "x");
    result = filepan::copyFile((dir.path() / "directory").c_str(),
                               (dir.path() / "directory-copy").c_str(), {}, progress);
    EXPECT(result.stage == CopyFileStage::kNone);
    EXPECT(fs::is_directory(dir.path() / "directory-copy"));
    // Contents are copied by the caller walking the tree.
    EXPECT(fs::is_empty(dir.path() / "directory-copy"));
}

void testFailuresReportStage() {
    TempDir dir;
    Progress progress;
    auto result = filepan::copyFile((dir.path() / "missing").c_str(),
                                    (dir.path() / "target").c_str(), {}, progress);
    EXPECT(result.stage == CopyFileStage::kStatSource);
    EXPECT(result.error == ENOENT);

    write(dir.path() / "source", "x");
    result = filepan::copyFile((dir.path() / "source").c_str(),
                               (dir.path() / "no-such-dir/target").c_str(), {}, progress);
    EXPECT(result.stage == CopyFileStage::kStatTarget || result.stage == CopyFileStage::kOpenTarget);
}

void testAbortRemovesPartialTarget() {
    TempDir dir;
    write(dir.path() / "source", randomBytes(32 * 1024 * 1024));
    Progress progress;
    progress.abortAfterCalls = 1;
    CopyFileOptions options;
    options.progressInterval = std::chrono::milliseconds(0);
    const auto result = filepan::copyFile((dir.path() / "source").c_str(),
                                          (dir.path() / "target").c_str(), options, progress);
    EXPECT(result.stage == CopyFileStage::kAborted);
    EXPECT(!fs::exists(dir.path() / "target"));
}

}  // namespace

int main() {
    testCopiesLargeFileWithAttributes();
    testExistingTarget();
    testLinksAndDirectories();
    testFailuresReportStage();
    testAbortRemovesPartialTarget();
    std::puts("copy_file_test: OK");
    return 0;
}
