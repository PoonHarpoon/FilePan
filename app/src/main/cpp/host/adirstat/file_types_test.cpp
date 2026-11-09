// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <cstdio>

#include "filepan/adirstat/file_types.hpp"
#include "test_util.hpp"

using filepan::adirstat::Category;
using filepan::adirstat::categoryOf;
using filepan::adirstat::extensionOf;
using filepan::adirstat::kHardLinkDuplicate;
using filepan::adirstat::NodeId;
using filepan::adirstat::NodeKind;
using filepan::adirstat::Tree;

namespace {

void testExtensions() {
    EXPECT(extensionOf("photo.JPG") == "jpg");
    EXPECT(extensionOf("backup.tar.gz") == "tar.gz");
    EXPECT(extensionOf("BACKUP.TAR.XZ") == "tar.xz");
    EXPECT(extensionOf("notes.gz") == "gz");
    EXPECT(extensionOf("guitar.gz") == "gz");
    EXPECT(extensionOf(".profile").empty());
    EXPECT(extensionOf("trailing.").empty());
    EXPECT(extensionOf("Makefile").empty());
    EXPECT(extensionOf("a.b.mp4") == "mp4");
}

void testCategories() {
    EXPECT(categoryOf("jpg") == Category::kImage);
    EXPECT(categoryOf("mkv") == Category::kVideo);
    EXPECT(categoryOf("flac") == Category::kAudio);
    EXPECT(categoryOf("pdf") == Category::kDocument);
    EXPECT(categoryOf("tar.zst") == Category::kArchive);
    EXPECT(categoryOf("apk") == Category::kPackage);
    EXPECT(categoryOf("kt") == Category::kCode);
    EXPECT(categoryOf("") == Category::kOther);
    EXPECT(categoryOf("unknownext") == Category::kOther);
    for (std::size_t i = 0; i < filepan::adirstat::kCategoryCount; ++i) {
        EXPECT(!filepan::adirstat::categoryName(static_cast<Category>(i)).empty());
    }
}

NodeId addFile(Tree &tree, NodeId parent, const char *name, std::uint64_t size) {
    const NodeId id = tree.add(parent, name, NodeKind::kFile);
    tree.mutableNode(id).ownSize = size;
    return id;
}

void testStats() {
    Tree tree("/r");
    const NodeId pics = tree.add(tree.root(), "pics", NodeKind::kDirectory);
    addFile(tree, pics, "a.jpg", 300);
    addFile(tree, pics, "b.JPG", 200);
    addFile(tree, pics, "c.png", 100);
    const NodeId dup = addFile(tree, pics, "d.png", 100);
    tree.mutableNode(dup).flags |= kHardLinkDuplicate;
    addFile(tree, tree.root(), "movie.mp4", 5000);
    addFile(tree, tree.root(), "README", 7);
    tree.finalize();
    filepan::adirstat::assignCategories(tree);

    const auto stats = filepan::adirstat::computeTypeStats(tree, tree.root());
    EXPECT(stats.all.files == 5);
    EXPECT(stats.all.size == 5607);
    EXPECT(stats.categories[static_cast<std::size_t>(Category::kImage)].files == 3);
    EXPECT(stats.categories[static_cast<std::size_t>(Category::kImage)].size == 600);
    EXPECT(stats.categories[static_cast<std::size_t>(Category::kVideo)].size == 5000);
    EXPECT(stats.categories[static_cast<std::size_t>(Category::kOther)].files == 1);
    EXPECT(stats.extensions.size() == 4);
    EXPECT(stats.extensions[0].extension == "mp4");
    EXPECT(stats.extensions[1].extension == "jpg" && stats.extensions[1].totals.files == 2);
    EXPECT(stats.extensions[3].extension.empty());

    const auto under = filepan::adirstat::computeTypeStats(tree, pics);
    EXPECT(under.all.files == 3 && under.extensions.size() == 2);
}

}  // namespace

int main() {
    testExtensions();
    testCategories();
    testStats();
    std::puts("adirstat_file_types_test: OK");
    return 0;
}
