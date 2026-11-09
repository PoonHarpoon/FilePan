// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/text/font_provider.hpp"

#include <array>
#include <cstdio>
#include <utility>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android/font.h>
#include <android/font_matcher.h>
#endif

namespace filepan::text {

namespace {

// The bundled fonts' names under assets/fonts/, by gfx::FontFamily.
constexpr std::array<const char *, 3> kBundledFonts = {
        "Archivo.ttf",
        "JetBrainsMono.ttf",
        "MaterialSymbolsRounded.ttf",
};

std::vector<std::byte> readFile(const std::string &path) {
    std::vector<std::byte> bytes;
    std::FILE *file = std::fopen(path.c_str(), "rbe");
    if (file == nullptr) {
        return bytes;
    }
    std::array<std::byte, 65536> buffer{};
    std::size_t count = 0;
    while ((count = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
        bytes.insert(bytes.end(), buffer.begin(),
                     buffer.begin() + static_cast<std::ptrdiff_t>(count));
    }
    std::fclose(file);
    return bytes;
}

class DirectoryFontProvider final : public FontProvider {
public:
    DirectoryFontProvider(const std::string &directory, std::vector<FontFile> fallback)
        : fallback_(std::move(fallback)) {
        for (std::size_t i = 0; i < kBundledFonts.size(); ++i) {
            fonts_[i] = readFile(directory + '/' + kBundledFonts[i]);
        }
    }

    std::span<const std::byte> bundledFont(gfx::FontFamily family) override {
        return fonts_[static_cast<std::size_t>(family)];
    }

    std::vector<FontFile> fallbackFonts(std::u16string_view, std::string_view, int) override {
        return fallback_;
    }

private:
    std::array<std::vector<std::byte>, kBundledFonts.size()> fonts_;
    std::vector<FontFile> fallback_;
};

}  // namespace

std::unique_ptr<FontProvider> createDirectoryFontProvider(std::string directory,
                                                          std::vector<FontFile> fallback) {
    return std::make_unique<DirectoryFontProvider>(directory, std::move(fallback));
}

#ifdef __ANDROID__

namespace {

class AndroidFontProvider final : public FontProvider {
public:
    explicit AndroidFontProvider(AAssetManager *assetManager) {
        for (std::size_t i = 0; i < kBundledFonts.size(); ++i) {
            const std::string path = std::string("fonts/") + kBundledFonts[i];
            // Streaming mode keeps an uncompressed asset mmapped from the APK rather than read.
            assets_[i] = AAssetManager_open(assetManager, path.c_str(), AASSET_MODE_STREAMING);
        }
    }

    ~AndroidFontProvider() override {
        for (AAsset *asset : assets_) {
            if (asset != nullptr) {
                AAsset_close(asset);
            }
        }
    }

    std::span<const std::byte> bundledFont(gfx::FontFamily family) override {
        AAsset *asset = assets_[static_cast<std::size_t>(family)];
        if (asset == nullptr) {
            return {};
        }
        const void *buffer = AAsset_getBuffer(asset);
        if (buffer == nullptr) {
            return {};
        }
        return {static_cast<const std::byte *>(buffer),
                static_cast<std::size_t>(AAsset_getLength64(asset))};
    }

    // AFontMatcher picks the font Android's own text stack would, from fonts.xml. It isn't
    // thread-safe, so each call gets its own; the text system caches the answers.
    std::vector<FontFile> fallbackFonts(std::u16string_view cluster, std::string_view locale,
                                        int weight) override {
        AFontMatcher *matcher = AFontMatcher_create();
        if (matcher == nullptr) {
            return {};
        }
        AFontMatcher_setStyle(matcher, static_cast<std::uint16_t>(weight), false);
        const std::string locales(locale);
        AFontMatcher_setLocales(matcher, locales.c_str());
        std::vector<FontFile> fonts;
        std::uint32_t runLength = 0;
        AFont *font = AFontMatcher_match(matcher, "sans-serif",
                                         reinterpret_cast<const std::uint16_t *>(cluster.data()),
                                         static_cast<std::uint32_t>(cluster.size()), &runLength);
        if (font != nullptr) {
            fonts.push_back({AFont_getFontFilePath(font),
                             static_cast<std::uint32_t>(AFont_getCollectionIndex(font))});
            AFont_close(font);
        }
        AFontMatcher_destroy(matcher);
        return fonts;
    }

private:
    std::array<AAsset *, kBundledFonts.size()> assets_{};
};

}  // namespace

std::unique_ptr<FontProvider> createAndroidFontProvider(AAssetManager *assetManager) {
    return std::make_unique<AndroidFontProvider>(assetManager);
}

#endif

}  // namespace filepan::text
