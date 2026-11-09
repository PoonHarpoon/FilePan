// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

// A minimal PNG codec for golden images: 8-bit RGBA only, compressed with zlib.
namespace filepan::test {

struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  // Non-premultiplied RGBA8, row by row from the top.
};

bool writePng(const std::filesystem::path &path, const RgbaImage &image);
// Reads 8-bit RGBA, non-interlaced PNGs, like the ones writePng() produces.
std::optional<RgbaImage> readPng(const std::filesystem::path &path);

}  // namespace filepan::test
