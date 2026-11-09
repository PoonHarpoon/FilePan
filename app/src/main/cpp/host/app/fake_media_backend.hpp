// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "filepan/vfs/backend.hpp"

// A server for the viewer's remote reads, in memory: files by path, served under any server id.
// It counts openReadParallel() calls, reads and how many run at once, and can hold reads at a
// gate or fail them. Streams share its state, so they may outlive it.
namespace filepan::test {

class FakeMediaBackend final : public vfs::Backend {
public:
    struct Shared {
        std::mutex mutex;
        std::condition_variable changed;
        std::map<std::string, std::vector<std::uint8_t>> files;
        bool held = false;
        // Reads fail with this errno while it's set.
        int failing = 0;
        int opens = 0;
        int parallelOpens = 0;
        int reads = 0;
        int running = 0;
        int mostRunning = 0;
        // Reads waiting at the gate.
        int waiting = 0;
        // list() calls, by folder path.
        std::map<std::string, int> lists;
    };

    FakeMediaBackend() : shared_(std::make_shared<Shared>()) {}

    // A file of size bytes, byte i being (i * 7 + seed) & 0xff.
    void add(const std::string &path, std::size_t size, std::uint8_t seed = 0) {
        std::vector<std::uint8_t> data(size);
        for (std::size_t i = 0; i < size; ++i) {
            data[i] = static_cast<std::uint8_t>(i * 7 + seed);
        }
        std::lock_guard lock(shared_->mutex);
        shared_->files[path] = std::move(data);
    }
    std::vector<std::uint8_t> file(const std::string &path) {
        std::lock_guard lock(shared_->mutex);
        return shared_->files[path];
    }
    void hold(bool on) {
        std::lock_guard lock(shared_->mutex);
        shared_->held = on;
        shared_->changed.notify_all();
    }
    void fail(int code) {
        std::lock_guard lock(shared_->mutex);
        shared_->failing = code;
    }
    template <typename F>
    auto with(F &&f) {
        std::lock_guard lock(shared_->mutex);
        return f(*shared_);
    }
    int parallelOpens() {
        return with([](Shared &s) { return s.parallelOpens; });
    }
    int reads() {
        return with([](Shared &s) { return s.reads; });
    }
    int waiting() {
        return with([](Shared &s) { return s.waiting; });
    }
    int mostRunning() {
        return with([](Shared &s) { return s.mostRunning; });
    }

