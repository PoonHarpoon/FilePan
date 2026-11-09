// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/base64.hpp"

namespace filepan {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int sextet(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::string base64Encode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const std::uint32_t v = bytes[i] << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const std::uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int s[4];
        int padding = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + static_cast<std::size_t>(k)];
            if (c == '=' && last && k >= 2) {
                s[k] = 0;
                ++padding;
                continue;
            }
            if (padding > 0) {
                return std::nullopt;  // Data after padding.
            }
            s[k] = sextet(c);
            if (s[k] < 0) {
                return std::nullopt;
            }
        }
        const std::uint32_t v = (static_cast<std::uint32_t>(s[0]) << 18) | (static_cast<std::uint32_t>(s[1]) << 12) |
                                (static_cast<std::uint32_t>(s[2]) << 6) | static_cast<std::uint32_t>(s[3]);
        out.push_back(static_cast<std::uint8_t>(v >> 16));
        if (padding < 2) {
            out.push_back(static_cast<std::uint8_t>(v >> 8));
        }
        if (padding < 1) {
            out.push_back(static_cast<std::uint8_t>(v));
        }
    }
    return out;
}

}  // namespace filepan
