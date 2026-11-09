// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/text/font_registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include FT_MULTIPLE_MASTERS_H
#include FT_OUTLINE_H
#include <hb-ot.h>
#include <hb-raster.h>

#include "filepan/text/png_decoder.hpp"
#include "filepan/text/unicode.hpp"

namespace filepan::text {

std::vector<FontFile> FontProvider::fallbackFonts(std::u16string_view, std::string_view, int) {
    return {};
}

namespace {

constexpr hb_tag_t kWght = HB_TAG('w', 'g', 'h', 't');
constexpr hb_tag_t kWdth = HB_TAG('w', 'd', 't', 'h');
constexpr hb_tag_t kFill = HB_TAG('F', 'I', 'L', 'L');

struct Mapping {
    void *address = nullptr;
    std::size_t size = 0;

    ~Mapping() {
        if (address != nullptr) {
            munmap(address, size);
        }
    }
};

// Maps a font file read-only; the pages are shared with every other process using the font.
std::shared_ptr<Mapping> mapFile(const std::string &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return nullptr;
    }
    struct stat status{};
    std::shared_ptr<Mapping> mapping;
    if (fstat(fd, &status) == 0 && status.st_size > 0) {
        const auto size = static_cast<std::size_t>(status.st_size);
        void *address = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (address != MAP_FAILED) {
            mapping = std::make_shared<Mapping>();
            mapping->address = address;
            mapping->size = size;
        }
    }
    close(fd);
    return mapping;
}

AxisRange axisRange(hb_face_t *face, hb_tag_t tag) {
    hb_ot_var_axis_info_t info{};
    if (!hb_ot_var_find_axis_info(face, tag, &info)) {
        return {};
    }
    return {true, info.min_value, info.max_value};
}

float metric(hb_font_t *font, hb_ot_metrics_tag_t tag) {
    hb_position_t position = 0;
    hb_ot_metrics_get_position_with_fallback(font, tag, &position);
    return static_cast<float>(position) / kHbScale;
}

std::uint64_t fallbackKey(char32_t c, bool emoji, int weight) {
    return static_cast<std::uint64_t>(c) | (static_cast<std::uint64_t>(emoji) << 21) |
           (static_cast<std::uint64_t>(std::clamp(weight / 100, 0, 15)) << 22);
}

struct RasterPaintDeleter {
    void operator()(hb_raster_paint_t *paint) const { hb_raster_paint_destroy(paint); }
};

}  // namespace

FontRegistry::FontRegistry(std::unique_ptr<FontProvider> provider, std::string locale)
    : provider_(std::move(provider)), locale_(std::move(locale)) {
    FT_Init_FreeType(&ftLibrary_);
    for (const auto family :
         {gfx::FontFamily::sans, gfx::FontFamily::mono, gfx::FontFamily::icons}) {
        const std::span<const std::byte> bytes = provider_->bundledFont(family);
        if (!bytes.empty()) {
            // The provider owns the bytes and outlives the registry.
            primary_[static_cast<std::size_t>(family)] = addFace(nullptr, bytes, 0);
        }
    }
}

FontRegistry::~FontRegistry() {
    for (Instance &instance : instances_) {
        if (instance.ftFace != nullptr) {
            FT_Done_Face(instance.ftFace);
        }
        hb_font_destroy(instance.hbFont);
    }
    for (Face &face : faces_) {
        hb_font_destroy(face.hbFont);
        hb_face_destroy(face.hbFace);
    }
    FT_Done_FreeType(ftLibrary_);
}

int FontRegistry::addFace(std::shared_ptr<const void> owner, std::span<const std::byte> bytes,
                          std::uint32_t index) {
    hb_blob_t *blob = hb_blob_create(reinterpret_cast<const char *>(bytes.data()),
                                     static_cast<unsigned>(bytes.size()), HB_MEMORY_MODE_READONLY,
                                     nullptr, nullptr);
    hb_face_t *hbFace = hb_face_create(blob, index);
    hb_blob_destroy(blob);
    if (hb_face_get_glyph_count(hbFace) == 0) {
        hb_face_destroy(hbFace);
        return kNoFace;
    }
    hb_face_make_immutable(hbFace);
    hb_font_t *hbFont = hb_font_create(hbFace);
    hb_font_make_immutable(hbFont);

    std::unique_lock lock(mutex_);
    Face &face = faces_.emplace_back();
    face.owner = std::move(owner);
    face.bytes = bytes;
    face.index = index;
    face.hbFace = hbFace;
    face.hbFont = hbFont;
    face.wght = axisRange(hbFace, kWght);
    face.wdth = axisRange(hbFace, kWdth);
    face.fill = axisRange(hbFace, kFill);
    face.colorPng = hb_ot_color_has_png(hbFace);
    face.color = face.colorPng || hb_ot_color_has_layers(hbFace) || hb_ot_color_has_paint(hbFace);
    return static_cast<int>(faces_.size() - 1);
}

