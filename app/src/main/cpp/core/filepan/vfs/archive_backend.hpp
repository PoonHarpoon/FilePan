// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "filepan/archive_stream.hpp"
#include "filepan/decision_promise.hpp"
#include "filepan/vfs/backend.hpp"

namespace filepan::vfs {

// Feeds an ArchiveReader from an InputStream.
class StreamArchiveSource : public ArchiveSource {
public:
    explicit StreamArchiveSource(InputStream &stream) : stream_(stream) {}

    std::int64_t read(void *buffer, std::size_t size) override {
        const auto length = stream_.read({static_cast<std::uint8_t *>(buffer), size});
        if (!length) {
            error_ = length.error();
            return -1;
        }
        return static_cast<std::int64_t>(*length);
    }

    std::int64_t seek(std::int64_t offset, int whence) override {
        const SeekOrigin origin = whence == SEEK_SET   ? SeekOrigin::kBegin
                                  : whence == SEEK_CUR ? SeekOrigin::kCurrent
                                                       : SeekOrigin::kEnd;
        const auto position = stream_.seek(offset, origin);
        if (!position) {
            error_ = position.error();
            return -1;
        }
        return static_cast<std::int64_t>(*position);
    }

    // The stream's own failure, which is more telling than libarchive's EIO.
    [[nodiscard]] const std::optional<Error> &error() const { return error_; }

private:
    InputStream &stream_;
    std::optional<Error> error_;
};

struct PasswordRequest {
    // The archive file, as Location::archive() takes it.
    Location archive;
    // The entry that needs it, or "/" when the archive's headers themselves are encrypted.
    std::string entryPath;
    // The previous password for this request was wrong.
    bool retry = false;
};

// Asks the user for an archive password. Called on an I/O thread, which then blocks until the
// promise is fulfilled: post the request to the UI thread and return. Fulfil it with the
// password, or with nullopt to give up; dropping the promise unfulfilled gives up too.
class PasswordProvider {
public:
    virtual ~PasswordProvider() = default;
    virtual void requestPassword(PasswordRequest request,
                                 DecisionPromise<std::optional<std::string>> promise) = 0;
};

struct ArchiveBackendOptions {
    // An index checked against the archive file's size and mtime this recently is used without
    // checking again; 0 checks on every call.
    std::chrono::milliseconds revalidateInterval{1000};
    // Indexes kept, least recently used dropped first.
    std::size_t maxCachedArchives = 8;
    // Passwords asked for per operation before it fails with EACCES.
    int maxPasswordAttempts = 5;
};

// Read-only access to the inside of an archive file (BackendKind::kArchive): see the design note
// in backend.hpp. The archive file is read through the backend that serves it, so archives on an
// SFTP server or inside other archives work too. An archive nested in another one is read as a
// stream, which zip and tar handle but 7z, needing random access, doesn't.
class ArchiveBackend final : public Backend {
public:
    // resolver serves the archive files' own backends and must outlive this one. passwords may
    // be null, in which case encrypted entries fail with EACCES.
    explicit ArchiveBackend(BackendResolver &resolver,
                            std::shared_ptr<PasswordProvider> passwords = nullptr,
                            ArchiveBackendOptions options = {});
    ~ArchiveBackend() override;

    [[nodiscard]] BackendKind kind() const override { return BackendKind::kArchive; }
    [[nodiscard]] Capabilities capabilities() const override;

    Result<void> list(const Location &directory, const ListOptions &options,
                      const ListCallback &onBatch, const CancellationToken &cancel) override;
    Result<Entry> stat(const Location &location) override;
    Result<Entry> lstat(const Location &location) override;
    // Writes fail with EROFS.
    Result<void> makeDirectory(const Location &location, std::uint32_t mode = 0777) override;
    Result<void> createFile(const Location &location, std::uint32_t mode = 0666) override;
    Result<void> rename(const Location &source, const Location &target, bool replace) override;
    Result<RemoveStats> remove(const Location &location, bool recursive,
                               RemoveCallbacks *callbacks,
                               const CancellationToken &cancel) override;
    // Streams the entry; seeking forward reads and discards, seeking backward starts over.
    Result<std::unique_ptr<InputStream>> openRead(const Location &location) override;
    Result<std::unique_ptr<OutputStream>> openWrite(const Location &location,
                                                    const OpenWriteOptions &options) override;
    Result<std::string> readLink(const Location &location) override;
    // Watches the archive file's directory through its backend and reports a rescan of the
    // watched directory whenever the archive file changes. ENOTSUP if that backend can't watch.
    Result<std::unique_ptr<WatchHandle>> watch(const Location &directory,
                                               WatchCallback callback) override;

    // Passwords remembered for an archive for the session, tried before asking. A password the
    // user enters is remembered once it works.
    void addPassword(const Location &archive, std::string password);
    void forgetPasswords(const Location &archive);
    // Drops the archive's cached index.
    void invalidate(const Location &archive);

    struct Index;

private:
    struct Slot;
    class EntryStream;

    Result<std::shared_ptr<const Index>> index(const Location &archive,
                                               const CancellationToken &cancel);
    Result<std::shared_ptr<const Index>> buildIndex(const Location &archive, Backend &outer,
                                                    const Entry &archiveEntry,
                                                    const CancellationToken &cancel);
    std::vector<std::string> passwords(const Location &archive);
    // Runs attempt with the remembered passwords, asking the user for another one while it fails
    // for want of the right password.
    template <typename T, typename F>
    Result<T> withPasswords(const Location &archive, std::string_view entryPath,
                            const CancellationToken &cancel, F &&attempt);

    BackendResolver &resolver_;
    std::shared_ptr<PasswordProvider> passwordProvider_;
    ArchiveBackendOptions options_;

    std::mutex mutex_;
    std::unordered_map<Location, std::shared_ptr<Slot>> slots_;
    std::uint64_t useCounter_ = 0;
    std::unordered_map<Location, std::vector<std::string>> passwords_;
};

}  // namespace filepan::vfs
