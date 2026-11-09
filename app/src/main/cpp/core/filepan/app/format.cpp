// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/format.hpp"

#include <array>
#include <cstdio>
#include <ctime>

#include "filepan/vfs/mime.hpp"

namespace filepan::app {

namespace {

namespace kit = ui::components;

constexpr std::array<const char *, 12> kMonths = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::optional<int> pinnedYear;

bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '~' || c == '/' || c == '-';
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

}  // namespace

std::string formatDate(vfs::Timestamp time) {
    const std::time_t seconds = static_cast<std::time_t>(time.seconds);
    std::tm local{};
    if (localtime_r(&seconds, &local) == nullptr) {
        return {};
    }
    return std::to_string(local.tm_mday) + " " + kMonths[static_cast<std::size_t>(local.tm_mon) % 12] + " " +
           std::to_string(local.tm_year + 1900);
}

int currentYear() {
    if (pinnedYear) {
        return *pinnedYear;
    }
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    return localtime_r(&now, &local) == nullptr ? 0 : local.tm_year + 1900;
}

void setCurrentYearForTesting(std::optional<int> year) { pinnedYear = year; }

std::string formatShortDate(vfs::Timestamp time) {
    const std::time_t seconds = static_cast<std::time_t>(time.seconds);
    std::tm local{};
    if (localtime_r(&seconds, &local) == nullptr) {
        return {};
    }
    std::string text = std::to_string(local.tm_mday) + " " + kMonths[static_cast<std::size_t>(local.tm_mon) % 12];
    if (local.tm_year + 1900 != currentYear()) {
        text += " " + std::to_string(local.tm_year + 1900);
    }
    return text;
}

std::string formatCount(std::uint64_t count) {
    return std::to_string(count) + (count == 1 ? " item" : " items");
}

std::string displayExtension(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size()) {
        return {};
    }
    std::string extension(name.substr(dot + 1));
    for (char &c : extension) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    // A long run after the last dot isn't an extension anyone reads as one.
    return extension.size() <= 8 ? extension : std::string();
}

std::string metaLine(const vfs::Entry &entry, std::optional<std::uint64_t> itemCount) {
    std::string meta;
    const auto add = [&meta](const std::string &part) {
        if (part.empty()) {
            return;
        }
        if (!meta.empty()) {
            meta += " · ";
        }
        meta += part;
    };
    if (entry.isDirectoryLike()) {
        if (itemCount) {
            add(formatCount(*itemCount));
        }
        if (entry.hasStat) {
            add(formatShortDate(entry.modifiedTime));
        }
        return meta;
    }
    if (entry.hasStat) {
        add(formatSize(entry.size));
        add(formatShortDate(entry.modifiedTime));
    }
    if (entry.brokenLink) {
        add("Broken link");
    }
    return meta;
}

std::string sizeColumn(const vfs::Entry &entry, std::optional<std::uint64_t> itemCount) {
    if (entry.isDirectoryLike()) {
        return itemCount ? formatCount(*itemCount) : std::string();
    }
    return entry.hasStat ? formatSize(entry.size) : std::string();
}

std::string spokenDescription(const vfs::Entry &entry, std::optional<std::uint64_t> itemCount) {
    std::string text = entry.name + ", " + kindName(entry);
    if (entry.isDirectoryLike()) {
        if (itemCount) {
            text += ", " + formatCount(*itemCount);
        }
    } else if (entry.hasStat) {
        text += ", " + formatSize(entry.size);
    }
    if (entry.hasStat) {
        text += ", modified " + formatDate(entry.modifiedTime);
    }
    return text;
}

kit::FileKind kitKind(const vfs::Entry &entry) {
    if (entry.isDirectoryLike()) {
        return kit::FileKind::folder;
    }
    switch (entry.mimeType().kind) {
    case vfs::FileKind::kDirectory:
        return kit::FileKind::folder;
    case vfs::FileKind::kImage:
        return kit::FileKind::image;
    case vfs::FileKind::kVideo:
        return kit::FileKind::video;
    case vfs::FileKind::kAudio:
        return kit::FileKind::audio;
    case vfs::FileKind::kArchive:
        return kit::FileKind::archive;
    case vfs::FileKind::kApk:
        return kit::FileKind::apk;
    case vfs::FileKind::kPdf:
        return kit::FileKind::pdf;
    case vfs::FileKind::kDocument:
    case vfs::FileKind::kSpreadsheet:
    case vfs::FileKind::kPresentation:
    case vfs::FileKind::kText:
        return kit::FileKind::document;
    case vfs::FileKind::kCode:
        return kit::FileKind::code;
    case vfs::FileKind::kFont:
    case vfs::FileKind::kOther:
        break;
    }
    return kit::FileKind::other;
}

