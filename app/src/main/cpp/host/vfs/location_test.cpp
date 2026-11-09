// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <set>
#include <unordered_set>

#include "filepan/vfs/location.hpp"
#include "test_util.hpp"

using filepan::vfs::BackendKind;
using filepan::vfs::Location;

int main() {
    const auto root = Location::local("/");
    EXPECT(root.isRoot() && root.name().empty() && !root.parent());
    EXPECT(Location::local("").path() == "/");

    const auto file = Location::local("//sdcard/./Download//a.txt/");
    EXPECT(file.path() == "/sdcard/Download/a.txt");
    EXPECT(file.name() == "a.txt");
    EXPECT(file.parent()->path() == "/sdcard/Download");
    EXPECT(file.parent()->parent()->parent() == root);
    EXPECT(root.child("sdcard").child("Download").child("a.txt") == file);
    EXPECT(file.withName("b.txt")->path() == "/sdcard/Download/b.txt");
    // Names are bytes.
    EXPECT(root.child("\xff\xfe").name() == "\xff\xfe");

    EXPECT(root.isAncestorOf(file));
    EXPECT(Location::local("/sdcard").isAncestorOf(file));
    EXPECT(!Location::local("/sd").isAncestorOf(file));
    EXPECT(file.isAncestorOf(file) && !file.isAncestorOf(file, false));

    const auto zip = Location::archive(file.withName("a.zip").value(), "/dir/inner.txt");
    EXPECT(zip.kind() == BackendKind::kArchive);
    EXPECT(zip.archiveFile()->path() == "/sdcard/Download/a.zip");
    EXPECT(zip.parent()->path() == "/dir");
    EXPECT(zip.parent()->archiveFile() != nullptr);
    EXPECT(zip.toString() == "archive:[local:/sdcard/Download/a.zip]/dir/inner.txt");
    // Equal by value, even with distinct archive file pointers.
    const auto sameZip = Location::archive(Location::local("/sdcard/Download/a.zip"), "dir/inner.txt");
    EXPECT(zip == sameZip && zip.hash() == sameZip.hash());
    EXPECT(zip != Location::archive(Location::local("/other.zip"), "/dir/inner.txt"));
    EXPECT(!Location::local("/dir").isAncestorOf(zip));
    EXPECT(Location::archive(Location::local("/sdcard/Download/a.zip")).isAncestorOf(zip));

    const auto server = Location::sftp("server-1", "/home/user");
    EXPECT(server.serverId() == "server-1" && server.name() == "user");
    EXPECT(server != Location::sftp("server-2", "/home/user"));
    EXPECT(server != Location::local("/home/user"));

    // A document tree: rooted at its id, never equal to a server of the same id.
    const auto tree = Location::document("Music 2", "/Albums//x/");
    EXPECT(tree.kind() == BackendKind::kDocument && tree.serverId() == "Music 2" && tree.path() == "/Albums/x");
    EXPECT(tree != Location::sftp("Music 2", "/Albums/x") && tree.toString() == "document:Music 2:/Albums/x");
    EXPECT(Location::document("Music 2").isAncestorOf(tree) && !Location::document("Music").isAncestorOf(tree));
    EXPECT(tree.parent() == Location::document("Music 2", "/Albums"));
    // withPath keeps the backend and root.
    EXPECT(tree.withPath("/a/b") == Location::document("Music 2", "/a/b"));
    EXPECT(server.withPath("/") == Location::sftp("server-1"));
    EXPECT(zip.withPath("/z") == Location::archive(Location::local("/sdcard/Download/a.zip"), "/z"));

    std::unordered_set<Location> set{file, zip, server, root};
    EXPECT(set.size() == 4 && set.contains(sameZip));
    std::set<Location> ordered{server, zip, file, root, sameZip};
    EXPECT(ordered.size() == 4);
    EXPECT(ordered.begin()->kind() == BackendKind::kLocal);
    EXPECT(*ordered.begin() == root);
    return 0;
}
