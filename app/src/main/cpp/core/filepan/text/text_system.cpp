// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/text/text_system.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <hb.h>

#include "filepan/text/font_registry.hpp"
#include "filepan/text/icu.hpp"
#include "filepan/text/unicode.hpp"

namespace filepan::text {

namespace {

using gfx::FaceKey;
using gfx::FontFamily;
using gfx::ShapedText;
using gfx::TextAlign;
using gfx::TextLayoutOptions;
using gfx::TextOverflow;
using gfx::TextStyle;

constexpr char16_t kEllipsis = u'\u2026';

// --- Shape cache ---------------------------------------------------------------------------

struct CacheKey {
    std::string text;
    TextStyle style;
    TextLayoutOptions options;

    bool operator==(const CacheKey &) const = default;
};

struct CacheKeyHash {
    std::size_t operator()(const CacheKey &key) const {
        std::size_t hash = std::hash<std::string>()(key.text);
        const auto mix = [&hash](std::uint64_t value) {
            hash ^= value + 0x9E3779B97F4A7C15ULL + (hash << 6) + (hash >> 2);
        };
        const TextStyle &s = key.style;
        mix(static_cast<std::uint64_t>(s.family) | (static_cast<std::uint64_t>(s.uppercase) << 8));
        mix(std::bit_cast<std::uint32_t>(s.size) |
            (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(s.weight)) << 32));
        mix(std::bit_cast<std::uint32_t>(s.stretch) |
            (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(s.letterSpacing)) << 32));
        mix(std::bit_cast<std::uint32_t>(s.lineHeight) |
            (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(s.iconFill)) << 32));
        const TextLayoutOptions &o = key.options;
        mix(std::bit_cast<std::uint32_t>(o.maxWidth) |
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(o.maxLines)) << 32));
        mix(static_cast<std::uint64_t>(o.overflow) | (static_cast<std::uint64_t>(o.align) << 8));
        return hash;
    }
};

// A bounded least-recently-used map from (text, style, options) to the shaped result.
class ShapeCache {
public:
    explicit ShapeCache(std::size_t capacity) : capacity_(capacity) {}

    bool find(const CacheKey &key, ShapedText &out) {
        std::lock_guard lock(mutex_);
        const auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }
        entries_.splice(entries_.begin(), entries_, it->second);
        out = it->second->second;
        return true;
    }

    void insert(CacheKey key, const ShapedText &value) {
        if (capacity_ == 0) {
            return;
        }
        std::lock_guard lock(mutex_);
        if (map_.contains(key)) {
            return;
        }
        entries_.emplace_front(key, value);
        map_.emplace(std::move(key), entries_.begin());
        if (map_.size() > capacity_) {
            map_.erase(entries_.back().first);
            entries_.pop_back();
        }
    }

private:
    using Entry = std::pair<CacheKey, ShapedText>;

    std::size_t capacity_;
    std::mutex mutex_;
    std::list<Entry> entries_;
    std::unordered_map<CacheKey, std::list<Entry>::iterator, CacheKeyHash> map_;
};

// --- Per-thread state ------------------------------------------------------------------------

// ICU and HarfBuzz objects that aren't thread-safe, one set per thread, plus scratch buffers.
struct ThreadState {
    std::string locale;
    icu::BreakIteratorPtr lineBreaks;
    icu::BreakIteratorPtr characterBreaks;
    icu::BiDiPtr bidi;
    hb_buffer_t *buffer = nullptr;

    ThreadState() = default;
    ThreadState(const ThreadState &) = delete;
    ThreadState &operator=(const ThreadState &) = delete;
    ~ThreadState() { hb_buffer_destroy(buffer); }

    void prepare(const std::string &newLocale) {
        if (buffer == nullptr) {
            buffer = hb_buffer_create();
            bidi.reset(ubidi_open());
        }
        if (locale != newLocale || lineBreaks == nullptr) {
            locale = newLocale;
            lineBreaks = icu::openBreakIterator(UBRK_LINE, locale);
            characterBreaks = icu::openBreakIterator(UBRK_CHARACTER, locale);
        }
    }
};

ThreadState &threadState() {
    thread_local ThreadState state;
    return state;
}

// --- Layout ----------------------------------------------------------------------------------

struct Glyph {
    std::uint32_t id = 0;
    std::uint32_t cluster = 0;  // UTF-16 index of the cluster's first unit.
    float advance = 0;
    float dx = 0;
    float dy = 0;
};

// A run of text in one face, direction and script, shaped as a whole. Glyphs are in visual
// order, as HarfBuzz outputs them.
struct Item {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::uint8_t level = 0;
    hb_script_t script = HB_SCRIPT_COMMON;
    int face = FontRegistry::kNoFace;
    FaceKey faceKey = 0;
    bool color = false;
    bool ellipsis = false;
    float ascent = 0;
    float descent = 0;
    std::uint32_t firstGlyph = 0;
    std::uint32_t glyphCount = 0;
};

