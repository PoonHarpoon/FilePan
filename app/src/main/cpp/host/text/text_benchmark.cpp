// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Shapes 1000 typical file names (about 25 characters) the way a file list does, with and
// without the shape cache, and rasterizes their glyphs.
//
//   build/host/text/text_benchmark

#include <chrono>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "filepan/text/text_system.hpp"

namespace {

using namespace filepan;
using Clock = std::chrono::steady_clock;

std::vector<std::string> fileNames() {
    const char *words[] = {"holiday", "IMG",     "report", "final",
                           "draft",   "photo",   "scan",   "invoice",
                           "backup",  "notes",   "copy",   "r\u00E9sum\u00E9",
                           "project", "data",    "music",  "track",
                           "video",   "meeting", "budget", "\u00DCbersicht"};
    const char *extensions[] = {".jpg", ".pdf", ".txt", ".mp4", ".zip", ".docx", ".png", ".mp3"};
    std::mt19937 random(42);
    std::vector<std::string> names;
    while (names.size() < 1000) {
        std::string name;
        while (name.size() < 18) {
            if (!name.empty()) {
                name += random() % 2 == 0 ? '_' : ' ';
            }
            name += words[random() % std::size(words)];
        }
        name += "_" + std::to_string(random() % 10000);
        name += extensions[random() % std::size(extensions)];
        names.push_back(std::move(name));
    }
    return names;
}

double microsPer(Clock::time_point start, std::size_t count) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count() /
           static_cast<double>(count);
}

}  // namespace

int main() {
    const std::vector<std::string> names = fileNames();
    std::size_t characters = 0;
    for (const std::string &name : names) {
        characters += name.size();
    }
    std::printf("%zu names, %.1f bytes on average\n", names.size(),
                static_cast<double>(characters) / static_cast<double>(names.size()));

    gfx::TextStyle style;
    style.size = 42;  // 16 dp at 2.625x.
    style.weight = 500;
    gfx::TextLayoutOptions options;
    options.maxWidth = 700;
    options.maxLines = 1;
    options.overflow = gfx::TextOverflow::ellipsisMiddle;

    auto uncached =
            text::createTextSystem(text::createDirectoryFontProvider(FILEPAN_FONTS_DIR), {"en", 0});
    uncached->shape("warm up", style, options);
    for (int round = 0; round < 3; ++round) {
        const auto start = Clock::now();
        std::size_t glyphs = 0;
        for (const std::string &name : names) {
            glyphs += uncached->shape(name, style, options).glyphs.size();
        }
        std::printf("shape, no cache:         %6.2f us per name (%zu glyphs)\n",
                    microsPer(start, names.size()), glyphs);
    }

    gfx::TextLayoutOptions narrow = options;
    narrow.maxWidth = 300;
    {
        const auto start = Clock::now();
        for (const std::string &name : names) {
            uncached->shape(name, style, narrow);
        }
        std::printf("shape + ellipsis:        %6.2f us per name\n", microsPer(start, names.size()));
    }

    auto cached = text::createTextSystem(text::createDirectoryFontProvider(FILEPAN_FONTS_DIR));
    for (const std::string &name : names) {
        cached->shape(name, style, options);
    }
    for (int round = 0; round < 3; ++round) {
        const auto start = Clock::now();
        for (const std::string &name : names) {
            cached->shape(name, style, options);
        }
        std::printf("shape, cache hit:        %6.2f us per name\n", microsPer(start, names.size()));
    }

    // Rasterizing every distinct glyph once at 4 subpixel offsets, as an atlas would.
    std::set<std::pair<gfx::FaceKey, std::uint32_t>> distinct;
    for (const std::string &name : names) {
        const gfx::ShapedText text = cached->shape(name, style, options);
        for (const gfx::GlyphRun &run : text.runs) {
            for (std::uint32_t i = 0; i < run.glyphCount; ++i) {
                distinct.emplace(run.face, text.glyphs[run.firstGlyph + i].glyph);
            }
        }
    }
    const auto start = Clock::now();
    gfx::GlyphBitmap bitmap;
    for (const auto &[face, glyph] : distinct) {
        for (const float subpixel : {0.0f, 0.25f, 0.5f, 0.75f}) {
            cached->rasterize(face, glyph, style.size, subpixel, bitmap);
        }
    }
    std::printf("rasterize:               %6.2f us per glyph (%zu glyphs x 4 offsets)\n",
                microsPer(start, distinct.size() * 4), distinct.size());
    return 0;
}
