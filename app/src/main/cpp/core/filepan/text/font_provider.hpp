// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "filepan/gfx/text.hpp"

namespace filepan::text {

// A font file on disk, e.g. one of Android's system fonts.
struct FontFile {
    std::string path;
    std::uint32_t index = 0;  // The face's index in a collection (.ttc).

    constexpr bool operator==(const FontFile &) const = default;
};

// Where the text system gets its fonts: the bundled ones for each family, and system fonts for
// text the bundled ones can't draw. The text system calls it from any thread, but caches what it
// returns, so it needn't be fast.
class FontProvider {
public:
    virtual ~FontProvider() = default;

    // The bundled font for family, as an in-memory TTF that stays valid and unchanged for the
    // provider's lifetime. Empty if it couldn't be loaded.
    virtual std::span<const std::byte> bundledFont(gfx::FontFamily family) = 0;

    // System fonts that may draw cluster (one grapheme cluster, UTF-16), best first, for a
    // BCP 47 locale and a CSS weight. The text system checks coverage itself and uses the first
    // that has every character. Must be thread-safe. The default has no fallback.
    virtual std::vector<FontFile> fallbackFonts(std::u16string_view cluster,
                                                std::string_view locale, int weight);
};

// Reads the bundled fonts from a directory (normally app/src/main/assets/fonts), for host builds
// and tests. Its fallback is a fixed list of font files, tried in order.
std::unique_ptr<FontProvider> createDirectoryFontProvider(std::string directory,
                                                          std::vector<FontFile> fallback = {});

}  // namespace filepan::text

#ifdef __ANDROID__
struct AAssetManager;

namespace filepan::text {

// Reads the bundled fonts from the APK's assets/fonts/ and falls back to Android's system fonts
// through AFontMatcher. assetManager must outlive the provider.
std::unique_ptr<FontProvider> createAndroidFontProvider(AAssetManager *assetManager);

}  // namespace filepan::text
#endif
