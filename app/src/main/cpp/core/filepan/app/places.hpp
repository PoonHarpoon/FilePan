// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "filepan/runtime/platform.hpp"
#include "filepan/vfs/location.hpp"

// How locations are named on screen: breadcrumbs and tab labels, relative to the storage
// volumes ("Internal / Download / Papers" rather than "/storage/emulated/0/Download/Papers").
namespace filepan::app {

// "Internal" for the primary volume, the volume's own description for the others.
std::string volumeLabel(const runtime::StorageVolume &volume);
// The mounted volume holding a local path, if any.
const runtime::StorageVolume *volumeFor(const std::string &path, const std::vector<runtime::StorageVolume> &volumes);

struct Crumb {
    std::string label;
    // Where tapping it goes; nullopt for the root crumb, which goes to the storage home.
    std::optional<vfs::Location> target;

    bool operator==(const Crumb &) const = default;
};

// The breadcrumb from the root: the volume (or "Device", the server, the archive's own crumbs),
// then each folder, the location itself last.
std::vector<Crumb> crumbsFor(const vfs::Location &location, const std::vector<runtime::StorageVolume> &volumes);
// A short name for a tab or title: the folder's name, or its volume's at a volume root.
std::string locationLabel(const vfs::Location &location, const std::vector<runtime::StorageVolume> &volumes);

}  // namespace filepan::app
