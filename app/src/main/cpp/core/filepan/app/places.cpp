// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/places.hpp"

namespace filepan::app {

namespace {

// The folders below base in path ("/a/b/c" below "/a" is {"b", "c"}).
std::vector<std::string> componentsBelow(const std::string &path, const std::string &base) {
    std::vector<std::string> out;
    std::size_t start = base == "/" ? 1 : base.size() + 1;
    while (start < path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string::npos ? path.size() : slash;
        if (end > start) {
            out.push_back(path.substr(start, end - start));
        }
        start = end + 1;
    }
    return out;
}

// Crumbs for each folder below base, each targeting itself.
void appendBelow(std::vector<Crumb> &crumbs, const vfs::Location &location, const std::string &base,
                 const std::function<vfs::Location(const std::string &)> &make) {
    std::string path = base == "/" ? std::string() : base;
    for (const std::string &name : componentsBelow(location.path(), base)) {
        path += "/" + name;
        crumbs.push_back({name, make(path)});
    }
}

}  // namespace

std::string volumeLabel(const runtime::StorageVolume &volume) {
    if (volume.primary) {
        return "Internal";
    }
    return volume.description.empty() ? std::string("Storage") : volume.description;
}

const runtime::StorageVolume *volumeFor(const std::string &path, const std::vector<runtime::StorageVolume> &volumes) {
    const runtime::StorageVolume *best = nullptr;
    for (const auto &volume : volumes) {
        if (volume.path.empty()) {
            continue;
        }
        const bool inside = path == volume.path ||
                            (path.size() > volume.path.size() && path.starts_with(volume.path) &&
                             (volume.path == "/" || path[volume.path.size()] == '/'));
        if (inside && (best == nullptr || volume.path.size() > best->path.size())) {
            best = &volume;
        }
    }
    return best;
}

std::vector<Crumb> crumbsFor(const vfs::Location &location, const std::vector<runtime::StorageVolume> &volumes) {
    std::vector<Crumb> crumbs;
    switch (location.kind()) {
    case vfs::BackendKind::kLocal: {
        const runtime::StorageVolume *volume = volumeFor(location.path(), volumes);
        const std::string base = volume ? volume->path : std::string("/");
        crumbs.push_back({volume ? volumeLabel(*volume) : std::string("Device"), std::nullopt});
        appendBelow(crumbs, location, base, [](const std::string &path) { return vfs::Location::local(path); });
        break;
    }
    case vfs::BackendKind::kSftp: {
        crumbs.push_back({location.serverId(), std::nullopt});
        const std::string id = location.serverId();
        appendBelow(crumbs, location, "/", [id](const std::string &path) { return vfs::Location::sftp(id, path); });
        break;
    }
    case vfs::BackendKind::kDocument: {
        // A tree's id is its name.
        crumbs.push_back({location.serverId(), std::nullopt});
        const std::string id = location.serverId();
        appendBelow(crumbs, location, "/",
                    [id](const std::string &path) { return vfs::Location::document(id, path); });
        break;
    }
    case vfs::BackendKind::kArchive: {
        const vfs::Location &file = *location.archiveFile();
        crumbs = crumbsFor(file, volumes);
        // The archive's own crumb opens its root rather than the file.
        if (crumbs.size() > 1) {
            crumbs.back().target = vfs::Location::archive(file, "/");
        }
        appendBelow(crumbs, location, "/",
                    [file](const std::string &path) { return vfs::Location::archive(file, path); });
        break;
    }
    }
    return crumbs;
}

std::string locationLabel(const vfs::Location &location, const std::vector<runtime::StorageVolume> &volumes) {
    if (location.isLocal()) {
        if (const auto *volume = volumeFor(location.path(), volumes); volume && volume->path == location.path()) {
            return volumeLabel(*volume);
        }
        if (location.isRoot()) {
            return "Device";
        }
    }
    if (location.isRoot()) {
        if (location.kind() == vfs::BackendKind::kSftp || location.kind() == vfs::BackendKind::kDocument) {
            return location.serverId();
        }
        if (location.kind() == vfs::BackendKind::kArchive) {
            return std::string(location.archiveFile()->name());
        }
    }
    return std::string(location.name());
}

}  // namespace filepan::app