std::string kindName(const vfs::Entry &entry) {
    if (entry.isDirectoryLike()) {
        return "Folder";
    }
    switch (entry.mimeType().kind) {
    case vfs::FileKind::kImage:
        return "Image";
    case vfs::FileKind::kVideo:
        return "Video";
    case vfs::FileKind::kAudio:
        return "Audio";
    case vfs::FileKind::kArchive:
        return "Archive";
    case vfs::FileKind::kApk:
        return "Android app";
    case vfs::FileKind::kPdf:
        return "PDF document";
    case vfs::FileKind::kDocument:
        return "Document";
    case vfs::FileKind::kSpreadsheet:
        return "Spreadsheet";
    case vfs::FileKind::kPresentation:
        return "Presentation";
    case vfs::FileKind::kText:
        return "Text";
    case vfs::FileKind::kCode:
        return "Source code";
    case vfs::FileKind::kFont:
        return "Font";
    case vfs::FileKind::kDirectory:
    case vfs::FileKind::kOther:
        break;
    }
    return "File";
}

std::string percentEncode(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size());
    for (const char c : bytes) {
        const auto u = static_cast<unsigned char>(c);
        if (unreserved(u)) {
            out += c;
        } else {
            out += '%';
            out += kHex[u >> 4];
            out += kHex[u & 15];
        }
    }
    return out;
}

std::optional<std::string> percentDecode(std::string_view token) {
    std::string out;
    out.reserve(token.size());
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] != '%') {
            out += token[i];
            continue;
        }
        if (i + 2 >= token.size()) {
            return std::nullopt;
        }
        const int high = hexValue(token[i + 1]);
        const int low = hexValue(token[i + 2]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out += static_cast<char>(high << 4 | low);
        i += 2;
    }
    return out;
}

// Each kind is a letter and '|'-separated, percent-encoded fields: "L|/storage/emulated/0",
// "S|nas|/home", "D|<tree id>|/Music", "A|<encoded archive file location>|/inner".
std::string encodeLocation(const vfs::Location &location) {
    switch (location.kind()) {
    case vfs::BackendKind::kLocal:
        return "L|" + percentEncode(location.path());
    case vfs::BackendKind::kSftp:
        return "S|" + percentEncode(location.serverId()) + "|" + percentEncode(location.path());
    case vfs::BackendKind::kDocument:
        return "D|" + percentEncode(location.serverId()) + "|" + percentEncode(location.path());
    case vfs::BackendKind::kArchive:
        return "A|" + percentEncode(encodeLocation(*location.archiveFile())) + "|" + percentEncode(location.path());
    }
    return {};
}

std::optional<vfs::Location> decodeLocation(std::string_view text) {
    if (text.size() < 2 || text[1] != '|') {
        return std::nullopt;
    }
    const char kind = text[0];
    const std::string_view rest = text.substr(2);
    const std::size_t bar = rest.find('|');
    if (kind == 'L') {
        auto path = percentDecode(rest);
        if (!path || path->empty() || (*path)[0] != '/') {
            return std::nullopt;
        }
        return vfs::Location::local(*path);
    }
    if (bar == std::string_view::npos) {
        return std::nullopt;
    }
    auto first = percentDecode(rest.substr(0, bar));
    auto path = percentDecode(rest.substr(bar + 1));
    if (!first || !path || path->empty() || (*path)[0] != '/') {
        return std::nullopt;
    }
    if (kind == 'S') {
        return vfs::Location::sftp(*first, *path);
    }
    if (kind == 'D') {
        return vfs::Location::document(*first, *path);
    }
    if (kind == 'A') {
        auto file = decodeLocation(*first);
        if (!file) {
            return std::nullopt;
        }
        return vfs::Location::archive(*file, *path);
    }
    return std::nullopt;
}

}  // namespace filepan::app
