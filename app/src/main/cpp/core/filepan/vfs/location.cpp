// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/vfs/location.hpp"

#include <utility>

namespace filepan::vfs {

namespace {

std::string normalize(std::string_view path) {
    std::string result;
    result.reserve(path.size() + 1);
    std::size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const auto component = path.substr(start, end - start);
        if (!component.empty() && component != ".") {
            result.push_back('/');
            result.append(component);
        }
        start = end + 1;
    }
    if (result.empty()) {
        result.push_back('/');
    }
    return result;
}

std::size_t mix(std::size_t seed, std::size_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

}  // namespace

Location::Location(BackendKind kind, std::shared_ptr<const Location> archiveFile,
                   std::string serverId, std::string path)
    : kind_(kind),
      archiveFile_(std::move(archiveFile)),
      serverId_(std::move(serverId)),
      path_(std::move(path)) {}

Location Location::local(std::string_view path) {
    return {BackendKind::kLocal, nullptr, {}, normalize(path)};
}

Location Location::archive(Location archiveFile, std::string_view pathInArchive) {
    return {BackendKind::kArchive, std::make_shared<const Location>(std::move(archiveFile)), {},
            normalize(pathInArchive)};
}

Location Location::sftp(std::string_view serverId, std::string_view path) {
    return {BackendKind::kSftp, nullptr, std::string(serverId), normalize(path)};
}

Location Location::document(std::string_view treeId, std::string_view path) {
    return {BackendKind::kDocument, nullptr, std::string(treeId), normalize(path)};
}

Location Location::withPath(std::string_view path) const { return {kind_, archiveFile_, serverId_, normalize(path)}; }

std::string_view Location::name() const {
    if (isRoot()) {
        return {};
    }
    return std::string_view(path_).substr(path_.rfind('/') + 1);
}

std::optional<Location> Location::parent() const {
    if (isRoot()) {
        return std::nullopt;
    }
    const auto slash = path_.rfind('/');
    return Location(kind_, archiveFile_, serverId_, slash == 0 ? "/" : path_.substr(0, slash));
}

Location Location::child(std::string_view name) const {
    std::string path;
    path.reserve(path_.size() + 1 + name.size());
    if (!isRoot()) {
        path.append(path_);
    }
    path.push_back('/');
    path.append(name);
    return {kind_, archiveFile_, serverId_, std::move(path)};
}

std::optional<Location> Location::withName(std::string_view name) const {
    auto parentLocation = parent();
    if (!parentLocation) {
        return std::nullopt;
    }
    return parentLocation->child(name);
}

bool Location::sameRoot(const Location &other) const {
    if (kind_ != other.kind_ || serverId_ != other.serverId_) {
        return false;
    }
    if (archiveFile_ == other.archiveFile_) {
        return true;
    }
    return archiveFile_ && other.archiveFile_ && *archiveFile_ == *other.archiveFile_;
}

bool Location::isAncestorOf(const Location &other, bool orSelf) const {
    if (!sameRoot(other)) {
        return false;
    }
    if (path_ == other.path_) {
        return orSelf;
    }
    if (isRoot()) {
        return true;
    }
    return other.path_.size() > path_.size() && other.path_.starts_with(path_)
           && other.path_[path_.size()] == '/';
}

std::string Location::toString() const {
    switch (kind_) {
        case BackendKind::kLocal:
            return "local:" + path_;
        case BackendKind::kArchive:
            return "archive:[" + archiveFile_->toString() + "]" + path_;
        case BackendKind::kSftp:
            return "sftp:" + serverId_ + ":" + path_;
        case BackendKind::kDocument:
            return "document:" + serverId_ + ":" + path_;
    }
    return path_;
}

std::size_t Location::hash() const {
    std::size_t seed = static_cast<std::size_t>(kind_);
    seed = mix(seed, std::hash<std::string>()(path_));
    if (!serverId_.empty()) {
        seed = mix(seed, std::hash<std::string>()(serverId_));
    }
    if (archiveFile_) {
        seed = mix(seed, archiveFile_->hash());
    }
    return seed;
}

bool operator==(const Location &a, const Location &b) {
    return a.path_ == b.path_ && a.sameRoot(b);
}

std::strong_ordering operator<=>(const Location &a, const Location &b) {
    if (const auto order = a.kind_ <=> b.kind_; order != 0) {
        return order;
    }
    if (a.archiveFile_ != b.archiveFile_) {
        // Both are archives here, so both are non-null.
        if (const auto order = *a.archiveFile_ <=> *b.archiveFile_; order != 0) {
            return order;
        }
    }
    if (const auto order = a.serverId_ <=> b.serverId_; order != 0) {
        return order;
    }
    return a.path_ <=> b.path_;
}

}  // namespace filepan::vfs
