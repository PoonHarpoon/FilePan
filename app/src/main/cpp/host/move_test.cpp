// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "filepan/move.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;
using filepan::MoveMethod;
using filepan::MoveOptions;
using filepan::test::TempDir;

namespace {

class Progress : public filepan::MoveCallbacks {
public:
    bool onProgress(std::uint64_t size, std::uint64_t count, std::string_view) override {
        bytes += size;
        items += count;
        ++calls;
        return calls <= abortAfter;
    }

    std::uint64_t bytes = 0;
    std::uint64_t items = 0;
    int calls = 0;
    int abortAfter = 1 << 30;
};

// Every rename fails as if the target were on another file system.
class CrossDevice : public filepan::MoveOperations {
public:
    int rename(const char *, const char *, bool) override {
        ++calls;
        errno = EXDEV;
        return -1;
    }
    int calls = 0;
};

void write(const fs::path &path, const std::string &content) {
    std::ofstream(path, std::ios::binary) << content;
}

std::string read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// a/ { one (5 bytes), sub/ { two (3 bytes) }, link -> one, broken -> missing, locked/ (0555) }
// A read-only directory with children couldn't be deleted after copying, so locked is empty.
void makeTree(const fs::path &root) {
    fs::create_directories(root / "sub");
    write(root / "one", "hello");
    write(root / "sub" / "two", "abc");
    fs::create_symlink("one", root / "link");
    fs::create_symlink("missing", root / "broken");
    fs::create_directory(root / "locked");
    fs::permissions(root / "locked", fs::perms(0555));
    fs::last_write_time(root / "sub", fs::file_time_type(std::chrono::seconds(1'000'000'000)));
}

void checkTree(const fs::path &root) {
    EXPECT(read(root / "one") == "hello");
    EXPECT(read(root / "sub" / "two") == "abc");
    EXPECT(fs::is_symlink(root / "link") && fs::read_symlink(root / "link") == "one");
    EXPECT(fs::is_symlink(root / "broken") && fs::read_symlink(root / "broken") == "missing");
    EXPECT((fs::status(root / "locked").permissions() & fs::perms::all) == fs::perms(0555));
    EXPECT(fs::last_write_time(root / "sub")
           == fs::file_time_type(std::chrono::seconds(1'000'000'000)));
}

void unlock(const fs::path &root) {
    std::error_code error;
    fs::permissions(root / "locked", fs::perms(0755), error);
}

void testRename() {
    TempDir temp;
    const auto a = temp.path() / "a";
    const auto b = temp.path() / "b";
    write(a, "1");
    write(b, "2");
    Progress progress;
    auto result = filepan::movePath(a.string(), b.string(), {}, progress);
    EXPECT(!result && result.error().code == EEXIST);
    EXPECT(read(b) == "2");
    result = filepan::movePath(a.string(), b.string(), {.replaceExisting = true}, progress);
    EXPECT(result && *result == MoveMethod::kRenamed);
    EXPECT(!fs::exists(a) && read(b) == "1");
    result = filepan::movePath(b.string(), (temp.path() / "c").string(), {}, progress);
    EXPECT(result && *result == MoveMethod::kRenamed && read(temp.path() / "c") == "1");
    EXPECT(progress.calls == 0);
}

void testCrossDeviceFallback() {
    TempDir temp;
    const auto source = temp.path() / "a";
    const auto target = temp.path() / "b";
    makeTree(source);
    Progress progress;
    CrossDevice operations;
    const auto result =
        filepan::movePath(source.string(), target.string(), {}, progress, operations);
    EXPECT(result && *result == MoveMethod::kCopied);
    EXPECT(operations.calls == 1);
    EXPECT(!fs::exists(source));
    checkTree(target);
    EXPECT(progress.bytes == 5 + 3);
    EXPECT(progress.items == 7);
    unlock(target);
}

void testCrossDeviceCancel() {
    TempDir temp;
    const auto source = temp.path() / "a";
    const auto target = temp.path() / "b";
    makeTree(source);
    Progress progress;
    progress.abortAfter = 0;
    CrossDevice operations;
    const MoveOptions options{.progressInterval = std::chrono::milliseconds(0)};
    const auto result =
        filepan::movePath(source.string(), target.string(), options, progress, operations);
    EXPECT(!result && result.error().code == ECANCELED);
    EXPECT(!fs::exists(target));
    checkTree(source);
    unlock(source);
}

void testCrossDeviceReplace() {
    TempDir temp;
    const auto source = temp.path() / "a";
    makeTree(source);
    const auto full = temp.path() / "full";
    fs::create_directory(full);
    write(full / "keep", "k");
    const auto empty = temp.path() / "empty";
    fs::create_directory(empty);
    const auto file = temp.path() / "file";
    write(file, "f");
    Progress progress;
    CrossDevice operations;
    const MoveOptions replace{.replaceExisting = true};

    auto result = filepan::movePath(source.string(), full.string(), {}, progress, operations);
    EXPECT(!result && result.error().code == EEXIST);
    result = filepan::movePath(source.string(), full.string(), replace, progress, operations);
    EXPECT(!result && result.error().code == ENOTEMPTY);
    EXPECT(read(full / "keep") == "k");
    result = filepan::movePath(source.string(), file.string(), replace, progress, operations);
    EXPECT(!result && result.error().code == ENOTDIR);
    result = filepan::movePath(file.string(), empty.string(), replace, progress, operations);
    EXPECT(!result && result.error().code == EISDIR);
    result = filepan::movePath(source.string(), empty.string(), replace, progress, operations);
    EXPECT(result && *result == MoveMethod::kCopied);
    checkTree(empty);
    unlock(empty);
}

// A real second file system, when the machine has one: /dev/shm is usually tmpfs.
void testRealCrossDevice() {
    const fs::path shm = "/dev/shm";
    TempDir temp;
    struct stat tempSt {};
    struct stat shmSt {};
    if (stat(temp.path().c_str(), &tempSt) == -1 || stat(shm.c_str(), &shmSt) == -1
        || tempSt.st_dev == shmSt.st_dev || access(shm.c_str(), W_OK) == -1) {
        std::printf("skipping the real cross-device move: no second file system\n");
        return;
    }
    const auto source = temp.path() / "a";
    makeTree(source);
    const auto target = shm / ("filepan-move-test-" + std::to_string(getpid()));
    Progress progress;
    const auto result = filepan::movePath(source.string(), target.string(), {}, progress);
    EXPECT(result && *result == MoveMethod::kCopied);
    EXPECT(!fs::exists(source));
    checkTree(target);
    unlock(target);
    fs::remove_all(target);
}

}  // namespace

int main() {
    testRename();
    testCrossDeviceFallback();
    testCrossDeviceCancel();
    testCrossDeviceReplace();
    testRealCrossDevice();
    return 0;
}