const Face &FontRegistry::face(int index) const {
    std::shared_lock lock(mutex_);
    return faces_[static_cast<std::size_t>(index)];
}

bool FontRegistry::covers(int faceIndex, std::u16string_view cluster) const {
    if (faceIndex == kNoFace) {
        return false;
    }
    hb_font_t *font = face(faceIndex).hbFont;
    for (std::size_t i = 0; i < cluster.size();) {
        const char32_t c = codePointAt(cluster, i);
        if (isDefaultIgnorable(c) || isControl(c)) {
            continue;
        }
        hb_codepoint_t glyph = 0;
        if (!hb_font_get_nominal_glyph(font, c, &glyph)) {
            return false;
        }
    }
    return true;
}

int FontRegistry::fallbackFace(std::u16string_view cluster, int weight) {
    std::size_t i = 0;
    const char32_t base = codePointAt(cluster, i);
    const bool emoji = cluster.find(u'\uFE0F') != std::u16string_view::npos;
    const std::uint64_t key = fallbackKey(base, emoji, weight);
    {
        std::shared_lock lock(mutex_);
        if (const auto it = fallbackCache_.find(key); it != fallbackCache_.end()) {
            return it->second;
        }
    }

    int result = kNoFace;
    int baseOnly = kNoFace;
    for (const FontFile &file : provider_->fallbackFonts(cluster, locale_, weight)) {
        const std::string fileKey = file.path + '\0' + std::to_string(file.index);
        int faceIndex = kNoFace;
        bool known = false;
        {
            std::shared_lock lock(mutex_);
            if (const auto it = facesByFile_.find(fileKey); it != facesByFile_.end()) {
                faceIndex = it->second;
                known = true;
            }
        }
        if (!known) {
            std::shared_ptr<Mapping> mapping = mapFile(file.path);
            if (mapping != nullptr) {
                const std::span bytes(static_cast<const std::byte *>(mapping->address),
                                      mapping->size);
                faceIndex = addFace(std::move(mapping), bytes, file.index);
            }
            std::unique_lock lock(mutex_);
            // Another thread may have loaded it meanwhile; the duplicate is harmless.
            facesByFile_.emplace(fileKey, faceIndex);
        }
        if (faceIndex == kNoFace) {
            continue;
        }
        if (covers(faceIndex, cluster)) {
            result = faceIndex;
            break;
        }
        if (baseOnly == kNoFace && covers(faceIndex, cluster.substr(0, i))) {
            baseOnly = faceIndex;
        }
    }
    if (result == kNoFace) {
        result = baseOnly;
    }
    std::unique_lock lock(mutex_);
    fallbackCache_.emplace(key, result);
    return result;
}

gfx::FaceKey FontRegistry::instanceKey(int faceIndex, const gfx::TextStyle &style) {
    const Face &f = face(faceIndex);
    const float wght = f.wght.present ? f.wght.clamp(std::round(style.weight)) : 0;
    const float wdth = f.wdth.present ? f.wdth.clamp(std::round(style.stretch * 10) / 10) : 0;
    const float fill = f.fill.present ? f.fill.clamp(std::round(style.iconFill * 100) / 100) : 0;
    const std::uint64_t key = static_cast<std::uint64_t>(faceIndex) |
                              (static_cast<std::uint64_t>(wght) << 16) |
                              (static_cast<std::uint64_t>(wdth * 10) << 28) |
                              (static_cast<std::uint64_t>(fill * 100) << 44);
    {
        std::shared_lock lock(mutex_);
        if (const auto it = instanceKeys_.find(key); it != instanceKeys_.end()) {
            return it->second;
        }
    }

    hb_font_t *font = hb_font_create(f.hbFace);
    hb_font_set_scale(font, kHbScale, kHbScale);
    std::vector<hb_variation_t> variations;
    if (f.wght.present) {
        variations.push_back({kWght, wght});
    }
    if (f.wdth.present) {
        variations.push_back({kWdth, wdth});
    }
    if (f.fill.present) {
        variations.push_back({kFill, fill});
    }
    hb_font_set_variations(font, variations.data(), static_cast<unsigned>(variations.size()));
    hb_font_make_immutable(font);

    std::unique_lock lock(mutex_);
    if (const auto it = instanceKeys_.find(key); it != instanceKeys_.end()) {
        hb_font_destroy(font);
        return it->second;
    }
    Instance &instance = instances_.emplace_back();
    instance.face = static_cast<std::uint32_t>(faceIndex);
    instance.wght = wght;
    instance.wdth = wdth;
    instance.fill = fill;
    instance.hbFont = font;
    instance.ascent = metric(font, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER);
    instance.descent = -metric(font, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER);
    instance.lineGap = std::max(0.0f, metric(font, HB_OT_METRICS_TAG_HORIZONTAL_LINE_GAP));
    instance.color = f.color;
    const auto faceKey = static_cast<gfx::FaceKey>(instances_.size() - 1);
    instanceKeys_.emplace(key, faceKey);
    return faceKey;
}

