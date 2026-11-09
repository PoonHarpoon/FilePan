// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <set>

#include "filepan/jobs/decision.hpp"
#include "filepan/runtime/app_host.hpp"
#include "filepan/ui/components/components.hpp"

// The jobs' questions as sheets: a name conflict (replace, skip, keep both, merge, with "apply to
// all") and an error (retry, skip, cancel the job).
//
// The QueuedDecisionProvider keeps every pending request, so they outlive the sheets: one sheet
// shows at a time, the next follows once it's answered, and a sheet dismissed without an answer
// leaves its job waiting until the user asks again (the band, the jobs sheet) or the app comes
// back to the foreground.
namespace filepan::app {

class Overlays;

class DecisionSheets {
public:
    using RequestId = jobs::QueuedDecisionProvider::RequestId;

    DecisionSheets(runtime::AppServices &services, Overlays &overlays);
    ~DecisionSheets();
    DecisionSheets(const DecisionSheets &) = delete;
    DecisionSheets &operator=(const DecisionSheets &) = delete;

    // Shows the oldest pending request unless a sheet is up or it was dismissed; closes a sheet
    // whose job stopped waiting.
    void refresh();
    // Shows a job's pending request (or, without one, the oldest), even if it was dismissed.
    void showFor(std::optional<jobs::JobId> job);
    // Forgets what was dismissed, so it shows again.
    void unsnooze();

    // The request whose sheet is up.
    [[nodiscard]] std::optional<RequestId> showing() const { return showing_; }
    [[nodiscard]] ui::components::BottomSheet *sheet() const { return sheet_; }
    [[nodiscard]] bool hasPending(jobs::JobId job);

    // An extraction or a test hit an encrypted entry without the right password: the job is aborted
    // without a sheet, and this asks for the password.
    std::function<void(jobs::JobId job)> onNeedsPassword;

private:
    void show(const jobs::QueuedDecisionProvider::Pending &pending);
    void close(bool answered);
    ui::NodePtr conflictContent(RequestId id, const jobs::ConflictRequest &request);
    ui::NodePtr errorContent(RequestId id, const jobs::ErrorRequest &request);

    runtime::AppServices &services_;
    Overlays &overlays_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::set<RequestId> snoozed_;
    std::optional<RequestId> showing_;
    bool answered_ = false;
    ui::components::BottomSheet *sheet_ = nullptr;
};

}  // namespace filepan::app
