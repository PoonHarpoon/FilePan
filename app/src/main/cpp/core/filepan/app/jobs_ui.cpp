// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/jobs_ui.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>

#include "filepan/app/format.hpp"
#include "filepan/app/sheet_parts.hpp"
#include "filepan/ui/components/components.hpp"

namespace filepan::app {

using namespace ui;
using namespace ui::components;
using jobs::JobKind;
using jobs::JobState;

const char *jobVerb(JobKind kind) {
    switch (kind) {
    case JobKind::kCopy:
        return "Copying";
    case JobKind::kMove:
        return "Moving";
    case JobKind::kDelete:
        return "Deleting";
    case JobKind::kMakeDirectory:
        return "Creating folder";
    case JobKind::kCreateFile:
        return "Creating file";
    case JobKind::kRename:
        return "Renaming";
    case JobKind::kExtractArchive:
        return "Extracting";
    case JobKind::kCreateArchive:
        return "Compressing";
    case JobKind::kChecksum:
        return "Hashing";
    case JobKind::kImport:
        return "Saving";
    case JobKind::kVerifyArchive:
        return "Testing";
    }
    return "Working";
}

const char *jobPastVerb(JobKind kind) {
    switch (kind) {
    case JobKind::kCopy:
        return "Copied";
    case JobKind::kMove:
        return "Moved";
    case JobKind::kDelete:
        return "Deleted";
    case JobKind::kMakeDirectory:
        return "Created folder";
    case JobKind::kCreateFile:
        return "Created file";
    case JobKind::kRename:
        return "Renamed";
    case JobKind::kExtractArchive:
        return "Extracted";
    case JobKind::kCreateArchive:
        return "Compressed";
    case JobKind::kChecksum:
        return "Hashed";
    case JobKind::kImport:
        return "Saved";
    case JobKind::kVerifyArchive:
        return "Tested";
    }
    return "Done";
}

std::string formatRate(double bytesPerSecond) {
    return formatSize(static_cast<std::uint64_t>(std::max(0.0, bytesPerSecond))) + "/s";
}

std::string formatEta(std::chrono::seconds eta) {
    const long long s = std::max<long long>(0, eta.count());
    if (s < 60) {
        return std::to_string(std::max<long long>(1, s)) + " s left";
    }
    if (s < 3600) {
        return std::to_string((s + 30) / 60) + " min left";
    }
    const long long minutes = (s + 30) / 60;
    return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min left";
}

namespace {

bool running(const JobView &view) { return !jobs::isFinished(view.progress.state); }

// "12 of 40", for the band's sentence.
std::string ofTotal(std::uint64_t done, std::uint64_t total) {
    return std::to_string(std::min(done, total)) + " of " + std::to_string(total);
}

const char *kindGlyph(JobKind kind);

std::vector<const jobs::JobProgress *> runningJobs(const std::vector<JobView> &views) {
    std::vector<const jobs::JobProgress *> active;
    for (const JobView &view : views) {
        if (running(view)) {
            active.push_back(&view.progress);
        }
    }
    return active;
}

}  // namespace

std::string bandText(const std::vector<JobView> &views) {
    const auto active = runningJobs(views);
    if (active.empty()) {
        return {};
    }
    std::size_t waiting = 0;
    bool scanning = false;
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
    std::uint64_t itemsDone = 0;
    std::uint64_t itemsTotal = 0;
    for (const auto *p : active) {
        waiting += p->state == JobState::kWaitingForDecision ? 1 : 0;
        scanning = scanning || p->scanning;
        bytesDone += std::min(p->bytesDone, p->bytesTotal);
        bytesTotal += p->bytesTotal;
        itemsDone += p->itemsDone;
        itemsTotal += p->itemsTotal;
    }
    std::string text;
    if (active.size() == 1) {
        const jobs::JobProgress &p = *active.front();
        text = jobVerb(p.kind);
        if (p.state == JobState::kWaitingForDecision) {
            return text + " · waiting for you";
        }
        if (p.state == JobState::kQueued) {
            return text + " · queued";
        }
        if (p.scanning) {
            return text + " · counting" + (p.itemsTotal > 0 ? " " + std::to_string(p.itemsTotal) : std::string());
        }
        // Bytes where the job moves any, since items vary too much in size to show progress.
        if (p.bytesTotal > 0) {
            text += " " + formatSizeOf(p.bytesDone, p.bytesTotal);
        } else if (p.itemsTotal > 1) {
            text += " " + ofTotal(p.itemsDone, p.itemsTotal);
        }
    } else {
        text = std::to_string(active.size()) + " jobs";
        if (waiting > 0) {
            return text + " · " + std::to_string(waiting) + " waiting for you";
        }
        if (scanning) {
            text += " · counting";
        } else if (bytesTotal > 0) {
            text += " · " + formatSizeOf(bytesDone, bytesTotal);
        } else if (itemsTotal > 0) {
            text += " · " + ofTotal(itemsDone, itemsTotal);
        }
    }
    return text;
}

std::string bandRate(const std::vector<JobView> &views) {
    double rate = 0;
    for (const auto *p : runningJobs(views)) {
        rate += p->state == JobState::kRunning ? p->bytesPerSecond : 0;
    }
    return rate >= 1 ? formatRate(rate) : std::string();
}

std::string bandIcon(const std::vector<JobView> &views) {
    const auto active = runningJobs(views);
    return active.size() == 1 ? kindGlyph(active.front()->kind) : "sync";
}

float jobFraction(const jobs::JobProgress &p) {
    if (jobs::isFinished(p.state)) {
        return 1;
    }
    if (p.scanning) {
        return -1;
    }
    if (p.bytesTotal > 0) {
        return std::clamp(static_cast<float>(static_cast<double>(p.bytesDone) / static_cast<double>(p.bytesTotal)), 0.0f,
                          1.0f);
    }
    if (p.itemsTotal > 0) {
        return std::clamp(static_cast<float>(static_cast<double>(p.itemsDone) / static_cast<double>(p.itemsTotal)), 0.0f,
                          1.0f);
    }
    return 0;
}

float bandProgress(const std::vector<JobView> &views) {
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
    std::uint64_t itemsDone = 0;
    std::uint64_t itemsTotal = 0;
    bool any = false;
    for (const JobView &view : views) {
        if (!running(view)) {
            continue;
        }
        any = true;
        const jobs::JobProgress &p = view.progress;
        if (p.scanning) {
            return 0;
        }
        bytesDone += std::min(p.bytesDone, p.bytesTotal);
        bytesTotal += p.bytesTotal;
        itemsDone += std::min(p.itemsDone, p.itemsTotal);
        itemsTotal += p.itemsTotal;
    }
    if (!any) {
        return -1;
    }
    if (bytesTotal > 0) {
        return static_cast<float>(static_cast<double>(bytesDone) / static_cast<double>(bytesTotal));
    }
    if (itemsTotal > 0) {
        return static_cast<float>(static_cast<double>(itemsDone) / static_cast<double>(itemsTotal));
    }
    return -1;
}

std::string describeError(const Error &error) {
    std::string text = error.code == 0 ? std::string("Something went wrong") : std::string(std::strerror(error.code));
    if (!error.message.empty()) {
        text += ": " + error.message;
    }
    if (!text.empty() && text.back() != '.') {
        text += '.';
    }
    return text;
}

std::string jobDetail(const jobs::JobProgress &p) {
    std::vector<std::string> parts;
    // The parts from here on go on a second line, so that the sizes never wrap mid-part.
    std::size_t secondLine = SIZE_MAX;
    switch (p.state) {
    case JobState::kQueued:
        return "Starts when a running job is done";
    case JobState::kFailed:
        return p.error ? describeError(*p.error) : "Failed.";
    case JobState::kCancelled:
        return "Cancelled";
    case JobState::kSucceeded:
        if (p.itemsTotal > 0) {
            parts.push_back(formatCount(p.itemsTotal));
        }
        if (p.bytesTotal > 0) {
            parts.push_back(formatSize(p.bytesTotal));
        }
        if (p.itemsSkipped > 0) {
            parts.push_back(std::to_string(p.itemsSkipped) + " skipped");
        }
        if (parts.empty()) {
            parts.push_back("Done");
        }
        break;
    case JobState::kWaitingForDecision:
    case JobState::kRunning:
        if (p.state == JobState::kWaitingForDecision) {
            parts.push_back("Waiting for you");
        }
        if (p.scanning) {
            parts.push_back("Counting");
            if (p.itemsTotal > 0) {
                parts.push_back(formatCount(p.itemsTotal));
            }
            if (p.bytesTotal > 0) {
                parts.push_back(formatSize(p.bytesTotal));
            }
            break;
        }
        if (p.bytesTotal > 0) {
            parts.push_back(formatSizeOf(p.bytesDone, p.bytesTotal));
            secondLine = parts.size();
        }
        if (p.itemsTotal > 1) {
            parts.push_back(std::to_string(std::min(p.itemsDone, p.itemsTotal)) + "/" + formatCount(p.itemsTotal));
        }
        if (p.state == JobState::kRunning && p.bytesPerSecond >= 1) {
            parts.push_back(formatRate(p.bytesPerSecond));
        }
        if (p.state == JobState::kRunning && p.eta) {
            parts.push_back(formatEta(*p.eta));
        }
        break;
    }
    std::string text;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        text += (i == 0 ? "" : i == secondLine ? "\n" : " · ") + parts[i];
    }
    return text;
}

