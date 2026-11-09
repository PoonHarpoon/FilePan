// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/jobs/job_context.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>

#include "filepan/vfs/mime.hpp"

namespace filepan::jobs {

ProgressTracker::ProgressTracker(JobProgress initial, const ProgressObserver &observer,
                                 std::chrono::milliseconds interval)
    : observer_(observer), interval_(interval), progress_(std::move(initial)) {}

JobProgress ProgressTracker::snapshot() const {
    std::lock_guard lock(mutex_);
    return progress_;
}

void ProgressTracker::updateRate(std::chrono::steady_clock::time_point now) {
    const double seconds = std::chrono::duration<double>(now - rateTime_).count();
    // Too short a window gives a noisy rate.
    if (seconds >= 0.05 && progress_.bytesDone >= rateBytes_) {
        const double sample = static_cast<double>(progress_.bytesDone - rateBytes_) / seconds;
        progress_.bytesPerSecond = progress_.bytesPerSecond == 0
                                       ? sample
                                       : 0.7 * progress_.bytesPerSecond + 0.3 * sample;
        rateBytes_ = progress_.bytesDone;
        rateTime_ = now;
    } else if (progress_.bytesDone < rateBytes_) {
        rateBytes_ = progress_.bytesDone;  // Undone bytes; restart the window.
        rateTime_ = now;
    }
    if (progress_.bytesPerSecond > 0 && progress_.bytesTotal > progress_.bytesDone
        && !progress_.scanning) {
        progress_.eta = std::chrono::seconds(static_cast<std::int64_t>(std::ceil(
            static_cast<double>(progress_.bytesTotal - progress_.bytesDone)
            / progress_.bytesPerSecond)));
    } else {
        progress_.eta.reset();
    }
}

void ProgressTracker::emit() {
    std::lock_guard emitLock(emitMutex_);
    JobProgress copy;
    {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        lastEmit_ = now;
        updateRate(now);
        copy = progress_;
    }
    if (observer_) {
        observer_(copy);
    }
}

Result<vfs::Backend *> JobContext::backendFor(const vfs::Location &location) {
    vfs::Backend *backend = backends_.backendFor(location);
    if (!backend) {
        return fail(ENOTSUP, "no backend for " + location.toString());
    }
    return backend;
}

void JobContext::setScanning(bool scanning) {
    tracker_.update([scanning](JobProgress &progress) { progress.scanning = scanning; });
}

void JobContext::addTotals(std::uint64_t bytes, std::uint64_t items) {
    tracker_.update([&](JobProgress &progress) {
        progress.bytesTotal += bytes;
        progress.itemsTotal += items;
    });
}

void JobContext::addDone(std::uint64_t bytes, std::uint64_t items) {
    tracker_.update([&](JobProgress &progress) {
        progress.bytesDone += bytes;
        progress.itemsDone += items;
    });
}

void JobContext::undoBytes(std::uint64_t bytes) {
    tracker_.update([&](JobProgress &progress) {
        progress.bytesDone -= std::min(bytes, progress.bytesDone);
    });
}

void JobContext::addSkipped(std::uint64_t bytes, std::uint64_t items) {
    tracker_.update([&](JobProgress &progress) {
        progress.bytesDone += bytes;
        progress.itemsDone += items;
        progress.itemsSkipped += items;
    });
}

void JobContext::setCurrent(const vfs::Location &location) {
    tracker_.update([&](JobProgress &progress) { progress.currentItem = location.path(); });
}

std::optional<ConflictDecision> JobContext::resolveConflict(ConflictRequest request) {
    auto &remembered = request.canMerge ? allDirectoryConflicts_ : allFileConflicts_;
    if (remembered) {
        return remembered;
    }
    const bool canMerge = request.canMerge;
    request.jobId = id_;
    request.jobKind = kind_;
    auto [promise, future] = makeDecision<ConflictDecision>();
    // Requested first, so whoever sees the waiting state also finds the request.
    decisions_.requestConflict(std::move(request), std::move(promise));
    tracker_.setState(JobState::kWaitingForDecision);
    auto decision = future.wait(cancel_);
    tracker_.setState(JobState::kRunning);
    if (!decision || decision->action == ConflictAction::kAbort) {
        return std::nullopt;
    }
    // Merging only makes sense for two directories; keeping both loses nothing otherwise.
    if (decision->action == ConflictAction::kMerge && !canMerge) {
        decision->action = ConflictAction::kKeepBoth;
    }
    if (decision->applyToAll) {
        remembered = decision;
    }
    return decision;
}

ErrorAction JobContext::resolveError(const vfs::Location &location, const Error &error,
                                     std::string_view operation, bool canRetry) {
    if (cancel_.isCancelled()) {
        return ErrorAction::kAbort;
    }
    if (allErrors_) {
        return allErrors_->action;
    }
    auto [promise, future] = makeDecision<ErrorDecision>();
    decisions_.requestError(
        ErrorRequest{
            .jobId = id_,
            .jobKind = kind_,
            .location = location,
            .error = error,
            .operation = std::string(operation),
            .canRetry = canRetry,
        },
        std::move(promise));
    tracker_.setState(JobState::kWaitingForDecision);
    const auto decision = future.wait(cancel_);
    tracker_.setState(JobState::kRunning);
    if (!decision) {
        return ErrorAction::kAbort;
    }
    ErrorAction action = decision->action;
    if (action == ErrorAction::kRetry && !canRetry) {
        action = ErrorAction::kSkip;
    }
    if (decision->applyToAll && action != ErrorAction::kRetry) {
        allErrors_ = ErrorDecision{action, true};
    }
    return action;
}

std::string keepBothName(std::string_view name, bool isDirectory, int number) {
    std::string_view extension = isDirectory ? std::string_view() : vfs::extensionOf(name);
    std::string_view stem =
        extension.empty() ? name : name.substr(0, name.size() - extension.size() - 1);
    // Strip an earlier " (n)" so that copies of "a (1)" are "a (2)", not "a (1) (1)".
    if (stem.ends_with(')')) {
        const auto open = stem.rfind(" (");
        if (open != std::string_view::npos && open + 2 < stem.size() - 1) {
            const auto digits = stem.substr(open + 2, stem.size() - 1 - open - 2);
            int value = 0;
            const auto [end, error] =
                std::from_chars(digits.data(), digits.data() + digits.size(), value);
            if (error == std::errc() && end == digits.data() + digits.size() && value > 0) {
                stem = stem.substr(0, open);
            }
        }
    }
    std::string result(stem);
    result.append(" (").append(std::to_string(number)).append(")");
    if (!extension.empty()) {
        result.append(".").append(extension);
    }
    return result;
}

Result<vfs::Location> freeLocation(vfs::Backend &backend, const vfs::Location &location,
                                   bool isDirectory) {
    const auto parent = location.parent();
    if (!parent) {
        return fail(EINVAL, location.path());
    }
    for (int number = 1; number < 10'000; ++number) {
        auto candidate = parent->child(keepBothName(location.name(), isDirectory, number));
        const auto existing = backend.lstat(candidate);
        if (!existing && existing.error().code == ENOENT) {
            return candidate;
        }
        if (!existing) {
            return std::unexpected(existing.error());
        }
    }
    return fail(EEXIST, location.path());
}

}  // namespace filepan::jobs