// The part of an item shown on a line.
struct Segment {
    std::uint32_t item = 0;
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::uint8_t level = 0;
};

struct LineRange {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    // What's shown, when an ellipsis hides part of it: [start, keepEnd) … [resumeAt, end).
    bool ellipsis = false;
    std::uint32_t keepEnd = 0;
    std::uint32_t resumeAt = 0;
};

constexpr bool isSpace(char16_t c) {
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == 0x3000 || c == 0x00A0 ||
           c == 0x2028 || c == 0x2029 || c == 0x000B || c == 0x000C || c == 0x0085;
}

constexpr bool isHardBreak(char16_t c) {
    return c == u'\n' || c == u'\r' || c == 0x2028 || c == 0x2029 || c == 0x000B || c == 0x000C ||
           c == 0x0085;
}

class Layout {
public:
    Layout(FontRegistry &registry, const std::string &locale, hb_language_t language,
           std::string_view utf8, const TextStyle &style, const TextLayoutOptions &options)
        : registry_(registry),
          locale_(locale),
          language_(language),
          utf8_(utf8),
          style_(style),
          options_(options),
          state_(threadState()) {}

    ShapedText run();

private:
    void decode();
    void uppercase();
    void findGraphemes();
    void resolveBidi();
    void itemize();
    void expandTabs();
    void shapeItem(Item &item, std::u16string_view text);
    void shapeEllipsis(std::uint8_t level);
    void breakLines();
    void applyOverflow(LineRange &line, bool cutByLines);
    void build(ShapedText &out);

    [[nodiscard]] float width(std::uint32_t start, std::uint32_t end) const {
        return prefix_[end] - prefix_[start];
    }
    [[nodiscard]] std::uint32_t trimEnd(std::uint32_t start, std::uint32_t end) const {
        while (end > start && isSpace(text_[end - 1])) {
            --end;
        }
        return end;
    }
    [[nodiscard]] float trimmedWidth(std::uint32_t start, std::uint32_t end) const {
        return width(start, trimEnd(start, end));
    }
    // The last grapheme boundary <= index.
    [[nodiscard]] std::uint32_t graphemeFloor(std::uint32_t index) const {
        const auto it = std::upper_bound(graphemes_.begin(), graphemes_.end(), index);
        return *(it - 1);
    }
    [[nodiscard]] float spacing() const {
        return style_.family == FontFamily::icons ? 0 : style_.letterSpacing * style_.size;
    }

    FontRegistry &registry_;
    const std::string &locale_;
    hb_language_t language_;
    std::string_view utf8_;
    const TextStyle &style_;
    const TextLayoutOptions &options_;
    ThreadState &state_;

    std::u16string text_;                   // What's laid out (uppercased if asked).
    std::vector<std::uint32_t> byteOf_;     // UTF-16 index -> byte in utf8_, plus the end.
    std::vector<std::uint32_t> graphemes_;  // Grapheme boundaries, including 0 and the end.
    std::vector<std::uint8_t> levels_;      // Bidi level per UTF-16 unit; empty if all 0.
    std::uint8_t paragraphLevel_ = 0;
    std::vector<Item> items_;
    std::vector<Glyph> glyphs_;
    std::vector<float> prefix_;  // prefix_[i]: advance of text_[0, i).
    std::vector<LineRange> lines_;
    int ellipsisItem_ = -1;
    bool truncated_ = false;
    int primaryFace_ = FontRegistry::kNoFace;
    FaceKey primaryKey_ = 0;
    float primaryAscent_ = 0;
    float primaryDescent_ = 0;
    float primaryLineGap_ = 0;
};

void Layout::decode() {
    utf8ToUtf16(utf8_, text_, byteOf_);
}

// Locale-aware uppercasing, one grapheme at a time so that each output character still maps to
// the source grapheme it came from (ß becomes SS, both pointing at ß).
void Layout::uppercase() {
    const bool ascii = std::ranges::all_of(text_, [](char16_t c) { return c < 0x80; });
    const std::string_view language = std::string_view(locale_).substr(0, 2);
    if (ascii && language != "tr" && language != "az") {
        for (char16_t &c : text_) {
            if (c >= u'a' && c <= u'z') {
                c = static_cast<char16_t>(c - u'a' + u'A');
            }
        }
        return;
    }
    std::u16string upper;
    std::vector<std::uint32_t> upperByteOf;
    upper.reserve(text_.size());
    upperByteOf.reserve(text_.size() + 1);
    std::u16string piece;
    for (std::size_t g = 0; g + 1 < graphemes_.size(); ++g) {
        const std::uint32_t start = graphemes_[g];
        const auto length = static_cast<std::int32_t>(graphemes_[g + 1] - start);
        piece.resize(static_cast<std::size_t>(length) * 3 + 4);
        UErrorCode status = U_ZERO_ERROR;
        const std::int32_t written =
                u_strToUpper(piece.data(), static_cast<std::int32_t>(piece.size()),
                             text_.data() + start, length, locale_.c_str(), &status);
        if (U_FAILURE(status) || written < 0) {
            upper.append(text_, start, static_cast<std::size_t>(length));
            upperByteOf.insert(upperByteOf.end(), static_cast<std::size_t>(length), byteOf_[start]);
            continue;
        }
        upper.append(piece.data(), static_cast<std::size_t>(written));
        upperByteOf.insert(upperByteOf.end(), static_cast<std::size_t>(written), byteOf_[start]);
    }
    upperByteOf.push_back(byteOf_.back());
    text_ = std::move(upper);
    byteOf_ = std::move(upperByteOf);
}

