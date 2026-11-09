// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "filepan/jobs/decision.hpp"
#include "filepan/jobs/job.hpp"
#include "filepan/vfs/backend.hpp"

namespace filepan::jobs {

// Receives a job's progress on the job's thread (or the canceller's, for a job cancelled before
// it started). Post it to the UI thread; don't block. Calls for one job never overlap.
using ProgressObserver = std::function<void(const JobProgress &progress)>;

struct JobQueueOptions {
    // Jobs that run at once; the rest wait in submission order.
    unsigned maxConcurrentJobs = 2;
    // Progress is reported at most this often per job; state changes always go out at once.
    std::chrono::milliseconds progressInterval{100};
};

// Runs file jobs on its own threads. No Android APIs: a foreground service and notification are
// driven from the progress observer by the platform layer.
class JobQueue {
public:
    JobQueue(std::shared_ptr<vfs::BackendResolver> backends,
             std::shared_ptr<DecisionProvider> decisions, ProgressObserver observer,
             JobQueueOptions options = {});
    // Cancels every job and waits for the running ones to stop.
    ~JobQueue();
    JobQueue(const JobQueue &) = delete;
    JobQueue &operator=(const JobQueue &) = delete;

    JobId submit(JobSpec spec);

    // Returns false for unknown or finished jobs. A running job stops at its next check, which
    // includes while it waits for a decision.
    bool cancel(JobId id);
    void cancelAll();

    [[nodiscard]] std::optional<JobProgress> progress(JobId id) const;
    // Every job not forgotten, in submission order.
    [[nodiscard]] std::vector<JobProgress> jobs() const;
    // For a job that succeeded.
    [[nodiscard]] std::optional<JobResult> result(JobId id) const;
    // Drops a finished job's record.
    void forget(JobId id);

    // Blocks until no job is queued or running.
    void waitUntilIdle();

private:
    struct Job;

    void workerLoop();
    void run(Job &job);

    std::shared_ptr<vfs::BackendResolver> backends_;
    std::shared_ptr<DecisionProvider> decisions_;
    ProgressObserver observer_;
    JobQueueOptions options_;

    mutable std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable idle_;
    bool stopping_ = false;
    JobId nextId_ = 1;
    std::map<JobId, std::shared_ptr<Job>> jobs_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::size_t running_ = 0;
    std::vector<std::thread> workers_;
};

}  // namespace filepan::jobs