// --- JobsModel ---

JobsModel::JobsModel(runtime::AppServices &services) : services_(services) {
    subscription_ = services.jobEvents.subscribe([this](const jobs::JobProgress &progress) { onProgress(progress); });
}

JobsModel::~JobsModel() { services_.jobEvents.unsubscribe(subscription_); }

jobs::JobId JobsModel::submit(jobs::JobSpec spec, JobMeta meta) {
    const JobKind kind = jobs::kindOf(spec);
    const jobs::JobId id = services_.jobs.submit(std::move(spec));
    if (meta.title.empty()) {
        meta.title = jobVerb(kind);
    }
    const bool quiet = meta.quiet;
    JobView view{.id = id, .title = meta.title, .progress = {.id = id, .kind = kind}};
    meta_[id] = std::move(meta);
    if (!quiet) {
        list.update([&](std::vector<JobView> &views) { views.push_back(std::move(view)); });
        if (onSubmitted) {
            onSubmitted();
        }
    }
    return id;
}

void JobsModel::track(jobs::JobId id, JobMeta meta) { meta_.insert_or_assign(id, std::move(meta)); }

void JobsModel::onProgress(const jobs::JobProgress &progress) {
    auto found = meta_.find(progress.id);
    if (found == meta_.end()) {
        // Submitted by someone else: shown by its kind.
        found = meta_.emplace(progress.id, JobMeta{.title = jobVerb(progress.kind)}).first;
    }
    const bool quiet = found->second.quiet;
    if (found->second.onProgress && !jobs::isFinished(progress.state)) {
        found->second.onProgress(progress);
    }
    std::optional<JobView> view;
    if (!quiet) {
        list.update([&](std::vector<JobView> &views) {
            auto at = std::ranges::find_if(views, [&](const JobView &v) { return v.id == progress.id; });
            if (at == views.end()) {
                views.push_back({.id = progress.id, .title = found->second.title});
                at = views.end() - 1;
            }
            at->progress = progress;
            view = *at;
        });
    }
    if (!jobs::isFinished(progress.state)) {
        return;
    }
    JobMeta meta = std::move(found->second);
    meta_.erase(found);
    if (meta.onFinished) {
        meta.onFinished(progress);
    }
    services_.jobs.forget(progress.id);
    if (!quiet) {
        // Old finished jobs go first.
        list.update([&](std::vector<JobView> &views) {
            std::size_t finished = std::ranges::count_if(views, [](const JobView &v) { return !running(v); });
            for (auto it = views.begin(); it != views.end() && finished > kKeepFinished;) {
                if (!running(*it)) {
                    it = views.erase(it);
                    --finished;
                } else {
                    ++it;
                }
            }
        });
    }
    changed_ = meta.changes;
    changes.set(changes.peek() + 1);
    if (!quiet && onFinished && view) {
        onFinished(*view, meta);
    }
}