void Layout::findGraphemes() {
    graphemes_.clear();
    graphemes_.push_back(0);
    const auto size = static_cast<std::uint32_t>(text_.size());
    // Below U+0300 there are no combining marks or surrogates: every unit is a grapheme except
    // CR LF.
    if (std::ranges::all_of(text_, [](char16_t c) { return c < 0x0300; })) {
        for (std::uint32_t i = 1; i <= size; ++i) {
            if (i < size && text_[i - 1] == u'\r' && text_[i] == u'\n') {
                continue;
            }
            graphemes_.push_back(i);
        }
        return;
    }
    UBreakIterator *iterator = state_.characterBreaks.get();
    UErrorCode status = U_ZERO_ERROR;
    if (iterator != nullptr) {
        ubrk_setText(iterator, text_.data(), static_cast<std::int32_t>(size), &status);
    }
    if (iterator == nullptr || U_FAILURE(status)) {
        for (std::uint32_t i = 1; i <= size; ++i) {
            graphemes_.push_back(i);
        }
        return;
    }
    ubrk_first(iterator);
    for (std::int32_t boundary = ubrk_next(iterator); boundary != UBRK_DONE;
         boundary = ubrk_next(iterator)) {
        graphemes_.push_back(static_cast<std::uint32_t>(boundary));
    }
    if (graphemes_.back() != size) {
        graphemes_.push_back(size);
    }
}

void Layout::resolveBidi() {
    levels_.clear();
    paragraphLevel_ = 0;
    if (std::ranges::none_of(text_, mayNeedBidi)) {
        return;
    }
    UBiDi *bidi = state_.bidi.get();
    if (bidi == nullptr) {
        return;
    }
    UErrorCode status = U_ZERO_ERROR;
    ubidi_setPara(bidi, text_.data(), static_cast<std::int32_t>(text_.size()), UBIDI_DEFAULT_LTR,
                  nullptr, &status);
    if (U_FAILURE(status)) {
        return;
    }
    paragraphLevel_ = ubidi_getParaLevel(bidi);
    const UBiDiLevel *levels = ubidi_getLevels(bidi, &status);
    if (U_FAILURE(status) || levels == nullptr) {
        paragraphLevel_ = 0;
        return;
    }
    levels_.assign(levels, levels + text_.size());
}

void Layout::shapeItem(Item &item, std::u16string_view text) {
    hb_buffer_t *buffer = state_.buffer;
    hb_buffer_clear_contents(buffer);
    hb_buffer_add_utf16(buffer, reinterpret_cast<const std::uint16_t *>(text.data()),
                        static_cast<int>(text.size()), item.start,
                        static_cast<int>(item.end - item.start));
    hb_buffer_set_direction(buffer, (item.level & 1) != 0 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_set_script(buffer, item.script);
    hb_buffer_set_language(buffer, language_);
    Instance &instance = registry_.instance(item.faceKey);
    static const hb_feature_t kNoLigatures[] = {
            {HB_TAG('l', 'i', 'g', 'a'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END},
            {HB_TAG('c', 'l', 'i', 'g'), 0, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END},
    };
    const float letterSpacing = spacing();
    const bool noLigatures = letterSpacing != 0;
    hb_shape(instance.hbFont, buffer, noLigatures ? kNoLigatures : nullptr, noLigatures ? 2 : 0);

    unsigned count = 0;
    const hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buffer, &count);
    const hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, nullptr);
    const float scale = style_.size / kHbScale;
    item.firstGlyph = static_cast<std::uint32_t>(glyphs_.size());
    item.glyphCount = count;
    item.color = instance.color;
    item.ascent = instance.ascent * style_.size;
    item.descent = instance.descent * style_.size;
    for (unsigned i = 0; i < count; ++i) {
        Glyph glyph;
        glyph.id = infos[i].codepoint;
        glyph.cluster = infos[i].cluster;
        glyph.advance = static_cast<float>(positions[i].x_advance) * scale;
        glyph.dx = static_cast<float>(positions[i].x_offset) * scale;
        glyph.dy = static_cast<float>(positions[i].y_offset) * scale;
        // CSS letter-spacing: after every cluster, here on its last glyph in buffer order.
        if (letterSpacing != 0 && (i + 1 == count || infos[i + 1].cluster != infos[i].cluster)) {
            glyph.advance += letterSpacing;
        }
        glyphs_.push_back(glyph);
    }
}

