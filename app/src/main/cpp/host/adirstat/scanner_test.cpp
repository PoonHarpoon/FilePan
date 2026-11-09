// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "filepan/adirstat/scanner.hpp"
#include "test_util.hpp"

using filepan::CancellationToken;
using filepan::adirstat::kHardLinkDuplicate;
using filepan::adirstat::kNoNode;
using filepan::adirstat::NodeId;
using filepan::adirstat::NodeKind;
using filepan::adirstat::ScanOptions;
using filepan::adirstat::ScanProgress;
using filepan::adirstat::Tree;
using filepan::test::TempDir;
namespace fs = std::filesystem;

namespace {

void write(const fs::path &path, std::size_t size) {
    std::ofstream out(path, std::ios::binary);
    out << std::string(size, 'x');
}

// The child of parent with that name, or kNoNode.
NodeId child(const Tree &tree, NodeId parent, std::string_view name) {
    for (NodeId id = tree[parent].firstChild; id != kNoNode; id = tree[id].nextSibling) {
        if (tree.name(id) == name) {
            return id;
        }
    }
    return kNoNode;
}

std::unique_ptr<Tree> scanOrDie(const fs::path &root, ScanOptions options = {}) {
    auto tree = filepan::adirstat::scan(root.string(), options, CancellationToken());
    EXPECT(tree.has_value());
    return std::move(*tree);
}

void testSumsSizesAndSortsLargestFirst() {
    TempDir dir;
    const fs::path root = dir.path();
    fs::create_directories(root / "big" / "deeper");
    fs::create_directories(root / "small");
    write(root / "big" / "a.bin", 50000);
    write(root / "big" / "deeper" / "b.bin", 30000);
    write(root / "small" / "c.txt", 100);
    write(root / "top.txt", 1000);

    for (const int threads : {1, 4}) {
        ScanProgress progress;
        auto result = filepan::adirstat::scan(root.string(), {.threads = threads}, CancellationToken(), &progress);
        EXPECT(result.has_value());
        const Tree &tree = **result;
        const auto &rootNode = tree[tree.root()];
        EXPECT(rootNode.files == 4);
        EXPECT(rootNode.dirs == 3);
        const NodeId big = child(tree, tree.root(), "big");
        EXPECT(big != kNoNode);
        // Largest first.
        EXPECT(tree[tree.root()].firstChild == big);
        EXPECT(tree[big].files == 2);
        EXPECT(tree[big].size >= 80000);
        EXPECT(tree[big].allocated > 0);
        EXPECT(rootNode.size >= 81100);
        EXPECT(tree.pathOf(child(tree, big, "deeper")) == (root / "big" / "deeper").string());
        EXPECT(progress.files.load() == 4);
        EXPECT(progress.dirs.load() == 4);
        EXPECT(progress.bytes.load() == 81100);
    }
}

void testCountsHardLinksOnce() {
    TempDir dir;
    const fs::path root = dir.path();
    fs::create_directories(root / "one");
    fs::create_directories(root / "two");
    write(root / "one" / "data", 40000);
    fs::create_hard_link(root / "one" / "data", root / "two" / "data");

    auto tree = scanOrDie(root);
    EXPECT((*tree)[tree->root()].files == 1);
    const NodeId a = child(*tree, child(*tree, tree->root(), "one"), "data");
    const NodeId b = child(*tree, child(*tree, tree->root(), "two"), "data");
    EXPECT(a != kNoNode && b != kNoNode);
    // Exactly one of the two names carries the size.
    EXPECT(((*tree)[a].flags & kHardLinkDuplicate) != ((*tree)[b].flags & kHardLinkDuplicate));
    const std::uint64_t dirs = (*tree)[child(*tree, tree->root(), "one")].ownSize
                               + (*tree)[child(*tree, tree->root(), "two")].ownSize + (*tree)[tree->root()].ownSize;
    EXPECT((*tree)[tree->root()].size == 40000 + dirs);
}

void testDoesNotFollowSymlinks() {
    TempDir dir;
    TempDir outside;
    const fs::path root = dir.path();
    write(outside.path() / "huge", 100000);
    fs::create_directory_symlink(outside.path(), root / "link");

    auto tree = scanOrDie(root);
    const NodeId link = child(*tree, tree->root(), "link");
    EXPECT(link != kNoNode);
    EXPECT((*tree)[link].kind == NodeKind::kSymlink);
    EXPECT((*tree)[link].childCount == 0);
    EXPECT((*tree)[tree->root()].size < 100000);
}

void testUnreadableDirectoryKeepsItsError() {
    if (geteuid() == 0) {
        return;  // Root reads everything.
    }
    TempDir dir;
    const fs::path root = dir.path();
    fs::create_directories(root / "locked");
    write(root / "locked" / "secret", 10);
    write(root / "visible", 10);
    fs::permissions(root / "locked", fs::perms::none);

    auto tree = scanOrDie(root);
    fs::permissions(root / "locked", fs::perms::owner_all);
    const NodeId locked = child(*tree, tree->root(), "locked");
    EXPECT((*tree)[locked].error == EACCES);
    EXPECT((*tree)[tree->root()].errors == 1);
    EXPECT((*tree)[tree->root()].files == 1);
}

void testMissingRootFails() {
    TempDir dir;
    auto result = filepan::adirstat::scan((dir.path() / "nope").string(), {}, CancellationToken());
    EXPECT(!result && result.error().code == ENOENT);
    EXPECT(!filepan::adirstat::scan("relative", {}, CancellationToken()));
}

void testCancelled() {
    TempDir dir;
    for (int i = 0; i < 20; ++i) {
        fs::create_directories(dir.path() / std::to_string(i) / "x");
    }
    CancellationToken cancel;
    cancel.cancel();
    auto result = filepan::adirstat::scan(dir.path().string(), {}, cancel);
    EXPECT(!result && result.error().isCancelled());
}

void testMountBoundary() {
    struct statx st {};
    st.stx_dev_major = 8;
    st.stx_dev_minor = 1;
    EXPECT(!filepan::adirstat::isMountBoundary(makedev(8, 1), st));
    EXPECT(filepan::adirstat::isMountBoundary(makedev(8, 2), st));
    // Bind mounts keep the device but are flagged as mount roots.
    st.stx_attributes_mask = 0x00002000;
    st.stx_attributes = 0x00002000;
    EXPECT(filepan::adirstat::isMountBoundary(makedev(8, 1), st));
}

void testRemoveSubtractsFromAncestors() {
    TempDir dir;
    const fs::path root = dir.path();
    fs::create_directories(root / "a" / "b");
    write(root / "a" / "b" / "f", 5000);
    write(root / "a" / "g", 3000);

    auto tree = scanOrDie(root);
    const NodeId a = child(*tree, tree->root(), "a");
    const NodeId b = child(*tree, a, "b");
    const std::uint64_t before = (*tree)[tree->root()].size;
    const std::uint64_t removed = (*tree)[b].size;
    tree->remove(b);
    EXPECT((*tree)[tree->root()].size == before - removed);
    EXPECT((*tree)[a].files == 1);
    EXPECT((*tree)[a].dirs == 0);
    EXPECT(child(*tree, a, "b") == kNoNode);
    // Removing it again, or something inside it, changes nothing.
    tree->remove(b);
    tree->remove(child(*tree, b, "f"));
    EXPECT((*tree)[tree->root()].size == before - removed);
    EXPECT(tree->isAncestorOf(tree->root(), a));
    EXPECT(!tree->isAncestorOf(a, a, false));
}

}  // namespace

int main() {
    testSumsSizesAndSortsLargestFirst();
    testCountsHardLinksOnce();
    testDoesNotFollowSymlinks();
    testUnreadableDirectoryKeepsItsError();
    testMissingRootFails();
    testCancelled();
    testMountBoundary();
    testRemoveSubtractsFromAncestors();
    std::puts("adirstat_scanner_test: OK");
    return 0;
}
