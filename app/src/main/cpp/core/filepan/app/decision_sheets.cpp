// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/decision_sheets.hpp"

#include <algorithm>
#include <cerrno>

#include "filepan/app/app.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/jobs_ui.hpp"
#include "filepan/app/sheet_parts.hpp"

namespace filepan::app {

using namespace ui;
using namespace ui::components;
using jobs::ConflictAction;
using jobs::ErrorAction;

namespace {

std::string inQuotes(std::string_view name) { return "“" + std::string(name) + "”"; }

std::string where(const vfs::Location &location) {
    const auto parent = location.parent();
    if (!parent) {
        return "this folder";
    }
    if (parent->isRoot()) {
        if (const vfs::Location *archive = parent->archiveFile()) {
            return inQuotes(archive->name());
        }
        return parent->isLocal()                                 ? "/"
               : parent->kind() == vfs::BackendKind::kDocument ? inQuotes(parent->serverId())
                                                               : "the server's root";
    }
    return inQuotes(parent->name());
}

std::string printable(const vfs::Location &location) {
    if (location.isLocal()) {
        return location.path();
    }
    if (const vfs::Location *archive = location.archiveFile()) {
        return printable(*archive) + " › " + location.path();
    }
    return location.serverId() + ":" + location.path();
}

// What a conflict sheet says about one side: newer, larger.
std::string compare(const vfs::Entry &self, const vfs::Entry &other) {
    if (!self.hasStat || !other.hasStat || self.isDirectoryLike()) {
        return {};
    }
    if (self.modifiedTime > other.modifiedTime) {
        return "Newer";
    }
    if (self.modifiedTime == other.modifiedTime && self.size > other.size) {
        return "Larger";
    }
    return {};
}

const char *operationVerb(jobs::JobKind kind) {
    switch (kind) {
    case jobs::JobKind::kCopy:
        return "copy";
    case jobs::JobKind::kMove:
        return "move";
    case jobs::JobKind::kDelete:
        return "delete";
    case jobs::JobKind::kExtractArchive:
        return "extract";
    case jobs::JobKind::kCreateArchive:
        return "compress";
    case jobs::JobKind::kImport:
        return "save";
    case jobs::JobKind::kVerifyArchive:
        return "test";
    default:
        return "finish";
    }
}

}  // namespace

DecisionSheets::DecisionSheets(runtime::AppServices &services, Overlays &overlays)
    : services_(services), overlays_(overlays) {
    std::weak_ptr<bool> alive = alive_;
    ui::UiLoop *loop = &services.loop;
    // On the job's thread: the sheet goes up on the UI thread.
    services.decisions.setListener([this, alive, loop] {
        loop->post([this, alive] {
            if (alive.lock()) {
                refresh();
            }
        });
    });
}

DecisionSheets::~DecisionSheets() {
    services_.decisions.setListener({});
    alive_.reset();
}

bool DecisionSheets::hasPending(jobs::JobId job) {
    for (const auto &pending : services_.decisions.pending()) {
        const jobs::JobId id = std::visit([](const auto &r) { return r.jobId; }, pending.request);
        if (id == job) {
            return true;
        }
    }
    return false;
}

void DecisionSheets::refresh() {
    const auto pending = services_.decisions.pending();
    if (showing_) {
        const bool still = std::ranges::any_of(pending, [&](const auto &p) { return p.id == *showing_; });
        if (!still && sheet_ != nullptr && sheet_->isOpen()) {
            // Its job was cancelled or answered elsewhere.
            close(true);
        }
        return;
    }
    for (const auto &p : pending) {
        if (!snoozed_.contains(p.id)) {
            show(p);
            return;
        }
    }
}

void DecisionSheets::showFor(std::optional<jobs::JobId> job) {
    const auto pending = services_.decisions.pending();
    const jobs::QueuedDecisionProvider::Pending *pick = nullptr;
    for (const auto &p : pending) {
        const jobs::JobId id = std::visit([](const auto &r) { return r.jobId; }, p.request);
        if (!job || id == *job) {
            pick = &p;
            break;
        }
    }
    if (pick == nullptr) {
        return;
    }
    snoozed_.erase(pick->id);
    if (showing_) {
        if (*showing_ == pick->id) {
            return;
        }
        // Answer the one that's up first; this one follows.
        return;
    }
    show(*pick);
}

void DecisionSheets::unsnooze() {
    snoozed_.clear();
    refresh();
}

void DecisionSheets::show(const jobs::QueuedDecisionProvider::Pending &pending) {
    // An extraction or test that hit an encrypted entry without (the right) password stops, and
    // the archive password sheet asks for one: the same sheet as browsing an encrypted archive.
    if (const auto *error = std::get_if<jobs::ErrorRequest>(&pending.request);
        error != nullptr &&
        (error->jobKind == jobs::JobKind::kExtractArchive || error->jobKind == jobs::JobKind::kVerifyArchive) &&
        error->error.code == EACCES && onNeedsPassword) {
        const jobs::JobId job = error->jobId;
        services_.decisions.resolve(pending.id, jobs::ErrorDecision{.action = ErrorAction::kAbort});
        onNeedsPassword(job);
        refresh();
        return;
    }
    showing_ = pending.id;
    answered_ = false;
    std::string title;
    NodePtr content;
    if (const auto *conflict = std::get_if<jobs::ConflictRequest>(&pending.request)) {
        title = conflict->targetEntry.isDirectoryLike() ? "Folder exists" : "File exists";
        content = conflictContent(pending.id, *conflict);
    } else {
        const auto &error = std::get<jobs::ErrorRequest>(pending.request);
        title = "Couldn't " + (error.operation.empty() ? std::string(operationVerb(error.jobKind)) : error.operation);
        content = errorContent(pending.id, error);
    }
    std::weak_ptr<bool> alive = alive_;
    sheet_ = &overlays_.showSheet(std::move(title), std::move(content), true, [this, alive, id = pending.id] {
        if (!alive.lock()) {
            return;
        }
        if (!answered_) {
            snoozed_.insert(id);
        }
        sheet_ = nullptr;
        showing_.reset();
        refresh();
    });
}

void DecisionSheets::close(bool answered) {
    answered_ = answered;
    if (sheet_ != nullptr) {
        sheet_->dismiss();
    }
}

NodePtr DecisionSheets::conflictContent(RequestId id, const jobs::ConflictRequest &request) {
    auto column = parts::sheetColumn(4);
    column->setDebugName("ConflictSheet");
    const std::string name(request.target.name());
    column->appendChild(parts::bodyText(inQuotes(name) + " is already in " + where(request.target) + "."));
    column->appendChild(parts::section("Incoming"));
    column->appendChild(parts::fileRow(request.sourceEntry, {}, compare(request.sourceEntry, request.targetEntry)));
    column->appendChild(parts::section("Existing"));
    column->appendChild(parts::fileRow(request.targetEntry, {}, compare(request.targetEntry, request.sourceEntry)));

    Signal<bool> all{false};
    auto checkbox = Checkbox::create({.checked = [all] { return all(); },
                                      .onChange = [all](bool on) { all.set(on); },
                                      .text = "Do this for every conflict in this job",
                                      .label = "Apply to all"});
    checkbox->updateStyle([](Style &s) { s.margin = {0, 6, 0, 0}; });
    column->appendChild(checkbox);

    auto answer = [this, id, all](ConflictAction action) {
        return [this, id, all, action] {
            services_.decisions.resolve(id, jobs::ConflictDecision{.action = action, .applyToAll = all.peek()});
            close(true);
        };
    };
    const bool merge = request.canMerge;
    auto keepBoth = Button::create({.label = "Keep both",
                                    .variant = merge ? ButtonVariant::glass : ButtonVariant::primary,
                                    .onTap = answer(ConflictAction::kKeepBoth)});
    auto skip = Button::create({.label = "Skip", .variant = ButtonVariant::glass, .onTap = answer(ConflictAction::kSkip)});
    auto replace = Button::create({.label = "Replace",
                                   .variant = ButtonVariant::glass,
                                   .onTap = answer(ConflictAction::kReplace),
                                   .accessibilityLabel = "Replace " + name});
    if (merge) {
        auto mergeButton = Button::create(
                {.label = "Merge", .variant = ButtonVariant::primary, .onTap = answer(ConflictAction::kMerge)});
        column->appendChild(parts::buttonRow({skip, keepBoth}));
        column->appendChild(parts::buttonRow({replace, mergeButton}));
    } else {
        column->appendChild(parts::buttonRow({skip, replace}));
        column->appendChild(parts::buttonRow({keepBoth}));
    }
    auto abort = Button::create({.label = "Cancel the " + std::string(operationVerb(request.jobKind)),
                                 .variant = ButtonVariant::text,
                                 .onTap = answer(ConflictAction::kAbort)});
    abort->updateStyle([](Style &s) {
        s.alignSelf = Align::center;
        s.margin = {0, 4, 0, 0};
    });
    column->appendChild(abort);
    return column;
}

NodePtr DecisionSheets::errorContent(RequestId id, const jobs::ErrorRequest &request) {
    auto column = parts::sheetColumn(8);
    column->setDebugName("ErrorSheet");
    column->appendChild(parts::bodyText(describeError(request.error)));
    column->appendChild(parts::monoText(printable(request.location), 2, false));

    Signal<bool> all{false};
    auto checkbox = Checkbox::create({.checked = [all] { return all(); },
                                      .onChange = [all](bool on) { all.set(on); },
                                      .text = "Skip every error like this in this job",
                                      .label = "Apply to all"});
    column->appendChild(checkbox);
    auto answer = [this, id, all](ErrorAction action) {
        return [this, id, all, action] {
            services_.decisions.resolve(id, jobs::ErrorDecision{.action = action, .applyToAll = all.peek()});
            close(true);
        };
    };
    std::vector<NodePtr> buttons;
    buttons.push_back(Button::create({.label = "Skip",
                                      .variant = request.canRetry ? ButtonVariant::glass : ButtonVariant::primary,
                                      .onTap = answer(ErrorAction::kSkip)}));
    if (request.canRetry) {
        buttons.push_back(Button::create(
                {.label = "Retry", .variant = ButtonVariant::primary, .icon = "refresh", .onTap = answer(ErrorAction::kRetry)}));
    }
    column->appendChild(parts::buttonRow(std::move(buttons)));
    auto abort = Button::create({.label = "Cancel the " + std::string(operationVerb(request.jobKind)),
                                 .variant = ButtonVariant::text,
                                 .onTap = answer(ErrorAction::kAbort)});
    abort->updateStyle([](Style &s) {
        s.alignSelf = Align::center;
        s.margin = {0, 4, 0, 0};
    });
    column->appendChild(abort);
    return column;
}

}  // namespace filepan::app
