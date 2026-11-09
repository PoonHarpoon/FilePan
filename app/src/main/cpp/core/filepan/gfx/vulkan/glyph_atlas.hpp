// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "filepan/gfx/text.hpp"

// Internal: packs rasterized glyphs into atlas pages. This is CPU-only bookkeeping; the renderer
// owns the page images and copies the pending uploads into them each frame.
namespace filepan::gfx::vulkan::detail {

class GlyphAtlas {
public:
    static constexpr int kPageSize = 1024;
    // Coverage pages are layers of one alpha8 array image; color glyphs share one rgba8 page.
    static constexpr int kAlphaPages = 4;
    static constexpr int kSubpixelSteps = 4;

    struct Glyph {
        std::uint16_t x = 0;  // In the page, in texels.
        std::uint16_t y = 0;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::int16_t left = 0;  // Bitmap offset from the pen position, as in GlyphBitmap.
        std::int16_t top = 0;
        std::uint8_t page = 0;  // Alpha layer, or 0 for the color page.
        bool color = false;
        bool empty = false;  // No outline, e.g. a space.
    };

    struct Upload {
        bool color = false;
        std::uint8_t page = 0;
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint32_t offset = 0;  // Into uploadData(); 4-byte aligned.
    };

    // Returns the glyph, rasterizing it on a miss, or nullptr if it can't be drawn this frame
    // (no outline, too big, or the atlas is full of glyphs this frame already uses). frame
    // marks the pages the glyph is on as in use.
    const Glyph *find(TextSystem &text, FaceKey face, std::uint32_t glyph, float size,
                      int subpixel, std::uint64_t frame);

    [[nodiscard]] const std::vector<Upload> &uploads() const { return uploads_; }
    [[nodiscard]] const std::vector<std::uint8_t> &uploadData() const { return uploadData_; }
    // Pages to clear before this frame's uploads, because they were evicted.
    [[nodiscard]] bool alphaPageNeedsClear(int page) const { return alphaPages_[page].needsClear; }
    [[nodiscard]] bool colorPageNeedsClear() const { return colorPage_.needsClear; }
    void clearUploads();

    [[nodiscard]] std::size_t glyphCount() const { return glyphs_.size(); }
    [[nodiscard]] std::uint32_t evictions() const { return evictions_; }

private:
    struct Key {
        FaceKey face;
        std::uint32_t glyph;
        std::uint32_t sizeBits;
        std::uint32_t subpixel;
        bool operator==(const Key &) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key &key) const;
    };
    struct Shelf {
        int y = 0;
        int height = 0;
        int x = 0;
    };
    struct Page {
        std::vector<Shelf> shelves;
        int nextY = 0;
        std::uint64_t lastUsed = 0;
        bool needsClear = false;
    };

    static bool allocate(Page &page, int width, int height, int &x, int &y);
    // Evicts the least recently used page that this frame doesn't use; -1 if there's none.
    int evict(bool color, std::uint64_t frame);

    std::unordered_map<Key, Glyph, KeyHash> glyphs_;
    std::array<Page, kAlphaPages> alphaPages_{};
    Page colorPage_;
    std::vector<Upload> uploads_;
    std::vector<std::uint8_t> uploadData_;
    GlyphBitmap scratch_;
    std::uint32_t evictions_ = 0;
};

}  // namespace filepan::gfx::vulkan::detail
