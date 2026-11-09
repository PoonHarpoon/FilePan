// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/securitykey/cbor.hpp"

#include <algorithm>
#include <cstddef>

namespace filepan::securitykey {

namespace {

constexpr int kMaxDepth = 16;

void writeHead(int majorType, std::uint64_t argument, Bytes &out) {
    const auto type = static_cast<std::uint8_t>(majorType << 5);
    int size = 0;
    if (argument < 24) {
        out.push_back(type | static_cast<std::uint8_t>(argument));
        return;
    }
    if (argument < 0x100) {
        out.push_back(type | 24);
        size = 1;
    } else if (argument < 0x10000) {
        out.push_back(type | 25);
        size = 2;
    } else if (argument < 0x100000000) {
        out.push_back(type | 26);
        size = 4;
    } else {
        out.push_back(type | 27);
        size = 8;
    }
    for (int shift = (size - 1) * 8; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(argument >> shift));
    }
}

// Canonical CBOR as in CTAP2: shorter encodings first, then bytewise.
bool encodedLess(const Bytes &a, const Bytes &b) {
    if (a.size() != b.size()) {
        return a.size() < b.size();
    }
    return a < b;
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    std::optional<CborValue> read(int depth) {
        if (depth > kMaxDepth) {
            return std::nullopt;
        }
        const auto initial = readByte();
        if (!initial) {
            return std::nullopt;
        }
        const int majorType = *initial >> 5;
        const int additional = *initial & 0x1F;
        if (majorType == 7) {
            if (additional == 20) {
                return CborValue(false);
            }
            if (additional == 21) {
                return CborValue(true);
            }
            return std::nullopt;
        }
        const auto argument = readArgument(additional);
        if (!argument) {
            return std::nullopt;
        }
        switch (majorType) {
        case 0:
            return CborValue(static_cast<std::int64_t>(*argument));
        case 1:
            return CborValue(-1 - static_cast<std::int64_t>(*argument));
        case 2: {
            auto bytes = readBytes(*argument);
            if (!bytes) {
                return std::nullopt;
            }
            return CborValue(std::move(*bytes));
        }
        case 3: {
            auto bytes = readBytes(*argument);
            if (!bytes) {
                return std::nullopt;
            }
            return CborValue(std::string(bytes->begin(), bytes->end()));
        }
        case 4: {
            if (!fits(*argument)) {
                return std::nullopt;
            }
            CborArray array;
            array.reserve(static_cast<std::size_t>(*argument));
            for (std::uint64_t i = 0; i < *argument; ++i) {
                auto element = read(depth + 1);
                if (!element) {
                    return std::nullopt;
                }
                array.push_back(std::move(*element));
            }
            return CborValue(std::move(array));
        }
        case 5: {
            if (!fits(*argument)) {
                return std::nullopt;
            }
            CborMap map;
            map.reserve(static_cast<std::size_t>(*argument));
            for (std::uint64_t i = 0; i < *argument; ++i) {
                auto key = read(depth + 1);
                if (!key) {
                    return std::nullopt;
                }
                auto value = read(depth + 1);
                if (!value) {
                    return std::nullopt;
                }
                map.emplace_back(std::move(*key), std::move(*value));
            }
            return CborValue(std::move(map));
        }
        default:
            return std::nullopt;
        }
    }

    [[nodiscard]] bool atEnd() const { return position_ == bytes_.size(); }

private:
    std::optional<std::uint8_t> readByte() {
        if (position_ >= bytes_.size()) {
            return std::nullopt;
        }
        return bytes_[position_++];
    }

    std::optional<std::uint64_t> readArgument(int additional) {
        int size = 0;
        if (additional < 24) {
            return additional;
        }
        switch (additional) {
        case 24:
            size = 1;
            break;
        case 25:
            size = 2;
            break;
        case 26:
            size = 4;
            break;
        case 27:
            size = 8;
            break;
        default:
            return std::nullopt;
        }
        std::uint64_t value = 0;
        for (int i = 0; i < size; ++i) {
            const auto byte = readByte();
            if (!byte) {
                return std::nullopt;
            }
            value = (value << 8) | *byte;
        }
        // Integers must fit an int64_t, as negative ones are -1 - value.
        if (value > static_cast<std::uint64_t>(INT64_MAX)) {
            return std::nullopt;
        }
        return value;
    }

    // Each element takes at least a byte.
    [[nodiscard]] bool fits(std::uint64_t count) const {
        return count <= bytes_.size() - position_;
    }

    std::optional<Bytes> readBytes(std::uint64_t length) {
        if (!fits(length)) {
            return std::nullopt;
        }
        const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(position_);
        Bytes bytes(begin, begin + static_cast<std::ptrdiff_t>(length));
        position_ += static_cast<std::size_t>(length);
        return bytes;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0;
};

}  // namespace

const CborValue *CborValue::find(std::int64_t key) const {
    if (const auto *entries = map()) {
        for (const auto &[entryKey, entryValue] : *entries) {
            if (const auto *integerKey = entryKey.integer(); integerKey && *integerKey == key) {
                return &entryValue;
            }
        }
    }
    return nullptr;
}

const CborValue *CborValue::find(std::string_view key) const {
    if (const auto *entries = map()) {
        for (const auto &[entryKey, entryValue] : *entries) {
            if (const auto *textKey = entryKey.text(); textKey && *textKey == key) {
                return &entryValue;
            }
        }
    }
    return nullptr;
}

void cborEncode(const CborValue &value, Bytes &out) {
    if (const auto *integer = value.integer()) {
        if (*integer >= 0) {
            writeHead(0, static_cast<std::uint64_t>(*integer), out);
        } else {
            writeHead(1, static_cast<std::uint64_t>(-1 - *integer), out);
        }
    } else if (const auto *bytes = value.bytes()) {
        writeHead(2, bytes->size(), out);
        out.insert(out.end(), bytes->begin(), bytes->end());
    } else if (const auto *text = value.text()) {
        writeHead(3, text->size(), out);
        out.insert(out.end(), text->begin(), text->end());
    } else if (const auto *array = value.array()) {
        writeHead(4, array->size(), out);
        for (const auto &element : *array) {
            cborEncode(element, out);
        }
    } else if (const auto *map = value.map()) {
        std::vector<std::pair<Bytes, const CborValue *>> entries;
        entries.reserve(map->size());
        for (const auto &[key, entryValue] : *map) {
            entries.emplace_back(cborEncode(key), &entryValue);
        }
        std::ranges::sort(entries, encodedLess, &std::pair<Bytes, const CborValue *>::first);
        writeHead(5, entries.size(), out);
        for (const auto &[key, entryValue] : entries) {
            out.insert(out.end(), key.begin(), key.end());
            cborEncode(*entryValue, out);
        }
    } else if (const auto *boolean = value.boolean()) {
        out.push_back(*boolean ? 0xF5 : 0xF4);
    }
}

Bytes cborEncode(const CborValue &value) {
    Bytes out;
    cborEncode(value, out);
    return out;
}

std::optional<CborValue> cborDecode(std::span<const std::uint8_t> bytes) {
    Reader reader(bytes);
    auto value = reader.read(0);
    if (!value || !reader.atEnd()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace filepan::securitykey
