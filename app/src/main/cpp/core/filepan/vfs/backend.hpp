// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "filepan/cancellation.hpp"
#include "filepan/error.hpp"
#include "filepan/vfs/entry.hpp"
#include "filepan/vfs/location.hpp"
#include "filepan/vfs/watch.hpp"

// How the archive and SFTP backends (archive_backend.hpp, sftp_backend.hpp) fit this interface:
//
// ArchiveBackend (BackendKind::kArchive) serves Locations whose root is the archive file's own
// Location, resolved through the BackendResolver, so an archive on an SFTP server or inside
// another archive works the same way. On first access it opens the archive with
// ArchiveReader::openSource() over an InputStream from the archive file's backend (an
// ArchiveSource adapter forwarding read/seek; seekable when the stream is, so zip and 7z read
// their central directory), reads every header once with readEntries(), and builds an index:
// path -> {header index, Entry}, with implied parent directories added. list(), stat() and
// lstat() are answered from the index. openRead() returns an InputStream that opens a fresh
// ArchiveReader, skipTo()s the entry's header index and wraps readData(); seeking forward reads
// and discards, seeking backward reopens. The backend is read-only: capabilities() omits kWrite,
// and writes fail with EROFS, so jobs copy out of archives and create new ones with
// CreateArchive. watch() watches the archive file's parent through its backend and reports a
// rescan when the archive file changes, after dropping the index. Passwords come from a
// provider passed to the backend, which the UI fills on demand.
//
// SftpBackend (BackendKind::kSftp) serves Locations whose root is a server id. It maps the id to
// a pooled SftpClient (one connection per server, reconnecting on kConnectionLost), since
// SftpClient serializes calls and splits transfers into chunks. list() calls listDirectory() and
// parses its batch format (attributes, plus link targets and their attributes) straight into
// Entries, delivering them as one batch, or several for very large directories. stat()/lstat()
// map to stat(path, followLinks); makeDirectory, rename (SFTPv3 rename never replaces, so
// replace means remove-then-rename), readLink, createSymlink, setTimes and chmod map to the
// client's calls and setAttributes(). remove() walks the tree itself, since there's no server-
// side recursive delete. openRead()/openWrite() wrap a handle with a tracked offset: seek only
// moves the offset, size() is fileAttributes(). openReadParallel() opens its handle on one of a
// few extra connections per server instead, so that several files transfer at once. There is
// no watch (capabilities() omits kWatch);
// the UI refreshes on demand. SftpError maps to Error: SSH_FX_* codes to errnos and the
// connection-level codes to ECONNREFUSED/EACCES/ENOTCONN, with the server's message kept.

namespace filepan::vfs {

enum class Capability : std::uint32_t {
    kWrite = 1u << 0,  // Create, write, remove and rename files.
    kSetTimes = 1u << 1,
    kChmod = 1u << 2,
    kSymlinks = 1u << 3,  // readLink() and createSymlink().
    kWatch = 1u << 4,
    kSeekableRead = 1u << 5,  // InputStream::seek() is cheap.
    kSeekableWrite = 1u << 6,
    kBirthTime = 1u << 7,
};

class Capabilities {
public:
    constexpr Capabilities() = default;
    constexpr Capabilities(std::initializer_list<Capability> capabilities) {
        for (const auto capability : capabilities) {
            bits_ |= static_cast<std::uint32_t>(capability);
        }
    }
    [[nodiscard]] constexpr bool has(Capability capability) const {
        return bits_ & static_cast<std::uint32_t>(capability);
    }

private:
    std::uint32_t bits_ = 0;
};

enum class SeekOrigin { kBegin, kCurrent, kEnd };

class InputStream {
public:
    virtual ~InputStream() = default;
    // Reads up to buffer.size() bytes. Returns 0 only at the end.
    virtual Result<std::size_t> read(std::span<std::uint8_t> buffer) = 0;
    // Returns the new position. ESPIPE where the backend can't seek.
    virtual Result<std::uint64_t> seek(std::int64_t offset, SeekOrigin origin) = 0;
    virtual Result<std::uint64_t> size() = 0;
};

class OutputStream {
public:
    virtual ~OutputStream() = default;
    // Writes all of buffer, or fails.
    virtual Result<void> write(std::span<const std::uint8_t> buffer) = 0;
    virtual Result<std::uint64_t> seek(std::int64_t offset, SeekOrigin origin) = 0;
    virtual Result<std::uint64_t> size() = 0;
    // Flushes and closes, reporting errors that only show up then (a deferred write error, a
    // remote close). Destroying an unclosed stream closes it and drops such errors.
    virtual Result<void> close() = 0;
};

enum class WriteMode {
    kCreateNew,  // Fails with EEXIST if the file exists.
    kReplace,    // Creates or truncates.
    kExisting,   // Opens an existing file without truncating it.
};

struct OpenWriteOptions {
    WriteMode mode = WriteMode::kCreateNew;
    std::uint32_t permissions = 0666;  // Before the umask.
};

struct ListOptions {
    // Without it, entries have only their name and the type from the directory listing.
    bool withStat = true;
    // Entries are buffered until a batch has at least this many, or the listing ends.
    std::size_t minBatchSize = 256;
    // Threads that may stat entries in parallel, for backends where that helps (local: large
    // directories only). 1 stats them on the calling thread.
    unsigned statThreads = 4;
};

// Receives the entries of a listing in order, on the listing thread.
using ListCallback = std::function<void(std::vector<Entry> batch)>;

class RemoveCallbacks {
public:
    enum class ErrorAction { kRetry, kSkip, kAbort };