void JobsModel::cancel(jobs::JobId id) { services_.jobs.cancel(id); }

void JobsModel::dismiss(jobs::JobId id) {
    list.update([&](std::vector<JobView> &views) {
        std::erase_if(views, [&](const JobView &v) { return v.id == id && !running(v); });
    });
}

void JobsModel::dismissFinished() {
    list.update([](std::vector<JobView> &views) { std::erase_if(views, [](const JobView &v) { return !running(v); }); });
}

bool JobsModel::active() const {
    return std::ranges::any_of(list(), [](const JobView &v) { return running(v); });
}

std::optional<JobView> JobsModel::find(jobs::JobId id) const {
    for (const JobView &view : list.peek()) {
        if (view.id == id) {
            return view;
        }
    }
    return std::nullopt;
}

// --- The band ---

NodePtr jobBand(JobsModel &model, std::function<void()> onTap) {
    JobsModel *m = &model;
    auto band = DataBand::create({.text = [m] { return bandText(m->list()); },
                                  .rate = [m] { return bandRate(m->list()); },
                                  .icon = [m] { return bandIcon(m->list()); },
                                  .progress = [m] { return bandProgress(m->list()); },
                                  .label = "File jobs",
                                  .onTap = std::move(onTap)});
    const Theme &t = currentTheme();
    band->updateStyle([&t](Style &s) {
        s.display = false;
        s.position = Position::absolute;
        s.left = t.spacing.dockInset;
        s.right = t.spacing.dockInset;
    });
    band->bind([b = band.get(), m] {
        const bool show = m->active();
        const float density = environment().density();
        const float bottom = environment().safeArea().bottom / density;
        const Theme &theme = ui::components::theme();
        untrack([&] {
            // Utilitarian: edge to edge, right on the status line.
            const bool edge = theme.style.utilitarian;
            b->updateStyle([&](Style &s) {
                s.display = show;
                s.left = edge ? -1 : theme.spacing.dockInset;
                s.right = edge ? -1 : theme.spacing.dockInset;
                s.bottom = bottom + theme.spacing.dockInset + theme.spacing.dock +
                           (edge ? theme.spacing.statusLine - 1 : 10);
            });
        });
    });
    return band;
}