Instance &FontRegistry::instance(gfx::FaceKey key) {
    std::shared_lock lock(mutex_);
    return instances_[key];
}

bool FontRegistry::ensureFtFace(Instance &instance) {
    if (instance.ftFace != nullptr) {
        return true;
    }
    if (instance.ftFailed) {
        return false;
    }
    const Face &f = face(static_cast<int>(instance.face));
    FT_Face ftFace = nullptr;
    {
        std::lock_guard lock(ftLibraryMutex_);
        if (FT_New_Memory_Face(ftLibrary_, reinterpret_cast<const FT_Byte *>(f.bytes.data()),
                               static_cast<FT_Long>(f.bytes.size()), static_cast<FT_Long>(f.index),
                               &ftFace) != 0) {
            instance.ftFailed = true;
            return false;
        }
    }
    FT_MM_Var *mmVar = nullptr;
    if (FT_HAS_MULTIPLE_MASTERS(ftFace) && FT_Get_MM_Var(ftFace, &mmVar) == 0) {
        std::vector<FT_Fixed> coords(mmVar->num_axis);
        for (FT_UInt i = 0; i < mmVar->num_axis; ++i) {
            const FT_Var_Axis &axis = mmVar->axis[i];
            float value = static_cast<float>(axis.def) / 65536.0f;
            if (axis.tag == kWght) {
                value = instance.wght;
            } else if (axis.tag == kWdth) {
                value = instance.wdth;
            } else if (axis.tag == kFill) {
                value = instance.fill;
            }
            coords[i] = static_cast<FT_Fixed>(std::lround(value * 65536.0f));
        }
        FT_Set_Var_Design_Coordinates(ftFace, mmVar->num_axis, coords.data());
        FT_Done_MM_Var(ftLibrary_, mmVar);
    }
    instance.ftFace = ftFace;
    return true;
}

bool FontRegistry::rasterize(gfx::FaceKey key, std::uint32_t glyph, float size, float subpixelX,
                             gfx::GlyphBitmap &out) {
    out = {};
    {
        std::shared_lock lock(mutex_);
        if (key >= instances_.size()) {
            return false;
        }
    }
    if (!(size > 0) || size > 2048) {
        return false;
    }
    Instance &inst = instance(key);
    if (inst.color && rasterizeColor(inst, glyph, size, subpixelX, out)) {
        return true;
    }
    return rasterizeOutline(inst, glyph, size, subpixelX, out);
}

// Unhinted on purpose: FilePan targets 400+ ppi screens, where hinting buys little sharpness but
// snaps outlines to the pixel grid, which fights the fractional, subpixel-positioned advances
// HarfBuzz lays out (also unhinted) and distorts Archivo's condensed and variable instances
// differently at each size. Light auto-hinting would also need FreeType's auto-fitter module.
bool FontRegistry::rasterizeOutline(Instance &instance, std::uint32_t glyph, float size,
                                    float subpixelX, gfx::GlyphBitmap &out) {
    std::lock_guard lock(instance.ftMutex);
    if (!ensureFtFace(instance)) {
        return false;
    }
    FT_Face ftFace = instance.ftFace;
    if (FT_Set_Char_Size(ftFace, 0, static_cast<FT_F26Dot6>(std::lround(size * 64)), 72, 72) != 0 ||
        FT_Load_Glyph(ftFace, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) {
        return false;
    }
    FT_GlyphSlot slot = ftFace->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE || slot->outline.n_points == 0) {
        return false;
    }
    FT_Outline_Translate(&slot->outline, static_cast<FT_Pos>(std::lround(subpixelX * 64)), 0);
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
        return false;
    }
    const FT_Bitmap &bitmap = slot->bitmap;
    if (bitmap.width == 0 || bitmap.rows == 0 || bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
        return false;
    }
    out.width = static_cast<int>(bitmap.width);
    out.height = static_cast<int>(bitmap.rows);
    out.left = slot->bitmap_left;
    out.top = slot->bitmap_top;
    out.format = gfx::GlyphFormat::alpha8;
    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height);
    for (int y = 0; y < out.height; ++y) {
        const unsigned char *row = bitmap.buffer + static_cast<std::ptrdiff_t>(y) * bitmap.pitch;
        std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * out.width, row,
                    static_cast<std::size_t>(out.width));
    }
    return true;
}

