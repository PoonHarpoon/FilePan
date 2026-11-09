// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>

namespace filepan {

// Binary units, with at most three digits: "512 B", "18 KiB", "2.3 MiB", "774 MiB", "1.1 GiB".
inline std::string formatSize(std::uint64_t bytes) {
    static constexpr const char *kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    if (bytes < 1000) {
        return std::to_string(bytes) + " B";
    }
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 999.5 && unit + 1 < std::size(kUnits)) {
        value /= 1024;
        ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, value < 9.95 ? "%.1f %s" : "%.0f %s", value, kUnits[unit]);
    return buffer;
}

// Progress through a total: "456 MiB / 1.1 GiB", or just the done part while the total is unknown.
inline std::string formatSizeOf(std::uint64_t done, std::uint64_t total) {
    if (total == 0) {
        return formatSize(done);
    }
    return formatSize(done < total ? done : total) + " / " + formatSize(total);
}

}  // namespace filepan
