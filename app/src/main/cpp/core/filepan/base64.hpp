// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace filepan {

// Standard base64 with padding.
std::string base64Encode(std::span<const std::uint8_t> bytes);
std::optional<std::vector<std::uint8_t>> base64Decode(std::string_view text);

}  // namespace filepan
