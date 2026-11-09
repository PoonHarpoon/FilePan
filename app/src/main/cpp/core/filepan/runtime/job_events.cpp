// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/runtime/job_events.hpp"

#include <algorithm>
#include <string>

#include "filepan/size_format.hpp"

namespace filepan::runtime {

namespace {

const char *verbFor(jobs::JobKind kind) {
    switch (kind) {
    case jobs::JobKind::kCopy:
        return "Copying";
    case jobs::JobKind::kMove:
        return "Moving";
    case jobs::JobKind::kDelete:
        return "Deleting";
    case jobs::JobKind::kMakeDirectory:
        return "Creating folder";
    case jobs::JobKind::kCreateFile:
        return "Creating file";
    case jobs::JobKind::kRename:
        return "Renaming";
    case jobs::JobKind::kExtractArchive:
        return "Extracting";
    case jobs::JobKind::kCreateArchive:
        return "Compressing";
    case jobs::JobKind::kChecksum:
        return "Computing checksums";
    case jobs::JobKind::kImport:
        return "Saving";
    case jobs::JobKind::kVerifyArchive:
        return "Testing archive";
    }
    return "Working";
}

}  // namespace

JobSummary summarizeJobs(const std::map<jobs::JobId, jobs::JobProgress> &active) {
    JobSummary summary;
    summary.activeJobs = static_cast<std::uint32_t>(active.size());
    if (active.empty()) {
        return summary;
    }
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
    std::uint64_t itemsDone = 0;
    std::uint64_t itemsTotal = 0;
    bool indeterminate = false;
    std::size_t waiting = 0;
    for (const auto &[id, progress] : active) {
        bytesDone += progress.bytesDone;
        bytesTotal += progress.bytesTotal;
        itemsDone += progress.itemsDone;
        itemsTotal += progress.itemsTotal;
        indeterminate = indeterminate || progress.scanning;
        if (progress.state == jobs::JobState::kQueued) {
            ++waiting;
        }
    }
    const jobs::JobProgress &first = active.begin()->second;
    if (active.size() == 1) {
        summary.title = verbFor(first.kind);
        if (first.itemsTotal > 1) {
            summary.title += " " + std::to_string(first.itemsTotal) + " items";
        }
        summary.text = first.state == jobs::JobState::kWaitingForDecision ? "Waiting for you"
                                                                            : first.currentItem;
    } else {
        summary.title = std::to_string(active.size()) + " jobs";
        summary.text = waiting > 0 ? std::to_string(waiting) + " waiting" : first.currentItem;
    }
    const bool deciding = active.size() == 1 && first.state == jobs::JobState::kWaitingForDecision;
    if (!indeterminate && !deciding && bytesTotal > 0) {
        summary.text = formatSizeOf(bytesDone, bytesTotal) + (summary.text.empty() ? "" : " · " + summary.text);
    }
    if (!indeterminate) {
        if (bytesTotal > 0) {
            summary.progressPermille =
                    static_cast<std::int32_t>(std::min<std::uint64_t>(1000, bytesDone * 1000 / bytesTotal));
        } else if (itemsTotal > 0) {
            summary.progressPermille =
                    static_cast<std::int32_t>(std::min<std::uint64_t>(1000, itemsDone * 1000 / itemsTotal));
        }
    }
    return summary;
}

JobNotifier::~JobNotifier() {
    if (timer_) {
        loop_.cancel(*timer_);
    }
}

void JobNotifier::onProgress(const jobs::JobProgress &progress) {
    if (jobs::isFinished(progress.state)) {
        jobs_.erase(progress.id);
    } else {
        jobs_[progress.id] = progress;
    }
    const bool active = !jobs_.empty();
    if (active != active_) {
        active_ = active;
        platform_.jobsActive(active);
        if (!active) {
            if (timer_) {
                loop_.cancel(*timer_);
                timer_.reset();
            }
            last_.reset();
            return;
        }
    }
    if (active && !timer_) {
        // At most one notification update per interval; Android drops faster ones anyway.
        const std::int64_t delay =
                lastNs_ == 0 ? 0 : std::max<std::int64_t>(0, lastNs_ + intervalNs_ - loop_.now());
        timer_ = loop_.postDelayed([this] { publish(); }, delay);
    }
}

void JobNotifier::publish() {
    timer_.reset();
    if (jobs_.empty()) {
        return;
    }
    JobSummary summary = summarizeJobs(jobs_);
    if (last_ && *last_ == summary) {
        return;
    }
    last_ = summary;
    lastNs_ = loop_.now();
    platform_.updateJobNotification(summary);
}

}  // namespace filepan::runtime