    virtual ~RemoveCallbacks() = default;
    // Entries removed since the last call. Returns false to abort.
    virtual bool onProgress(std::uint64_t removedCount, std::string_view currentPath) = 0;
    // An entry inside the tree couldn't be removed or listed. Its directories are kept when it is
    // skipped.
    virtual ErrorAction onError(std::string_view path, const Error &error) = 0;
};

struct RemoveStats {
    std::uint64_t removedCount = 0;
    std::uint64_t skippedCount = 0;
};

// One kind of storage. Synchronous: call it on an I/O thread, never the UI thread. Methods may be
// called concurrently from several threads. Paths of the Locations passed must belong to this
// backend.
class Backend {
public:
    virtual ~Backend() = default;

    [[nodiscard]] virtual BackendKind kind() const = 0;
    [[nodiscard]] virtual Capabilities capabilities() const = 0;

    // Streams the directory's entries ("." and ".." excluded) in batches. Returns ECANCELED once
    // cancel is set, possibly after some batches.
    virtual Result<void> list(const Location &directory, const ListOptions &options,
                              const ListCallback &onBatch, const CancellationToken &cancel) = 0;

    // Follows symbolic links: the entry describes the target, under the location's name.
    virtual Result<Entry> stat(const Location &location) = 0;
    // Doesn't follow: a symlink is described as one, with its target resolved as in a listing.
    virtual Result<Entry> lstat(const Location &location) = 0;

    virtual Result<void> makeDirectory(const Location &location, std::uint32_t mode = 0777) = 0;
    // Creates an empty file; EEXIST if anything exists there.
    virtual Result<void> createFile(const Location &location, std::uint32_t mode = 0666) = 0;
    // Within this backend. Without replace, an existing target fails with EEXIST; with it, the
    // target is replaced as rename(2) would. EXDEV when the backend can't rename between the two
    // (another mount); the caller then copies.
    virtual Result<void> rename(const Location &source, const Location &target, bool replace) = 0;
    // Removes a file, a symlink or an empty directory; recursively removes a directory tree if
    // recursive is set. callbacks may be null, which aborts on the first error.
    virtual Result<RemoveStats> remove(const Location &location, bool recursive,
                                       RemoveCallbacks *callbacks,
                                       const CancellationToken &cancel) = 0;

    virtual Result<std::unique_ptr<InputStream>> openRead(const Location &location) = 0;
    // A stream for reading alongside others, such as files prefetched together. Backends whose
    // connections serialize calls may give it a connection of its own; the rest open it as
    // openRead() does.
    virtual Result<std::unique_ptr<InputStream>> openReadParallel(const Location &location) {
        return openRead(location);
    }
    virtual Result<std::unique_ptr<OutputStream>> openWrite(const Location &location,
                                                            const OpenWriteOptions &options) = 0;

    // Optional; ENOTSUP unless capabilities() says otherwise.
    virtual Result<std::string> readLink(const Location &location);
    virtual Result<void> createSymlink(const Location &link, std::string_view target);
    // Doesn't follow symlinks. A nullopt time is left unchanged.
    virtual Result<void> setTimes(const Location &location, std::optional<Timestamp> accessTime,
                                  std::optional<Timestamp> modifiedTime);
    virtual Result<void> chmod(const Location &location, std::uint32_t mode);
    virtual Result<std::unique_ptr<WatchHandle>> watch(const Location &directory,
                                                       WatchCallback callback);
};

// Maps a Location to the backend serving it, e.g. the SFTP backend for its server id.
class BackendResolver {
public:
    virtual ~BackendResolver() = default;
    // Null if no backend serves it (an unknown server).
    virtual Backend *backendFor(const Location &location) = 0;
};

// Serves one backend per kind; the SFTP backend serves every server, the document backend every
// tree.
class SimpleBackendResolver : public BackendResolver {
public:
    void set(BackendKind kind, std::shared_ptr<Backend> backend);
    Backend *backendFor(const Location &location) override;

private:
    std::shared_ptr<Backend> backends_[4];
};

}  // namespace filepan::vfs
