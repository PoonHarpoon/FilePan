// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "filepan/cancellation.hpp"
#include "filepan/decision_promise.hpp"
#include "filepan/error.hpp"
#include "filepan/jobs/job.hpp"
#include "filepan/vfs/entry.hpp"
#include "filepan/vfs/location.hpp"

namespace filepan::jobs {

using filepan::DecisionFuture;
using filepan::DecisionPromise;
using filepan::makeDecision;

enum class ConflictAction {
    kReplace,
    kSkip,
    // Keep the existing file and give the new one a free name: "name (1).ext".
    kKeepBoth,
    // Directories only: copy into the existing directory, deciding conflicts inside it in turn.
    kMerge,
    kAbort,
};

struct ConflictDecision {
    ConflictAction action = ConflictAction::kSkip;
    // Apply to the job's later conflicts of the same kind (between directories, or otherwise).
    bool applyToAll = false;
};

struct ConflictRequest {
    JobId jobId = 0;
    JobKind jobKind = JobKind::kCopy;
    vfs::Location source;
    vfs::Location target;
    vfs::Entry sourceEntry;
    vfs::Entry targetEntry;
    // Both are directories, so kMerge is offered.
    bool canMerge = false;
};

enum class ErrorAction { kRetry, kSkip, kAbort };

struct ErrorDecision {
    ErrorAction action = ErrorAction::kAbort;
    // Apply to the job's later errors. Ignored for kRetry, which would loop.
    bool applyToAll = false;
};

struct ErrorRequest {
    JobId jobId = 0;
    JobKind jobKind = JobKind::kCopy;
    vfs::Location location;
    Error error;
    // What failed, for display: "copy", "remove", "create directory"...
    std::string operation;
    // False where the job can't redo the step (in the middle of reading an archive); kRetry then
    // skips.
    bool canRetry = true;
};

// Asks the user. Called on the job's thread, which then blocks on the future until the promise is
// fulfilled, however long that takes, so an implementation must return promptly and keep the
// promise (e.g. post it to the UI thread or queue it). Dropping the promise unfulfilled aborts.
class DecisionProvider {
public:
    virtual ~DecisionProvider() = default;
    virtual void requestConflict(ConflictRequest request,
                                 DecisionPromise<ConflictDecision> promise) = 0;
    virtual void requestError(ErrorRequest request, DecisionPromise<ErrorDecision> promise) = 0;
};

// Keeps pending decisions until someone resolves them, so they survive while no UI is attached:
// a UI that comes back reads pending() and shows them again.
class QueuedDecisionProvider : public DecisionProvider {
public:
    using RequestId = std::uint64_t;

    struct Pending {
        RequestId id;
        std::variant<ConflictRequest, ErrorRequest> request;
    };

    // Called on the job thread after each new request, e.g. to post a refresh to the UI thread.
    // May be empty while no UI is attached.
    void setListener(std::function<void()> listener);

    // Oldest first. Requests whose job stopped waiting (cancelled) are dropped.
    std::vector<Pending> pending();

    // Return false for unknown ids, a mismatched kind, or a job that stopped waiting.
    bool resolve(RequestId id, ConflictDecision decision);
    bool resolve(RequestId id, ErrorDecision decision);

    void requestConflict(ConflictRequest request,
                         DecisionPromise<ConflictDecision> promise) override;
    void requestError(ErrorRequest request, DecisionPromise<ErrorDecision> promise) override;

private:
    struct Entry {
        RequestId id;
        std::variant<ConflictRequest, ErrorRequest> request;
        std::variant<DecisionPromise<ConflictDecision>, DecisionPromise<ErrorDecision>> promise;
    };

    void add(Entry entry);
    void prune();

    std::mutex mutex_;
    RequestId nextId_ = 1;
    std::vector<Entry> entries_;
    std::function<void()> listener_;
};

}  // namespace filepan::jobs
