// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "filepan/vfs/backend.hpp"

// Document trees: folders the user granted through the system's picker (Android's Storage Access
// Framework, ACTION_OPEN_DOCUMENT_TREE). A provider addresses documents by opaque ids, not paths,
// so DocumentBackend resolves a Location's path one component at a time by listing, and caches
// what it learns. The DocumentProvider is the platform's side: ContentResolver calls on Android, a
// directory on the host in tests.
namespace filepan::vfs {

// One document as its provider describes it.
struct DocumentInfo {
    enum Flag : std::uint32_t {
        kSupportsWrite = 1u << 0,   // A file's contents, or a directory's children.
        kSupportsDelete = 1u << 1,
        kSupportsRename = 1u << 2,
        kSupportsMove = 1u << 3,
    };

    std::string id;  // Opaque; never parsed.
    std::string name;
    bool directory = false;
    std::uint64_t size = 0;
    std::int64_t modifiedMs = 0;  // 0 when the provider doesn't say.
    std::uint32_t flags = 0;
};

// The platform's document calls. tree is the tree's URI as the platform granted it. Synchronous,
// called on I/O threads, possibly concurrently. Errors are errnos: ENOENT for a document that's
// gone, EACCES for a revoked grant, ENOTSUP for what the provider can't do.
class DocumentProvider {
public:
    virtual ~DocumentProvider() = default;

    // The tree's top document.
    virtual Result<DocumentInfo> root(std::string_view tree) = 0;
    virtual Result<std::vector<DocumentInfo>> children(std::string_view tree, std::string_view documentId) = 0;
    // A new empty file or directory in parent. Providers may pick another name on a clash; the
    // backend checks for one first.
    virtual Result<DocumentInfo> create(std::string_view tree, std::string_view parentId, std::string_view name,
                                        bool directory) = 0;
    // Returns the document's id afterwards, which may change.
    virtual Result<std::string> rename(std::string_view tree, std::string_view documentId,
                                       std::string_view name) = 0;
    // To another directory of the same tree; returns the new id. ENOTSUP unless supported.
    virtual Result<std::string> move(std::string_view tree, std::string_view documentId,
                                     std::string_view fromParentId, std::string_view toParentId) = 0;
    // A file, or a directory with whatever is left in it.
    virtual Result<void> remove(std::string_view tree, std::string_view documentId) = 0;
    // A file descriptor the caller owns. mode is "r", "w" (no truncation), "wt" or "rw".
    virtual Result<int> open(std::string_view tree, std::string_view documentId, std::string_view mode) = 0;
};

// Serves Locations of kind kDocument: their root is a tree id, which setTrees() maps to the tree's
// URI. No symlinks, permissions or times; renaming between directories needs the provider's move,
// and fails with EXDEV without it, so that jobs copy instead.
class DocumentBackend final : public Backend {
public:
    explicit DocumentBackend(std::shared_ptr<DocumentProvider> provider);

    // Tree id -> tree URI. Forgets what it cached of trees that are gone or changed.
    void setTrees(std::map<std::string, std::string> trees);

    [[nodiscard]] BackendKind kind() const override { return BackendKind::kDocument; }
    [[nodiscard]] Capabilities capabilities() const override { return {Capability::kWrite}; }

    Result<void> list(const Location &directory, const ListOptions &options, const ListCallback &onBatch,
                      const CancellationToken &cancel) override;
    Result<Entry> stat(const Location &location) override;
    Result<Entry> lstat(const Location &location) override { return stat(location); }
    Result<void> makeDirectory(const Location &location, std::uint32_t mode) override;
    Result<void> createFile(const Location &location, std::uint32_t mode) override;
    Result<void> rename(const Location &source, const Location &target, bool replace) override;
    Result<RemoveStats> remove(const Location &location, bool recursive, RemoveCallbacks *callbacks,
                               const CancellationToken &cancel) override;
    Result<std::unique_ptr<InputStream>> openRead(const Location &location) override;
    Result<std::unique_ptr<OutputStream>> openWrite(const Location &location,
                                                    const OpenWriteOptions &options) override;

private:
    struct Resolved {
        std::string tree;
        DocumentInfo info;
    };

    Result<std::string> treeUri(const Location &location);
    // The document at location, from the cache or by listing down from the tree's top.
    Result<Resolved> resolve(const Location &location);
    // Lists a directory and caches its children.
    Result<std::vector<DocumentInfo>> listChildren(const Location &directory, const std::string &tree,
                                                   const std::string &documentId);
    Result<DocumentInfo> createChild(const Location &location, bool directory);
    Result<void> removeTree(const std::string &tree, const Location &location, const DocumentInfo &info,
                            RemoveCallbacks *callbacks, const CancellationToken &cancel, RemoveStats &stats);
    // Drops location and everything cached below it.
    void forget(const Location &location);

    std::shared_ptr<DocumentProvider> provider_;
    std::mutex mutex_;
    std::map<std::string, std::string> trees_;
    // Keyed by the Location's string form; the tree id is part of it.
    std::map<std::string, DocumentInfo> cache_;
};

}  // namespace filepan::vfs
