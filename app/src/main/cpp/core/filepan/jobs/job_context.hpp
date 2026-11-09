// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Internal to the jobs module: what a running job uses to report progress and ask for decisions.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "filepan/cancellation.hpp"
#include "filepan/error.hpp"
#include "filepan/jobs/decision.hpp"
#include "filepan/jobs/job.hpp"
#include "filepan/jobs/job_queue.hpp"
#include "filepan/vfs/backend.hpp"

namespace filepan::jobs {

// Holds a job's progress and passes it to the observer at most once per interval, except for
// state changes, which always go out at once. Emissions for one job never overlap and never go
// backwards.
class ProgressTracker {
public:
    ProgressTracker(JobProgress initial, const ProgressObserver &observer,
                    std::chrono::milliseconds interval);

    // Applies mutate to the progress, then emits if the interval has passed or force is set.
    template <typename F>
    void update(F &&mutate, bool force = false) {
        bool emit;
        {
            std::lock_guard lock(mutex_);
            std::forward<F>(mutate)(progress_);
            emit = force || std::chrono::steady_clock::now() - lastEmit_ >= interval_;
        }
        if (emit) {
            this->emit();
        }
    }

    void setState(JobState state) {
        update([state](JobProgress &progress) { progress.state = state; }, true);
    }

    [[nodiscard]] JobProgress snapshot() const;

private:
    void emit();
    void updateRate(std::chrono::steady_clock::time_point now);

    const ProgressObserver &observer_;
    const std::chrono::milliseconds interval_;
    mutable std::mutex mutex_;
    JobProgress progress_;
    std::chrono::steady_clock::time_point lastEmit_{};
    // For the rate: bytes done at the last sample, and when.
    std::uint64_t rateBytes_ = 0;
    std::chrono::steady_clock::time_point rateTime_ = std::chrono::steady_clock::now();
    // Serializes emissions, so one taken later is delivered later.
    std::mutex emitMutex_;
};

// A step either happened or was skipped by the user's decision; failures are errors, and
// ECANCELED means cancelled or aborted.
enum class Step { kDone, kSkipped };

class JobContext {
public:
    JobContext(JobId id, JobKind kind, CancellationToken cancel, vfs::BackendResolver &backends,
               DecisionProvider &decisions, ProgressTracker &tracker)
        : id_(id),
          kind_(kind),
          cancel_(std::move(cancel)),
          backends_(backends),
          decisions_(decisions),
          tracker_(tracker) {}

    [[nodiscard]] JobId id() const { return id_; }
    [[nodiscard]] JobKind kind() const { return kind_; }
    [[nodiscard]] const CancellationToken &cancel() const { return cancel_; }
    [[nodiscard]] bool isCancelled() const { return cancel_.isCancelled(); }
    ProgressTracker &tracker() { return tracker_; }

    Result<vfs::Backend *> backendFor(const vfs::Location &location);

    void setScanning(bool scanning);
    void addTotals(std::uint64_t bytes, std::uint64_t items);
    void addDone(std::uint64_t bytes, std::uint64_t items);
    // Takes back bytes reported by an attempt that failed and will be retried or skipped.
    void undoBytes(std::uint64_t bytes);
    void addSkipped(std::uint64_t bytes, std::uint64_t items);
    void setCurrent(const vfs::Location &location);

    // Asks about a conflict, unless an earlier "apply to all" answers it. nullopt means abort.
    std::optional<ConflictDecision> resolveConflict(ConflictRequest request);

    // Asks about a failure, unless an earlier "apply to all" answers it. Cancellation aborts.
    ErrorAction resolveError(const vfs::Location &location, const Error &error,
                             std::string_view operation, bool canRetry = true);

    // Runs f until it succeeds or the user skips or aborts. f returns Result<T>; this returns
    // Result<std::optional<T>> (std::monostate for void), with nullopt for a skip. Cancellation
    // fails with ECANCELED; an abort fails with the step's own error.
    template <typename F,
              typename T = typename std::invoke_result_t<F>::value_type,
              typename U = std::conditional_t<std::is_void_v<T>, std::monostate, T>>
    Result<std::optional<U>> attempt(const vfs::Location &location, std::string_view operation,
                                     F &&f) {
        while (true) {
            if (isCancelled()) {
                return fail(ECANCELED);
            }
            auto result = f();
            if (result) {
                if constexpr (std::is_void_v<T>) {
                    return std::optional<U>(std::monostate());
                } else {
                    return std::optional<U>(std::move(*result));
                }
            }
            if (result.error().code == ECANCELED || isCancelled()) {
                return fail(ECANCELED);
            }
            switch (resolveError(location, result.error(), operation)) {
                case ErrorAction::kRetry:
                    continue;
                case ErrorAction::kSkip:
                    return std::optional<U>();
                case ErrorAction::kAbort:
                    return std::unexpected(result.error());
            }
        }
    }

private:
    JobId id_;
    JobKind kind_;
    CancellationToken cancel_;
    vfs::BackendResolver &backends_;
    DecisionProvider &decisions_;
    ProgressTracker &tracker_;
    std::optional<ConflictDecision> allFileConflicts_;
    std::optional<ConflictDecision> allDirectoryConflicts_;
    std::optional<ErrorDecision> allErrors_;
};

// Runs one job to completion; implemented per kind in the jobs' .cpp files.
Result<void> runCopyOrMove(JobContext &context, const std::vector<vfs::Location> &sources,
                           const vfs::Location &targetDirectory, bool move);
Result<void> runDelete(JobContext &context, const DeleteJob &job);
Result<void> runMakeDirectory(JobContext &context, const MakeDirectoryJob &job);
Result<void> runCreateFile(JobContext &context, const CreateFileJob &job);
Result<void> runRename(JobContext &context, const RenameJob &job);
Result<void> runExtractArchive(JobContext &context, const ExtractArchiveJob &job);
Result<void> runCreateArchive(JobContext &context, const CreateArchiveJob &job);
Result<void> runChecksum(JobContext &context, const ChecksumJob &job, JobResult &result);
Result<void> runVerifyArchive(JobContext &context, const VerifyArchiveJob &job, JobResult &result);
// Moves the job's files out of it, closing them as it goes.
Result<void> runImport(JobContext &context, ImportJob &job);

// "name (1).ext", or "name (2).ext" for a name that already ends in " (1)". Keeps compound
// extensions ("a (1).tar.gz"); directories keep their whole name as the stem.
std::string keepBothName(std::string_view name, bool isDirectory, int number);

// The first free location next to location for keep-both.
Result<vfs::Location> freeLocation(vfs::Backend &backend, const vfs::Location &location,
                                   bool isDirectory);

}  // namespace filepan::jobs
