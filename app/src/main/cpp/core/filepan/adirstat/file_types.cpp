// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/adirstat/file_types.hpp"

#include <algorithm>
#include <flat_map>
#include <utility>

namespace filepan::adirstat {
namespace {

struct ExtensionCategory {
    std::string_view extension;
    Category category;
};

// Sorted by extension for lookup.
constexpr ExtensionCategory kExtensions[] = {
        {"3gp", Category::kVideo},       {"7z", Category::kArchive},     {"aab", Category::kPackage},
        {"aac", Category::kAudio},       {"apk", Category::kPackage},    {"apkm", Category::kPackage},
        {"apks", Category::kPackage},    {"arw", Category::kImage},      {"avi", Category::kVideo},
        {"avif", Category::kImage},      {"bmp", Category::kImage},      {"bz2", Category::kArchive},
        {"c", Category::kCode},          {"cc", Category::kCode},        {"cpp", Category::kCode},
        {"cr2", Category::kImage},       {"css", Category::kCode},       {"csv", Category::kDocument},
        {"deb", Category::kPackage},     {"dng", Category::kImage},      {"doc", Category::kDocument},
        {"docx", Category::kDocument},   {"epub", Category::kDocument},  {"flac", Category::kAudio},
        {"gif", Category::kImage},       {"go", Category::kCode},        {"gz", Category::kArchive},
        {"h", Category::kCode},          {"heic", Category::kImage},     {"heif", Category::kImage},
        {"hpp", Category::kCode},        {"htm", Category::kCode},       {"html", Category::kCode},
        {"img", Category::kArchive},     {"iso", Category::kArchive},    {"java", Category::kCode},
        {"jpeg", Category::kImage},      {"jpg", Category::kImage},      {"js", Category::kCode},
        {"json", Category::kCode},       {"jxl", Category::kImage},      {"kt", Category::kCode},
        {"lz4", Category::kArchive},     {"m4a", Category::kAudio},      {"m4v", Category::kVideo},
        {"md", Category::kDocument},     {"mid", Category::kAudio},      {"mkv", Category::kVideo},
        {"mov", Category::kVideo},       {"mp3", Category::kAudio},      {"mp4", Category::kVideo},
        {"mpeg", Category::kVideo},      {"mpg", Category::kVideo},      {"obb", Category::kPackage},
        {"odp", Category::kDocument},    {"ods", Category::kDocument},   {"odt", Category::kDocument},
        {"ogg", Category::kAudio},       {"opus", Category::kAudio},     {"pdf", Category::kDocument},
        {"png", Category::kImage},       {"ppt", Category::kDocument},   {"pptx", Category::kDocument},
        {"py", Category::kCode},         {"rar", Category::kArchive},    {"raw", Category::kImage},
        {"rpm", Category::kPackage},     {"rs", Category::kCode},        {"rtf", Category::kDocument},
        {"sh", Category::kCode},         {"svg", Category::kImage},      {"tar", Category::kArchive},
        {"tar.bz2", Category::kArchive}, {"tar.gz", Category::kArchive}, {"tar.lz4", Category::kArchive},
        {"tar.xz", Category::kArchive},  {"tar.zst", Category::kArchive}, {"tgz", Category::kArchive},
        {"tif", Category::kImage},       {"tiff", Category::kImage},     {"ts", Category::kCode},
        {"txt", Category::kDocument},    {"wav", Category::kAudio},      {"webm", Category::kVideo},
        {"webp", Category::kImage},      {"wma", Category::kAudio},      {"wmv", Category::kVideo},
        {"xapk", Category::kPackage},    {"xls", Category::kDocument},   {"xlsx", Category::kDocument},
        {"xml", Category::kCode},        {"xz", Category::kArchive},     {"yaml", Category::kCode},
        {"yml", Category::kCode},        {"zip", Category::kArchive},    {"zst", Category::kArchive},
};

static_assert(std::ranges::is_sorted(kExtensions, {}, &ExtensionCategory::extension));

// Second suffixes that make a compound extension with "tar" in front.
constexpr std::string_view kTarCompressions[] = {"bz2", "gz", "lz4", "xz", "zst"};

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

std::string_view categoryName(Category category) {
    switch (category) {
        case Category::kOther:
            return "Other";
        case Category::kImage:
            return "Images";
        case Category::kVideo:
            return "Videos";
        case Category::kAudio:
            return "Audio";
        case Category::kDocument:
            return "Documents";
        case Category::kArchive:
            return "Archives";
        case Category::kPackage:
            return "Apps and packages";
        case Category::kCode:
            return "Code and data";
    }
    return {};
}

std::string extensionOf(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) {
        return {};
    }
    std::string extension;
    for (const char c : name.substr(dot + 1)) {
        extension += lower(c);
    }
    if (std::ranges::find(kTarCompressions, extension) != std::end(kTarCompressions) && dot >= 4) {
        std::string before;
        for (const char c : name.substr(dot - 4, 4)) {
            before += lower(c);
        }
        if (before == ".tar") {
            return "tar." + extension;
        }
    }
    return extension;
}

Category categoryOf(std::string_view extension) {
    const auto it = std::ranges::lower_bound(kExtensions, extension, {}, &ExtensionCategory::extension);
    return it != std::end(kExtensions) && it->extension == extension ? it->category : Category::kOther;
}

void assignCategories(Tree &tree) {
    for (NodeId id = 0; id < tree.nodeCount(); ++id) {
        Node &node = tree.mutableNode(id);
        if (node.kind == NodeKind::kFile) {
            node.category = static_cast<std::uint8_t>(categoryOf(extensionOf(tree.name(id))));
        }
    }
}

TypeStats computeTypeStats(const Tree &tree, NodeId from) {
    TypeStats stats;
    std::flat_map<std::string, std::size_t> index;
    std::vector<NodeId> stack{from};
    while (!stack.empty()) {
        const NodeId id = stack.back();
        stack.pop_back();
        const Node &node = tree[id];
        for (NodeId child = node.firstChild; child != kNoNode; child = tree[child].nextSibling) {
            stack.push_back(child);
        }
        if (node.kind != NodeKind::kFile || (node.flags & kHardLinkDuplicate)) {
            continue;
        }
        const auto category = static_cast<Category>(node.category);
        TypeTotals &byCategory = stats.categories[node.category];
        ++byCategory.files;
        byCategory.size += node.ownSize;
        ++stats.all.files;
        stats.all.size += node.ownSize;

        std::string extension = extensionOf(tree.name(id));
        auto [it, added] = index.try_emplace(std::move(extension), stats.extensions.size());
        if (added) {
            stats.extensions.push_back({.extension = it->first, .category = category, .totals = {}});
        }
        TypeTotals &byExtension = stats.extensions[it->second].totals;
        ++byExtension.files;
        byExtension.size += node.ownSize;
    }
    std::ranges::sort(stats.extensions, [](const ExtensionTotals &a, const ExtensionTotals &b) {
        if (a.totals.size != b.totals.size) {
            return a.totals.size > b.totals.size;
        }
        return a.extension < b.extension;
    });
    return stats;
}

}  // namespace filepan::adirstat
