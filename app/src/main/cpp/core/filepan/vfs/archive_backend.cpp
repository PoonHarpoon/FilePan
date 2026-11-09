// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/vfs/archive_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <string_view>
#include <utility>

#include <sys/stat.h>

#include "filepan/archive_stream.hpp"

namespace filepan::vfs {

namespace {

// An Error code private to this file: the archive needs a (different) password.
constexpr int kNeedsPassword = -1000;
constexpr int kMaxLinkDepth = 40;
constexpr std::size_t kDiscardBufferSize = 64 * 1024;

// Normalizes a path stored in an archive the way Location does: absolute, without empty or "."
// components or a trailing slash.
std::string normalizePath(std::string_view path) {
    std::string result;
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
    return result.empty() ? "/" : result;
}

std::string_view parentPath(std::string_view path) {
    const auto slash = path.rfind('/');
    return slash == 0 ? std::string_view("/") : path.substr(0, slash);
}

std::string_view baseName(std::string_view path) { return path.substr(path.rfind('/') + 1); }

std::string joinPath(std::string_view directory, std::string_view relative) {
    if (relative.starts_with('/')) {
        return normalizePath(relative);
    }
    std::string joined(directory);
    joined.push_back('/');
    joined.append(relative);
    return normalizePath(joined);
}

// Resolves ".." lexically, as a link target inside an archive needs; nullopt if it climbs above
// the root.
std::optional<std::string> resolveDots(std::string_view path) {
    std::vector<std::string_view> components;
    std::size_t start = 1;
    while (start <= path.size()) {
        auto end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const auto component = path.substr(start, end - start);
        if (component == "..") {
            if (components.empty()) {
                return std::nullopt;
            }
            components.pop_back();
        } else if (!component.empty()) {
            components.push_back(component);
        }
        start = end + 1;
    }
    std::string result;
    for (const auto component : components) {
        result.push_back('/');
        result.append(component);
    }
    return result.empty() ? "/" : result;
}

EntryType typeFromMode(std::uint32_t mode) {
    switch (mode & S_IFMT) {
        case 0:
        case S_IFREG: return EntryType::kFile;
        case S_IFDIR: return EntryType::kDirectory;
        case S_IFLNK: return EntryType::kSymlink;
        default: return EntryType::kOther;
    }
}

bool isPasswordMessage(std::string_view message) {
    return message.find("assphrase") != std::string_view::npos
           || message.find("assword") != std::string_view::npos;
}

Error archiveFailure(const ArchiveError &error, bool encrypted, std::string_view context) {
    if (encrypted && isPasswordMessage(error.message)) {
        return {kNeedsPassword, error.message};
    }
    std::string message(context);
    if (!error.message.empty()) {
        message.append(": ").append(error.message);
    }
    return {error.code > 0 ? error.code : EIO, std::move(message)};
}

// An open archive: the archive file's stream and a reader over it.
struct OpenArchive {
    std::unique_ptr<InputStream> stream;
    std::unique_ptr<StreamArchiveSource> source;
    std::unique_ptr<ArchiveReader> reader;

    Error failure(bool encrypted, std::string_view context) const {
        if (source->error()) {
            return *source->error();
        }
        return archiveFailure(reader ? reader->error() : ArchiveError{EIO, {}}, encrypted,
                              context);
    }
};

Result<OpenArchive> openArchive(Backend &outer, const Location &archive,
                                const std::vector<std::string> &passwords) {
    OpenArchive open;
    auto stream = outer.openRead(archive);
    if (!stream) {
        return std::unexpected(stream.error());
    }
    open.stream = std::move(*stream);
    open.source = std::make_unique<StreamArchiveSource>(*open.stream);
    // Random access lets zip and 7z read their central directory; a stream that can only seek by
    // reading (an archive in an archive) is read straight through instead.
    const bool seekable = outer.capabilities().has(Capability::kSeekableRead);
    ArchiveError error;
    open.reader = ArchiveReader::openSource(*open.source, seekable, passwords, error);
    if (!open.reader) {
        if (open.source->error()) {
            return std::unexpected(*open.source->error());
        }
        return std::unexpected(archiveFailure(error, true, archive.toString()));
    }
    return open;
}

}  // namespace

struct ArchiveBackend::Index {
    struct Node {
        Entry entry;
        // The header whose data this entry reads: its own, or a hard link's target's. -1 for a
        // directory the archive doesn't list.
        std::int64_t dataIndex = -1;
        bool encrypted = false;
        // Directories only, in archive order.
        std::vector<std::string> children;
    };

