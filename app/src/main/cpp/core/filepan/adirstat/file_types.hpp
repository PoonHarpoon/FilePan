// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "filepan/adirstat/tree.hpp"

namespace filepan::adirstat {

// What a file is, by its name, for the statistics and the colors the screen gives it.
enum class Category : std::uint8_t {
    kOther,
    kImage,
    kVideo,
    kAudio,
    kDocument,
    kArchive,
    kPackage,
    kCode,
};
inline constexpr std::size_t kCategoryCount = 8;

[[nodiscard]] std::string_view categoryName(Category category);

// The lowercase extension without its dot ("jpg", "tar.gz"), or empty for none. A leading dot
// alone (".profile") isn't an extension.
[[nodiscard]] std::string extensionOf(std::string_view name);
[[nodiscard]] Category categoryOf(std::string_view extension);

// Sets Node::category for every file in the tree.
void assignCategories(Tree &tree);

struct TypeTotals {
    std::uint64_t files = 0;
    std::uint64_t size = 0;
};

struct ExtensionTotals {
    std::string extension;  // Empty for files without one.
    Category category;
    TypeTotals totals;
};

struct TypeStats {
    std::array<TypeTotals, kCategoryCount> categories{};
    std::vector<ExtensionTotals> extensions;  // Largest first.
    TypeTotals all;
};

// The totals by category and extension of the files under from, hard links counted once.
// Categories must have been assigned.
[[nodiscard]] TypeStats computeTypeStats(const Tree &tree, NodeId from);

}  // namespace filepan::adirstat
