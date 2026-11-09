// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/gfx/vulkan/glyph_atlas.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace filepan::gfx::vulkan::detail {

std::size_t GlyphAtlas::KeyHash::operator()(const Key &key) const {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::uint32_t value : {key.face, key.glyph, key.sizeBits, key.subpixel}) {
        hash = (hash ^ value) * 0x100000001b3ull;
    }
    return static_cast<std::size_t>(hash ^ (hash >> 32));
}

bool GlyphAtlas::allocate(Page &page, int width, int height, int &x, int &y) {
    // Shelf packing: reuse a shelf that's tall enough without wasting much, else open one.
    for (Shelf &shelf : page.shelves) {
        if (height <= shelf.height && height * 4 >= shelf.height * 3 - 8 &&
            shelf.x + width <= kPageSize) {
            x = shelf.x;
            y = shelf.y;
            shelf.x += width;
            return true;
        }
    }
    if (page.nextY + height > kPageSize || width > kPageSize) {
        return false;
    }
    // Round shelf heights up a little so nearby sizes share shelves.
    const int shelfHeight = std::min((height + 3) & ~3, kPageSize - page.nextY);
    page.shelves.push_back({page.nextY, shelfHeight, width});
    x = 0;
    y = page.nextY;
    page.nextY += shelfHeight;
    return true;
}

int GlyphAtlas::evict(bool color, std::uint64_t frame) {
    int victim = -1;
    std::uint64_t oldest = ~0ull;
    const int pageCount = color ? 1 : kAlphaPages;
    for (int i = 0; i < pageCount; ++i) {
        const Page &page = color ? colorPage_ : alphaPages_[i];
        if (page.lastUsed != frame && page.lastUsed < oldest) {
            oldest = page.lastUsed;
            victim = i;
        }
    }
    if (victim < 0) {
        return -1;
    }
    for (auto it = glyphs_.begin(); it != glyphs_.end();) {
        const Glyph &glyph = it->second;
        if (!glyph.empty && glyph.color == color && glyph.page == victim) {
            it = glyphs_.erase(it);
        } else {
            ++it;
        }
    }
    Page &page = color ? colorPage_ : alphaPages_[victim];
    page = {};
    page.needsClear = true;
    ++evictions_;
    return victim;
}

const GlyphAtlas::Glyph *GlyphAtlas::find(TextSystem &text, FaceKey face, std::uint32_t glyph,
                                          float size, int subpixel, std::uint64_t frame) {
    const Key key{face, glyph, std::bit_cast<std::uint32_t>(size),
                  static_cast<std::uint32_t>(subpixel)};
    if (auto it = glyphs_.find(key); it != glyphs_.end()) {
        Glyph &found = it->second;
        if (found.empty) {
            return nullptr;
        }
        (found.color ? colorPage_ : alphaPages_[found.page]).lastUsed = frame;
        return &found;
    }

    scratch_.pixels.clear();
    scratch_.width = 0;
    scratch_.height = 0;
    const bool hasOutline = text.rasterize(face, glyph, size,
                                           static_cast<float>(subpixel) / kSubpixelSteps, scratch_);
    const std::size_t texel = scratch_.format == GlyphFormat::rgba8 ? 4 : 1;
    if (!hasOutline || scratch_.width <= 0 || scratch_.height <= 0 ||
        scratch_.pixels.size() <
                static_cast<std::size_t>(scratch_.width) * scratch_.height * texel) {
        Glyph empty;
        empty.empty = true;
        glyphs_.emplace(key, empty);
        return nullptr;
    }
    const bool color = scratch_.format == GlyphFormat::rgba8;
    // One texel of padding on the right and bottom keeps filtered samples from bleeding.
    const int width = scratch_.width + 1;
    const int height = scratch_.height + 1;
    if (width > kPageSize || height > kPageSize) {
        return nullptr;
    }

    int page = -1;
    int x = 0;
    int y = 0;
    if (color) {
        if (allocate(colorPage_, width, height, x, y)) {
            page = 0;
        }
    } else {
        for (int i = 0; i < kAlphaPages && page < 0; ++i) {
            if (allocate(alphaPages_[i], width, height, x, y)) {
                page = i;
            }
        }
    }
    if (page < 0) {
        page = evict(color, frame);
        if (page < 0 || !allocate(color ? colorPage_ : alphaPages_[page], width, height, x, y)) {
            return nullptr;
        }
    }
    (color ? colorPage_ : alphaPages_[page]).lastUsed = frame;

    Glyph entry;
    entry.x = static_cast<std::uint16_t>(x);
    entry.y = static_cast<std::uint16_t>(y);
    entry.width = static_cast<std::uint16_t>(scratch_.width);
    entry.height = static_cast<std::uint16_t>(scratch_.height);
    entry.left = static_cast<std::int16_t>(scratch_.left);
    entry.top = static_cast<std::int16_t>(scratch_.top);
    entry.page = static_cast<std::uint8_t>(page);
    entry.color = color;

    Upload upload;
    upload.color = color;
    upload.page = entry.page;
    upload.x = entry.x;
    upload.y = entry.y;
    upload.width = entry.width;
    upload.height = entry.height;
    upload.offset = static_cast<std::uint32_t>((uploadData_.size() + 3) & ~std::size_t{3});
    const std::size_t bytes = static_cast<std::size_t>(entry.width) * entry.height * texel;
    uploadData_.resize(upload.offset + bytes);
    std::memcpy(uploadData_.data() + upload.offset, scratch_.pixels.data(), bytes);
    uploads_.push_back(upload);

    return &glyphs_.emplace(key, entry).first->second;
}

void GlyphAtlas::clearUploads() {
    uploads_.clear();
    uploadData_.clear();
    for (Page &page : alphaPages_) {
        page.needsClear = false;
    }
    colorPage_.needsClear = false;
}

}  // namespace filepan::gfx::vulkan::detail