    std::uint64_t archiveSize = 0;
    Timestamp archiveModifiedTime;
    std::unordered_map<std::string, Node> nodes;

    const Node *find(const std::string &path) const {
        const auto found = nodes.find(path);
        return found != nodes.end() ? &found->second : nullptr;
    }

    // Follows symlinks within the archive; null when it leads nowhere.
    const Node *resolve(const std::string &path, std::string *resolvedPath = nullptr) const {
        std::string current = path;
        for (int depth = 0; depth < kMaxLinkDepth; ++depth) {
            const Node *node = find(current);
            if (!node || node->entry.type != EntryType::kSymlink) {
                if (node && resolvedPath) {
                    *resolvedPath = current;
                }
                return node;
            }
            const auto target =
                resolveDots(joinPath(parentPath(current), node->entry.linkTarget));
            if (!target) {
                return nullptr;
            }
            current = *target;
        }
        return nullptr;
    }
};

struct ArchiveBackend::Slot {
    std::mutex mutex;
    std::shared_ptr<const Index> index;
    std::chrono::steady_clock::time_point checkedAt;
    std::uint64_t lastUse = 0;
};

class ArchiveBackend::EntryStream final : public InputStream {
public:
    EntryStream(Backend &outer, Location archive, std::int64_t dataIndex, std::uint64_t size,
                bool encrypted)
        : outer_(outer),
          archive_(std::move(archive)),
          dataIndex_(dataIndex),
          size_(size),
          encrypted_(encrypted) {}

    // Opens the archive at the entry. An encrypted entry's first block is read here, so that a
    // wrong password shows up now rather than on the first read().
    Result<void> open(const std::vector<std::string> &passwords) {
        passwords_ = passwords;
        auto archive = openArchive(outer_, archive_, passwords_);
        if (!archive) {
            return std::unexpected(archive.error());
        }
        archive_open_ = std::move(*archive);
        position_ = 0;
        pending_.clear();
        pendingOffset_ = 0;
        const auto status = archive_open_.reader->skipTo(static_cast<std::uint64_t>(dataIndex_));
        if (status == ArchiveReader::Status::kEnd) {
            return fail(ENOENT, "the archive changed: " + archive_.toString());
        }
        if (status != ArchiveReader::Status::kOk) {
            return std::unexpected(archive_open_.failure(encrypted_, archive_.toString()));
        }
        if (encrypted_) {
            pending_.resize(kDiscardBufferSize);
            const std::int64_t length =
                archive_open_.reader->readData(pending_.data(), pending_.size());
            if (length < 0) {
                return std::unexpected(archive_open_.failure(true, archive_.toString()));
            }
            pending_.resize(static_cast<std::size_t>(length));
        }
        return {};
    }

    Result<std::size_t> read(std::span<std::uint8_t> buffer) override {
        if (pendingOffset_ < pending_.size()) {
            const std::size_t length = std::min(buffer.size(), pending_.size() - pendingOffset_);
            std::copy_n(pending_.begin() + static_cast<std::ptrdiff_t>(pendingOffset_), length,
                        buffer.begin());
            pendingOffset_ += length;
            position_ += length;
            return length;
        }
        const std::int64_t length = archive_open_.reader->readData(buffer.data(), buffer.size());
        if (length < 0) {
            auto error = archive_open_.failure(encrypted_, archive_.toString());
            if (error.code == kNeedsPassword) {
                error.code = EACCES;
            }
            return std::unexpected(std::move(error));
        }
        position_ += static_cast<std::uint64_t>(length);
        return static_cast<std::size_t>(length);
    }

