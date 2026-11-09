// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/jobs/job_queue.hpp"

#include <algorithm>
#include <utility>

#include "filepan/jobs/job_context.hpp"

namespace filepan::jobs {

struct JobQueue::Job {
    Job(JobId id, JobSpec spec, const ProgressObserver &observer,
        std::chrono::milliseconds interval)
        : id(id),
          spec(std::move(spec)),
          tracker(initialProgress(id, kindOf(this->spec)), observer, interval) {}

    static JobProgress initialProgress(JobId id, JobKind kind) {
        JobProgress progress;
        progress.id = id;
        progress.kind = kind;
        return progress;
    }

    const JobId id;
    // Not const: an ImportJob's files move into the run, which closes them.
    JobSpec spec;
    CancellationToken cancel;
    ProgressTracker tracker;
    std::optional<JobResult> result;  // Guarded by the queue's mutex.

    // Closes the files a job that won't run was handed; its record stays until forgotten.
    void releaseFiles() {
        if (auto *import = std::get_if<ImportJob>(&spec)) {
            import->items.clear();
        }
    }
};

JobQueue::JobQueue(std::shared_ptr<vfs::BackendResolver> backends,
                   std::shared_ptr<DecisionProvider> decisions, ProgressObserver observer,
                   JobQueueOptions options)
    : backends_(std::move(backends)),
      decisions_(std::move(decisions)),
      observer_(std::move(observer)),
      options_(options) {
    const unsigned count = std::max(1u, options_.maxConcurrentJobs);
    workers_.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

JobQueue::~JobQueue() {
    cancelAll();
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    workAvailable_.notify_all();
    for (auto &worker : workers_) {
        worker.join();
    }
}

JobId JobQueue::submit(JobSpec spec) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard lock(mutex_);
        const JobId id = nextId_++;
        job = std::make_shared<Job>(id, std::move(spec), observer_, options_.progressInterval);
        jobs_.emplace(id, job);
    }
    // Reported before a worker can see it, so it can't overtake the running state.
    job->tracker.setState(JobState::kQueued);
    bool cancelled;
    {
        std::lock_guard lock(mutex_);
        cancelled = stopping_ || job->cancel.isCancelled();
        if (!cancelled) {
            queue_.push_back(job);
        }
    }
    if (cancelled) {
        // Cancelled in between: nothing will run it.
        job->releaseFiles();
        job->tracker.setState(JobState::kCancelled);
        return job->id;
    }
    workAvailable_.notify_one();
    return job->id;
}

bool JobQueue::cancel(JobId id) {
    std::shared_ptr<Job> job;
    bool wasQueued = false;
    {
        std::lock_guard lock(mutex_);
        const auto found = jobs_.find(id);
        if (found == jobs_.end() || isFinished(found->second->tracker.snapshot().state)) {
            return false;
        }
        job = found->second;
        job->cancel.cancel();
        const auto queued = std::ranges::find(queue_, job);
        if (queued != queue_.end()) {
            queue_.erase(queued);
            wasQueued = true;
        }
    }
    if (wasQueued) {
        // It never started, so nothing else reports for it, and no worker will touch its spec.
        job->releaseFiles();
        job->tracker.setState(JobState::kCancelled);
        idle_.notify_all();
    }
    return true;
}

void JobQueue::cancelAll() {
    std::vector<JobId> ids;
    {
        std::lock_guard lock(mutex_);
        for (const auto &[id, job] : jobs_) {
            ids.push_back(id);
        }
    }
    for (const JobId id : ids) {
        cancel(id);
    }
}

std::optional<JobProgress> JobQueue::progress(JobId id) const {
    std::lock_guard lock(mutex_);
    const auto found = jobs_.find(id);
    if (found == jobs_.end()) {
        return std::nullopt;
    }
    return found->second->tracker.snapshot();
}

std::vector<JobProgress> JobQueue::jobs() const {
    std::lock_guard lock(mutex_);
    std::vector<JobProgress> result;
    result.reserve(jobs_.size());
    for (const auto &[id, job] : jobs_) {
        result.push_back(job->tracker.snapshot());
    }
    return result;
}

std::optional<JobResult> JobQueue::result(JobId id) const {
    std::lock_guard lock(mutex_);
    const auto found = jobs_.find(id);
    if (found == jobs_.end()) {
        return std::nullopt;
    }
    return found->second->result;
}

void JobQueue::forget(JobId id) {
    std::lock_guard lock(mutex_);
    const auto found = jobs_.find(id);
    if (found != jobs_.end() && isFinished(found->second->tracker.snapshot().state)) {
        jobs_.erase(found);
    }
}

void JobQueue::waitUntilIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && running_ == 0; });
}

void JobQueue::workerLoop() {
    while (true) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(mutex_);
            workAvailable_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            ++running_;
        }
        run(*job);
        {
            std::lock_guard lock(mutex_);
            --running_;
        }
        idle_.notify_all();
    }
}

void JobQueue::run(Job &job) {
    job.tracker.setState(JobState::kRunning);
    JobContext context(job.id, kindOf(job.spec), job.cancel, *backends_, *decisions_,
                       job.tracker);
    JobResult result;
    const Result<void> outcome = std::visit(
        [&](auto &spec) -> Result<void> {
            using T = std::decay_t<decltype(spec)>;
            if constexpr (std::is_same_v<T, CopyJob>) {
                return runCopyOrMove(context, spec.sources, spec.targetDirectory, false);
            } else if constexpr (std::is_same_v<T, MoveJob>) {
                return runCopyOrMove(context, spec.sources, spec.targetDirectory, true);
            } else if constexpr (std::is_same_v<T, DeleteJob>) {
                return runDelete(context, spec);
            } else if constexpr (std::is_same_v<T, MakeDirectoryJob>) {
                return runMakeDirectory(context, spec);
            } else if constexpr (std::is_same_v<T, CreateFileJob>) {
                return runCreateFile(context, spec);
            } else if constexpr (std::is_same_v<T, RenameJob>) {
                return runRename(context, spec);
            } else if constexpr (std::is_same_v<T, ExtractArchiveJob>) {
                return runExtractArchive(context, spec);
            } else if constexpr (std::is_same_v<T, CreateArchiveJob>) {
                return runCreateArchive(context, spec);
            } else if constexpr (std::is_same_v<T, ChecksumJob>) {
                return runChecksum(context, spec, result);
            } else if constexpr (std::is_same_v<T, ImportJob>) {
                return runImport(context, spec);
            } else {
                static_assert(std::is_same_v<T, VerifyArchiveJob>);
                return runVerifyArchive(context, spec, result);
            }
        },
        job.spec);

    if (outcome) {
        {
            std::lock_guard lock(mutex_);
            job.result = std::move(result);
        }
        job.tracker.update(
            [](JobProgress &progress) {
                progress.state = JobState::kSucceeded;
                progress.scanning = false;
                progress.eta.reset();
            },
            true);
    } else if (outcome.error().code == ECANCELED || job.cancel.isCancelled()) {
        job.tracker.setState(JobState::kCancelled);
    } else {
        job.tracker.update(
            [&](JobProgress &progress) {
                progress.state = JobState::kFailed;
                progress.error = outcome.error();
                progress.eta.reset();
            },
            true);
    }
}

}  // namespace filepan::jobs
