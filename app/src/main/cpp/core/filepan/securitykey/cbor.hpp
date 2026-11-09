// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The subset of CBOR that CTAP2 uses: integers, byte and text strings, arrays, maps and
// booleans. Maps are encoded in CTAP2's canonical order.
namespace filepan::securitykey {

using Bytes = std::vector<std::uint8_t>;

struct CborValue;
using CborArray = std::vector<CborValue>;
// In the order read; encoding sorts it.
using CborMap = std::vector<std::pair<CborValue, CborValue>>;

struct CborValue {
    std::variant<std::int64_t, Bytes, std::string, CborArray, CborMap, bool> value;

    CborValue(std::int64_t integer) : value(integer) {}
    CborValue(int integer) : value(static_cast<std::int64_t>(integer)) {}
    CborValue(Bytes bytes) : value(std::move(bytes)) {}
    CborValue(std::string text) : value(std::move(text)) {}
    CborValue(const char *text) : value(std::string(text)) {}
    CborValue(CborArray array) : value(std::move(array)) {}
    CborValue(CborMap map) : value(std::move(map)) {}
    CborValue(bool boolean) : value(boolean) {}

    [[nodiscard]] const std::int64_t *integer() const { return std::get_if<std::int64_t>(&value); }
    [[nodiscard]] const Bytes *bytes() const { return std::get_if<Bytes>(&value); }
    [[nodiscard]] const std::string *text() const { return std::get_if<std::string>(&value); }
    [[nodiscard]] const CborArray *array() const { return std::get_if<CborArray>(&value); }
    [[nodiscard]] const CborMap *map() const { return std::get_if<CborMap>(&value); }
    [[nodiscard]] const bool *boolean() const { return std::get_if<bool>(&value); }

    // A map's value for an integer or text key, if this is a map that has it.
    [[nodiscard]] const CborValue *find(std::int64_t key) const;
    [[nodiscard]] const CborValue *find(std::string_view key) const;

    bool operator==(const CborValue &) const = default;
};

Bytes cborEncode(const CborValue &value);
// Appends to out.
void cborEncode(const CborValue &value, Bytes &out);

// nullopt for anything malformed, truncated, nested too deeply, unsupported or followed by
// trailing bytes.
std::optional<CborValue> cborDecode(std::span<const std::uint8_t> bytes);

}  // namespace filepan::securitykey