    Result<std::uint64_t> seek(std::int64_t offset, SeekOrigin origin) override {
        const std::int64_t base = origin == SeekOrigin::kBegin     ? 0
                                  : origin == SeekOrigin::kCurrent ? static_cast<std::int64_t>(position_)
                                                                   : static_cast<std::int64_t>(size_);
        const std::int64_t target = base + offset;
        if (target < 0) {
            return fail(EINVAL);
        }
        if (static_cast<std::uint64_t>(target) < position_) {
            if (auto reopened = open(passwords_); !reopened) {
                return std::unexpected(reopened.error());
            }
        }
        std::vector<std::uint8_t> discard(kDiscardBufferSize);
        while (position_ < static_cast<std::uint64_t>(target)) {
            const std::size_t length = static_cast<std::size_t>(
                std::min<std::uint64_t>(discard.size(), static_cast<std::uint64_t>(target) - position_));
            const auto read = this->read(std::span(discard).first(length));
            if (!read) {
                return std::unexpected(read.error());
            }
            if (*read == 0) {
                break;  // Past the end, as lseek allows; reads return 0.
            }
        }
        return position_;
    }

    Result<std::uint64_t> size() override { return size_; }

private:
    Backend &outer_;
    Location archive_;
    std::int64_t dataIndex_;
    std::uint64_t size_;
    bool encrypted_;
    std::vector<std::string> passwords_;
    OpenArchive archive_open_;
    std::uint64_t position_ = 0;
    std::vector<std::uint8_t> pending_;
    std::size_t pendingOffset_ = 0;
};

ArchiveBackend::ArchiveBackend(BackendResolver &resolver,
                               std::shared_ptr<PasswordProvider> passwords,
                               ArchiveBackendOptions options)
    : resolver_(resolver), passwordProvider_(std::move(passwords)), options_(options) {}

ArchiveBackend::~ArchiveBackend() = default;

Capabilities ArchiveBackend::capabilities() const { return {Capability::kWatch}; }

std::vector<std::string> ArchiveBackend::passwords(const Location &archive) {
    std::lock_guard lock(mutex_);
    const auto found = passwords_.find(archive);
    return found != passwords_.end() ? found->second : std::vector<std::string>();
}

void ArchiveBackend::addPassword(const Location &archive, std::string password) {
    std::lock_guard lock(mutex_);
    auto &list = passwords_[archive];
    if (std::ranges::find(list, password) == list.end()) {
        list.push_back(std::move(password));
    }
}

void ArchiveBackend::forgetPasswords(const Location &archive) {
    std::lock_guard lock(mutex_);
    passwords_.erase(archive);
}

void ArchiveBackend::invalidate(const Location &archive) {
    std::lock_guard lock(mutex_);
    slots_.erase(archive);
}

template <typename T, typename F>
Result<T> ArchiveBackend::withPasswords(const Location &archive, std::string_view entryPath,
                                        const CancellationToken &cancel, F &&attempt) {
    std::optional<std::string> candidate;
    for (int asked = 0;; ++asked) {
        auto passwordList = passwords(archive);
        if (candidate) {
            passwordList.push_back(*candidate);
        }
        auto result = attempt(passwordList);
        if (result || result.error().code != kNeedsPassword) {
            if (result && candidate) {
                addPassword(archive, std::move(*candidate));
            }
            return result;
        }
        const std::string what = archive.toString() + ":" + std::string(entryPath);
        if (!passwordProvider_) {
            return fail(EACCES, "password required: " + what);
        }
        if (asked >= options_.maxPasswordAttempts) {
            return fail(EACCES, "wrong password: " + what);
        }
        auto [promise, future] = makeDecision<std::optional<std::string>>();
        passwordProvider_->requestPassword(
            PasswordRequest{archive, std::string(entryPath), candidate.has_value()},
            std::move(promise));
        const auto answer = future.wait(cancel);
        if (cancel.isCancelled()) {
            return fail(ECANCELED);
        }
        if (!answer || !*answer) {
            return fail(EACCES, "password required: " + what);
        }
        candidate = std::move(**answer);
    }
}

Result<std::shared_ptr<const ArchiveBackend::Index>> ArchiveBackend::index(
    const Location &archive, const CancellationToken &cancel) {
    std::shared_ptr<Slot> slot;
    {
        std::lock_guard lock(mutex_);
        auto &found = slots_[archive];
        if (!found) {
            found = std::make_shared<Slot>();
        }
        slot = found;
        slot->lastUse = ++useCounter_;
        if (slots_.size() > std::max<std::size_t>(options_.maxCachedArchives, 1)) {
            // The slot just used is the newest, so it stays.
            slots_.erase(std::ranges::min_element(
                slots_, {}, [](const auto &item) { return item.second->lastUse; }));
        }
    }
    std::lock_guard slotLock(slot->mutex);
    const auto now = std::chrono::steady_clock::now();
    if (slot->index && now - slot->checkedAt < options_.revalidateInterval) {
        return slot->index;
    }
    Backend *outer = resolver_.backendFor(archive);
    if (!outer) {
        return fail(ENOTSUP, "no backend for " + archive.toString());
    }
    const auto archiveEntry = outer->stat(archive);
    if (!archiveEntry) {
        return std::unexpected(archiveEntry.error());
    }
    if (archiveEntry->type != EntryType::kFile) {
        return fail(archiveEntry->type == EntryType::kDirectory ? EISDIR : EINVAL,
                    archive.toString());
    }
    if (!slot->index || slot->index->archiveSize != archiveEntry->size
        || slot->index->archiveModifiedTime != archiveEntry->modifiedTime) {
        auto built = buildIndex(archive, *outer, *archiveEntry, cancel);
        if (!built) {
            slot->index.reset();
            return built;
        }
        slot->index = std::move(*built);
    }
    slot->checkedAt = now;
    return slot->index;
}

Result<std::shared_ptr<const ArchiveBackend::Index>> ArchiveBackend::buildIndex(
    const Location &archive, Backend &outer, const Entry &archiveEntry,
    const CancellationToken &cancel) {
    using Node = Index::Node;
    return withPasswords<std::shared_ptr<const Index>>(
        archive, "/", cancel,
        [&](const std::vector<std::string> &passwordList)
            -> Result<std::shared_ptr<const Index>> {
            auto open = openArchive(outer, archive, passwordList);
            if (!open) {
                return std::unexpected(open.error());
            }
            auto index = std::make_shared<Index>();
            index->archiveSize = archiveEntry.size;
            index->archiveModifiedTime = archiveEntry.modifiedTime;
            auto &nodes = index->nodes;
            const auto makeDirectory = [&](const std::string &path) {
                Node node;
                node.entry.name = std::string(baseName(path));
                node.entry.hidden = Entry::isHiddenName(node.entry.name);
                node.entry.type = node.entry.targetType = EntryType::kDirectory;
                node.entry.hasStat = true;
                node.entry.mode = S_IFDIR | 0755;
                node.entry.modifiedTime = archiveEntry.modifiedTime;
                node.entry.linkCount = 1;
                return node;
            };
            nodes.emplace("/", makeDirectory("/"));
            // Adds path to its parent, creating the parents the archive doesn't list.
            const auto link = [&](const std::string &path, auto &self) -> void {
                if (path == "/") {
                    return;
                }
                const std::string parent(parentPath(path));
                auto found = nodes.find(parent);
                if (found == nodes.end()) {
                    nodes.emplace(parent, makeDirectory(parent));
                    self(parent, self);
                    // Linking may have rehashed the map.
                    found = nodes.find(parent);
                } else if (found->second.entry.type != EntryType::kDirectory) {
                    // A file and a directory of the same name: the directory wins.
                    auto children = std::move(found->second.children);
                    found->second = makeDirectory(parent);
                    found->second.children = std::move(children);
                }
                found->second.children.emplace_back(baseName(path));
            };
            std::vector<std::pair<std::string, std::string>> hardlinks;
            ArchiveEntryHeader header;
            for (std::int64_t headerIndex = 0;; ++headerIndex) {
                if (cancel.isCancelled()) {
                    return fail(ECANCELED);
                }
                const auto status = open->reader->readHeader(header);
                if (status == ArchiveReader::Status::kEnd) {
                    break;
                }
                if (status == ArchiveReader::Status::kError) {
                    return std::unexpected(open->failure(true, archive.toString()));
                }
                const std::string path = normalizePath(header.pathname);
                if (path == "/") {
                    continue;
                }
                Node node;
                Entry &entry = node.entry;
                entry.name = std::string(baseName(path));
                entry.hidden = Entry::isHiddenName(entry.name);
                entry.hasStat = true;
                entry.type = header.hardlink.empty() ? typeFromMode(header.mode) : EntryType::kFile;
                entry.targetType = entry.type;
                std::uint32_t permissions = header.mode & 07777;
                if (permissions == 0) {
                    permissions = entry.type == EntryType::kDirectory ? 0755 : 0644;
                }
                const std::uint32_t typeBits = entry.type == EntryType::kFile ? S_IFREG
                                               : (header.mode & S_IFMT);
                entry.mode = typeBits | permissions;
                entry.size = static_cast<std::uint64_t>(std::max<std::int64_t>(header.size, 0));
                entry.uid = static_cast<std::uint32_t>(header.uid);
                entry.gid = static_cast<std::uint32_t>(header.gid);
                entry.linkCount = 1;
                entry.inode = static_cast<std::uint64_t>(headerIndex) + 1;
                if (header.flags & ArchiveReader::kEntryMtimeSet) {
                    entry.modifiedTime = {header.mtime, static_cast<std::uint32_t>(header.mtimeNsec)};
                }
                if (header.flags & ArchiveReader::kEntryAtimeSet) {
                    entry.accessTime = {header.atime, static_cast<std::uint32_t>(header.atimeNsec)};
                }
                if (header.flags & ArchiveReader::kEntryBirthtimeSet) {
                    entry.hasBirthTime = true;
                    entry.birthTime = {header.birthtime,
                                       static_cast<std::uint32_t>(header.birthtimeNsec)};
                }
                if (entry.type == EntryType::kSymlink) {
                    entry.linkTarget = header.symlink;
                    entry.size = 0;
                }
                node.dataIndex = headerIndex;
                node.encrypted = header.flags & ArchiveReader::kEntryEncrypted;
                if (!header.hardlink.empty()) {
                    hardlinks.emplace_back(path, normalizePath(header.hardlink));
                }
                const auto existing = nodes.find(path);
                if (existing != nodes.end()) {
                    // A later entry of the same path replaces the earlier one, as when extracting.
                    if (entry.type == EntryType::kDirectory) {
                        node.children = std::move(existing->second.children);
                    }
                    existing->second = std::move(node);
                } else {
                    nodes.emplace(path, std::move(node));
                    link(path, link);
                }
            }
            for (const auto &[path, targetPath] : hardlinks) {
                const auto target = nodes.find(targetPath);
                auto &node = nodes.at(path);
                if (target != nodes.end() && target->second.entry.type == EntryType::kFile
                    && target->second.dataIndex >= 0) {
                    node.dataIndex = target->second.dataIndex;
                    node.encrypted = target->second.encrypted;
                    node.entry.size = target->second.entry.size;
                    node.entry.inode = target->second.entry.inode;
                }
            }
            for (auto &[path, node] : nodes) {
                if (node.entry.type != EntryType::kSymlink) {
                    continue;
                }
                const Node *target = index->resolve(path);
                if (target) {
                    node.entry.targetType = target->entry.type;
                    node.entry.size = target->entry.size;
                } else {
                    node.entry.targetType = EntryType::kOther;
                    node.entry.brokenLink = true;
                }
            }
            return std::shared_ptr<const Index>(std::move(index));
        });
}

Result<void> ArchiveBackend::list(const Location &directory, const ListOptions &options,
                                  const ListCallback &onBatch, const CancellationToken &cancel) {
    auto index = this->index(*directory.archiveFile(), cancel);
    if (!index) {
        return std::unexpected(index.error());
    }
    const auto *node = (*index)->find(directory.path());
    std::string resolvedPath = directory.path();
    if (node && node->entry.type == EntryType::kSymlink) {
        node = (*index)->resolve(directory.path(), &resolvedPath);
    }
    if (!node) {
        return fail(ENOENT, directory.toString());
    }
    if (node->entry.type != EntryType::kDirectory) {
        return fail(ENOTDIR, directory.toString());
    }
    const std::size_t batchSize = std::max<std::size_t>(options.minBatchSize, 1);
    std::vector<Entry> batch;
    batch.reserve(std::min(batchSize, node->children.size()));
    for (const auto &name : node->children) {
        if (cancel.isCancelled()) {
            return fail(ECANCELED);
        }
        const auto *child = (*index)->find(joinPath(resolvedPath, name));
        if (!child) {
            continue;
        }
        Entry entry = child->entry;
        if (!options.withStat) {
            Entry bare;
            bare.name = std::move(entry.name);
            bare.hidden = entry.hidden;
            bare.type = entry.type;
            bare.targetType = entry.type == EntryType::kSymlink ? EntryType::kOther : entry.type;
            entry = std::move(bare);
        }
        batch.push_back(std::move(entry));
        if (batch.size() >= batchSize) {
            onBatch(std::exchange(batch, {}));
        }
    }
    if (!batch.empty()) {
        onBatch(std::move(batch));
    }
    return {};
}

Result<Entry> ArchiveBackend::stat(const Location &location) {
    auto index = this->index(*location.archiveFile(), CancellationToken());
    if (!index) {
        return std::unexpected(index.error());
    }
    const auto *node = (*index)->resolve(location.path());
    if (!node) {
        return fail(ENOENT, location.toString());
    }
    Entry entry = node->entry;
    entry.name = std::string(location.name());
    entry.hidden = Entry::isHiddenName(entry.name);
    return entry;
}

Result<Entry> ArchiveBackend::lstat(const Location &location) {
    auto index = this->index(*location.archiveFile(), CancellationToken());
    if (!index) {
        return std::unexpected(index.error());
    }
    const auto *node = (*index)->find(location.path());
    if (!node) {
        return fail(ENOENT, location.toString());
    }
    return node->entry;
}

Result<void> ArchiveBackend::makeDirectory(const Location &location, std::uint32_t) {
    return fail(EROFS, location.toString());
}

Result<void> ArchiveBackend::createFile(const Location &location, std::uint32_t) {
    return fail(EROFS, location.toString());
}

Result<void> ArchiveBackend::rename(const Location &source, const Location &, bool) {
    return fail(EROFS, source.toString());
}

Result<RemoveStats> ArchiveBackend::remove(const Location &location, bool, RemoveCallbacks *,
                                           const CancellationToken &) {
    return fail(EROFS, location.toString());
}

Result<std::unique_ptr<OutputStream>> ArchiveBackend::openWrite(const Location &location,
                                                               const OpenWriteOptions &) {
    return fail(EROFS, location.toString());
}

Result<std::unique_ptr<InputStream>> ArchiveBackend::openRead(const Location &location) {
    const Location &archive = *location.archiveFile();
    const CancellationToken cancel;
    auto index = this->index(archive, cancel);
    if (!index) {
        return std::unexpected(index.error());
    }
    std::string resolvedPath;
    const auto *node = (*index)->resolve(location.path(), &resolvedPath);
    if (!node) {
        return fail(ENOENT, location.toString());
    }
    if (node->entry.type == EntryType::kDirectory) {
        return fail(EISDIR, location.toString());
    }
    Backend *outer = resolver_.backendFor(archive);
    if (!outer) {
        return fail(ENOTSUP, "no backend for " + archive.toString());
    }
    // Keep the index alive for the node while the password loop runs.
    const auto keepIndex = *index;
    return withPasswords<std::unique_ptr<InputStream>>(
        archive, resolvedPath, cancel,
        [&](const std::vector<std::string> &passwordList)
            -> Result<std::unique_ptr<InputStream>> {
            auto stream = std::make_unique<EntryStream>(*outer, archive, node->dataIndex,
                                                        node->entry.size, node->encrypted);
            if (auto opened = stream->open(passwordList); !opened) {
                return std::unexpected(opened.error());
            }
            return std::unique_ptr<InputStream>(std::move(stream));
        });
}

Result<std::string> ArchiveBackend::readLink(const Location &location) {
    auto entry = lstat(location);
    if (!entry) {
        return std::unexpected(entry.error());
    }
    if (entry->type != EntryType::kSymlink) {
        return fail(EINVAL, location.toString());
    }
    return std::move(entry->linkTarget);
}

Result<std::unique_ptr<WatchHandle>> ArchiveBackend::watch(const Location &directory,
                                                           WatchCallback callback) {
    const Location archive = *directory.archiveFile();
    const auto parent = archive.parent();
    Backend *outer = resolver_.backendFor(archive);
    if (!parent || !outer || !outer->capabilities().has(Capability::kWatch)) {
        return fail(ENOTSUP, directory.toString());
    }
    const std::string name(archive.name());
    return outer->watch(*parent, [this, archive, name, path = directory.path(),
                                  callback = std::move(callback)](const DirectoryChange &change) {
        const bool touched = change.rescan || change.directoryGone
                             || std::ranges::binary_search(change.names, name);
        if (!touched) {
            return;
        }
        invalidate(archive);
        callback(DirectoryChange{path, {}, true, change.directoryGone});
    });
}

}  // namespace filepan::vfs
