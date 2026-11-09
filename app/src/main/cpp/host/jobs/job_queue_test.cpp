// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The queue's machinery and the copy and move jobs: conflicts, errors, cancellation, progress
// throttling and bounded concurrency.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ranges>
#include <set>

#include <sys/stat.h>
#include <unistd.h>

#include "job_test_util.hpp"

using namespace filepan::jobs;
using namespace filepan::test;
using namespace std::chrono_literals;
using filepan::vfs::Location;

namespace {

JobProgress finalProgress(JobQueue &queue, JobId id) {
    queue.waitUntilIdle();
    const auto progress = queue.progress(id);
    EXPECT(progress && isFinished(progress->state));
    return *progress;
}

// src/ { a.txt, b.txt, c.txt, d/ { inner.txt, keep.txt }, link -> a.txt }
void makeSource(const fs::path &root) {
    fs::create_directories(root / "d");
    writeFile(root / "a.txt", "new a");
    writeFile(root / "b.txt", "new b");
    writeFile(root / "c.txt", "new c");
    writeFile(root / "d" / "inner.txt", "new inner");
    writeFile(root / "d" / "keep.txt", "keep");
    fs::create_symlink("a.txt", root / "link");
}

std::vector<Location> children(const fs::path &root) {
    std::vector<Location> result;
    for (const auto &entry : fs::directory_iterator(root)) {
        result.push_back(local(entry.path()));
    }
    std::ranges::sort(result);
    return result;
}

void testCopyTree() {
    TempDir temp;
    makeSource(temp.path() / "src");
    fs::create_directory(temp.path() / "dst");
    auto decisions = std::make_shared<ScriptedDecisions>();
    ProgressLog log;
    JobQueue queue(makeBackends(temp.path()), decisions, log.observer());
    const auto id = queue.submit(CopyJob{{local(temp.path() / "src")}, local(temp.path() / "dst")});
    const auto progress = finalProgress(queue, id);
    EXPECT(progress.state == JobState::kSucceeded);
    EXPECT(progress.kind == JobKind::kCopy);
    EXPECT(progress.itemsTotal == 8 && progress.itemsDone == 8 && progress.itemsSkipped == 0);
    EXPECT(progress.bytesTotal == 5 * 3 + 9 + 4 && progress.bytesDone == progress.bytesTotal);
    EXPECT(readFile(temp.path() / "dst" / "src" / "d" / "inner.txt") == "new inner");
    EXPECT(fs::read_symlink(temp.path() / "dst" / "src" / "link") == "a.txt");
    EXPECT(readFile(temp.path() / "src" / "a.txt") == "new a");
    EXPECT(decisions->conflicts.empty() && decisions->errors.empty());

    // Copying into the same directory makes "name (1)" instead of conflicting.
    const auto again = queue.submit(CopyJob{{local(temp.path() / "src" / "a.txt")},
                                            local(temp.path() / "src")});
    EXPECT(finalProgress(queue, again).state == JobState::kSucceeded);
    EXPECT(readFile(temp.path() / "src" / "a (1).txt") == "new a");

    // A directory into itself is refused through the error decision; abort fails the job.
    const auto inside = queue.submit(CopyJob{{local(temp.path() / "src")},
                                             local(temp.path() / "src" / "d")});
    const auto refused = finalProgress(queue, inside);
    EXPECT(refused.state == JobState::kFailed && refused.error->code == EINVAL);
    EXPECT(decisions->errors.size() == 1);

    // States went queued -> running -> succeeded for the first job.
    std::vector<JobState> states;
    for (const auto &report : log.reports()) {
        if (report.progress.id == id
            && (states.empty() || states.back() != report.progress.state)) {
            states.push_back(report.progress.state);
        }
    }
    EXPECT((states == std::vector{JobState::kQueued, JobState::kRunning, JobState::kSucceeded}));
}

// Every conflict answer, decided by a scripted provider.
void testConflicts() {
    TempDir temp;
    const auto source = temp.path() / "src";
    const auto target = temp.path() / "dst";
    makeSource(source);
    fs::create_directories(target / "d");
    writeFile(target / "a.txt", "old a");
    writeFile(target / "b.txt", "old b");
    writeFile(target / "c.txt", "old c");
    writeFile(target / "d" / "inner.txt", "old inner");
    writeFile(target / "d" / "other.txt", "other");

    auto decisions = std::make_shared<ScriptedDecisions>();
    decisions->conflictAnswers["a.txt"] = {ConflictAction::kReplace, false};
    decisions->conflictAnswers["b.txt"] = {ConflictAction::kSkip, false};
    decisions->conflictAnswers["c.txt"] = {ConflictAction::kKeepBoth, false};
    decisions->conflictAnswers["d"] = {ConflictAction::kMerge, false};
    // Inside the merged directory: applies to all later file conflicts too.
    decisions->conflictAnswers["inner.txt"] = {ConflictAction::kReplace, true};
    ProgressLog log;
    JobQueue queue(makeBackends(temp.path()), decisions, log.observer());
    const auto id = queue.submit(CopyJob{children(source), local(target)});
    const auto progress = finalProgress(queue, id);
    EXPECT(progress.state == JobState::kSucceeded);
    EXPECT(decisions->conflicts.size() == 5);
    EXPECT(decisions->conflicts[0].target.name() == "a.txt");
    EXPECT(!decisions->conflicts[0].canMerge);
    EXPECT(decisions->conflicts[0].sourceEntry.size == 5);
    EXPECT(decisions->conflicts[0].targetEntry.size == 5);
    for (const auto &conflict : decisions->conflicts) {
        EXPECT(conflict.jobId == id && conflict.jobKind == JobKind::kCopy);
        EXPECT(conflict.canMerge == (conflict.target.name() == "d"));
    }

    EXPECT(readFile(target / "a.txt") == "new a");
    EXPECT(readFile(target / "b.txt") == "old b");
    EXPECT(readFile(target / "c.txt") == "old c");
    EXPECT(readFile(target / "c (1).txt") == "new c");
    EXPECT(readFile(target / "d" / "inner.txt") == "new inner");
    EXPECT(readFile(target / "d" / "other.txt") == "other");
    EXPECT(readFile(target / "d" / "keep.txt") == "keep");
    EXPECT(progress.itemsSkipped == 1);
    EXPECT(progress.itemsDone == progress.itemsTotal);
    EXPECT(progress.bytesDone == progress.bytesTotal);

    // With "apply to all" remembered, a second conflict in the same job isn't asked about.
    writeFile(source / "d" / "inner.txt", "newer inner");
    writeFile(source / "d" / "keep.txt", "newer keep");
    decisions->conflicts.clear();
    // Whichever of inner.txt and keep.txt comes first in directory order answers for both.
    decisions->conflictAnswers["keep.txt"] = {ConflictAction::kReplace, true};
    const auto second = queue.submit(CopyJob{{local(source / "d")}, local(target)});
    const auto secondProgress = finalProgress(queue, second);
    EXPECT(secondProgress.state == JobState::kSucceeded);
    // d, then the first file; the second is answered without asking.
    EXPECT(decisions->conflicts.size() == 2);
    EXPECT(readFile(target / "d" / "keep.txt") == "newer keep");
    EXPECT(readFile(target / "d" / "inner.txt") == "newer inner");

    // Abort cancels the job.
    decisions->conflicts.clear();
    decisions->conflictAnswers.clear();
    const auto aborted = queue.submit(CopyJob{{local(source / "a.txt")}, local(target)});
    EXPECT(finalProgress(queue, aborted).state == JobState::kCancelled);
}

void testErrorDecisions() {
    if (geteuid() == 0) {
        return;  // Permissions don't stop root.
    }
    TempDir temp;
    fs::create_directories(temp.path() / "src");
    fs::create_directories(temp.path() / "dst");
    writeFile(temp.path() / "src" / "ok.txt", "ok");
    writeFile(temp.path() / "src" / "secret.txt", "secret");
    chmod((temp.path() / "src" / "secret.txt").c_str(), 0);

    auto decisions = std::make_shared<ScriptedDecisions>();
    decisions->errorAnswer = {ErrorAction::kSkip, false};
    JobQueue queue(makeBackends(temp.path()), decisions, nullptr);
    const auto id = queue.submit(CopyJob{children(temp.path() / "src"), local(temp.path() / "dst")});
    const auto progress = finalProgress(queue, id);
    EXPECT(progress.state == JobState::kSucceeded && progress.itemsSkipped == 1);
    EXPECT(decisions->errors.size() == 1);
    EXPECT(decisions->errors[0].error.code == EACCES);
    EXPECT(decisions->errors[0].location.name() == "secret.txt");
    EXPECT(readFile(temp.path() / "dst" / "ok.txt") == "ok");
    EXPECT(!fs::exists(temp.path() / "dst" / "secret.txt"));

    // Abort fails the job with the step's error.
    decisions->errorAnswer = {ErrorAction::kAbort, false};
    fs::remove(temp.path() / "dst" / "ok.txt");
    const auto failed = queue.submit(CopyJob{children(temp.path() / "src"), local(temp.path() / "dst")});
    const auto failure = finalProgress(queue, failed);
    EXPECT(failure.state == JobState::kFailed && failure.error->code == EACCES);
    chmod((temp.path() / "src" / "secret.txt").c_str(), 0644);
}

// Local to the fake remote backend and back: the stream path, and a move across backends.
void testCrossBackend() {
    TempDir temp;
    makeSource(temp.path() / "src");
    fs::create_directories(temp.path() / "remote");
    auto decisions = std::make_shared<ScriptedDecisions>();
    JobQueue queue(makeBackends(temp.path()), decisions, nullptr);
    const auto copied = queue.submit(CopyJob{{local(temp.path() / "src")},
                                             FakeRemoteBackend::at("/remote")});
    EXPECT(finalProgress(queue, copied).state == JobState::kSucceeded);
    EXPECT(readFile(temp.path() / "remote" / "src" / "d" / "inner.txt") == "new inner");
    EXPECT(fs::read_symlink(temp.path() / "remote" / "src" / "link") == "a.txt");

    fs::create_directories(temp.path() / "back");
    const auto moved = queue.submit(MoveJob{{FakeRemoteBackend::at("/remote/src")},
                                            local(temp.path() / "back")});
    const auto progress = finalProgress(queue, moved);
    EXPECT(progress.state == JobState::kSucceeded);
    EXPECT(progress.itemsDone == 8 && progress.bytesDone == progress.bytesTotal);
    EXPECT(readFile(temp.path() / "back" / "src" / "c.txt") == "new c");
    EXPECT(!fs::exists(temp.path() / "remote" / "src"));
}

void testLocalMove() {
    TempDir temp;
    makeSource(temp.path() / "src");
    fs::create_directories(temp.path() / "dst" / "d");
    writeFile(temp.path() / "dst" / "d" / "other.txt", "other");
    auto decisions = std::make_shared<ScriptedDecisions>();
    decisions->conflictAnswers["d"] = {ConflictAction::kMerge, false};
    JobQueue queue(makeBackends(temp.path()), decisions, nullptr);
    const auto id = queue.submit(MoveJob{children(temp.path() / "src"), local(temp.path() / "dst")});
    const auto progress = finalProgress(queue, id);
    EXPECT(progress.state == JobState::kSucceeded);
    EXPECT(fs::is_empty(temp.path() / "src"));
    EXPECT(readFile(temp.path() / "dst" / "d" / "inner.txt") == "new inner");
    EXPECT(readFile(temp.path() / "dst" / "d" / "other.txt") == "other");
    EXPECT(fs::is_symlink(temp.path() / "dst" / "link"));
    EXPECT(progress.itemsDone == progress.itemsTotal);

    // Onto another file system, when there is one: movePath() copies and deletes.
    struct stat tempSt {};
    struct stat shmSt {};
    if (stat(temp.path().c_str(), &tempSt) == 0 && stat("/dev/shm", &shmSt) == 0
        && tempSt.st_dev != shmSt.st_dev && access("/dev/shm", W_OK) == 0) {
        const fs::path shm = fs::path("/dev/shm") / ("filepan-job-test-" + std::to_string(getpid()));
        fs::create_directories(shm);
        const auto across = queue.submit(MoveJob{{local(temp.path() / "dst" / "d")}, local(shm)});
        const auto moved = finalProgress(queue, across);
        EXPECT(moved.state == JobState::kSucceeded);
        EXPECT(moved.itemsTotal == 4 && moved.itemsDone == 4 && moved.bytesDone == 9 + 4 + 5);
        EXPECT(readFile(shm / "d" / "other.txt") == "other");
        EXPECT(!fs::exists(temp.path() / "dst" / "d"));
        fs::remove_all(shm);
    } else {
        std::printf("skipping the cross-device move job: no second file system\n");
    }
}

// Cancelling a running copy stops it partway.
void testCancelMidway() {
    TempDir temp;
    fs::create_directories(temp.path() / "src");
    fs::create_directories(temp.path() / "dst");
    for (int i = 0; i < 400; ++i) {
        writeFile(temp.path() / "src" / ("f" + std::to_string(i)), std::string(1024, 'x'));
    }
    auto decisions = std::make_shared<ScriptedDecisions>();
    ProgressLog log;
    // Slow reads so the copy is still running when cancelled.
    JobQueue queue(makeBackends(temp.path(), 1024, 2ms), decisions, log.observer(),
                   {.progressInterval = 10ms});
    const auto id = queue.submit(CopyJob{{FakeRemoteBackend::at("/src")},
                                         local(temp.path() / "dst")});
    EXPECT(log.waitFor([&](const JobProgress &progress) {
        return progress.id == id && progress.itemsDone >= 20;
    }));
    EXPECT(queue.cancel(id));
    const auto progress = finalProgress(queue, id);
    EXPECT(progress.state == JobState::kCancelled);
    EXPECT(progress.itemsDone < progress.itemsTotal);
    EXPECT(!queue.cancel(id));
    const auto copied = std::distance(fs::directory_iterator(temp.path() / "dst" / "src"),
                                      fs::directory_iterator());
    EXPECT(copied > 0 && copied < 400);
}

// A provider that keeps requests until resolved, while nothing is listening.
void testQueuedDecisions() {
    TempDir temp;
    writeFile(temp.path() / "a.txt", "new");
    fs::create_directories(temp.path() / "dst");
    writeFile(temp.path() / "dst" / "a.txt", "old");
    auto decisions = std::make_shared<QueuedDecisionProvider>();
    std::atomic<int> notified = 0;
    ProgressLog log;
    JobQueue queue(makeBackends(temp.path()), decisions, log.observer());

    // No UI attached: the request just waits.
    const auto id = queue.submit(CopyJob{{local(temp.path() / "a.txt")}, local(temp.path() / "dst")});
    EXPECT(log.waitFor([&](const JobProgress &progress) {
        return progress.id == id && progress.state == JobState::kWaitingForDecision;
    }));
    std::this_thread::sleep_for(100ms);
    EXPECT(queue.progress(id)->state == JobState::kWaitingForDecision);

    // The UI comes back, finds the request and answers it.
    decisions->setListener([&] { ++notified; });
    auto pending = decisions->pending();
    EXPECT(pending.size() == 1);
    const auto *conflict = std::get_if<ConflictRequest>(&pending[0].request);
    EXPECT(conflict && conflict->target.name() == "a.txt" && conflict->jobId == id);
    EXPECT(!decisions->resolve(pending[0].id, ErrorDecision{}));  // Wrong kind.
    EXPECT(decisions->resolve(pending[0].id, ConflictDecision{ConflictAction::kReplace, false}));
    EXPECT(!decisions->resolve(pending[0].id, ConflictDecision{}));  // Already resolved.
    EXPECT(finalProgress(queue, id).state == JobState::kSucceeded);
    EXPECT(readFile(temp.path() / "dst" / "a.txt") == "new");
    EXPECT(notified == 0);

    // Cancelling a job that waits for a decision ends it and drops the request.
    const auto waiting = queue.submit(CopyJob{{local(temp.path() / "a.txt")}, local(temp.path() / "dst")});
    EXPECT(log.waitFor([&](const JobProgress &progress) {
        return progress.id == waiting && progress.state == JobState::kWaitingForDecision;
    }));
    EXPECT(notified == 1);
    EXPECT(queue.cancel(waiting));
    EXPECT(finalProgress(queue, waiting).state == JobState::kCancelled);
    EXPECT(decisions->pending().empty());
}

// Progress reports for one job are at least the interval apart, except state changes.
void testProgressThrottling() {
    TempDir temp;
    fs::create_directories(temp.path() / "dst");
    writeFile(temp.path() / "big", std::string(2 * 1024 * 1024, 'x'));
    auto decisions = std::make_shared<ScriptedDecisions>();
    ProgressLog log;
    // 128 reads of 16 KiB, 4 ms apart: about half a second of steady progress.
    JobQueue queue(makeBackends(temp.path(), 16 * 1024, 4ms), decisions, log.observer(),
                   {.progressInterval = 100ms});
    const auto start = std::chrono::steady_clock::now();
    const auto id = queue.submit(CopyJob{{FakeRemoteBackend::at("/big")}, local(temp.path() / "dst")});
    const auto progress = finalProgress(queue, id);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT(progress.state == JobState::kSucceeded && progress.bytesDone == 2 * 1024 * 1024);

    int progressReports = 0;
    std::optional<ProgressLog::Report> previous;
    bool sawRate = false;
    for (const auto &report : log.reports()) {
        if (previous && report.progress.state == previous->progress.state
            && report.progress.state == JobState::kRunning) {
            ++progressReports;
            EXPECT(report.time - previous->time >= 95ms);
        }
        sawRate |= report.progress.bytesPerSecond > 0 && report.progress.eta.has_value();
        previous = report;
    }
    EXPECT(elapsed >= 400ms);
    EXPECT(progressReports >= 2);
    EXPECT(progressReports <= elapsed / 100ms + 1);
    EXPECT(sawRate);
}

void testBoundedConcurrency() {
    TempDir temp;
    fs::create_directories(temp.path() / "dst");
    writeFile(temp.path() / "file", std::string(64 * 1024, 'x'));
    auto decisions = std::make_shared<ScriptedDecisions>();
    ProgressLog log;
    std::map<JobId, JobState> states;
    std::size_t maxRunning = 0;
    log.onReport = [&](const JobProgress &progress) {
        states[progress.id] = progress.state;
        maxRunning = std::max<std::size_t>(maxRunning, std::ranges::count(
            states | std::views::values, JobState::kRunning));
    };
    JobQueue queue(makeBackends(temp.path(), 4 * 1024, 2ms), decisions, log.observer(),
                   {.maxConcurrentJobs = 2});
    std::vector<JobId> ids;
    for (int i = 0; i < 5; ++i) {
        fs::create_directory(temp.path() / "dst" / std::to_string(i));
        ids.push_back(queue.submit(CopyJob{{FakeRemoteBackend::at("/file")},
                                           local(temp.path() / "dst" / std::to_string(i))}));
    }
    // The ones not yet started are queued, in order.
    const auto snapshot = queue.jobs();
    EXPECT(snapshot.size() == 5 && snapshot[4].id == ids[4]);
    queue.waitUntilIdle();
    EXPECT(maxRunning == 2);
    for (const auto id : ids) {
        EXPECT(queue.progress(id)->state == JobState::kSucceeded);
        queue.forget(id);
    }
    EXPECT(queue.jobs().empty());
    for (int i = 0; i < 5; ++i) {
        EXPECT(fs::file_size(temp.path() / "dst" / std::to_string(i) / "file") == 64 * 1024);
    }

    // Destroying the queue cancels what is left.
    {
        JobQueue doomed(makeBackends(temp.path(), 1024, 5ms), decisions, nullptr,
                        {.maxConcurrentJobs = 1});
        for (int i = 0; i < 3; ++i) {
            doomed.submit(CopyJob{{FakeRemoteBackend::at("/file")},
                                  local(temp.path() / "dst" / std::to_string(i))});
        }
    }
}

}  // namespace

int main() {
    testCopyTree();
    testConflicts();
    testErrorDecisions();
    testCrossBackend();
    testLocalMove();
    testCancelMidway();
    testQueuedDecisions();
    testProgressThrottling();
    testBoundedConcurrency();
    return 0;
}
