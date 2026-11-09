// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <memory>
#include <mutex>

#include "filepan/vfs/backend.hpp"

namespace filepan::vfs {

class DirectoryWatcher;

// The device's own file systems, through system calls. Listing uses DirectoryReader's batched
// getdents64 + statx and builds Entries directly from the statx results.
class LocalBackend final : public Backend {
public:
    // watcher may be null; one is created on the first watch() call.
    explicit LocalBackend(std::shared_ptr<DirectoryWatcher> watcher = nullptr);
    ~LocalBackend() override;

    [[nodiscard]] BackendKind kind() const override { return BackendKind::kLocal; }
    [[nodiscard]] Capabilities capabilities() const override;

    Result<void> list(const Location &directory, const ListOptions &options,
                      const ListCallback &onBatch, const CancellationToken &cancel) override;
    Result<Entry> stat(const Location &location) override;
    Result<Entry> lstat(const Location &location) override;
    Result<void> makeDirectory(const Location &location, std::uint32_t mode = 0777) override;
    Result<void> createFile(const Location &location, std::uint32_t mode = 0666) override;
    Result<void> rename(const Location &source, const Location &target, bool replace) override;
    Result<RemoveStats> remove(const Location &location, bool recursive,
                               RemoveCallbacks *callbacks,
                               const CancellationToken &cancel) override;
    Result<std::unique_ptr<InputStream>> openRead(const Location &location) override;
    Result<std::unique_ptr<OutputStream>> openWrite(const Location &location,
                                                    const OpenWriteOptions &options) override;
    Result<std::string> readLink(const Location &location) override;
    Result<void> createSymlink(const Location &link, std::string_view target) override;
    Result<void> setTimes(const Location &location, std::optional<Timestamp> accessTime,
                          std::optional<Timestamp> modifiedTime) override;
    Result<void> chmod(const Location &location, std::uint32_t mode) override;
    Result<std::unique_ptr<WatchHandle>> watch(const Location &directory,
                                               WatchCallback callback) override;

private:
    std::mutex watcherMutex_;
    std::shared_ptr<DirectoryWatcher> watcher_;
};

// The local backend's streams over an open file descriptor, which they then own; for backends that
// are handed one, such as document trees.
std::unique_ptr<InputStream> fdInputStream(int fd);
std::unique_ptr<OutputStream> fdOutputStream(int fd);

}  // namespace filepan::vfs
