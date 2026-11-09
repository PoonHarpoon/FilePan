// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "filepan/text/collator.hpp"
#include "filepan/text/text_system.hpp"
#include "test_util.hpp"

namespace {

using namespace filepan;
using gfx::FontFamily;
using gfx::ShapedText;
using gfx::TextLayoutOptions;
using gfx::TextOverflow;
using gfx::TextStyle;

const std::string kTestFonts = FILEPAN_HB_TEST_FONTS_DIR;
const std::string kHebrewFont = kTestFonts + "/subset/data/fonts/NotoIKEAHebrewLatin-Regular.ttf";
const std::string kArabicFont = kTestFonts + "/api/fonts/Mada-VF.ttf";
const std::string kColrFont = kTestFonts + "/api/fonts/test_glyphs-glyf_colr_1.ttf";
const std::string kCbdtFont = kTestFonts + "/api/fonts/NotoColorEmoji.subset.ttf";

std::unique_ptr<gfx::TextSystem> makeSystem(std::string locale = "en", std::size_t cache = 4096) {
    std::vector<text::FontFile> fallback = {{kHebrewFont}, {kArabicFont}, {kColrFont}, {kCbdtFont}};
    return text::createTextSystem(text::createDirectoryFontProvider(FILEPAN_FONTS_DIR, fallback),
                                  {std::move(locale), cache});
}

TextStyle sans(float size = 16, float weight = 400, float stretch = 100) {
    TextStyle style;
    style.size = size;
    style.weight = weight;
    style.stretch = stretch;
    return style;
}

bool sameGlyphs(const ShapedText &a, const ShapedText &b) {
    return std::ranges::equal(a.glyphs, b.glyphs, [](const auto &x, const auto &y) {
        return x.glyph == y.glyph && x.position == y.position;
    });
}

const gfx::TextCluster *clusterAt(const ShapedText &text, std::uint32_t byte) {
    for (const gfx::TextCluster &cluster : text.clusters) {
        if (cluster.byte == byte) {
            return &cluster;
        }
    }
    return nullptr;
}

void testBasics(gfx::TextSystem &system) {
    const ShapedText text = system.shape("Hello", sans(), {});
    EXPECT(text.glyphs.size() == 5);
    EXPECT(text.runs.size() == 1);
    EXPECT(text.runs[0].glyphCount == 5 && text.runs[0].size == 16);
    EXPECT(text.lines.size() == 1);
    EXPECT(text.clusters.size() == 5);
    EXPECT(text.size.width > 20 && text.size.width < 60);
    EXPECT(text.size.height > 14 && text.size.height < 30);
    EXPECT(!text.truncated);
    for (std::size_t i = 1; i < text.glyphs.size(); ++i) {
        EXPECT(text.glyphs[i].position.x > text.glyphs[i - 1].position.x);
    }
    EXPECT(text.lines[0].startByte == 0 && text.lines[0].endByte == 5);
    EXPECT(text.lines[0].baseline > text.lines[0].top);

    // Empty text still has a line, for the caret.
    const ShapedText empty = system.shape("", sans(), {});
    EXPECT(empty.glyphs.empty() && empty.lines.size() == 1 && empty.size.height > 0);
    EXPECT(empty.caretRect(0).height > 0);

    // A fixed line height.
    TextStyle tall = sans();
    tall.lineHeight = 2;
    EXPECT(std::abs(system.shape("Hello", tall, {}).size.height - 32) < 0.01f);

    // Letter spacing adds after each grapheme.
    TextStyle spaced = sans();
    spaced.letterSpacing = 0.1f;
    const float spacedWidth = system.shape("Hello", spaced, {}).size.width;
    EXPECT(std::abs(spacedWidth - (text.size.width + 5 * 1.6f)) < 0.05f);
}

void testWidthsMonotonic(gfx::TextSystem &system) {
    const std::string_view sample = "Hamburgefonstiv";
    float previous = 0;
    for (float weight = 100; weight <= 900; weight += 100) {
        const float width = system.shape(sample, sans(16, weight), {}).size.width;
        EXPECT(width >= previous);
        previous = width;
    }
    EXPECT(previous > system.shape(sample, sans(16, 100), {}).size.width);

    previous = 0;
    for (float stretch = 62; stretch <= 125; stretch += 7) {
        const float width = system.shape(sample, sans(16, 400, stretch), {}).size.width;
        EXPECT(width > previous);
        previous = width;
    }
    // Each instance is its own face.
    EXPECT(system.shape(sample, sans(16, 800, 62), {}).runs[0].face !=
           system.shape(sample, sans(16, 400, 100), {}).runs[0].face);

    TextStyle mono = sans();
    mono.family = FontFamily::mono;
    const float thin = system.shape("iiii", mono, {}).size.width;
    const float wide = system.shape("WWWW", mono, {}).size.width;
    EXPECT(std::abs(thin - wide) < 0.01f);
}

void testIconLigatures(gfx::TextSystem &system) {
    TextStyle icons = sans(24);
    icons.family = FontFamily::icons;
    for (const char *name : {"folder", "home", "settings", "arrow_back"}) {
        const ShapedText icon = system.shape(name, icons, {});
        EXPECT(icon.glyphs.size() == 1);
        EXPECT(icon.glyphs[0].glyph != 0);
        EXPECT(std::abs(icon.size.width - 24) < 0.5f);
    }
    // FILL is an axis: a filled folder is another instance (which may substitute the glyph).
    TextStyle filled = icons;
    filled.iconFill = 1;
    const ShapedText outline = system.shape("folder", icons, {});
    const ShapedText solid = system.shape("folder", filled, {});
    EXPECT(outline.runs[0].face != solid.runs[0].face);
    gfx::GlyphBitmap a;
    gfx::GlyphBitmap b;
    EXPECT(system.rasterize(outline.runs[0].face, outline.glyphs[0].glyph, 24, 0, a));
    EXPECT(system.rasterize(solid.runs[0].face, solid.glyphs[0].glyph, 24, 0, b));
    EXPECT(a.pixels != b.pixels);
    // Uppercase doesn't apply to icon names.
    TextStyle upper = icons;
    upper.uppercase = true;
    EXPECT(system.shape("folder", upper, {}).glyphs.size() == 1);
}

std::string visibleText(const ShapedText &text, std::string_view source) {
    std::string out;
    for (std::size_t i = 0; i < text.clusters.size(); ++i) {
        const std::uint32_t start = text.clusters[i].byte;
        std::uint32_t end = static_cast<std::uint32_t>(source.size());
        // A cluster ends where the next one in the source starts.
        for (const gfx::TextCluster &other : text.clusters) {
            if (other.byte > start && other.byte < end) {
                end = other.byte;
            }
        }
        out.append(source.substr(start, std::min<std::size_t>(end - start, 4)).substr(0, 1));
    }
    return out;
}

void testEllipsis(gfx::TextSystem &system) {
    const std::string_view name = "holiday_photos_from_the_beach_2024.jpg";
    const float full = system.shape(name, sans(), {}).size.width;
    TextLayoutOptions options;
    options.maxWidth = full / 2;
    options.maxLines = 1;

    options.overflow = TextOverflow::ellipsisEnd;
    const ShapedText end = system.shape(name, sans(), options);
    EXPECT(end.truncated);
    EXPECT(end.lines.size() == 1);
    EXPECT(end.size.width <= options.maxWidth + 0.01f);
    EXPECT(clusterAt(end, 0) != nullptr);
    EXPECT(clusterAt(end, static_cast<std::uint32_t>(name.size() - 1)) == nullptr);
    // The ellipsis is the last glyph, and isn't a cluster.
    EXPECT(end.glyphs.size() == end.clusters.size() + 1);
    const gfx::PositionedGlyph ellipsis = end.glyphs.back();
    EXPECT(ellipsis.position.x >= end.glyphs[end.glyphs.size() - 2].position.x);

    options.overflow = TextOverflow::ellipsisMiddle;
    const ShapedText middle = system.shape(name, sans(), options);
    EXPECT(middle.truncated);
    EXPECT(middle.size.width <= options.maxWidth + 0.01f);
    // Keeps the start and the extension.
    EXPECT(clusterAt(middle, 0) != nullptr);
    const std::size_t dot = name.rfind('.');
    for (std::size_t i = dot; i < name.size(); ++i) {
        EXPECT(clusterAt(middle, static_cast<std::uint32_t>(i)) != nullptr);
    }
    EXPECT(clusterAt(middle, 20) == nullptr);  // Something in the middle is gone.
    const std::string shown = visibleText(middle, name);
    EXPECT(shown.starts_with("holiday"));
    EXPECT(shown.ends_with("2024.jpg"));

    // Very narrow: still the extension.
    options.maxWidth = system.shape("hol\u2026.jpg", sans(), {}).size.width + 1;
    const ShapedText narrow = system.shape(name, sans(), options);
    EXPECT(clusterAt(narrow, static_cast<std::uint32_t>(dot)) != nullptr);
    EXPECT(narrow.size.width <= options.maxWidth + 0.01f);

    // Cut at the start: the end stays, the start goes behind the ellipsis.
    options.maxWidth = full * 0.6f;
    options.overflow = TextOverflow::ellipsisStart;
    const ShapedText start = system.shape(name, sans(), options);
    EXPECT(start.truncated && start.lines.size() == 1);
    EXPECT(start.size.width <= options.maxWidth + 0.01f);
    EXPECT(clusterAt(start, 0) == nullptr);
    EXPECT(clusterAt(start, static_cast<std::uint32_t>(name.size() - 1)) != nullptr);
    EXPECT(visibleText(start, name).ends_with("2024.jpg"));
    // The ellipsis leads.
    EXPECT(start.glyphs.size() == start.clusters.size() + 1);
    EXPECT(start.glyphs.front().position.x <= start.glyphs[1].position.x);

    // Clip doesn't add anything.
    options.overflow = TextOverflow::clip;
    const ShapedText clipped = system.shape(name, sans(), options);
    EXPECT(clipped.truncated);
    EXPECT(clipped.glyphs.size() == name.size());

    // Text that fits isn't touched.
    options.maxWidth = full + 1;
    options.overflow = TextOverflow::ellipsisMiddle;
    EXPECT(!system.shape(name, sans(), options).truncated);
}

void testLineBreaking(gfx::TextSystem &system) {
    const std::string_view text = "The quick brown fox jumps over the lazy dog again and again";
    TextLayoutOptions options;
    options.maxWidth = 120;
    const ShapedText wrapped = system.shape(text, sans(), options);
    EXPECT(wrapped.lines.size() >= 3);
    EXPECT(!wrapped.truncated);
    EXPECT(wrapped.lines.front().startByte == 0);
    EXPECT(wrapped.lines.back().endByte == text.size());
    for (std::size_t i = 0; i < wrapped.lines.size(); ++i) {
        const gfx::TextLine &line = wrapped.lines[i];
        EXPECT(line.width <= options.maxWidth + 0.01f);
        if (i > 0) {
            EXPECT(line.startByte == wrapped.lines[i - 1].endByte);
            EXPECT(line.top >= wrapped.lines[i - 1].top + wrapped.lines[i - 1].height - 0.01f);
            // Lines break after spaces, between words.
            EXPECT(text[line.startByte - 1] == ' ');
        }
    }
    EXPECT(std::abs(wrapped.size.height - wrapped.lines.back().top - wrapped.lines.back().height) <
           0.01f);
    // Every cluster is on its line.
    for (const gfx::TextCluster &cluster : wrapped.clusters) {
        const gfx::TextLine &line = wrapped.lines[cluster.line];
        EXPECT(cluster.byte >= line.startByte && cluster.byte < line.endByte);
    }

    options.maxLines = 2;
    const ShapedText two = system.shape(text, sans(), options);
    EXPECT(two.lines.size() == 2);
    EXPECT(two.truncated);
    EXPECT(two.lines[1].width <= options.maxWidth + 0.01f);

    // Hard breaks.
    const ShapedText hard = system.shape("one\ntwo\r\nthree", sans(), {});
    EXPECT(hard.lines.size() == 3);
    EXPECT(hard.lines[1].startByte == 4 && hard.lines[2].startByte == 9);
    EXPECT(system.shape("end\n", sans(), {}).lines.size() == 2);
    TextLayoutOptions oneLine;
    oneLine.maxLines = 1;
    const ShapedText cut = system.shape("first\nsecond", sans(), oneLine);
    EXPECT(cut.lines.size() == 1 && cut.truncated);

    // A word longer than the line breaks between graphemes.
    TextLayoutOptions narrow;
    narrow.maxWidth = 40;
    const ShapedText word = system.shape("Supercalifragilistic", sans(), narrow);
    EXPECT(word.lines.size() >= 3);
    for (const gfx::TextLine &line : word.lines) {
        EXPECT(line.width <= narrow.maxWidth + 0.01f);
    }

    // Alignment.
    TextLayoutOptions centered;
    centered.maxWidth = 300;
    centered.align = gfx::TextAlign::center;
    const ShapedText center = system.shape("Hi", sans(), centered);
    EXPECT(center.size.width == 300);
    EXPECT(std::abs(center.glyphs[0].position.x - (300 - center.lines[0].width) / 2) < 0.01f);
    centered.align = gfx::TextAlign::end;
    const ShapedText right = system.shape("Hi", sans(), centered);
    EXPECT(std::abs(right.glyphs[0].position.x - (300 - right.lines[0].width)) < 0.01f);
}

void testBidi(gfx::TextSystem &system) {
    // "abc שלום def": the Hebrew word runs right to left inside left-to-right text.
    const std::string text = "abc \u05E9\u05DC\u05D5\u05DD def";
    const ShapedText shaped = system.shape(text, sans(), {});
    EXPECT(shaped.lines.size() == 1);
    const gfx::TextCluster *shin = clusterAt(shaped, 4);
    const gfx::TextCluster *lamed = clusterAt(shaped, 6);
    const gfx::TextCluster *mem = clusterAt(shaped, 10);
    const gfx::TextCluster *a = clusterAt(shaped, 0);
    const gfx::TextCluster *d = clusterAt(shaped, 13);
    EXPECT(shin && lamed && mem && a && d);
    EXPECT(shin->rightToLeft && mem->rightToLeft && !a->rightToLeft && !d->rightToLeft);
    EXPECT(shin->x > lamed->x && lamed->x > mem->x);  // Visually reversed.
    EXPECT(a->x < mem->x && shin->x < d->x);
    EXPECT(shin->width > 1);          // Drawn by the Hebrew fallback font, not as zero-width boxes.
    EXPECT(shaped.runs.size() == 3);  // Archivo, the fallback, Archivo.
    EXPECT(shaped.runs[0].face != shaped.runs[1].face);

    // A right-to-left paragraph: Arabic first, then a Latin file extension.
    const std::string arabic = "\u0645\u0644\u0641.txt";
    const ShapedText rtl = system.shape(arabic, sans(), {});
    const gfx::TextCluster *meem = clusterAt(rtl, 0);
    const gfx::TextCluster *feh = clusterAt(rtl, 4);
    const gfx::TextCluster *t = clusterAt(rtl, 7);
    EXPECT(meem && feh && t);
    EXPECT(meem->rightToLeft && !t->rightToLeft);
    EXPECT(meem->x > feh->x && feh->x > t->x);  // The Latin run sits left of the Arabic.
    // Arabic letters join: the initial meem isn't the isolated form.
    const ShapedText alone = system.shape("\u0645", sans(), {});
    std::uint32_t meemGlyph = 0;
    for (std::size_t i = 0; i < rtl.glyphs.size(); ++i) {
        if (std::abs(rtl.glyphs[i].position.x - meem->x) < 0.01f) {
            meemGlyph = rtl.glyphs[i].glyph;
        }
    }
    EXPECT(meemGlyph != 0 && meemGlyph != alone.glyphs[0].glyph);
    // Start alignment in a right-to-left paragraph is on the right.
    TextLayoutOptions boxed;
    boxed.maxWidth = 200;
    const ShapedText aligned = system.shape(arabic, sans(), boxed);
    EXPECT(aligned.size.width == 200);
    EXPECT(std::abs(clusterAt(aligned, 0)->x + clusterAt(aligned, 0)->width - 200) < 0.5f);
}

void testGraphemes(gfx::TextSystem &system) {
    // e + combining acute: one cluster.
    const ShapedText accent = system.shape("e\u0301x", sans(), {});
    EXPECT(accent.clusters.size() == 2);
    EXPECT(accent.clusters[0].byte == 0 && accent.clusters[1].byte == 3);
    // A ZWJ family emoji, a flag and a keycap: one cluster each.
    const std::string emoji =
            "\U0001F468\u200D\U0001F469\u200D\U0001F467|\U0001F1E9\U0001F1EA|8\uFE0F\u20E3";
    const ShapedText clusters = system.shape(emoji, sans(), {});
    EXPECT(clusters.clusters.size() == 5);
    EXPECT(clusters.clusters[1].byte == 18 && clusters.clusters[2].byte == 19);
    EXPECT(clusters.clusters[3].byte == 27 && clusters.clusters[4].byte == 28);
    // Hangul jamo and a Devanagari conjunct.
    EXPECT(system.shape("\u1100\u1161\u11A8", sans(), {}).clusters.size() == 1);
    EXPECT(system.shape("\u0915\u094D\u0937", sans(), {}).clusters.size() == 1);
    // Invalid UTF-8 becomes replacement characters without breaking anything.
    const ShapedText invalid = system.shape("a\xFF\xC3z", sans(), {});
    EXPECT(invalid.clusters.size() == 4);
}

void expectRoundTrip(const ShapedText &text, std::uint32_t endByte) {
    for (const gfx::TextCluster &cluster : text.clusters) {
        const gfx::Rect caret = text.caretRect(cluster.byte);
        EXPECT(caret.width == 0 && caret.height > 0);
        const float inward = cluster.rightToLeft ? -0.01f : 0.01f;
        const gfx::Vec2 point{caret.x + inward, caret.y + caret.height / 2};
        EXPECT(text.hitTest(point) == cluster.byte);
    }
    // After the end.
    const gfx::Rect last = text.caretRect(endByte);
    EXPECT(last.height > 0);
    const gfx::TextLine &line = text.lines.back();
    const bool rtlEnd = text.clusters.back().rightToLeft;
    const gfx::Vec2 beyond{rtlEnd ? last.x - 100 : last.x + 100, line.top + line.height / 2};
    EXPECT(text.hitTest(beyond) == endByte);
}

void testHitTestCaret(gfx::TextSystem &system) {
    const std::string latin = "Hello, world";
    const ShapedText text = system.shape(latin, sans(), {});
    expectRoundTrip(text, static_cast<std::uint32_t>(latin.size()));
    EXPECT(text.caretRect(0).x == 0);
    EXPECT(std::abs(text.caretRect(static_cast<std::uint32_t>(latin.size())).x - text.size.width) <
           0.01f);
    EXPECT(text.hitTest({-50, -50}) == 0);

    const std::string mixed = "abc \u05E9\u05DC\u05D5\u05DD def";
    expectRoundTrip(system.shape(mixed, sans(), {}), static_cast<std::uint32_t>(mixed.size()));

    const std::string arabic = "\u0645\u0644\u0641 \u0628";
    expectRoundTrip(system.shape(arabic, sans(), {}), static_cast<std::uint32_t>(arabic.size()));

    TextLayoutOptions wrap;
    wrap.maxWidth = 80;
    const std::string lines = "one two three four five six";
    const ShapedText multi = system.shape(lines, sans(), wrap);
    EXPECT(multi.lines.size() > 1);
    for (const gfx::TextCluster &cluster : multi.clusters) {
        const gfx::Rect caret = multi.caretRect(cluster.byte);
        EXPECT(std::abs(caret.y - multi.lines[cluster.line].top) < 0.01f);
        EXPECT(multi.hitTest({caret.x + 0.01f, caret.y + caret.height / 2}) == cluster.byte);
    }
    // A point below the text lands on the last line.
    EXPECT(multi.hitTest({0, 1000}) == multi.lines.back().startByte);

    // Ligatures: the icon's six letters share the glyph's width.
    TextStyle icons = sans(24);
    icons.family = FontFamily::icons;
    const ShapedText icon = system.shape("folder", icons, {});
    EXPECT(icon.clusters.size() == 6);
    EXPECT(std::abs(icon.clusters[1].x - icon.size.width / 6) < 0.01f);
}

void testTabs(gfx::TextSystem &system) {
    TextStyle mono = sans();
    mono.family = FontFamily::mono;
    // A tab goes to the next stop, four spaces apart, wherever it starts.
    const auto x = [&](std::string_view s) {
        const ShapedText text = system.shape(s, mono, {});
        return text.caretRect(static_cast<std::uint32_t>(s.size())).x;
    };
    // Trailing spaces don't count towards the size, but the caret goes past them.
    const float space = x(" ");
    EXPECT(space > 0);
    EXPECT(std::abs(x("\t") - 4 * space) < 0.01f);
    EXPECT(std::abs(x("ab\t") - 4 * space) < 0.01f);
    EXPECT(std::abs(x("abcd\t") - 8 * space) < 0.01f);
    EXPECT(std::abs(x("\t\tx") - 9 * space) < 0.01f);
    // Stops count from each paragraph's start.
    TextLayoutOptions unbounded;
    const ShapedText lines = system.shape("abcdef\n\tx", mono, unbounded);
    EXPECT(std::abs(lines.caretRect(8).x - 4 * space) < 0.01f);
}

void testUppercase(gfx::TextSystem &system) {
    TextStyle upper = sans();
    upper.uppercase = true;
    EXPECT(sameGlyphs(system.shape("hello", upper, {}), system.shape("HELLO", sans(), {})));

    // ß becomes SS: one cluster, pointing at ß, as wide as both letters.
    const ShapedText strasse = system.shape("stra\u00DFe", upper, {});
    const ShapedText reference = system.shape("STRASSE", sans(), {});
    EXPECT(strasse.glyphs.size() == 7);
    EXPECT(std::abs(strasse.size.width - reference.size.width) < 0.01f);
    EXPECT(strasse.clusters.size() == 6);
    const gfx::TextCluster *eszett = clusterAt(strasse, 4);
    EXPECT(eszett != nullptr && eszett->width > reference.clusters[4].width * 1.5f);
    EXPECT(strasse.lines[0].endByte == 7);

    // Turkish: i becomes İ, not I.
    auto turkish = makeSystem("tr");
    const ShapedText dotted = turkish->shape("i", upper, {});
    const ShapedText dotless = system.shape("i", upper, {});
    EXPECT(dotted.glyphs.size() == 1 && dotless.glyphs.size() == 1);
    EXPECT(dotted.glyphs[0].glyph != dotless.glyphs[0].glyph);
    EXPECT(dotted.glyphs[0].glyph == turkish->shape("\u0130", sans(), {}).glyphs[0].glyph);
}

void testRasterize(gfx::TextSystem &system) {
    const ShapedText text = system.shape("Ag ", sans(32), {});
    const gfx::FaceKey face = text.runs[0].face;
    gfx::GlyphBitmap capital;
    EXPECT(system.rasterize(face, text.glyphs[0].glyph, 32, 0, capital));
    EXPECT(capital.format == gfx::GlyphFormat::alpha8);
    EXPECT(capital.width > 10 && capital.width < 32);
    EXPECT(capital.height > 15 && capital.height < 32);
    EXPECT(capital.top >= capital.height - 1 && capital.top < 32);  // Sits on the baseline.
    EXPECT(capital.pixels.size() == static_cast<std::size_t>(capital.width * capital.height));
    EXPECT(*std::ranges::max_element(capital.pixels) == 255);
    EXPECT(*std::ranges::min_element(capital.pixels) == 0);

    gfx::GlyphBitmap descender;
    EXPECT(system.rasterize(face, text.glyphs[1].glyph, 32, 0, descender));
    EXPECT(descender.top < descender.height);  // Goes below the baseline.

    gfx::GlyphBitmap shifted;
    EXPECT(system.rasterize(face, text.glyphs[0].glyph, 32, 0.5f, shifted));
    EXPECT(shifted.pixels != capital.pixels);

    gfx::GlyphBitmap space;
    EXPECT(!system.rasterize(face, text.glyphs[2].glyph, 32, 0, space));
    EXPECT(space.pixels.empty() && space.width == 0);

    // Bolder instances have more ink.
    const ShapedText bold = system.shape("A", sans(32, 900), {});
    gfx::GlyphBitmap heavy;
    EXPECT(system.rasterize(bold.runs[0].face, bold.glyphs[0].glyph, 32, 0, heavy));
    const auto ink = [](const gfx::GlyphBitmap &bitmap) {
        long sum = 0;
        for (const std::uint8_t value : bitmap.pixels) {
            sum += value;
        }
        return sum;
    };
    EXPECT(ink(heavy) > ink(capital) * 1.3);

    // Bad input.
    gfx::GlyphBitmap none;
    EXPECT(!system.rasterize(9999, 1, 32, 0, none));
    EXPECT(!system.rasterize(face, 1, 0, 0, none));

    // A COLRv1 color glyph, through the fallback font.
    const ShapedText color = system.shape("\U000F0100", sans(64), {});
    EXPECT(color.runs.size() == 1 && color.runs[0].color);
    gfx::GlyphBitmap rgba;
    EXPECT(system.rasterize(color.runs[0].face, color.glyphs[0].glyph, 64, 0, rgba));
    EXPECT(rgba.format == gfx::GlyphFormat::rgba8);
    EXPECT(rgba.width > 10 && rgba.height > 10);
    EXPECT(rgba.pixels.size() == static_cast<std::size_t>(rgba.width * rgba.height * 4));
    bool colorful = false;
    for (std::size_t i = 0; i + 3 < rgba.pixels.size(); i += 4) {
        EXPECT(rgba.pixels[i] <= rgba.pixels[i + 3] && rgba.pixels[i + 1] <= rgba.pixels[i + 3] &&
               rgba.pixels[i + 2] <= rgba.pixels[i + 3]);  // Premultiplied.
        colorful = colorful || rgba.pixels[i] != rgba.pixels[i + 1] ||
                   rgba.pixels[i + 1] != rgba.pixels[i + 2];
    }
    EXPECT(colorful);

    // CBDT bitmaps need a PNG decoder, which only Android has.
    const ShapedText cbdt = system.shape("\u2049", sans(32), {});
    EXPECT(cbdt.runs.size() == 1 && cbdt.runs[0].color);
    gfx::GlyphBitmap png;
    EXPECT(!system.rasterize(cbdt.runs[0].face, cbdt.glyphs[0].glyph, 32, 0, png));
}

void testCache(gfx::TextSystem &system) {
    const ShapedText first = system.shape("cached.txt", sans(), {});
    const ShapedText second = system.shape("cached.txt", sans(), {});
    EXPECT(sameGlyphs(first, second) && first.size == second.size);
    // A tiny cache still gives right answers as it evicts.
    auto small = makeSystem("en", 2);
    for (int round = 0; round < 3; ++round) {
        for (const char *name : {"a.txt", "b.txt", "c.txt"}) {
            EXPECT(sameGlyphs(small->shape(name, sans(), {}), system.shape(name, sans(), {})));
        }
    }
}

void testThreads() {
    auto system = makeSystem("en", 64);
    std::vector<std::string> names;
    for (int i = 0; i < 200; ++i) {
        names.push_back("file_" + std::to_string(i * 7919) +
                        (i % 3 == 0 ? " \u05E9\u05DC\u05D5\u05DD" : "") + ".txt");
    }
    // Expected results, shaped on one thread by a separate system.
    auto reference = makeSystem("en", 0);
    std::vector<ShapedText> expected;
    for (const std::string &name : names) {
        expected.push_back(reference->shape(name, sans(15, 500), {}));
    }

    std::atomic<int> failures = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int round = 0; round < 20; ++round) {
                for (std::size_t i = static_cast<std::size_t>(t); i < names.size(); i += 3) {
                    const ShapedText text = system->shape(names[i], sans(15, 500), {});
                    if (!sameGlyphs(text, expected[i])) {
                        ++failures;
                    }
                    for (const gfx::GlyphRun &run : text.runs) {
                        gfx::GlyphBitmap bitmap;
                        const std::uint32_t glyph = text.glyphs[run.firstGlyph].glyph;
                        system->rasterize(run.face, glyph, run.size, 0.25f * (round % 4), bitmap);
                        if (bitmap.pixels.size() !=
                            static_cast<std::size_t>(bitmap.width * bitmap.height)) {
                            ++failures;
                        }
                    }
                }
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    EXPECT(failures == 0);
}

// ICU collation with numeric ordering: digit runs by value, case and accents secondary, and only
// identical strings equal.
void testCollator() {
    auto collator = text::Collator::create("en-US");
    EXPECT(collator != nullptr);
    std::vector<std::string> names = {"file10.txt", "File2.txt", "file1.txt", "éclair", "eclair", "Zebra", "apple",
                                      "file01.txt"};
    std::ranges::sort(names, [&](const std::string &a, const std::string &b) { return collator->compare(a, b) < 0; });
    const std::vector<std::string> expected = {"apple", "eclair", "éclair", "file01.txt", "file1.txt", "File2.txt",
                                               "file10.txt", "Zebra"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] != expected[i]) {
            std::fprintf(stderr, "collator order %zu: %s, expected %s\n", i, names[i].c_str(), expected[i].c_str());
        }
    }
    EXPECT(names == expected);
    EXPECT(collator->compare("a", "a") == 0);
    EXPECT(collator->compare("a", "b") < 0 && collator->compare("b", "a") > 0);
    // Without numeric ordering, "10" sorts before "2".
    auto plain = text::Collator::create("en", {.numeric = false});
    EXPECT(plain->compare("file10", "file2") < 0);
    // An unknown locale falls back to the root collation.
    EXPECT(text::Collator::create("xx-unknown") != nullptr);
}

}  // namespace

int main() {
    auto system = makeSystem();
    testBasics(*system);
    testWidthsMonotonic(*system);
    testIconLigatures(*system);
    testEllipsis(*system);
    testLineBreaking(*system);
    testBidi(*system);
    testGraphemes(*system);
    testHitTestCaret(*system);
    testTabs(*system);
    testUppercase(*system);
    testRasterize(*system);
    testCache(*system);
    testThreads();
    testCollator();
    std::puts("text_test: OK");
    return 0;
}