// --- The jobs sheet ---

namespace {

struct StampLook {
    std::string text;
    StampTone tone = StampTone::ink;
    bool operator==(const StampLook &) const = default;
};

StampLook stampFor(const jobs::JobProgress &p) {
    switch (p.state) {
    case JobState::kQueued:
        return {"Queued", StampTone::ink};
    case JobState::kWaitingForDecision:
        return {"Waiting", StampTone::secondary};
    case JobState::kRunning: {
        const float f = jobFraction(p);
        return {f < 0 ? "Counting" : std::to_string(static_cast<int>(f * 100)) + "%", StampTone::accent};
    }
    case JobState::kSucceeded:
        return {"Done", StampTone::accent};
    case JobState::kFailed:
        return {"Failed", StampTone::destructive};
    case JobState::kCancelled:
        return {"Cancelled", StampTone::ink};
    }
    return {};
}

const char *kindGlyph(JobKind kind) {
    switch (kind) {
    case JobKind::kCopy:
        return "content_copy";
    case JobKind::kMove:
        return "drive_file_move";
    case JobKind::kDelete:
        return "delete";
    case JobKind::kMakeDirectory:
        return "create_new_folder";
    case JobKind::kCreateFile:
        return "note_add";
    case JobKind::kRename:
        return "drive_file_rename_outline";
    case JobKind::kExtractArchive:
        return "unarchive";
    case JobKind::kCreateArchive:
        return "archive";
    case JobKind::kChecksum:
        return "fingerprint";
    case JobKind::kImport:
        return "save";
    case JobKind::kVerifyArchive:
        return "fact_check";
    }
    return "sync";
}

NodePtr jobCard(Signal<JobView> view, JobsModel *model, std::function<void(jobs::JobId)> onDecide) {
    const Theme &t = currentTheme();
    auto card = Box::create({.gap = 6, .padding = {14, 10, 6, 12}},
                            {.background = gfx::Paint{t.colors.film.withAlpha(0.05f)},
                             .radii = gfx::CornerRadii::uniform(kCorner),
                             .borderWidth = 1,
                             .border = t.colors.film.withAlpha(0.09f)});
    card->setDebugName("JobCard");
    auto top = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 10});
    auto glyph = Icon::create(kindGlyph(view.peek().progress.kind), 20);
    auto title = Text::create(view.peek().title, t.type.body);
    title->setMaxLines(1);
    title->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    title->updateStyle([](Style &s) {
        s.grow = 1;
        s.shrink = 1;
        s.basis = 0;
    });
    auto stampSlot = Box::create();
    const jobs::JobId id = view.peek().id;
    auto cancel = IconButton::create({.icon = "close",
                                      .label = "Cancel " + view.peek().title,
                                      .style = IconButtonStyle::plain,
                                      .size = 40,
                                      .onTap = [model, id] { model->cancel(id); }});
    auto dismiss = IconButton::create({.icon = "done",
                                       .label = "Dismiss " + view.peek().title,
                                       .style = IconButtonStyle::plain,
                                       .size = 40,
                                       .onTap = [model, id] { model->dismiss(id); }});
    top->appendChild(glyph);
    top->appendChild(title);
    top->appendChild(stampSlot);
    top->appendChild(cancel);
    top->appendChild(dismiss);
    card->appendChild(top);

    auto lower = Box::create({.gap = 6, .padding = {30, 0, 8, 0}});
    // Mixed case: uppercasing would turn "MiB" into "MIB".
    auto detail = parts::monoText({}, 2, false);
    auto track = parts::progressTrack([view] { return jobFraction(view().progress); });
    track->updateStyle([](Style &s) { s.margin = {0, 2, 0, 2}; });
    auto current = parts::monoText({}, 1, false);
    auto error = Text::create({}, t.type.body);
    auto decide = Button::create({.label = "Decide",
                                  .variant = ButtonVariant::outline,
                                  .icon = "help",
                                  .onTap = [onDecide, id] { onDecide(id); },
                                  .accessibilityLabel = "Decide what to do"});
    decide->updateStyle([](Style &s) {
        s.alignSelf = Align::start;
        s.margin = {0, 4, 0, 0};
    });
    lower->appendChild(detail);
    lower->appendChild(track);
    lower->appendChild(current);
    lower->appendChild(error);
    lower->appendChild(decide);
    card->appendChild(lower);

    auto look = std::make_shared<std::optional<StampLook>>();
    Signal<std::string> stampText{std::string()};
    card->bind([=, glyph = glyph.get(), title = title.get(), slot = stampSlot.get(), cancel = cancel.get(),
                dismiss = dismiss.get(), detail = detail.get(), track = track.get(), current = current.get(),
                error = error.get(), decide = decide.get()] {
        const JobView &v = view();
        const Theme &th = theme();
        untrack([&] {
            const jobs::JobProgress &p = v.progress;
            const bool live = !jobs::isFinished(p.state);
            title->setText(v.title);
            title->setColor(ink(th, th.text.primary));
            glyph->setColor(p.state == JobState::kFailed ? legible(th.colors.destructive, th.colors.ground)
                                                          : legible(th.colors.accent, th.colors.ground));
            const StampLook next = stampFor(p);
            if (*look != next) {
                stampText.set(next.text);
                if (!*look || (*look)->tone != next.tone) {
                    slot->clearChildren();
                    slot->appendChild(Stamp([stampText] { return stampText(); }, next.tone));
                }
                *look = next;
            }
            cancel->updateStyle([&](Style &s) { s.display = live; });
            dismiss->updateStyle([&](Style &s) { s.display = !live; });
            const bool failed = p.state == JobState::kFailed;
            detail->setText(failed ? std::string() : jobDetail(p));
            detail->updateStyle([&](Style &s) { s.display = !failed; });
            track->updateStyle([&](Style &s) { s.display = live && p.state != JobState::kQueued; });
            track->markPaintDirty();
            current->setText(p.currentItem);
            current->updateStyle([&](Style &s) { s.display = live && !p.currentItem.empty(); });
            error->setText(failed ? jobDetail(p) : std::string());
            error->setColor(legible(th.colors.destructive, th.colors.ground));
            error->updateStyle([&](Style &s) { s.display = failed; });
            decide->updateStyle([&](Style &s) { s.display = p.state == JobState::kWaitingForDecision; });
        });
    });
    return card;
}

}  // namespace

