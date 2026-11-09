// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/adirstat/tree.hpp"

#include <algorithm>
#include <utility>

namespace filepan::adirstat {

Tree::Tree(std::string rootPath) : rootPath_(std::move(rootPath)) {
    Node root;
    root.kind = NodeKind::kDirectory;
    nodes_.push_back(root);
}

NodeId Tree::add(NodeId parent, std::string_view name, NodeKind kind) {
    const auto id = static_cast<NodeId>(nodes_.size());
    Node node;
    node.parent = parent;
    node.kind = kind;
    node.nameOffset = static_cast<std::uint32_t>(names_.size());
    node.nameLength = static_cast<std::uint16_t>(std::min<std::size_t>(name.size(), UINT16_MAX));
    names_.append(name.substr(0, node.nameLength));
    Node &p = nodes_[parent];
    node.nextSibling = p.firstChild;
    p.firstChild = id;
    ++p.childCount;
    nodes_.push_back(node);
    return id;
}

std::string_view Tree::name(NodeId id) const {
    const Node &node = nodes_[id];
    return std::string_view(names_).substr(node.nameOffset, node.nameLength);
}

std::string Tree::pathOf(NodeId id) const {
    std::vector<NodeId> chain;
    for (NodeId at = id; at != root(); at = nodes_[at].parent) {
        chain.push_back(at);
    }
    std::string path = rootPath_;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (path.empty() || path.back() != '/') {
            path += '/';
        }
        path += name(*it);
    }
    return path;
}

int Tree::depthOf(NodeId id) const {
    int depth = 0;
    for (NodeId at = id; at != root(); at = nodes_[at].parent) {
        ++depth;
    }
    return depth;
}

bool Tree::isAncestorOf(NodeId ancestor, NodeId id, bool orSelf) const {
    if (!orSelf) {
        if (id == root()) {
            return false;
        }
        id = nodes_[id].parent;
    }
    // Children always have larger ids than their parents.
    while (id != kNoNode && id >= ancestor) {
        if (id == ancestor) {
            return true;
        }
        id = nodes_[id].parent;
    }
    return false;
}

std::vector<NodeId> Tree::children(NodeId id) const {
    std::vector<NodeId> out;
    out.reserve(nodes_[id].childCount);
    for (NodeId child = nodes_[id].firstChild; child != kNoNode; child = nodes_[child].nextSibling) {
        out.push_back(child);
    }
    return out;
}

void Tree::finalize() {
    for (Node &node : nodes_) {
        const bool counted = !(node.flags & kHardLinkDuplicate);
        node.size = counted ? node.ownSize : 0;
        node.allocated = counted ? node.ownAllocated : 0;
        node.files = counted && node.kind != NodeKind::kDirectory && node.kind != NodeKind::kMountPoint ? 1 : 0;
        node.dirs = 0;
        node.errors = node.error != 0 ? 1 : 0;
        // A folder's own mtime only says an entry came or went; its date is its newest file's.
        node.newest = node.kind == NodeKind::kDirectory || node.kind == NodeKind::kMountPoint ? 0 : node.modified;
    }
    // Every child comes after its parent, so one backward pass sums the subtrees.
    for (auto id = static_cast<NodeId>(nodes_.size()); id-- > 1;) {
        const Node &node = nodes_[id];
        Node &parent = nodes_[node.parent];
        parent.size += node.size;
        parent.allocated += node.allocated;
        parent.files += node.files;
        parent.dirs += node.dirs + (node.kind == NodeKind::kDirectory || node.kind == NodeKind::kMountPoint);
        parent.errors += node.errors;
        parent.newest = std::max(parent.newest, node.newest);
    }
    std::vector<NodeId> list;
    for (Node &node : nodes_) {
        if (node.childCount < 2) {
            continue;
        }
        list.clear();
        for (NodeId child = node.firstChild; child != kNoNode; child = nodes_[child].nextSibling) {
            list.push_back(child);
        }
        std::ranges::sort(list, [&](NodeId a, NodeId b) {
            if (nodes_[a].size != nodes_[b].size) {
                return nodes_[a].size > nodes_[b].size;
            }
            return name(a) < name(b);
        });
        node.firstChild = list.front();
        for (std::size_t i = 0; i < list.size(); ++i) {
            nodes_[list[i]].nextSibling = i + 1 < list.size() ? list[i + 1] : kNoNode;
        }
    }
}

void Tree::remove(NodeId id) {
    if (id == root()) {
        return;
    }
    // Already gone, on its own or with an ancestor.
    for (NodeId at = id; at != kNoNode; at = nodes_[at].parent) {
        if (nodes_[at].flags & kRemoved) {
            return;
        }
    }
    Node &node = nodes_[id];
    Node &parent = nodes_[node.parent];
    if (parent.firstChild == id) {
        parent.firstChild = node.nextSibling;
    } else {
        NodeId before = parent.firstChild;
        while (nodes_[before].nextSibling != id) {
            before = nodes_[before].nextSibling;
        }
        nodes_[before].nextSibling = node.nextSibling;
    }
    --parent.childCount;
    node.nextSibling = kNoNode;
    node.flags |= kRemoved;
    const std::uint32_t dirs = node.dirs + (node.kind == NodeKind::kDirectory || node.kind == NodeKind::kMountPoint);
    for (NodeId at = node.parent; at != kNoNode; at = nodes_[at].parent) {
        Node &ancestor = nodes_[at];
        ancestor.size -= node.size;
        ancestor.allocated -= node.allocated;
        ancestor.files -= node.files;
        ancestor.dirs -= dirs;
        ancestor.errors -= node.errors;
    }
}

}  // namespace filepan::adirstat
