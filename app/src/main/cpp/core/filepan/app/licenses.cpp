// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/licenses.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <map>
#include <optional>

namespace filepan::app {

namespace {

void appendUtf8(std::string &out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Entities decoded; nullopt for an unknown or unterminated one.
std::optional<std::string> decode(std::string_view text) {
    std::string out;
    while (!text.empty()) {
        const std::size_t amp = text.find('&');
        out.append(text.substr(0, amp));
        if (amp == std::string_view::npos) {
            break;
        }
        const std::size_t semi = text.find(';', amp);
        if (semi == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view name = text.substr(amp + 1, semi - amp - 1);
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name.size() > 1 && name[0] == '#') {
            const bool hex = name[1] == 'x' || name[1] == 'X';
            const std::string_view digits = name.substr(hex ? 2 : 1);
            std::uint32_t cp = 0;
            const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
            if (digits.empty() || error != std::errc() || end != digits.data() + digits.size() || cp > 0x10FFFF) {
                return std::nullopt;
            }
            appendUtf8(out, cp);
        } else {
            return std::nullopt;
        }
        text.remove_prefix(semi + 1);
    }
    return out;
}

// Collapses runs of white space, as the notices are written wrapped.
std::string collapse(std::string_view text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = !out.empty();
        } else {
            if (space) {
                out += ' ';
                space = false;
            }
            out += c;
        }
    }
    return out;
}

struct Tag {
    std::string name;
    std::map<std::string, std::string, std::less<>> attributes;
    bool closing = false;
    bool selfClosing = false;
};

// Parses the inside of <...>.
std::optional<Tag> parseTag(std::string_view body) {
    Tag tag;
    if (body.starts_with('/')) {
        tag.closing = true;
        body.remove_prefix(1);
    }
    if (body.ends_with('/')) {
        tag.selfClosing = true;
        body.remove_suffix(1);
    }
    std::size_t i = 0;
    while (i < body.size() && !std::isspace(static_cast<unsigned char>(body[i]))) {
        ++i;
    }
    tag.name = std::string(body.substr(0, i));
    if (tag.name.empty()) {
        return std::nullopt;
    }
    while (true) {
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i]))) {
            ++i;
        }
        if (i >= body.size()) {
            break;
        }
        const std::size_t eq = body.find('=', i);
        if (eq == std::string_view::npos || eq + 1 >= body.size() || body[eq + 1] != '"') {
            return std::nullopt;
        }
        std::string key(body.substr(i, eq - i));
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) {
            key.pop_back();
        }
        const std::size_t close = body.find('"', eq + 2);
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        auto value = decode(body.substr(eq + 2, close - eq - 2));
        if (!value || key.empty()) {
            return std::nullopt;
        }
        tag.attributes[key] = std::move(*value);
        i = close + 1;
    }
    return tag;
}

}  // namespace

std::string Licenses::fileFor(std::string_view license) const {
    for (const auto &text : texts) {
        if (text.license == license) {
            return text.file;
        }
    }
    return {};
}

std::vector<std::string> Licenses::missingTexts() const {
    std::vector<std::string> missing;
    for (const auto &notice : notices) {
        if (fileFor(notice.license).empty()) {
            missing.push_back(notice.name + ": " + (notice.license.empty() ? "(no license)" : notice.license));
        }
    }
    return missing;
}

Result<Licenses> parseLicenses(std::string_view xml) {
    Licenses out;
    std::vector<std::string> stack;
    std::string text;
    std::optional<Notice> notice;
    bool sawRoot = false;
    std::size_t at = 0;
    while (at < xml.size()) {
        const std::size_t open = xml.find('<', at);
        text.append(xml.substr(at, open == std::string_view::npos ? std::string_view::npos : open - at));
        if (open == std::string_view::npos) {
            break;
        }
        const std::string_view rest = xml.substr(open);
        if (rest.starts_with("<!--")) {
            const std::size_t end = xml.find("-->", open + 4);
            if (end == std::string_view::npos) {
                return fail(EINVAL, "unterminated comment");
            }
            at = end + 3;
            continue;
        }
        if (rest.starts_with("<?")) {
            const std::size_t end = xml.find("?>", open + 2);
            if (end == std::string_view::npos) {
                return fail(EINVAL, "unterminated declaration");
            }
            at = end + 2;
            continue;
        }
        const std::size_t close = xml.find('>', open);
        if (close == std::string_view::npos) {
            return fail(EINVAL, "unterminated tag");
        }
        auto tag = parseTag(xml.substr(open + 1, close - open - 1));
        if (!tag) {
            return fail(EINVAL, "malformed tag at " + std::to_string(open));
        }
        at = close + 1;
        if (tag->closing) {
            if (stack.empty() || stack.back() != tag->name) {
                return fail(EINVAL, "mismatched </" + tag->name + ">");
            }
            stack.pop_back();
            if (tag->name == "notice") {
                if (!notice || notice->name.empty()) {
                    return fail(EINVAL, "a notice without a name");
                }
                out.notices.push_back(std::move(*notice));
                notice.reset();
            } else if (notice && stack.size() == 2 && stack.back() == "notice") {
                auto value = decode(text);
                if (!value) {
                    return fail(EINVAL, "bad entity in <" + tag->name + ">");
                }
                std::string clean = collapse(*value);
                if (tag->name == "name") notice->name = std::move(clean);
                else if (tag->name == "url") notice->url = std::move(clean);
                else if (tag->name == "copyright") notice->copyright = std::move(clean);
                else if (tag->name == "license") notice->license = std::move(clean);
            }
            text.clear();
            continue;
        }
        if (stack.empty()) {
            if (tag->name != "notices" || sawRoot) {
                return fail(EINVAL, "not a notices file");
            }
            sawRoot = true;
        }
        text.clear();
        if (tag->name == "text" && stack.size() == 1) {
            const auto license = tag->attributes.find("license");
            const auto file = tag->attributes.find("file");
            if (license == tag->attributes.end() || file == tag->attributes.end() || file->second.empty() ||
                file->second.find('/') != std::string::npos) {
                return fail(EINVAL, "a <text> needs a license and a file name");
            }
            out.texts.push_back({license->second, file->second});
        }
        if (tag->name == "notice" && stack.size() == 1) {
            notice.emplace();
        }
        if (!tag->selfClosing) {
            stack.push_back(tag->name);
        }
    }
    if (!sawRoot || !stack.empty()) {
        return fail(EINVAL, sawRoot ? "unclosed elements" : "not a notices file");
    }
    return out;
}

}  // namespace filepan::app