NodePtr jobsSheet(JobsModel &model, std::function<void(jobs::JobId id)> onDecide) {
    JobsModel *m = &model;
    auto column = parts::sheetColumn(10);
    column->setDebugName("JobsSheet");
    auto cards = Box::create({.gap = 10});
    auto empty = parts::bodyText("Nothing running. Copies, moves, extractions and other file jobs show up here.");
    auto clear = Button::create({.label = "Clear finished",
                                 .variant = ButtonVariant::glass,
                                 .icon = "clear_all",
                                 .onTap = [m] { m->dismissFinished(); }});
    auto footer = parts::buttonRow({clear});
    column->appendChild(cards);
    column->appendChild(empty);
    column->appendChild(footer);

    struct Slot {
        NodePtr node;
        Signal<JobView> view;
    };
    auto slots = std::make_shared<std::map<jobs::JobId, Slot>>();
    auto order = std::make_shared<std::vector<jobs::JobId>>();
    column->bind([m, slots, order, onDecide, cards = cards.get(), empty = empty.get(), footer = footer.get()] {
        const std::vector<JobView> &views = m->list();
        untrack([&] {
            std::vector<jobs::JobId> ids;
            for (const JobView &v : views) {
                ids.push_back(v.id);
                if (auto found = slots->find(v.id); found != slots->end()) {
                    found->second.view.set(v);
                } else {
                    Signal<JobView> signal(v);
                    Slot slot{nullptr, signal};
                    cards->build([&] { slot.node = jobCard(signal, m, onDecide); });
                    slots->emplace(v.id, std::move(slot));
                }
            }
            for (auto it = slots->begin(); it != slots->end();) {
                it = std::ranges::find(ids, it->first) == ids.end() ? slots->erase(it) : std::next(it);
            }
            if (ids != *order) {
                cards->clearChildren();
                for (jobs::JobId id : ids) {
                    cards->appendChild(slots->at(id).node);
                }
                *order = ids;
            }
            empty->updateStyle([&](Style &s) { s.display = views.empty(); });
            const bool anyFinished =
                    std::ranges::any_of(views, [](const JobView &v) { return jobs::isFinished(v.progress.state); });
            footer->updateStyle([&](Style &s) { s.display = anyFinished; });
        });
    });
    return parts::scrolling(column);
}

}  // namespace filepan::app
