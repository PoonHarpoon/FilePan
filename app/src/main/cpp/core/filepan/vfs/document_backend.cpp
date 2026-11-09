// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/vfs/document_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <iterator>
#include <optional>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

#include "filepan/vfs/local_backend.hpp"

namespace filepan::vfs {

namespace {

Entry toEntry(const DocumentInfo &info, std::string_view name) {
    Entry entry;
    entry.name = std::string(name);
    entry.type = entry.targetType = info.directory ? EntryType::kDirectory : EntryType::kFile;
    entry.hidden = Entry::isHiddenName(entry.name);
    entry.hasStat = true;
    entry.size = info.directory ? 0 : info.size;
    entry.modifiedTime = {info.modifiedMs / 1000, static_cast<std::uint32_t>(info.modifiedMs % 1000) * 1'000'000};
    const std::uint32_t write = (info.flags & DocumentInfo::kSupportsWrite) != 0 ? 0222 : 0;
    entry.mode = info.directory ? (S_IFDIR | 0555 | write) : (S_IFREG | 0444 | write);
    entry.linkCount = 1;
    return entry;
}

// A location's key in the cache; those below it extend it with '/' and their names.
std::string keyOf(const Location &location) { return location.toString(); }

}  // namespace

DocumentBackend::DocumentBackend(std::shared_ptr<DocumentProvider> provider) : provider_(std::move(provider)) {}

void DocumentBackend::setTrees(std::map<std::string, std::string> trees) {
    std::lock_guard lock(mutex_);
    for (const auto &[id, uri] : trees_) {
        if (const auto at = trees.find(id); at == trees.end() || at->second != uri) {
            const std::string prefix = keyOf(Location::document(id));
            std::erase_if(cache_, [&](const auto &item) { return item.first.starts_with(prefix); });
        }
    }
    trees_ = std::move(trees);
}

Result<std::string> DocumentBackend::treeUri(const Location &location) {
    if (location.kind() != BackendKind::kDocument) {
        return fail(EINVAL, location.toString());
    }
    std::lock_guard lock(mutex_);
    const auto at = trees_.find(location.serverId());
    if (at == trees_.end()) {
        return fail(ENOENT, "No such storage: " + location.serverId());
    }
    return at->second;
}

void DocumentBackend::forget(const Location &location) {
    const std::string key = keyOf(location);
    const std::string below = location.isRoot() ? key : key + "/";
    std::lock_guard lock(mutex_);
    std::erase_if(cache_, [&](const auto &item) { return item.first == key || item.first.starts_with(below); });
}

Result<std::vector<DocumentInfo>> DocumentBackend::listChildren(const Location &directory, const std::string &tree,
                                                               const std::string &documentId) {
    auto children = provider_->children(tree, documentId);
    if (!children) {
        return children;
    }
    // What the listing has is current; whatever it doesn't is gone.
    forget(directory);
    std::lock_guard lock(mutex_);
    for (const DocumentInfo &child : *children) {
        if (!child.name.empty() && child.name.find('/') == std::string::npos) {
            cache_[keyOf(directory.child(child.name))] = child;
        }
    }
    return children;
}

Result<DocumentBackend::Resolved> DocumentBackend::resolve(const Location &location) {
    auto tree = treeUri(location);
    if (!tree) {
        return std::unexpected(tree.error());
    }
    {
        std::lock_guard lock(mutex_);
        if (const auto at = cache_.find(keyOf(location)); at != cache_.end()) {
            return Resolved{*tree, at->second};
        }
    }
    if (location.isRoot()) {
        auto root = provider_->root(*tree);
        if (!root) {
            return std::unexpected(root.error());
        }
        root->directory = true;
        std::lock_guard lock(mutex_);
        cache_[keyOf(location)] = *root;
        return Resolved{*tree, *root};
    }
    auto parent = resolve(*location.parent());
    if (!parent) {
        return parent;
    }
    if (!parent->info.directory) {
        return fail(ENOTDIR, location.toString());
    }
    auto children = listChildren(*location.parent(), *tree, parent->info.id);
    if (!children) {
        return std::unexpected(children.error());
    }
    const auto at = std::ranges::find(*children, location.name(), &DocumentInfo::name);
    if (at == children->end()) {
        return fail(ENOENT, location.toString());
    }
    return Resolved{*tree, *at};
}

Result<void> DocumentBackend::list(const Location &directory, const ListOptions &options,
                                   const ListCallback &onBatch, const CancellationToken &cancel) {
    auto resolved = resolve(directory);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    if (!resolved->info.directory) {
        return fail(ENOTDIR, directory.toString());
    }
    auto children = listChildren(directory, resolved->tree, resolved->info.id);
    if (!children) {
        return std::unexpected(children.error());
    }
    if (cancel.isCancelled()) {
        return fail(ECANCELED);
    }
    const std::size_t batchSize = std::max<std::size_t>(options.minBatchSize, 1);
    std::vector<Entry> batch;
    for (const DocumentInfo &child : *children) {
        if (child.name.empty() || child.name.find('/') != std::string::npos) {
            continue;
        }
        batch.push_back(toEntry(child, child.name));
        if (batch.size() >= batchSize) {
            onBatch(std::exchange(batch, {}));
            if (cancel.isCancelled()) {
                return fail(ECANCELED);
            }
        }
    }
    if (!batch.empty()) {
        onBatch(std::move(batch));
    }
    return {};
}

Result<Entry> DocumentBackend::stat(const Location &location) {
    auto resolved = resolve(location);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    return toEntry(resolved->info, location.name());
}

Result<DocumentInfo> DocumentBackend::createChild(const Location &location, bool directory) {
    const auto parentLocation = location.parent();
    if (!parentLocation) {
        return fail(EEXIST, location.toString());
    }
    auto parent = resolve(*parentLocation);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    if (!parent->info.directory) {
        return fail(ENOTDIR, parentLocation->toString());
    }
    // Providers rename on a clash ("a (1).txt") rather than fail, so look first.
    if (const auto existing = resolve(location); existing) {
        return fail(EEXIST, location.toString());
    } else if (existing.error().code != ENOENT) {
        return std::unexpected(existing.error());
    }
    auto created = provider_->create(parent->tree, parent->info.id, location.name(), directory);
    if (!created) {
        return created;
    }
    created->directory = directory;
    std::lock_guard lock(mutex_);
    // Under the name the provider gave it, which is normally the one asked for.
    cache_[keyOf(parentLocation->child(created->name.empty() ? location.name() : created->name))] = *created;
    return created;
}

Result<void> DocumentBackend::makeDirectory(const Location &location, std::uint32_t) {
    auto created = createChild(location, true);
    return created ? Result<void>() : std::unexpected(created.error());
}

Result<void> DocumentBackend::createFile(const Location &location, std::uint32_t) {
    auto created = createChild(location, false);
    return created ? Result<void>() : std::unexpected(created.error());
}

Result<void> DocumentBackend::rename(const Location &source, const Location &target, bool replace) {
    if (!source.sameRoot(target)) {
        return fail(EXDEV);
    }
    if (source == target) {
        return {};
    }
    if (source.isRoot() || target.isRoot() || source.isAncestorOf(target)) {
        return fail(EINVAL, target.toString());
    }
    auto from = resolve(source);
    if (!from) {
        return std::unexpected(from.error());
    }
    auto toParent = resolve(*target.parent());
    if (!toParent) {
        return std::unexpected(toParent.error());
    }
    if (const auto existing = resolve(target); existing) {
        if (!replace) {
            return fail(EEXIST, target.toString());
        }
        // Not atomic: there's no replacing rename.
        if (auto removed = provider_->remove(existing->tree, existing->info.id); !removed) {
            return removed;
        }
        forget(target);
    } else if (existing.error().code != ENOENT) {
        return std::unexpected(existing.error());
    }
    std::string id = from->info.id;
    if (*source.parent() != *target.parent()) {
        auto fromParent = resolve(*source.parent());
        if (!fromParent) {
            return std::unexpected(fromParent.error());
        }
        auto moved = provider_->move(from->tree, id, fromParent->info.id, toParent->info.id);
        if (!moved) {
            // Without a move, the job copies and deletes instead.
            return fail(moved.error().code == ENOTSUP ? EXDEV : moved.error().code, moved.error().message);
        }
        id = std::move(*moved);
        // Moved under its old name, in case the rename below fails.
        forget(source);
        forget(target.parent()->child(source.name()));
    }
    if (source.name() != target.name()) {
        auto renamed = provider_->rename(from->tree, id, target.name());
        forget(source);
        if (!renamed) {
            return std::unexpected(renamed.error());
        }
    }
    forget(target);
    return {};
}

Result<void> DocumentBackend::removeTree(const std::string &tree, const Location &location,
                                         const DocumentInfo &info, RemoveCallbacks *callbacks,
                                         const CancellationToken &cancel, RemoveStats &stats) {
    if (cancel.isCancelled()) {
        return fail(ECANCELED);
    }
    if (info.directory) {
        auto children = listChildren(location, tree, info.id);
        if (!children) {
            return std::unexpected(children.error());
        }
        bool kept = false;
        for (const DocumentInfo &child : *children) {
            const Location childLocation = location.child(child.name);
            while (true) {
                auto removed = removeTree(tree, childLocation, child, callbacks, cancel, stats);
                if (removed) {
                    break;
                }
                if (removed.error().isCancelled() || callbacks == nullptr) {
                    return removed;
                }
                const auto action = callbacks->onError(childLocation.path(), removed.error());
                if (action == RemoveCallbacks::ErrorAction::kAbort) {
                    return fail(ECANCELED);
                }
                if (action == RemoveCallbacks::ErrorAction::kSkip) {
                    // Its directories stay.
                    ++stats.skippedCount;
                    kept = true;
                    break;
                }
            }
        }
        if (kept) {
            return {};
        }
    }
    if (auto removed = provider_->remove(tree, info.id); !removed) {
        return removed;
    }
    forget(location);
    ++stats.removedCount;
    if (callbacks != nullptr && !callbacks->onProgress(1, location.path())) {
        return fail(ECANCELED);
    }
    return {};
}

Result<RemoveStats> DocumentBackend::remove(const Location &location, bool recursive, RemoveCallbacks *callbacks,
                                            const CancellationToken &cancel) {
    if (location.isRoot()) {
        return fail(EBUSY, "The storage's top folder can't be deleted");
    }
    auto resolved = resolve(location);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    if (resolved->info.directory && !recursive) {
        auto children = listChildren(location, resolved->tree, resolved->info.id);
        if (!children) {
            return std::unexpected(children.error());
        }
        if (!children->empty()) {
            return fail(ENOTEMPTY, location.toString());
        }
    }
    RemoveStats stats;
    if (auto removed = removeTree(resolved->tree, location, resolved->info, callbacks, cancel, stats); !removed) {
        return std::unexpected(removed.error());
    }
    return stats;
}

Result<std::unique_ptr<InputStream>> DocumentBackend::openRead(const Location &location) {
    auto resolved = resolve(location);
    if (!resolved) {
        return std::unexpected(resolved.error());
    }
    if (resolved->info.directory) {
        return fail(EISDIR, location.toString());
    }
    auto fd = provider_->open(resolved->tree, resolved->info.id, "r");
    if (!fd) {
        return std::unexpected(fd.error());
    }
    return fdInputStream(*fd);
}

Result<std::unique_ptr<OutputStream>> DocumentBackend::openWrite(const Location &location,
                                                                 const OpenWriteOptions &options) {
    auto existing = resolve(location);
    if (!existing && existing.error().code != ENOENT) {
        return std::unexpected(existing.error());
    }
    std::optional<Resolved> target;
    std::string mode;
    if (existing) {
        if (existing->info.directory) {
            return fail(EISDIR, location.toString());
        }
        switch (options.mode) {
        case WriteMode::kCreateNew:
            return fail(EEXIST, location.toString());
        case WriteMode::kReplace:
            mode = "wt";
            break;
        case WriteMode::kExisting:
            mode = "rw";
            break;
        }
        target = std::move(*existing);
    } else {
        if (options.mode == WriteMode::kExisting) {
            return std::unexpected(existing.error());
        }
        auto created = createChild(location, false);
        if (!created) {
            return std::unexpected(created.error());
        }
        auto tree = treeUri(location);
        if (!tree) {
            return std::unexpected(tree.error());
        }
        target = Resolved{std::move(*tree), std::move(*created)};
        mode = "w";
    }
    auto fd = provider_->open(target->tree, target->info.id, mode);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    // Its size changes as it's written.
    forget(location);
    return fdOutputStream(*fd);
}

}  // namespace filepan::vfs
