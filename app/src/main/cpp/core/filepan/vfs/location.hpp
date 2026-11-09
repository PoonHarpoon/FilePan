// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace filepan::vfs {

enum class BackendKind : std::uint8_t {
    kLocal,
    // Inside an archive file; the root is the archive's own Location, which may itself be in an
    // archive or on a server.
    kArchive,
    // On an SFTP server; the root is the server's id in the app's server list.
    kSftp,
    // In a document tree the user granted through the system's picker (the Storage Access
    // Framework); the root is the tree's id in the app's saved list.
    kDocument,
};

// Where a file lives: a backend, the backend's root, and an absolute path of raw bytes within it.
// Paths are '/'-separated with no trailing slash ("/" is the root), and names are bytes, not
// necessarily UTF-8. Immutable and cheap to copy except for the path string.
class Location {
public:
    // path must be absolute; it is normalized by dropping empty and "." components and trailing
    // slashes. ".." is kept as is: it is a valid name on some backends and resolving it needs the
    // file system.
    static Location local(std::string_view path);
    static Location archive(Location archiveFile, std::string_view pathInArchive = "/");
    static Location sftp(std::string_view serverId, std::string_view path = "/");
    static Location document(std::string_view treeId, std::string_view path = "/");

    [[nodiscard]] BackendKind kind() const { return kind_; }
    [[nodiscard]] bool isLocal() const { return kind_ == BackendKind::kLocal; }
    // The archive file, for kArchive; null otherwise.
    [[nodiscard]] const Location *archiveFile() const { return archiveFile_.get(); }
    // The server id, for kSftp, or the tree id, for kDocument; empty otherwise.
    [[nodiscard]] const std::string &serverId() const { return serverId_; }
    [[nodiscard]] const std::string &path() const { return path_; }

    [[nodiscard]] bool isRoot() const { return path_.size() == 1; }
    // The last path component; empty for the root.
    [[nodiscard]] std::string_view name() const;
    // nullopt for the root.
    [[nodiscard]] std::optional<Location> parent() const;
    // name must be a single non-empty component without '/'.
    [[nodiscard]] Location child(std::string_view name) const;
    // The same backend and root at another absolute path.
    [[nodiscard]] Location withPath(std::string_view path) const;
    // A sibling with another name; the root has none.
    [[nodiscard]] std::optional<Location> withName(std::string_view name) const;
    // Same backend and root, and other's path is this one or below it.
    [[nodiscard]] bool isAncestorOf(const Location &other, bool orSelf = true) const;
    // Same backend and root.
    [[nodiscard]] bool sameRoot(const Location &other) const;

    // For logs and tests, e.g. "archive:[local:/sdcard/a.zip]/dir/file".
    [[nodiscard]] std::string toString() const;

    [[nodiscard]] std::size_t hash() const;

    friend bool operator==(const Location &a, const Location &b);
    friend std::strong_ordering operator<=>(const Location &a, const Location &b);

private:
    Location(BackendKind kind, std::shared_ptr<const Location> archiveFile, std::string serverId,
             std::string path);

    BackendKind kind_;
    std::shared_ptr<const Location> archiveFile_;
    std::string serverId_;
    std::string path_;
};

}  // namespace filepan::vfs

template <>
struct std::hash<filepan::vfs::Location> {
    std::size_t operator()(const filepan::vfs::Location &location) const noexcept {
        return location.hash();
    }
};