void Layout::itemize() {
    items_.clear();
    glyphs_.clear();
    const FontFamily family = style_.family;
    hb_unicode_funcs_t *unicode = hb_unicode_funcs_get_default();

    // Shaping sees control characters as invisible (default-ignorable) or as spaces, so fonts
    // without glyphs for them don't draw boxes. Line breaking still sees the originals.
    std::u16string shapingText = text_;
    for (char16_t &c : shapingText) {
        if (c == u'\t') {
            c = u' ';
        } else if (isControl(c)) {
            c = 0x200B;
        }
    }

    // Scripts per grapheme: punctuation, spaces and marks (Common, Inherited) take the script
    // before them, or the first real script if they lead.
    const std::size_t graphemeCount = graphemes_.size() - 1;
    std::vector<hb_script_t> scripts(graphemeCount, HB_SCRIPT_COMMON);
    hb_script_t lastScript = HB_SCRIPT_COMMON;
    std::size_t firstStrong = graphemeCount;
    for (std::size_t g = 0; g < graphemeCount; ++g) {
        std::size_t index = graphemes_[g];
        const hb_script_t script = hb_unicode_script(unicode, codePointAt(text_, index));
        if (script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED ||
            script == HB_SCRIPT_UNKNOWN) {
            scripts[g] = lastScript;
        } else {
            scripts[g] = lastScript = script;
            firstStrong = std::min(firstStrong, g);
        }
    }
    for (std::size_t g = 0; g < firstStrong && firstStrong < graphemeCount; ++g) {
        scripts[g] = scripts[firstStrong];
    }

    const bool fallbackAllowed = family != FontFamily::icons;
    const int weight = static_cast<int>(std::lround(style_.weight));
    for (std::size_t g = 0; g < graphemeCount; ++g) {
        const std::uint32_t start = graphemes_[g];
        const std::uint32_t end = graphemes_[g + 1];
        const std::u16string_view cluster(text_.data() + start, end - start);

        int face = primaryFace_;
        const bool wantsEmoji = cluster.find(u'\uFE0F') != std::u16string_view::npos;
        if (fallbackAllowed && (wantsEmoji || !registry_.covers(primaryFace_, cluster))) {
            const int fallback = registry_.fallbackFace(cluster, weight);
            if (fallback != FontRegistry::kNoFace) {
                face = fallback;
            }
        }
        const hb_script_t script = scripts[g];
        const std::uint8_t level = levels_.empty() ? 0 : levels_[start];
        if (!items_.empty()) {
            Item &last = items_.back();
            if (last.face == face && last.level == level && last.script == script) {
                last.end = end;
                continue;
            }
        }
        Item item;
        item.start = start;
        item.end = end;
        item.level = level;
        item.script = script;
        item.face = face;
        items_.push_back(item);
    }

    for (Item &item : items_) {
        if (item.face == FontRegistry::kNoFace) {
            continue;
        }
        item.faceKey =
                item.face == primaryFace_ ? primaryKey_ : registry_.instanceKey(item.face, style_);
        shapeItem(item, shapingText);
    }
    expandTabs();

    prefix_.assign(text_.size() + 1, 0.0f);
    for (const Glyph &glyph : glyphs_) {
        prefix_[glyph.cluster + 1] += glyph.advance;
    }
    for (std::size_t i = 1; i < prefix_.size(); ++i) {
        prefix_[i] += prefix_[i - 1];
    }
}

// Tabs, shaped as spaces, advance to the next stop: every four spaces from the start of their
// paragraph. A wrapped line keeps its paragraph's stops rather than starting its own.
void Layout::expandTabs() {
    if (text_.find(u'\t') == std::u16string::npos) {
        return;
    }
    std::vector<float> advances(text_.size(), 0.0f);
    std::vector<Glyph *> tabs(text_.size(), nullptr);
    for (Glyph &glyph : glyphs_) {
        if (glyph.cluster < text_.size()) {
            advances[glyph.cluster] += glyph.advance;
            if (text_[glyph.cluster] == u'\t') {
                tabs[glyph.cluster] = &glyph;
            }
        }
    }
    float x = 0;
    for (std::size_t i = 0; i < text_.size(); ++i) {
        if (isHardBreak(text_[i])) {
            x = 0;
            continue;
        }
        if (Glyph *tab = tabs[i]; tab != nullptr && tab->advance > 0) {
            const float stop = 4 * tab->advance;
            tab->advance = (std::floor(x / stop + 1e-3f) + 1) * stop - x;
            advances[i] = tab->advance;
        }
        x += advances[i];
    }
}

