// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/text/png_decoder.hpp"

#include <algorithm>

#ifdef __ANDROID__
#include <android/imagedecoder.h>
#endif

namespace filepan::text {

#ifdef __ANDROID__

bool decodePng(std::span<const std::byte> png, int width, int height,
               std::vector<std::uint8_t> &rgba) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    AImageDecoder *decoder = nullptr;
    if (AImageDecoder_createFromBuffer(png.data(), png.size(), &decoder) !=
        ANDROID_IMAGE_DECODER_SUCCESS) {
        return false;
    }
    bool ok = AImageDecoder_setAndroidBitmapFormat(decoder, ANDROID_BITMAP_FORMAT_RGBA_8888) ==
                      ANDROID_IMAGE_DECODER_SUCCESS &&
              AImageDecoder_setUnpremultipliedRequired(decoder, false) ==
                      ANDROID_IMAGE_DECODER_SUCCESS &&
              AImageDecoder_setTargetSize(decoder, width, height) == ANDROID_IMAGE_DECODER_SUCCESS;
    if (ok) {
        const std::size_t stride = AImageDecoder_getMinimumStride(decoder);
        std::vector<std::uint8_t> pixels(stride * static_cast<std::size_t>(height));
        ok = AImageDecoder_decodeImage(decoder, pixels.data(), stride, pixels.size()) ==
             ANDROID_IMAGE_DECODER_SUCCESS;
        if (ok) {
            const std::size_t row = static_cast<std::size_t>(width) * 4;
            rgba.resize(row * static_cast<std::size_t>(height));
            for (int y = 0; y < height; ++y) {
                const std::uint8_t *source = pixels.data() + stride * static_cast<std::size_t>(y);
                std::copy(source, source + row, rgba.data() + row * static_cast<std::size_t>(y));
            }
        }
    }
    AImageDecoder_delete(decoder);
    return ok;
}

#else

bool decodePng(std::span<const std::byte>, int, int, std::vector<std::uint8_t> &) {
    return false;
}

#endif

}  // namespace filepan::text
