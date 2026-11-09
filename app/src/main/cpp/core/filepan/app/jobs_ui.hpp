// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "filepan/jobs/job.hpp"
#include "filepan/runtime/app_host.hpp"
#include "filepan/ui/node.hpp"
#include "filepan/ui/reactive.hpp"
#include "filepan/vfs/location.hpp"

// The file jobs as the app shows them (glass-signal-mobile.md, "Jobs running"):
//
//   JobsModel   Submits jobs and follows their progress through AppServices::jobEvents, as
//               signals the UI thread reads: the jobs sheet's list and the data band's summary.
//               When a job finishes it runs the job's own callback, tells the app (snackbars)
//               and bumps `changes` with the folders it touched, so screens that can't watch
//               them refresh.
//   jobBand     The data band above the dock: `COPYING 12/40 · 38 MB/S` with a progress fill,
//               summarizing when several jobs run. Tapping it opens the jobs sheet.
//   jobsSheet   Every job with its progress, rate, ETA and current item; cancel for running
//               ones, dismiss for finished ones.
namespace filepan::app {

struct JobMeta {
    // The jobs sheet's line for it: "Copy 3 items to Papers".
    std::string title;
    // The snackbar when it succeeds ("Copied 3 items"); empty says nothing.
    std::string doneText;
    // Folders it changes.
    std::vector<vfs::Location> changes;
    // Kept off the band and the jobs sheet, with no snackbar: checksums for the properties sheet.
    bool quiet = false;
    // On the UI thread once it finishes, however it ends. The JobQueue still has its result.
    std::function<void(const jobs::JobProgress &progress)> onFinished;
    // On the UI thread with each update before it finishes.
    std::function<void(const jobs::JobProgress &progress)> onProgress;
};

struct JobView {
    jobs::JobId id = 0;
    std::string title;
    jobs::JobProgress progress;
};

// "Copying", "Extracting"...
const char *jobVerb(jobs::JobKind kind);
// "Copied", "Extracted"...
const char *jobPastVerb(jobs::JobKind kind);
// "36 MiB/s".
std::string formatRate(double bytesPerSecond);
// "42 s left", "3 min left", "1 h 5 min left".
std::string formatEta(std::chrono::seconds eta);
// The band's text for the jobs still running (finished ones are ignored): "Copying 456 MiB / 1.1 GiB",
// "2 jobs · 456 MiB / 1.4 GiB", or items when no bytes move ("Deleting 12 of 40"); empty for none.
std::string bandText(const std::vector<JobView> &jobs);
// Their combined transfer rate, "36 MiB/s"; empty when nothing is moving.
std::string bandRate(const std::vector<JobView> &jobs);
// The band's icon: the job kind's glyph, or "sync" for several jobs.
std::string bandIcon(const std::vector<JobView> &jobs);
// 0-1 over the running jobs, or -1 while any is still counting.
float bandProgress(const std::vector<JobView> &jobs);
// A job's detail in the sheet: "456 MiB / 1.1 GiB", then "12/40 items · 36 MiB/s · 42 s left" on a
// second line.
std::string jobDetail(const jobs::JobProgress &progress);
// 0-1, or -1 while counting.
float jobFraction(const jobs::JobProgress &progress);
// A failure in a sentence: "No space left on device."
std::string describeError(const Error &error);

class JobsModel {
public:
    // At most this many finished jobs stay in the sheet.
    static constexpr std::size_t kKeepFinished = 20;

    explicit JobsModel(runtime::AppServices &services);
    ~JobsModel();
    JobsModel(const JobsModel &) = delete;
    JobsModel &operator=(const JobsModel &) = delete;

    jobs::JobId submit(jobs::JobSpec spec, JobMeta meta);
    // Shows a job submitted elsewhere as meta describes it.
    void track(jobs::JobId id, JobMeta meta);
    void cancel(jobs::JobId id);
    // Takes a finished job off the sheet.
    void dismiss(jobs::JobId id);
    void dismissFinished();

    // Tracked: the jobs that aren't quiet, in submission order, finished ones until dismissed.
    ui::Signal<std::vector<JobView>> list;
    // Tracked: any of them still queued or running.
    [[nodiscard]] bool active() const;
    [[nodiscard]] std::optional<JobView> find(jobs::JobId id) const;

    // Bumps whenever a job finishes; changed() has the folders the last ones changed.
    ui::Signal<std::uint64_t> changes{0};
    [[nodiscard]] const std::vector<vfs::Location> &changed() const { return changed_; }

    // A job that isn't quiet finished.
    std::function<void(const JobView &view, const JobMeta &meta)> onFinished;
    // A job that isn't quiet was submitted.
    std::function<void()> onSubmitted;

    void onProgress(const jobs::JobProgress &progress);

private:
    runtime::AppServices &services_;
    std::uint64_t subscription_ = 0;
    std::unordered_map<jobs::JobId, JobMeta> meta_;
    std::vector<vfs::Location> changed_;
};

// The data band, placed above the dock and shown while jobs run. onTap opens the jobs sheet.
ui::NodePtr jobBand(JobsModel &model, std::function<void()> onTap);

// The jobs sheet's content. onDecide shows a job's pending conflict or error again.
ui::NodePtr jobsSheet(JobsModel &model, std::function<void(jobs::JobId id)> onDecide);

}  // namespace filepan::app