void Layout::shapeEllipsis(std::uint8_t level) {
    if (ellipsisItem_ >= 0) {
        return;
    }
    int face = primaryFace_;
    const std::u16string_view ellipsis(&kEllipsis, 1);
    if (!registry_.covers(face, ellipsis)) {
        face = registry_.primaryFace(FontFamily::sans);
    }
    if (face == FontRegistry::kNoFace) {
        return;
    }
    Item item;
    item.level = level;
    item.script = HB_SCRIPT_COMMON;
    item.face = face;
    item.faceKey = face == primaryFace_ ? primaryKey_ : registry_.instanceKey(face, style_);
    item.ellipsis = true;
    item.start = 0;
    item.end = 1;
    shapeItem(item, ellipsis);
    // The ellipsis isn't part of the text; its glyphs belong to no cluster.
    for (std::uint32_t i = 0; i < item.glyphCount; ++i) {
        glyphs_[item.firstGlyph + i].cluster = std::numeric_limits<std::uint32_t>::max();
    }
    item.start = item.end = 0;
    ellipsisItem_ = static_cast<int>(items_.size());
    items_.push_back(item);
}

void Layout::breakLines() {
    lines_.clear();
    const auto size = static_cast<std::uint32_t>(text_.size());
    const float maxWidth = options_.maxWidth;
    const bool bounded = std::isfinite(maxWidth);
    const std::size_t maxLines = options_.maxLines > 0 ? static_cast<std::size_t>(options_.maxLines)
                                                       : std::numeric_limits<std::size_t>::max();
    const bool hasHardBreaks = std::ranges::any_of(text_, isHardBreak);

    if (!hasHardBreaks && (!bounded || trimmedWidth(0, size) <= maxWidth)) {
        lines_.push_back({0, size});
        return;
    }

    // Break opportunities, and whether each is mandatory.
    std::vector<std::pair<std::uint32_t, bool>> breaks;
    UBreakIterator *iterator = state_.lineBreaks.get();
    UErrorCode status = U_ZERO_ERROR;
    if (iterator != nullptr) {
        ubrk_setText(iterator, text_.data(), static_cast<std::int32_t>(size), &status);
    }
    if (iterator != nullptr && U_SUCCESS(status)) {
        ubrk_first(iterator);
        for (std::int32_t boundary = ubrk_next(iterator); boundary != UBRK_DONE;
             boundary = ubrk_next(iterator)) {
            const bool hard = ubrk_getRuleStatus(iterator) >= UBRK_LINE_HARD;
            breaks.emplace_back(static_cast<std::uint32_t>(boundary), hard);
        }
    }
    if (breaks.empty() || breaks.back().first != size) {
        breaks.emplace_back(size, false);
    }

    std::uint32_t lineStart = 0;
    std::uint32_t lastFit = 0;  // 0: no opportunity fits on this line yet.
    bool cutByLines = false;
    std::size_t b = 0;
    while (b < breaks.size()) {
        if (lines_.size() + 1 == maxLines) {
            // The last allowed line takes the rest; overflow handles what doesn't fit.
            break;
        }
        const auto [position, hard] = breaks[b];
        if (!bounded || trimmedWidth(lineStart, position) <= maxWidth) {
            if (hard) {
                lines_.push_back({lineStart, position});
                lineStart = position;
                lastFit = 0;
            } else {
                lastFit = position;
            }
            ++b;
            continue;
        }
        if (lastFit > lineStart) {
            lines_.push_back({lineStart, lastFit});
            lineStart = lastFit;
            lastFit = 0;
            continue;  // Reconsider this opportunity on the new line.
        }
        // One word is wider than the line: break it between graphemes.
        auto g = std::upper_bound(graphemes_.begin(), graphemes_.end(), lineStart);
        std::uint32_t cut = *g;  // At least one grapheme per line.
        for (++g; g != graphemes_.end() && *g < position; ++g) {
            if (trimmedWidth(lineStart, *g) > maxWidth) {
                break;
            }
            cut = *g;
        }
        lines_.push_back({lineStart, cut});
        lineStart = cut;
        lastFit = 0;
    }

    if (lineStart < size || lines_.empty() ||
        (lines_.size() < maxLines && isHardBreak(text_[size - 1]))) {
        LineRange last{lineStart, size};
        // A hard break inside the last allowed line ends it, and the rest is cut.
        for (std::uint32_t i = lineStart; i < size; ++i) {
            if (isHardBreak(text_[i])) {
                const std::uint32_t after =
                        i + 1 < size && text_[i] == u'\r' && text_[i + 1] == u'\n' ? i + 2 : i + 1;
                if (after < size) {
                    last.end = after;
                    cutByLines = true;
                }
                break;
            }
        }
        lines_.push_back(last);
    }
    applyOverflow(lines_.back(), cutByLines);
}

