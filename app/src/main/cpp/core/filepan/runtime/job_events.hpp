// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "filepan/jobs/job.hpp"
#include "filepan/runtime/platform.hpp"
#include "filepan/ui/ui_loop.hpp"

// The JobQueue's progress, on the UI thread: AppHost posts each update there, hands it to the
// JobNotifier (the foreground service and its notification) and then to JobEvents, which the
// app's screens subscribe to.
namespace filepan::runtime {

// Subscribers to job progress. UI thread only.
class JobEvents {
public:
    using Listener = std::function<void(const jobs::JobProgress &progress)>;

    std::uint64_t subscribe(Listener listener) {
        const std::uint64_t id = nextId_++;
        listeners_.emplace_back(id, std::move(listener));
        return id;
    }
    void unsubscribe(std::uint64_t id) {
        std::erase_if(listeners_, [id](const auto &entry) { return entry.first == id; });
    }
    // Listeners may subscribe or unsubscribe while it runs; those changes apply to the next.
    void publish(const jobs::JobProgress &progress) const {
        const auto listeners = listeners_;
        for (const auto &[id, listener] : listeners) {
            listener(progress);
        }
    }

private:
    std::uint64_t nextId_ = 1;
    std::vector<std::pair<std::uint64_t, Listener>> listeners_;
};

// What the jobs notification says about the jobs still running.
JobSummary summarizeJobs(const std::map<jobs::JobId, jobs::JobProgress> &active);

// Drives Platform::jobsActive (at once, when it changes) and updateJobNotification (at most once
// per interval, and only when the summary changed). UI thread only.
class JobNotifier {
public:
    static constexpr std::int64_t kDefaultIntervalNs = 500'000'000;

    JobNotifier(Platform &platform, ui::UiLoop &loop, std::int64_t intervalNs = kDefaultIntervalNs)
        : platform_(platform), loop_(loop), intervalNs_(intervalNs) {}
    ~JobNotifier();
    JobNotifier(const JobNotifier &) = delete;
    JobNotifier &operator=(const JobNotifier &) = delete;

    void onProgress(const jobs::JobProgress &progress);

    [[nodiscard]] bool active() const { return active_; }

private:
    void publish();

    Platform &platform_;
    ui::UiLoop &loop_;
    std::int64_t intervalNs_;
    std::map<jobs::JobId, jobs::JobProgress> jobs_;
    bool active_ = false;
    std::optional<ui::UiLoop::TimerId> timer_;
    std::optional<JobSummary> last_;
    std::int64_t lastNs_ = 0;
};

}  // namespace filepan::runtime
