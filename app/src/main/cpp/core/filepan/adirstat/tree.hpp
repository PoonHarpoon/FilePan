// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace filepan::adirstat {

using NodeId = std::uint32_t;
inline constexpr NodeId kNoNode = std::numeric_limits<NodeId>::max();

enum class NodeKind : std::uint8_t {
    kFile,
    kDirectory,
    kSymlink,  // Never followed; counts its own size only.
    kOther,  // Devices, sockets, FIFOs.
    kMountPoint,  // A directory on another file system, not descended.
};

enum NodeFlags : std::uint8_t {
    // A further name for a file already counted: shown, but its size counts once.
    kHardLinkDuplicate = 1 << 0,
    // Taken out by remove(); still in the arena so ids stay stable.
    kRemoved = 1 << 1,
};

// One file or directory. Sizes are in bytes; the aggregates include the node itself.
struct Node {
    NodeId parent = kNoNode;
    NodeId firstChild = kNoNode;
    NodeId nextSibling = kNoNode;
    std::uint32_t childCount = 0;
    std::uint32_t nameOffset = 0;
    std::uint16_t nameLength = 0;
    NodeKind kind = NodeKind::kFile;
    std::uint8_t flags = 0;
    std::uint8_t category = 0;  // A file_types Category, once assigned.
    int error = 0;  // errno if a directory couldn't be read.
    std::uint64_t ownSize = 0;  // st_size.
    std::uint64_t ownAllocated = 0;  // st_blocks * 512.
    std::int64_t modified = 0;  // mtime, seconds.

    std::uint64_t size = 0;
    std::uint64_t allocated = 0;
    std::uint32_t files = 0;  // Non-directories in the subtree.
    std::uint32_t dirs = 0;  // Directories in the subtree, not counting this one.
    std::uint32_t errors = 0;  // Unreadable directories in the subtree, this one included.
    std::int64_t newest = 0;  // The latest mtime of a non-directory in the subtree; 0 for none.
};

// The result of a disk usage scan: an arena of nodes, the root first. Children are linked in a
// list that finalize() sorts largest first. Not thread-safe; the scanner serializes its writes.
class Tree {
public:
    explicit Tree(std::string rootPath);

    [[nodiscard]] NodeId root() const { return 0; }
    [[nodiscard]] const std::string &rootPath() const { return rootPath_; }
    [[nodiscard]] std::size_t nodeCount() const { return nodes_.size(); }
    [[nodiscard]] const Node &operator[](NodeId id) const { return nodes_[id]; }
    [[nodiscard]] Node &mutableNode(NodeId id) { return nodes_[id]; }

    // Appends a child of parent; parent must already exist, so ids grow downwards.
    NodeId add(NodeId parent, std::string_view name, NodeKind kind);

    [[nodiscard]] std::string_view name(NodeId id) const;
    // The absolute path; the root's is rootPath().
    [[nodiscard]] std::string pathOf(NodeId id) const;
    [[nodiscard]] int depthOf(NodeId id) const;
    [[nodiscard]] bool isAncestorOf(NodeId ancestor, NodeId id, bool orSelf = true) const;
    // The children in list order, skipping removed ones.
    [[nodiscard]] std::vector<NodeId> children(NodeId id) const;

    // Sums the aggregates bottom-up and sorts every child list by size, largest first, then by
    // name. Run once after the scan.
    void finalize();

    // Takes a node and its subtree out of the tree, subtracting it from its ancestors, as after
    // deleting it. The root can't be removed.
    void remove(NodeId id);

private:
    std::string rootPath_;
    std::vector<Node> nodes_;
    std::string names_;
};

}  // namespace filepan::adirstat