void Layout::applyOverflow(LineRange &line, bool cutByLines) {
    const float maxWidth = options_.maxWidth;
    const bool bounded = std::isfinite(maxWidth);
    const auto size = static_cast<std::uint32_t>(text_.size());
    const bool overflows = bounded && trimmedWidth(line.start, line.end) > maxWidth;
    if (!overflows && !cutByLines) {
        return;
    }
    truncated_ = true;
    if (options_.overflow == TextOverflow::clip) {
        return;
    }
    shapeEllipsis(paragraphLevel_);
    if (ellipsisItem_ < 0) {
        return;
    }
    const float ellipsisWidth = [&] {
        const Item &item = items_[static_cast<std::size_t>(ellipsisItem_)];
        float total = 0;
        for (std::uint32_t i = 0; i < item.glyphCount; ++i) {
            total += glyphs_[item.firstGlyph + i].advance;
        }
        return total;
    }();
    const float available =
            bounded ? maxWidth - ellipsisWidth : std::numeric_limits<float>::infinity();
    line.ellipsis = true;

    if (options_.overflow == TextOverflow::ellipsisStart && !cutByLines) {
        // The ellipsis first, then as much of the end as fits.
        const std::uint32_t end = size;
        auto g = std::lower_bound(graphemes_.begin(), graphemes_.end(), end);
        std::uint32_t suffixStart = end;
        while (g != graphemes_.begin() && *(g - 1) > line.start) {
            const std::uint32_t candidate = *(g - 1);
            if (width(candidate, end) > available) {
                break;
            }
            suffixStart = candidate;
            --g;
        }
        line.end = end;
        line.keepEnd = line.start;
        line.resumeAt = suffixStart;
        return;
    }

    if (options_.overflow == TextOverflow::ellipsisMiddle && !cutByLines) {
        // Keep the end of the text, including a file's extension: the suffix gets half the room,
        // or what the extension needs if that's more, and the prefix the rest.
        const std::uint32_t end = size;
        std::uint32_t extension = end;
        for (std::uint32_t i = end; i > line.start + 1; --i) {
            if (text_[i - 1] == u'.') {
                extension = i - 1;
                break;
            }
            if (end - i > 16) {
                break;
            }
        }
        float suffixBudget = std::max(0.0f, available / 2);
        if (extension < end && width(extension, end) <= available * 0.75f) {
            suffixBudget = std::max(suffixBudget, width(extension, end));
        }
        auto g = std::lower_bound(graphemes_.begin(), graphemes_.end(), end);
        std::uint32_t suffixStart = end;
        while (g != graphemes_.begin() && *(g - 1) > line.start) {
            const std::uint32_t candidate = *(g - 1);
            if (width(candidate, end) > suffixBudget) {
                break;
            }
            suffixStart = candidate;
            --g;
        }
        std::uint32_t prefixEnd = line.start;
        for (auto p = std::upper_bound(graphemes_.begin(), graphemes_.end(), line.start);
             p != graphemes_.end() && *p <= suffixStart; ++p) {
            if (width(line.start, *p) + width(suffixStart, end) > available) {
                break;
            }
            prefixEnd = *p;
        }
        // Whatever room the prefix left goes back to the suffix.
        while (g != graphemes_.begin() && *(g - 1) >= prefixEnd && *(g - 1) > line.start) {
            const std::uint32_t candidate = *(g - 1);
            if (width(line.start, prefixEnd) + width(candidate, end) > available) {
                break;
            }
            suffixStart = candidate;
            --g;
        }
        line.end = end;
        line.keepEnd = trimEnd(line.start, prefixEnd);
        line.resumeAt = suffixStart;
        return;
    }

    std::uint32_t keep = line.start;
    for (auto g = std::upper_bound(graphemes_.begin(), graphemes_.end(), line.start);
         g != graphemes_.end() && *g <= line.end; ++g) {
        if (trimmedWidth(line.start, *g) > available) {
            break;
        }
        keep = *g;
    }
    // Text cut by maxLines ends at its hard break, which mustn't come before the ellipsis.
    keep = trimEnd(line.start, keep);
    line.keepEnd = keep;
    line.resumeAt = line.end;
}