    [[nodiscard]] vfs::BackendKind kind() const override { return vfs::BackendKind::kSftp; }
    [[nodiscard]] vfs::Capabilities capabilities() const override { return {vfs::Capability::kSeekableRead}; }
    Result<void> list(const vfs::Location &directory, const vfs::ListOptions &, const vfs::ListCallback &onBatch,
                      const CancellationToken &) override {
        std::vector<vfs::Entry> batch;
        std::lock_guard lock(shared_->mutex);
        ++shared_->lists[directory.path()];
        const std::string prefix = directory.isRoot() ? "/" : directory.path() + "/";
        std::string lastFolder;
        for (const auto &[path, data] : shared_->files) {
            if (!path.starts_with(prefix)) {
                continue;
            }
            // A file further down makes a folder here.
            if (const std::size_t slash = path.find('/', prefix.size()); slash != std::string::npos) {
                std::string name = path.substr(prefix.size(), slash - prefix.size());
                if (name != lastFolder) {
                    vfs::Entry entry;
                    entry.name = name;
                    entry.type = vfs::EntryType::kDirectory;
                    entry.targetType = vfs::EntryType::kDirectory;
                    entry.hasStat = true;
                    batch.push_back(std::move(entry));
                    lastFolder = std::move(name);
                }
            } else {
                vfs::Entry entry;
                entry.name = path.substr(prefix.size());
                entry.type = vfs::EntryType::kFile;
                entry.targetType = vfs::EntryType::kFile;
                entry.size = data.size();
                entry.hasStat = true;
                batch.push_back(std::move(entry));
            }
        }
        onBatch(std::move(batch));
        return {};
    }
    Result<vfs::Entry> stat(const vfs::Location &location) override {
        std::lock_guard lock(shared_->mutex);
        if (location.isRoot() || !shared_->files.contains(location.path())) {
            vfs::Entry entry;
            entry.name = std::string(location.name());
            entry.type = vfs::EntryType::kDirectory;
            entry.targetType = vfs::EntryType::kDirectory;
            return entry;
        }
        vfs::Entry entry;
        entry.name = std::string(location.name());
        entry.type = vfs::EntryType::kFile;
        entry.targetType = vfs::EntryType::kFile;
        entry.size = shared_->files[location.path()].size();
        entry.hasStat = true;
        return entry;
    }
    Result<vfs::Entry> lstat(const vfs::Location &location) override { return stat(location); }
    Result<void> makeDirectory(const vfs::Location &, std::uint32_t) override { return filepan::fail(EROFS); }
    Result<void> createFile(const vfs::Location &, std::uint32_t) override { return filepan::fail(EROFS); }
    Result<void> rename(const vfs::Location &, const vfs::Location &, bool) override { return filepan::fail(EROFS); }
    Result<vfs::RemoveStats> remove(const vfs::Location &, bool, vfs::RemoveCallbacks *,
                                    const CancellationToken &) override {
        return filepan::fail(EROFS);
    }
    Result<std::unique_ptr<vfs::OutputStream>> openWrite(const vfs::Location &,
                                                         const vfs::OpenWriteOptions &) override {
        return filepan::fail(EROFS);
    }
    Result<std::unique_ptr<vfs::InputStream>> openRead(const vfs::Location &location) override {
        return open(location, false);
    }
    Result<std::unique_ptr<vfs::InputStream>> openReadParallel(const vfs::Location &location) override {
        return open(location, true);
    }

private:
    class Stream final : public vfs::InputStream {
    public:
        Stream(std::shared_ptr<Shared> shared, std::string path) : shared_(std::move(shared)), path_(std::move(path)) {}

        Result<std::size_t> read(std::span<std::uint8_t> buffer) override {
            std::unique_lock lock(shared_->mutex);
            ++shared_->running;
            shared_->mostRunning = std::max(shared_->mostRunning, shared_->running);
            ++shared_->waiting;
            shared_->changed.wait(lock, [&] { return !shared_->held; });
            --shared_->waiting;
            --shared_->running;
            ++shared_->reads;
            if (shared_->failing != 0) {
                return filepan::fail(shared_->failing, "fake");
            }
            const std::vector<std::uint8_t> &data = shared_->files[path_];
            const std::size_t n = at_ >= data.size() ? 0 : std::min(buffer.size(), data.size() - at_);
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(at_), n, buffer.begin());
            at_ += n;
            return n;
        }
        Result<std::uint64_t> seek(std::int64_t offset, vfs::SeekOrigin origin) override {
            if (origin != vfs::SeekOrigin::kBegin || offset < 0) {
                return filepan::fail(EINVAL);
            }
            at_ = static_cast<std::size_t>(offset);
            return at_;
        }
        Result<std::uint64_t> size() override {
            std::lock_guard lock(shared_->mutex);
            return shared_->files[path_].size();
        }

    private:
        std::shared_ptr<Shared> shared_;
        std::string path_;
        std::size_t at_ = 0;
    };

    Result<std::unique_ptr<vfs::InputStream>> open(const vfs::Location &location, bool parallel) {
        std::lock_guard lock(shared_->mutex);
        ++(parallel ? shared_->parallelOpens : shared_->opens);
        if (!shared_->files.contains(location.path())) {
            return filepan::fail(ENOENT);
        }
        if (shared_->failing != 0) {
            return filepan::fail(shared_->failing, "fake");
        }
        return std::unique_ptr<vfs::InputStream>(std::make_unique<Stream>(shared_, location.path()));
    }

    std::shared_ptr<Shared> shared_;
};

class FakeMediaResolver final : public vfs::BackendResolver {
public:
    explicit FakeMediaResolver(vfs::Backend &backend) : backend_(backend) {}
    vfs::Backend *backendFor(const vfs::Location &) override { return &backend_; }

private:
    vfs::Backend &backend_;
};

}  // namespace filepan::test
