// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace filepan::text {

// Decodes a PNG (a CBDT color emoji bitmap) scaled to width x height, into premultiplied RGBA8.
// Android decodes with AImageDecoder; the host has no PNG decoder and always returns false, so
// CBDT emoji only render on Android.
bool decodePng(std::span<const std::byte> png, int width, int height,
               std::vector<std::uint8_t> &rgba);

}  // namespace filepan::text
