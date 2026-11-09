// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "filepan/app/file_actions.hpp"
#include "filepan/runtime/app_host.hpp"

// The properties sheet: what a file or a selection is. Name, location, type and MIME type,
// size (counted recursively on the I/O pool for folders, live as it counts), the times,
// permissions and owner for local files, a symlink's target, and checksums computed by a job on
// request, each copied to the clipboard with a tap.
namespace filepan::app {

class Overlays;
class JobsModel;

// "rwxr-xr-x".
std::string formatPermissions(std::uint32_t mode);
// "12 Oct 2026, 18:44" in local time.
std::string formatDateTime(vfs::Timestamp time);
// Lowercase hex.
std::string hexDigest(const std::vector<std::uint8_t> &digest);

struct PropertiesHost {
    runtime::AppServices &services;
    Overlays &overlays;
    JobsModel &jobs;
    // The folder as the breadcrumb names it ("Internal / Download"); the path when empty.
    std::string locationLabel;
};

// Opens the sheet; the recursive count and any checksum job stop when it's dismissed.
void showProperties(PropertiesHost host, const FileTargets &targets);

}  // namespace filepan::app
