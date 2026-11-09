// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

#include <unistd.h>

#include "filepan/vfs/local_backend.hpp"
#include "filepan/vfs/save.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;
using namespace filepan;
using namespace filepan::vfs;
using filepan::test::TempDir;

namespace {

void write(const fs::path &path, std::string_view content) { std::ofstream(path, std::ios::binary) << content; }

std::string read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::span<const std::uint8_t> bytes(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t *>(s.data()), s.size()};
}

// Only the file itself: no temporary left next to it.
std::size_t entryCount(const fs::path &dir) {
    return static_cast<std::size_t>(std::distance(fs::directory_iterator(dir), fs::directory_iterator()));
}

void testReplace() {
    TempDir temp;
    LocalBackend backend;
    const fs::path path = temp.path() / "notes.txt";
    write(path, "old");
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    const Location location = Location::local(path.string());
    const auto before = backend.stat(location);
    EXPECT(before);
    auto saved = saveFile(backend, location, bytes("new contents"), FileStamp::of(*before));
    EXPECT(saved && saved->size == 12);
    EXPECT(read(path) == "new contents");
    EXPECT((saved->mode & 07777) == 0640);
    EXPECT(entryCount(temp.path()) == 1);
    // A second save against the first one's stamp.
    EXPECT(saveFile(backend, location, bytes(""), FileStamp::of(*saved)));
    EXPECT(read(path).empty());
    // Created when missing, without a stamp.
    const Location fresh = Location::local((temp.path() / "fresh.txt").string());
    EXPECT(saveFile(backend, fresh, bytes("hi"), std::nullopt) && read(temp.path() / "fresh.txt") == "hi");
}

void testConflict() {
    TempDir temp;
    LocalBackend backend;
    const fs::path path = temp.path() / "a.txt";
    write(path, "one");
    const Location location = Location::local(path.string());
    const FileStamp stamp = FileStamp::of(*backend.stat(location));
    // Someone else writes it.
    write(path, "someone else's");
    auto saved = saveFile(backend, location, bytes("mine"), stamp);
    EXPECT(!saved && saved.error().code == kSaveConflict);
    EXPECT(read(path) == "someone else's");
    // Without the stamp, it's overwritten.
    EXPECT(saveFile(backend, location, bytes("mine"), std::nullopt) && read(path) == "mine");
    // Gone since it was read.
    fs::remove(path);
    saved = saveFile(backend, location, bytes("mine"), stamp);
    EXPECT(!saved && saved.error().code == kSaveConflict && !fs::exists(path));
}

void testLinks() {
    TempDir temp;
    LocalBackend backend;
    const fs::path target = temp.path() / "target.txt";
    write(target, "x");
    fs::create_symlink(target, temp.path() / "link.txt");
    fs::create_hard_link(target, temp.path() / "hard.txt");
    // Through the symlink: the link stays a link, and the target gets the bytes.
    const Location link = Location::local((temp.path() / "link.txt").string());
    EXPECT(saveFile(backend, link, bytes("via link"), FileStamp::of(*backend.stat(link))));
    EXPECT(fs::is_symlink(temp.path() / "link.txt") && read(target) == "via link");
    // Through a hard link: both names still share the file.
    const Location hard = Location::local((temp.path() / "hard.txt").string());
    EXPECT(saveFile(backend, hard, bytes("via hard link"), std::nullopt));
    EXPECT(read(target) == "via hard link" && fs::hard_link_count(target) == 2);
}

void testFailures() {
    TempDir temp;
    LocalBackend backend;
    // A directory isn't a file to save over.
    fs::create_directory(temp.path() / "dir");
    auto saved = saveFile(backend, Location::local((temp.path() / "dir").string()), bytes("x"), std::nullopt);
    EXPECT(!saved && saved.error().code == EISDIR);
    // A folder that can't be written: the old file is untouched and nothing is left behind.
    const fs::path locked = temp.path() / "locked";
    fs::create_directory(locked);
    write(locked / "f.txt", "keep");
    fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
    if (::geteuid() != 0) {
        saved = saveFile(backend, Location::local((locked / "f.txt").string()), bytes("x"), std::nullopt);
        EXPECT(!saved && read(locked / "f.txt") == "keep" && entryCount(locked) == 1);
    }
    fs::permissions(locked, fs::perms::owner_all);
}

}  // namespace

int main() {
    testReplace();
    testConflict();
    testLinks();
    testFailures();
    return 0;
}