void Layout::build(ShapedText &out) {
    const float size = style_.size;
    const bool rtlParagraph = (paragraphLevel_ & 1) != 0;
    std::vector<Segment> segments;
    std::vector<std::uint8_t> segmentLevels;
    std::vector<std::int32_t> visualOrder;
    // Per UTF-16 index: the x extent of the HarfBuzz cluster starting there, on the current line.
    std::vector<float> clusterLeft(text_.size(), std::numeric_limits<float>::quiet_NaN());
    std::vector<float> clusterRight(text_.size(), 0.0f);

    struct LineGlyphs {
        std::size_t firstGlyph;
        std::size_t firstCluster;
        float width;
        float hang;
    };
    std::vector<LineGlyphs> lineGlyphs;

    float top = 0;
    float widest = 0;
    for (std::size_t lineIndex = 0; lineIndex < lines_.size(); ++lineIndex) {
        const LineRange &line = lines_[lineIndex];
        // Logical segments: items clipped to what the line shows, with the ellipsis between.
        segments.clear();
        const auto addRange = [&](std::uint32_t start, std::uint32_t end) {
            for (std::uint32_t i = 0; i < items_.size(); ++i) {
                const Item &item = items_[i];
                if (item.ellipsis || item.end <= start || item.start >= end) {
                    continue;
                }
                segments.push_back(
                        {i, std::max(start, item.start), std::min(end, item.end), item.level});
            }
        };
        if (line.ellipsis) {
            addRange(line.start, line.keepEnd);
            segments.push_back({static_cast<std::uint32_t>(ellipsisItem_), 0, 0, paragraphLevel_});
            addRange(line.resumeAt, line.end);
        } else {
            addRange(line.start, line.end);
        }

        // Visual order.
        visualOrder.resize(segments.size());
        segmentLevels.clear();
        bool mixed = false;
        for (const Segment &segment : segments) {
            segmentLevels.push_back(segment.level);
            mixed = mixed || segment.level != 0;
        }
        if (mixed && !segments.empty()) {
            ubidi_reorderVisual(segmentLevels.data(), static_cast<std::int32_t>(segments.size()),
                                visualOrder.data());
        } else {
            for (std::size_t i = 0; i < segments.size(); ++i) {
                visualOrder[i] = static_cast<std::int32_t>(i);
            }
        }

        // Vertical metrics.
        float ascent = primaryAscent_;
        float descent = primaryDescent_;
        for (const Segment &segment : segments) {
            const Item &item = items_[segment.item];
            ascent = std::max(ascent, item.ascent);
            descent = std::max(descent, item.descent);
        }
        float height = 0;
        float baseline = 0;
        if (style_.lineHeight > 0) {
            height = style_.lineHeight * size;
            baseline = top + (height - (primaryAscent_ + primaryDescent_)) / 2 + primaryAscent_;
        } else {
            height = ascent + descent + primaryLineGap_;
            baseline = top + primaryLineGap_ / 2 + ascent;
        }

        // Glyphs, left to right.
        LineGlyphs placed{out.glyphs.size(), out.clusters.size(), 0, 0};
        float x = 0;
        for (const std::int32_t v : visualOrder) {
            const Segment &segment = segments[static_cast<std::size_t>(v)];
            const Item &item = items_[segment.item];
            const auto faceKey = item.faceKey;
            const std::size_t runStart = out.glyphs.size();
            for (std::uint32_t i = 0; i < item.glyphCount; ++i) {
                const Glyph &glyph = glyphs_[item.firstGlyph + i];
                if (!item.ellipsis &&
                    (glyph.cluster < segment.start || glyph.cluster >= segment.end)) {
                    continue;
                }
                out.glyphs.push_back({glyph.id, {x + glyph.dx, baseline - glyph.dy}});
                if (!item.ellipsis) {
                    float &left = clusterLeft[glyph.cluster];
                    float &right = clusterRight[glyph.cluster];
                    if (std::isnan(left)) {
                        left = x;
                        right = x + glyph.advance;
                    } else {
                        left = std::min(left, x);
                        right = std::max(right, x + glyph.advance);
                    }
                }
                x += glyph.advance;
            }
            const auto count = static_cast<std::uint32_t>(out.glyphs.size() - runStart);
            if (count == 0) {
                continue;
            }
            if (!out.runs.empty()) {
                gfx::GlyphRun &previous = out.runs.back();
                if (previous.face == faceKey &&
                    previous.firstGlyph + previous.glyphCount == runStart) {
                    previous.glyphCount += count;
                    continue;
                }
            }
            out.runs.push_back(
                    {faceKey, size, item.color, static_cast<std::uint32_t>(runStart), count});
        }
        // Trailing spaces hang past the line's end and don't count towards its width.
        placed.hang = line.ellipsis ? 0 : width(trimEnd(line.start, line.end), line.end);
        placed.width = std::max(0.0f, x - placed.hang);
        widest = std::max(widest, placed.width);

        // Grapheme clusters, in logical order. A HarfBuzz cluster spanning several graphemes (a
        // ligature) is shared among them evenly.
        const auto addClusters = [&](std::uint32_t start, std::uint32_t end) {
            auto g = std::lower_bound(graphemes_.begin(), graphemes_.end(), start);
            while (g != graphemes_.end() && *g < end) {
                // A group: graphemes up to the next one that starts a HarfBuzz cluster.
                auto groupEnd = g + 1;
                while (groupEnd != graphemes_.end() && *groupEnd < end &&
                       std::isnan(clusterLeft[*groupEnd])) {
                    ++groupEnd;
                }
                const std::uint32_t first = *g;
                float left = clusterLeft[first];
                float right = clusterRight[first];
                if (std::isnan(left)) {
                    left = right = x;
                }
                const auto count = static_cast<float>(groupEnd - g);
                const bool rtl = !levels_.empty() && (levels_[first] & 1) != 0;
                const float share = (right - left) / count;
                for (auto k = g; k != groupEnd; ++k) {
                    const auto n = static_cast<float>(k - g);
                    const float clusterX = rtl ? right - (n + 1) * share : left + n * share;
                    const std::uint32_t byte = byteOf_[*k];
                    if (out.clusters.size() > placed.firstCluster &&
                        out.clusters.back().byte == byte) {
                        // Uppercasing made several graphemes from one: they're one cluster.
                        gfx::TextCluster &previous = out.clusters.back();
                        const float l = std::min(previous.x, clusterX);
                        const float r = std::max(previous.x + previous.width, clusterX + share);
                        previous.x = l;
                        previous.width = r - l;
                        continue;
                    }
                    out.clusters.push_back(
                            {byte, static_cast<std::uint32_t>(lineIndex), clusterX, share, rtl});
                }
                g = groupEnd;
            }
        };
        if (line.ellipsis) {
            addClusters(line.start, line.keepEnd);
            addClusters(line.resumeAt, line.end);
        } else {
            addClusters(line.start, line.end);
        }
        // Reset the extents this line used.
        std::fill(clusterLeft.begin() + line.start, clusterLeft.begin() + line.end,
                  std::numeric_limits<float>::quiet_NaN());

        out.lines.push_back(
                {top, baseline, height, placed.width, byteOf_[line.start], byteOf_[line.end]});
        lineGlyphs.push_back(placed);
        top += height;
    }

    // Alignment.
    const bool bounded = std::isfinite(options_.maxWidth);
    const float box = bounded ? options_.maxWidth : widest;
    // Whether lines are placed against anything but the left edge.
    const bool moves = rtlParagraph || options_.align != TextAlign::start;
    for (std::size_t lineIndex = 0; lineIndex < out.lines.size(); ++lineIndex) {
        const LineGlyphs &placed = lineGlyphs[lineIndex];
        float offset = 0;
        const bool alignRight = (options_.align == TextAlign::end) != rtlParagraph &&
                                options_.align != TextAlign::center;
        if (options_.align == TextAlign::center) {
            offset = (box - placed.width) / 2;
        } else if (alignRight) {
            offset = box - placed.width;
        }
        if (rtlParagraph) {
            offset -= placed.hang;  // In a right-to-left line, trailing spaces hang on the left.
        }
        if (offset == 0) {
            continue;
        }
        const std::size_t glyphEnd = lineIndex + 1 < lineGlyphs.size()
                                             ? lineGlyphs[lineIndex + 1].firstGlyph
                                             : out.glyphs.size();
        for (std::size_t i = placed.firstGlyph; i < glyphEnd; ++i) {
            out.glyphs[i].position.x += offset;
        }
        const std::size_t clusterEnd = lineIndex + 1 < lineGlyphs.size()
                                               ? lineGlyphs[lineIndex + 1].firstCluster
                                               : out.clusters.size();
        for (std::size_t i = placed.firstCluster; i < clusterEnd; ++i) {
            out.clusters[i].x += offset;
        }
    }
    out.size = {moves && bounded ? options_.maxWidth : widest, top};
    out.truncated = truncated_;
}

