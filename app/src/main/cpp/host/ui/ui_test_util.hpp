// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

#include "filepan/gfx/text.hpp"
#include "test_util.hpp"

namespace filepan::test {

// Text with trivial metrics: every code point is size * 0.5 wide, lines are size * 1.2 tall
// (or the style's line height). Wraps or truncates at maxWidth like the real thing would.
class FakeTextSystem final : public gfx::TextSystem {
public:
    gfx::ShapedText shape(std::string_view utf8, const gfx::TextStyle &style,
                          const gfx::TextLayoutOptions &options) override {
        ++shapeCalls;
        std::size_t codePoints = 0;
        for (char c : utf8) {
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
                ++codePoints;
            }
        }
        const float advance = style.size * 0.5f;
        const float naturalWidth = static_cast<float>(codePoints) * advance;
        const float lineHeight = style.size * (style.lineHeight > 0 ? style.lineHeight : 1.2f);
        gfx::ShapedText out;
        int lines = 1;
        float width = naturalWidth;
        if (naturalWidth > options.maxWidth) {
            const bool singleLine = options.maxLines == 1;
            if (singleLine) {
                width = options.maxWidth;
                out.truncated = true;
            } else {
                const float perLine = std::max(advance, std::floor(options.maxWidth / advance) * advance);
                lines = static_cast<int>(std::ceil(naturalWidth / perLine));
                if (options.maxLines > 0 && lines > options.maxLines) {
                    lines = options.maxLines;
                    out.truncated = true;
                }
                width = std::min(perLine, options.maxWidth);
            }
        }
        for (int i = 0; i < lines; ++i) {
            out.lines.push_back({static_cast<float>(i) * lineHeight, static_cast<float>(i) * lineHeight + style.size,
                                 lineHeight, width, 0, static_cast<std::uint32_t>(utf8.size())});
        }
        out.size = {width, static_cast<float>(lines) * lineHeight};
        return out;
    }

    bool rasterize(gfx::FaceKey, std::uint32_t, float, float, gfx::GlyphBitmap &) override { return false; }

    int shapeCalls = 0;
};

inline bool near(float a, float b, float epsilon = 0.01f) { return std::abs(a - b) <= epsilon; }

}  // namespace filepan::test
