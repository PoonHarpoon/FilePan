// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>

#include "filepan/gfx/text.hpp"
#include "filepan/text/font_provider.hpp"

namespace filepan::text {

// HarfBuzz fonts are created at this scale whatever the text size, so one hb_font_t serves every
// size: positions come out in 1/kHbScale em.
inline constexpr int kHbScale = 1 << 16;

struct AxisRange {
    bool present = false;
    float min = 0;
    float max = 0;

    [[nodiscard]] float clamp(float value) const {
        return value < min ? min : value > max ? max : value;
    }
};

// One font file (or one face of a collection), shared by all its variation instances.
struct Face {
    std::shared_ptr<const void> owner;  // Keeps bytes alive (an mmap or the provider's memory).
    std::span<const std::byte> bytes;
    std::uint32_t index = 0;
    hb_face_t *hbFace = nullptr;
    hb_font_t *hbFont = nullptr;  // The default instance, for coverage checks.
    AxisRange wght;
    AxisRange wdth;
    AxisRange fill;
    bool color = false;     // Has COLR or CBDT glyphs.
    bool colorPng = false;  // Has CBDT/sbix bitmaps.
};

// A face at one variation instance; what a gfx::FaceKey names.
struct Instance {
    std::uint32_t face = 0;
    float wght = 0;
    float wdth = 0;
    float fill = 0;
    hb_font_t *hbFont = nullptr;  // Scaled to kHbScale; immutable, so safe on any thread.
    // Line metrics in em, for this instance.
    float ascent = 0;
    float descent = 0;  // Positive, below the baseline.
    float lineGap = 0;
    bool color = false;
    // FreeType faces aren't thread-safe, so each instance has its own, used under its own lock.
    // Rasterizing is nearly all on the render thread, so the lock is uncontended in practice,
    // and one FT_Face per instance means the variation coordinates are set once, not per glyph.
    std::mutex ftMutex;
    FT_Face ftFace = nullptr;
    bool ftFailed = false;
};

// Owns every face and instance. Thread-safe; faces and instances live as long as it does.
class FontRegistry {
public:
    static constexpr int kNoFace = -1;

    FontRegistry(std::unique_ptr<FontProvider> provider, std::string locale);
    ~FontRegistry();
    FontRegistry(const FontRegistry &) = delete;
    FontRegistry &operator=(const FontRegistry &) = delete;

    // The bundled face for a family, or kNoFace if it couldn't be loaded.
    [[nodiscard]] int primaryFace(gfx::FontFamily family) const {
        return primary_[static_cast<std::size_t>(family)];
    }

    // Whether face has a glyph for every visible character of cluster.
    [[nodiscard]] bool covers(int face, std::u16string_view cluster) const;

    // A system face that covers cluster, or kNoFace. Cached per character and weight.
    int fallbackFace(std::u16string_view cluster, int weight);

    [[nodiscard]] gfx::FaceKey instanceKey(int face, const gfx::TextStyle &style);
    [[nodiscard]] Instance &instance(gfx::FaceKey key);

    bool rasterize(gfx::FaceKey key, std::uint32_t glyph, float size, float subpixelX,
                   gfx::GlyphBitmap &out);

private:
    int addFace(std::shared_ptr<const void> owner, std::span<const std::byte> bytes,
                std::uint32_t index);
    [[nodiscard]] const Face &face(int index) const;
    bool ensureFtFace(Instance &instance);
    bool rasterizeOutline(Instance &instance, std::uint32_t glyph, float size, float subpixelX,
                          gfx::GlyphBitmap &out);
    bool rasterizeColor(Instance &instance, std::uint32_t glyph, float size, float subpixelX,
                        gfx::GlyphBitmap &out);

    std::unique_ptr<FontProvider> provider_;
    std::string locale_;
    std::array<int, 3> primary_{kNoFace, kNoFace, kNoFace};

    mutable std::shared_mutex mutex_;  // Guards everything below.
    std::deque<Face> faces_;
    std::deque<Instance> instances_;
    std::unordered_map<std::uint64_t, gfx::FaceKey> instanceKeys_;
    std::unordered_map<std::string, int> facesByFile_;
    std::unordered_map<std::uint64_t, int> fallbackCache_;

    std::mutex ftLibraryMutex_;  // FT_New_Face and FT_Done_Face aren't thread-safe.
    FT_Library ftLibrary_ = nullptr;
};

}  // namespace filepan::text