// Color glyphs: COLRv0/v1 through harfbuzz-raster, CBDT bitmaps through the platform's PNG
// decoder. Neither touches FreeType, so there's no lock: HarfBuzz fonts are immutable and the
// paint context is per thread.
bool FontRegistry::rasterizeColor(Instance &instance, std::uint32_t glyph, float size,
                                  float subpixelX, gfx::GlyphBitmap &out) {
    hb_font_t *font = instance.hbFont;
    const float scale = size / kHbScale;
    hb_glyph_extents_t extents{};
    if (!hb_font_get_glyph_extents(font, glyph, &extents)) {
        return false;
    }

    hb_face_t *hbFace = hb_font_get_face(font);
    if (hb_ot_color_glyph_has_paint(hbFace, glyph) ||
        hb_ot_color_glyph_get_layers(hbFace, glyph, 0, nullptr, nullptr) > 0) {
        thread_local std::unique_ptr<hb_raster_paint_t, RasterPaintDeleter> paint(
                hb_raster_paint_create_or_fail());
        if (paint == nullptr) {
            return false;
        }
        hb_raster_paint_t *p = paint.get();
        hb_raster_paint_reset(p);
        // The scale factor shrinks font units to pixels; the transform shifts by subpixelX
        // before that, so in font units.
        hb_raster_paint_set_scale_factor(p, 1 / scale, 1 / scale);
        hb_raster_paint_set_transform(p, 1, 0, 0, 1, subpixelX / scale, 0);
        hb_raster_paint_set_foreground(p, HB_COLOR(0, 0, 0, 255));
        if (!hb_raster_paint_set_glyph_extents(p, &extents) ||
            !hb_raster_paint_glyph_or_fail(p, font, glyph)) {
            hb_raster_paint_clear(p);
            return false;
        }
        hb_raster_image_t *image = hb_raster_paint_render(p);
        if (image == nullptr) {
            return false;
        }
        hb_raster_extents_t imageExtents{};
        hb_raster_image_get_extents(image, &imageExtents);
        const std::uint8_t *buffer = hb_raster_image_get_buffer(image);
        out.width = static_cast<int>(imageExtents.width);
        out.height = static_cast<int>(imageExtents.height);
        out.left = imageExtents.x_origin;
        out.top = imageExtents.y_origin + out.height;
        out.format = gfx::GlyphFormat::rgba8;
        out.pixels.resize(static_cast<std::size_t>(out.width) * out.height * 4);
        // harfbuzz-raster stores rows bottom to top as premultiplied BGRA in native-endian
        // 32-bit words.
        for (int y = 0; y < out.height; ++y) {
            const auto *source = reinterpret_cast<const std::uint32_t *>(
                    buffer + static_cast<std::size_t>(out.height - 1 - y) * imageExtents.stride);
            std::uint8_t *target = out.pixels.data() + static_cast<std::size_t>(y) * out.width * 4;
            for (int x = 0; x < out.width; ++x) {
                const std::uint32_t pixel = source[x];
                target[x * 4 + 0] = static_cast<std::uint8_t>(pixel >> 16);
                target[x * 4 + 1] = static_cast<std::uint8_t>(pixel >> 8);
                target[x * 4 + 2] = static_cast<std::uint8_t>(pixel);
                target[x * 4 + 3] = static_cast<std::uint8_t>(pixel >> 24);
            }
        }
        hb_raster_paint_recycle_image(p, image);
        return out.width > 0 && out.height > 0;
    }

    if (face(static_cast<int>(instance.face)).colorPng) {
        hb_blob_t *png = hb_ot_color_glyph_reference_png(font, glyph);
        unsigned length = 0;
        const char *data = hb_blob_get_data(png, &length);
        const float left = extents.x_bearing * scale + subpixelX;
        const float top = extents.y_bearing * scale;
        const int width = static_cast<int>(std::lround(extents.width * scale));
        const int height = static_cast<int>(std::lround(-extents.height * scale));
        bool ok = false;
        if (length > 0 && width > 0 && height > 0) {
            ok = decodePng(std::span(reinterpret_cast<const std::byte *>(data), length), width,
                           height, out.pixels);
        }
        hb_blob_destroy(png);
        if (ok) {
            out.width = width;
            out.height = height;
            out.left = static_cast<int>(std::lround(left));
            out.top = static_cast<int>(std::lround(top));
            out.format = gfx::GlyphFormat::rgba8;
            return true;
        }
        out = {};
    }
    return false;
}

}  // namespace filepan::text
