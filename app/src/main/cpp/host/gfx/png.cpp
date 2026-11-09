// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "png.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>

#include <zlib.h>

namespace filepan::test {

namespace {

constexpr std::array<std::uint8_t, 8> kSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

void putU32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t getU32(const std::uint8_t *in) {
    return static_cast<std::uint32_t>(in[0]) << 24 | static_cast<std::uint32_t>(in[1]) << 16 |
           static_cast<std::uint32_t>(in[2]) << 8 | in[3];
}

void putChunk(std::vector<std::uint8_t> &out, std::string_view type,
              const std::vector<std::uint8_t> &data) {
    putU32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), data.begin(), data.end());
    putU32(out, static_cast<std::uint32_t>(
                        crc32(0, out.data() + start, static_cast<uInt>(out.size() - start))));
}

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

}  // namespace

bool writePng(const std::filesystem::path &path, const RgbaImage &image) {
    const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
    // Each row filtered with Paeth, which suits smooth renders.
    std::vector<std::uint8_t> raw;
    raw.reserve((stride + 1) * image.height);
    for (int y = 0; y < image.height; ++y) {
        raw.push_back(4);
        const std::uint8_t *row = image.pixels.data() + y * stride;
        const std::uint8_t *above = y > 0 ? row - stride : nullptr;
        for (std::size_t x = 0; x < stride; ++x) {
            const int a = x >= 4 ? row[x - 4] : 0;
            const int b = above != nullptr ? above[x] : 0;
            const int c = x >= 4 && above != nullptr ? above[x - 4] : 0;
            raw.push_back(static_cast<std::uint8_t>(row[x] - paeth(a, b, c)));
        }
    }
    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> compressed(compressedSize);
    if (compress2(compressed.data(), &compressedSize, raw.data(), static_cast<uLong>(raw.size()),
                  9) != Z_OK) {
        return false;
    }
    compressed.resize(compressedSize);

    std::vector<std::uint8_t> out(kSignature.begin(), kSignature.end());
    std::vector<std::uint8_t> header;
    putU32(header, static_cast<std::uint32_t>(image.width));
    putU32(header, static_cast<std::uint32_t>(image.height));
    header.insert(header.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, no interlacing.
    putChunk(out, "IHDR", header);
    putChunk(out, "IDAT", compressed);
    putChunk(out, "IEND", {});
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
    return file.good();
}

std::optional<RgbaImage> readPng(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());
    if (data.size() < kSignature.size() ||
        std::memcmp(data.data(), kSignature.data(), kSignature.size()) != 0) {
        return std::nullopt;
    }
    RgbaImage image;
    std::vector<std::uint8_t> compressed;
    std::size_t offset = kSignature.size();
    while (offset + 12 <= data.size()) {
        const std::uint32_t length = getU32(data.data() + offset);
        if (offset + 12 + length > data.size()) {
            return std::nullopt;
        }
        const std::string_view type(reinterpret_cast<const char *>(data.data() + offset + 4), 4);
        const std::uint8_t *body = data.data() + offset + 8;
        if (type == "IHDR") {
            if (length < 13 || body[8] != 8 || body[9] != 6 || body[12] != 0) {
                return std::nullopt;  // Only 8-bit RGBA without interlacing.
            }
            image.width = static_cast<int>(getU32(body));
            image.height = static_cast<int>(getU32(body + 4));
        } else if (type == "IDAT") {
            compressed.insert(compressed.end(), body, body + length);
        } else if (type == "IEND") {
            break;
        }
        offset += 12 + length;
    }
    if (image.width <= 0 || image.height <= 0) {
        return std::nullopt;
    }
    const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
    std::vector<std::uint8_t> raw((stride + 1) * image.height);
    uLongf rawSize = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &rawSize, compressed.data(), static_cast<uLong>(compressed.size())) !=
                Z_OK ||
        rawSize != raw.size()) {
        return std::nullopt;
    }
    image.pixels.resize(stride * image.height);
    for (int y = 0; y < image.height; ++y) {
        const std::uint8_t filter = raw[y * (stride + 1)];
        const std::uint8_t *in = raw.data() + y * (stride + 1) + 1;
        std::uint8_t *row = image.pixels.data() + y * stride;
        const std::uint8_t *above = y > 0 ? row - stride : nullptr;
        for (std::size_t x = 0; x < stride; ++x) {
            const int a = x >= 4 ? row[x - 4] : 0;
            const int b = above != nullptr ? above[x] : 0;
            const int c = x >= 4 && above != nullptr ? above[x - 4] : 0;
            int predicted = 0;
            switch (filter) {
                case 0:
                    break;
                case 1:
                    predicted = a;
                    break;
                case 2:
                    predicted = b;
                    break;
                case 3:
                    predicted = (a + b) / 2;
                    break;
                case 4:
                    predicted = paeth(a, b, c);
                    break;
                default:
                    return std::nullopt;
            }
            row[x] = static_cast<std::uint8_t>(in[x] + predicted);
        }
    }
    return image;
}

}  // namespace filepan::test