ShapedText Layout::run() {
    ShapedText out;
    primaryFace_ = registry_.primaryFace(style_.family);
    if (primaryFace_ == FontRegistry::kNoFace || !(style_.size > 0)) {
        return out;
    }
    primaryKey_ = registry_.instanceKey(primaryFace_, style_);
    const Instance &primary = registry_.instance(primaryKey_);
    primaryAscent_ = primary.ascent * style_.size;
    primaryDescent_ = primary.descent * style_.size;
    primaryLineGap_ = primary.lineGap * style_.size;
    state_.prepare(locale_);

    decode();
    findGraphemes();
    if (style_.uppercase && style_.family != FontFamily::icons) {
        uppercase();
        findGraphemes();
    }
    resolveBidi();
    itemize();
    breakLines();
    build(out);
    return out;
}

// --- The text system -------------------------------------------------------------------------

class TextSystemImpl final : public gfx::TextSystem {
public:
    TextSystemImpl(std::unique_ptr<FontProvider> provider, TextSystemOptions options)
        : locale_(std::move(options.locale)),
          language_(hb_language_from_string(locale_.c_str(), -1)),
          registry_(std::move(provider), locale_),
          cache_(options.shapeCacheEntries) {}

    ShapedText shape(std::string_view utf8, const TextStyle &style,
                     const TextLayoutOptions &options) override {
        CacheKey key{std::string(utf8), style, options};
        ShapedText result;
        if (cache_.find(key, result)) {
            return result;
        }
        result = Layout(registry_, locale_, language_, utf8, style, options).run();
        cache_.insert(std::move(key), result);
        return result;
    }

    bool rasterize(FaceKey face, std::uint32_t glyph, float size, float subpixelX,
                   gfx::GlyphBitmap &out) override {
        return registry_.rasterize(face, glyph, size, subpixelX, out);
    }

private:
    std::string locale_;
    hb_language_t language_;
    FontRegistry registry_;
    ShapeCache cache_;
};

}  // namespace

std::unique_ptr<gfx::TextSystem> createTextSystem(std::unique_ptr<FontProvider> fontProvider,
                                                  TextSystemOptions options) {
    return std::make_unique<TextSystemImpl>(std::move(fontProvider), std::move(options));
}

}  // namespace filepan::text
