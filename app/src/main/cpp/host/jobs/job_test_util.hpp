// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "filepan/jobs/job_queue.hpp"
#include "filepan/vfs/local_backend.hpp"
#include "test_util.hpp"

namespace filepan::test {

namespace fs = std::filesystem;

inline void writeFile(const fs::path &path, const std::string &content) {
    std::ofstream(path, std::ios::binary) << content;
}

inline std::string readFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Stands in for a remote backend: serves a local directory as Location::sftp("fake", path), so
// jobs take the cross-backend stream path. Reads come in small, slow chunks.
class FakeRemoteBackend : public vfs::Backend {
public:
    FakeRemoteBackend(fs::path root, std::size_t chunkSize = 64 * 1024,
                      std::chrono::microseconds readDelay = {})
        : root_(std::move(root)), chunkSize_(chunkSize), readDelay_(readDelay) {}

    static vfs::Location at(std::string_view path) { return vfs::Location::sftp("fake", path); }

    [[nodiscard]] vfs::BackendKind kind() const override { return vfs::BackendKind::kSftp; }
    [[nodiscard]] vfs::Capabilities capabilities() const override {
        return {vfs::Capability::kWrite, vfs::Capability::kSetTimes, vfs::Capability::kSymlinks};
    }

    Result<void> list(const vfs::Location &directory, const vfs::ListOptions &options,
                      const vfs::ListCallback &onBatch,
                      const CancellationToken &cancel) override {
        return local_.list(map(directory), options, onBatch, cancel);
    }
    Result<vfs::Entry> stat(const vfs::Location &location) override {
        return local_.stat(map(location));
    }
    Result<vfs::Entry> lstat(const vfs::Location &location) override {
        return local_.lstat(map(location));
    }
    Result<void> makeDirectory(const vfs::Location &location, std::uint32_t mode) override {
        return local_.makeDirectory(map(location), mode);
    }
    Result<void> createFile(const vfs::Location &location, std::uint32_t mode) override {
        return local_.createFile(map(location), mode);
    }
    Result<void> rename(const vfs::Location &source, const vfs::Location &target,
                        bool replace) override {
        return local_.rename(map(source), map(target), replace);
    }
    Result<vfs::RemoveStats> remove(const vfs::Location &location, bool recursive,
                                    vfs::RemoveCallbacks *callbacks,
                                    const CancellationToken &cancel) override {
        return local_.remove(map(location), recursive, callbacks, cancel);
    }
    Result<std::unique_ptr<vfs::InputStream>> openRead(const vfs::Location &location) override {
        auto stream = local_.openRead(map(location));
        if (!stream) {
            return std::unexpected(stream.error());
        }
        return std::make_unique<SlowStream>(std::move(*stream), chunkSize_, readDelay_);
    }
    Result<std::unique_ptr<vfs::OutputStream>> openWrite(
        const vfs::Location &location, const vfs::OpenWriteOptions &options) override {
        return local_.openWrite(map(location), options);
    }
    Result<std::string> readLink(const vfs::Location &location) override {
        return local_.readLink(map(location));
    }
    Result<void> createSymlink(const vfs::Location &link, std::string_view target) override {
        return local_.createSymlink(map(link), target);
    }
    Result<void> setTimes(const vfs::Location &location, std::optional<vfs::Timestamp> access,
                          std::optional<vfs::Timestamp> modified) override {
        return local_.setTimes(map(location), access, modified);
    }

private:
    class SlowStream : public vfs::InputStream {
    public:
        SlowStream(std::unique_ptr<vfs::InputStream> stream, std::size_t chunkSize,
                   std::chrono::microseconds delay)
            : stream_(std::move(stream)), chunkSize_(chunkSize), delay_(delay) {}
        Result<std::size_t> read(std::span<std::uint8_t> buffer) override {
            if (delay_.count()) {
                std::this_thread::sleep_for(delay_);
            }
            return stream_->read(buffer.first(std::min(buffer.size(), chunkSize_)));
        }
        Result<std::uint64_t> seek(std::int64_t offset, vfs::SeekOrigin origin) override {
            return stream_->seek(offset, origin);
        }
        Result<std::uint64_t> size() override { return stream_->size(); }

    private:
        std::unique_ptr<vfs::InputStream> stream_;
        std::size_t chunkSize_;
        std::chrono::microseconds delay_;
    };

    vfs::Location map(const vfs::Location &location) const {
        return vfs::Location::local(root_.string() + location.path());
    }

    vfs::LocalBackend local_;
    fs::path root_;
    std::size_t chunkSize_;
    std::chrono::microseconds readDelay_;
};

// Answers conflicts by the target's name and errors with a fixed decision, synchronously, and
// records what was asked.
class ScriptedDecisions : public jobs::DecisionProvider {
public:
    void requestConflict(jobs::ConflictRequest request,
                         jobs::DecisionPromise<jobs::ConflictDecision> promise) override {
        std::lock_guard lock(mutex_);
        const std::string name(request.target.name());
        conflicts.push_back(request);
        const auto found = conflictAnswers.find(name);
        promise.fulfil(found != conflictAnswers.end() ? found->second : defaultConflict);
    }

    void requestError(jobs::ErrorRequest request,
                      jobs::DecisionPromise<jobs::ErrorDecision> promise) override {
        std::lock_guard lock(mutex_);
        errors.push_back(request);
        promise.fulfil(errorAnswer);
    }

    std::mutex mutex_;
    std::map<std::string, jobs::ConflictDecision> conflictAnswers;
    jobs::ConflictDecision defaultConflict{jobs::ConflictAction::kAbort, false};
    jobs::ErrorDecision errorAnswer{jobs::ErrorAction::kAbort, false};
    std::vector<jobs::ConflictRequest> conflicts;
    std::vector<jobs::ErrorRequest> errors;
};

// Records every progress report.
class ProgressLog {
public:
    jobs::ProgressObserver observer() {
        return [this](const jobs::JobProgress &progress) {
            std::lock_guard lock(mutex_);
            reports_.push_back({std::chrono::steady_clock::now(), progress});
            if (onReport) {
                onReport(progress);
            }
            condition_.notify_all();
        };
    }

    struct Report {
        std::chrono::steady_clock::time_point time;
        jobs::JobProgress progress;
    };

    std::vector<Report> reports() {
        std::lock_guard lock(mutex_);
        return reports_;
    }

    bool waitFor(const std::function<bool(const jobs::JobProgress &)> &predicate,
                 std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [&] {
            for (const auto &report : reports_) {
                if (predicate(report.progress)) return true;
            }
            return false;
        });
    }

    // Called with the log's lock held; keep it short.
    std::function<void(const jobs::JobProgress &)> onReport;

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Report> reports_;
};

inline std::shared_ptr<vfs::SimpleBackendResolver> makeBackends(const fs::path &fakeRoot,
                                                                std::size_t chunkSize = 64 * 1024,
                                                                std::chrono::microseconds delay = {}) {
    auto backends = std::make_shared<vfs::SimpleBackendResolver>();
    backends->set(vfs::BackendKind::kLocal, std::make_shared<vfs::LocalBackend>());
    backends->set(vfs::BackendKind::kSftp,
                  std::make_shared<FakeRemoteBackend>(fakeRoot, chunkSize, delay));
    return backends;
}

inline vfs::Location local(const fs::path &path) { return vfs::Location::local(path.string()); }

}  // namespace filepan::test
