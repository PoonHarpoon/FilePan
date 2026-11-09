// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "filepan/gfx/text.hpp"
#include "filepan/text/font_provider.hpp"

// FilePan's gfx::TextSystem, on FreeType, HarfBuzz and ICU.
//
// Layout notes beyond gfx/text.hpp:
// - Lines are aligned within maxWidth when it's finite, otherwise within the widest line. When
//   that moves anything (center, end, or start in a right-to-left paragraph) and maxWidth is
//   finite, size.width is maxWidth; otherwise it's the widest line.
// - Trailing spaces hang: they don't count towards a line's width.
// - letterSpacing is added after every grapheme cluster, as in CSS, and turns off optional
//   ligatures (except for icons, which need them).
// - With maxLines, the last line takes the rest of the text and overflow applies to it;
//   ellipsisMiddle and ellipsisStart keep the end of the whole text.
namespace filepan::text {

struct TextSystemOptions {
    // BCP 47; used for uppercasing, line breaking, shaping and fallback font choice.
    std::string locale = "en";
    // Shaped layouts kept for reuse; file lists reshape the same names all the time.
    std::size_t shapeCacheEntries = 4096;
};

std::unique_ptr<gfx::TextSystem> createTextSystem(std::unique_ptr<FontProvider> fontProvider,
                                                  TextSystemOptions options = {});

}  // namespace filepan::text
