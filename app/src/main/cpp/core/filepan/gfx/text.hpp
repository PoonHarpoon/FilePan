// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include "filepan/gfx/geometry.hpp"

// The contract between text shaping (FreeType + HarfBuzz + ICU, in text/), the renderer, which
// rasterizes glyphs into its atlas, and the UI framework, which lays text out.
namespace filepan::gfx {

// Identifies a font face at one variation instance (e.g. Archivo at wdth 62, wght 800). Assigned
// by the TextSystem; stable for its lifetime.
using FaceKey = std::uint32_t;

enum class FontFamily : std::uint8_t {
    sans,   // Archivo (variable: wdth 62..125, wght 100..900), with system fallback.
    mono,   // JetBrains Mono, with system fallback.
    icons,  // Material Symbols; text is the icon's ligature name, e.g. "folder".
};

struct TextStyle {
    FontFamily family = FontFamily::sans;
    float size = 16;           // Pixels.
    float weight = 400;
    float stretch = 100;       // Percent; Archivo's wdth axis. Ignored by faces without it.
    float letterSpacing = 0;   // In em.
    float lineHeight = 0;      // Multiple of size; 0 uses the font's own line height.
    bool uppercase = false;    // Locale-aware uppercasing before shaping.
    float iconFill = 0;        // Material Symbols' FILL axis, 0 or 1.

    constexpr bool operator==(const TextStyle &) const = default;
};

enum class TextOverflow : std::uint8_t {
    clip,
    ellipsisEnd,
    // Keeps the start and the end, which keeps a file's extension visible: "holiday…2024.jpg".
    ellipsisMiddle,
    // Keeps the end, cut at the start: a path whose last folders matter most ("…/Download/Papers").
    ellipsisStart,
};

enum class TextAlign : std::uint8_t { start, center, end };

struct TextLayoutOptions {
    float maxWidth = std::numeric_limits<float>::infinity();
    int maxLines = 0;  // 0 means unlimited.
    TextOverflow overflow = TextOverflow::ellipsisEnd;
    TextAlign align = TextAlign::start;

    constexpr bool operator==(const TextLayoutOptions &) const = default;
};

struct PositionedGlyph {
    std::uint32_t glyph = 0;
    Vec2 position;  // Pen position on the baseline, relative to the layout's top left.
};

// Consecutive glyphs from one face at one size.
struct GlyphRun {
    FaceKey face = 0;
    float size = 0;
    bool color = false;  // Color glyphs (emoji) ignore the text color.
    std::uint32_t firstGlyph = 0;
    std::uint32_t glyphCount = 0;
};

struct TextLine {
    float top = 0;
    float baseline = 0;
    float height = 0;
    float width = 0;
    std::uint32_t startByte = 0;  // Into the source string.
    std::uint32_t endByte = 0;
};

// A grapheme cluster's visual extent, for caret placement and hit testing.
struct TextCluster {
    std::uint32_t byte = 0;  // Start of the cluster in the source string.
    std::uint32_t line = 0;
    float x = 0;
    float width = 0;
    bool rightToLeft = false;
};

struct ShapedText {
    Size size;
    std::vector<PositionedGlyph> glyphs;
    std::vector<GlyphRun> runs;
    std::vector<TextLine> lines;
    std::vector<TextCluster> clusters;  // In logical order.
    bool truncated = false;             // An ellipsis or maxLines cut something off.

    // The byte offset of the caret position closest to point.
    [[nodiscard]] std::uint32_t hitTest(Vec2 point) const;
    // The caret's rect (zero width) before the cluster starting at byte, or after the last one.
    [[nodiscard]] Rect caretRect(std::uint32_t byte) const;
};

enum class GlyphFormat : std::uint8_t {
    alpha8,  // Coverage, tinted with the text color.
    rgba8,   // Premultiplied color (emoji).
};

struct GlyphBitmap {
    int width = 0;
    int height = 0;
    int left = 0;  // Offset of the bitmap's left edge from the pen position.
    int top = 0;   // Offset of the bitmap's top edge above the baseline (positive is up).
    GlyphFormat format = GlyphFormat::alpha8;
    std::vector<std::uint8_t> pixels;  // Tightly packed rows.
};

// Shapes and rasterizes text. Both calls are thread-safe: the UI thread shapes while the render
// thread rasterizes.
class TextSystem {
public:
    virtual ~TextSystem() = default;

    virtual ShapedText shape(std::string_view utf8, const TextStyle &style,
                             const TextLayoutOptions &options) = 0;

    // subpixelX is the pen position's fractional part, quantized by the caller. Returns false
    // if the glyph has no outline (e.g. a space); out is then left empty.
    virtual bool rasterize(FaceKey face, std::uint32_t glyph, float size, float subpixelX,
                           GlyphBitmap &out) = 0;
};

}  // namespace filepan::gfx
