// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/directory_model.hpp"

#include <algorithm>
#include <cerrno>
#include <utility>

namespace filepan::app {

namespace {

char fold(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// ASCII case-insensitive; other bytes (UTF-8) match exactly.
bool containsFolded(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                [](char a, char b) { return fold(a) == fold(b); });
    return it != haystack.end();
}

}  // namespace

DirectoryModel::DirectoryModel(ui::UiLoop &loop, ui::IoPool &io, vfs::BackendResolver &backends,
                               const vfs::NameCollator *collator)
    : loop_(loop), io_(io), backends_(backends), collator_(collator) {}

DirectoryModel::~DirectoryModel() {
    stop();
    for (auto &[name, job] : countJobs_) {
        job.cancel();
    }
}

void DirectoryModel::stop() {
    ++generation_;
    listCancel_.cancel();
    if (listJob_) {
        listJob_->cancel();
        listJob_.reset();
    }
    if (watchJob_) {
        watchJob_->cancel();
        watchJob_.reset();
    }
    // Waits for a running callback; it only posts, so this is short.
    watch_.reset();
    incoming_.reset();
}

void DirectoryModel::load(const vfs::Location &location) {
    stop();
    for (auto &[name, job] : countJobs_) {
        job.cancel();
    }
    countJobs_.clear();
    counts_.clear();
    location_ = location;
    entries_.clear();
    ui::batch([&] {
        error.set(std::nullopt);
        status.set(Status::loading);
        rebuild();
    });
    startListing(false);
}

void DirectoryModel::refresh() {
    if (!location_) {
        return;
    }
    stop();
    counts_.clear();
    ui::batch([&] {
        error.set(std::nullopt);
        status.set(Status::loading);
    });
    startListing(true);
}

void DirectoryModel::startListing(bool keepEntries) {
    const std::uint64_t generation = generation_;
    listCancel_ = CancellationToken();
    if (keepEntries) {
        incoming_.emplace();
    }
    std::weak_ptr<DirectoryModel> weak = weak_from_this();
    vfs::BackendResolver *backends = &backends_;
    ui::UiLoop *loop = &loop_;
    listJob_ = io_.submit(
            [backends, loop, weak, generation, location = *location_, cancel = listCancel_]() -> Result<void> {
                vfs::Backend *backend = backends->backendFor(location);
                if (backend == nullptr) {
                    return fail(ENOENT, "No backend for this location");
                }
                return backend->list(
                        location, {.minBatchSize = 128},
                        [loop, weak, generation](std::vector<vfs::Entry> batch) {
                            loop->post([weak, generation, batch = std::make_shared<std::vector<vfs::Entry>>(
                                                                   std::move(batch))]() mutable {
                                if (auto self = weak.lock()) {
                                    self->appendBatch(generation, std::move(*batch));
                                }
                            });
                        },
                        cancel);
            },
            [weak, generation](Result<void> result) {
                if (auto self = weak.lock()) {
                    self->listingDone(generation, std::move(result));
                }
            });
}

void DirectoryModel::appendBatch(std::uint64_t generation, std::vector<vfs::Entry> batch) {
    if (generation != generation_) {
        return;
    }
    if (incoming_) {
        // A refresh keeps showing the old entries until the new listing is complete.
        for (auto &entry : batch) {
            incoming_->push_back(std::move(entry));
        }
        return;
    }
    for (auto &entry : batch) {
        entries_.push_back(std::move(entry));
    }
    scheduleRebuild();
}

void DirectoryModel::listingDone(std::uint64_t generation, Result<void> result) {
    if (generation != generation_) {
        return;
    }
    listJob_.reset();
    if (!result) {
        if (result.error().isCancelled()) {
            return;
        }
        incoming_.reset();
        entries_.clear();
        ui::batch([&] {
            error.set(result.error());
            status.set(Status::error);
            rebuild();
        });
        return;
    }
    if (incoming_) {
        entries_ = std::move(*incoming_);
        incoming_.reset();
    }
    ui::batch([&] {
        rebuild();
        status.set(Status::loaded);
    });
    startWatch(generation);
}

void DirectoryModel::startWatch(std::uint64_t generation) {
    std::weak_ptr<DirectoryModel> weak = weak_from_this();
    vfs::BackendResolver *backends = &backends_;
    ui::UiLoop *loop = &loop_;
    // Watching may touch the file system (inotify_add_watch, an archive's parent), so it starts
    // on the pool; the handle comes back to the UI thread, which owns it.
    watchJob_ = io_.submit(
            [backends, loop, weak, generation, location = *location_]() -> std::shared_ptr<vfs::WatchHandle> {
                vfs::Backend *backend = backends->backendFor(location);
                if (backend == nullptr || !backend->capabilities().has(vfs::Capability::kWatch)) {
                    return nullptr;
                }
                auto handle = backend->watch(location, [loop, weak, generation](const vfs::DirectoryChange &change) {
                    loop->post([weak, generation, names = change.names, rescan = change.rescan,
                                gone = change.directoryGone] {
                        if (auto self = weak.lock()) {
                            self->onChange(generation, names, rescan, gone);
                        }
                    });
                });
                return handle ? std::shared_ptr<vfs::WatchHandle>(std::move(*handle)) : nullptr;
            },
            [weak, generation](std::shared_ptr<vfs::WatchHandle> handle) {
                auto self = weak.lock();
                if (!self || generation != self->generation_ || !handle) {
                    return;
                }
                self->watchJob_.reset();
                // Owned uniquely from here: the pool's copy is gone once this runs.
                struct Holder final : vfs::WatchHandle {
                    std::shared_ptr<vfs::WatchHandle> inner;
                };
                auto holder = std::make_unique<Holder>();
                holder->inner = std::move(handle);
                self->watch_ = std::move(holder);
            });
}

void DirectoryModel::onChange(std::uint64_t generation, std::vector<std::string> names, bool rescan, bool gone) {
    if (generation != generation_) {
        return;
    }
    if (gone) {
        stop();
        entries_.clear();
        ui::batch([&] {
            error.set(Error{ENOENT, "The folder was moved or deleted"});
            status.set(Status::error);
            rebuild();
        });
        return;
    }
    if (rescan) {
        refresh();
        return;
    }
    vfs::BackendResolver *backends = &backends_;
    std::weak_ptr<DirectoryModel> weak = weak_from_this();
    io_.submit(
            [backends, location = *location_, names = std::move(names)] {
                std::vector<std::pair<std::string, std::optional<vfs::Entry>>> stats;
                vfs::Backend *backend = backends->backendFor(location);
                for (const std::string &name : names) {
                    std::optional<vfs::Entry> entry;
                    if (backend != nullptr) {
                        if (auto stat = backend->lstat(location.child(name))) {
                            entry = std::move(*stat);
                            entry->name = name;
                        }
                    }
                    stats.emplace_back(name, std::move(entry));
                }
                return stats;
            },
            [weak, generation](std::vector<std::pair<std::string, std::optional<vfs::Entry>>> stats) {
                if (auto self = weak.lock()) {
                    self->applyStats(generation, std::move(stats));
                }
            });
}

void DirectoryModel::applyStats(std::uint64_t generation,
                                std::vector<std::pair<std::string, std::optional<vfs::Entry>>> stats) {
    if (generation != generation_) {
        return;
    }
    for (auto &[name, entry] : stats) {
        const auto at = std::ranges::find_if(entries_, [&](const vfs::Entry &e) { return e.name == name; });
        counts_.erase(name);
        if (!entry) {
            if (at != entries_.end()) {
                entries_.erase(at);
            }
        } else if (at != entries_.end()) {
            *at = std::move(*entry);
        } else {
            entries_.push_back(std::move(*entry));
        }
    }
    rebuild();
}

void DirectoryModel::setOptions(Options options) {
    if (options == options_) {
        return;
    }
    options_ = std::move(options);
    rebuild();
}

void DirectoryModel::scheduleRebuild() {
    if (rebuildPosted_) {
        return;
    }
    rebuildPosted_ = true;
    std::weak_ptr<DirectoryModel> weak = weak_from_this();
    loop_.post([weak] {
        if (auto self = weak.lock()) {
            self->rebuildPosted_ = false;
            self->rebuild();
        }
    });
}

void DirectoryModel::rebuild() {
    visible_.clear();
    visibleIndex_.clear();
    for (const vfs::Entry &entry : entries_) {
        if ((entry.hidden || userHidden(entry.name)) && !options_.showHidden) {
            continue;
        }
        if (!containsFolded(entry.name, options_.filter)) {
            continue;
        }
        visible_.push_back(entry);
    }
    vfs::SortOptions sort{.key = options_.sort.key,
                          .descending = options_.sort.descending,
                          .directoriesFirst = options_.sort.directoriesFirst,
                          .collator = collator_};
    vfs::sortEntries(visible_, sort);
    folders_ = static_cast<std::size_t>(
            std::ranges::count_if(visible_, [](const vfs::Entry &e) { return e.isDirectoryLike(); }));
    for (std::size_t i = 0; i < visible_.size(); ++i) {
        visibleIndex_.emplace(visible_[i].name, i);
    }
    version.set(version.peek() + 1);
}

bool DirectoryModel::userHidden(const std::string &name) const {
    return std::ranges::binary_search(options_.hiddenNames, name);
}

std::optional<std::size_t> DirectoryModel::indexOf(const std::string &name) const {
    const auto found = visibleIndex_.find(name);
    return found == visibleIndex_.end() ? std::nullopt : std::optional(found->second);
}

std::optional<std::uint64_t> DirectoryModel::itemCount(const std::string &name) const {
    const auto found = counts_.find(name);
    return found == counts_.end() ? std::nullopt : std::optional(found->second);
}

void DirectoryModel::requestCount(const std::string &name) {
    if (!location_ || counts_.contains(name) || countJobs_.contains(name)) {
        return;
    }
    vfs::BackendResolver *backends = &backends_;
    std::weak_ptr<DirectoryModel> weak = weak_from_this();
    const std::uint64_t generation = generation_;
    CancellationToken cancel;
    auto job = io_.submit(
            [backends, location = location_->child(name), cancel]() -> std::optional<std::uint64_t> {
                vfs::Backend *backend = backends->backendFor(location);
                if (backend == nullptr) {
                    return std::nullopt;
                }
                std::uint64_t count = 0;
                auto listed = backend->list(location, {.withStat = false, .statThreads = 1},
                                            [&count](std::vector<vfs::Entry> batch) { count += batch.size(); },
                                            cancel);
                return listed ? std::optional(count) : std::nullopt;
            },
            [weak, generation, name](std::optional<std::uint64_t> count) {
                auto self = weak.lock();
                if (!self) {
                    return;
                }
                self->countJobs_.erase(name);
                if (count && generation == self->generation_) {
                    self->counts_[name] = *count;
                    self->countVersion.set(self->countVersion.peek() + 1);
                }
            });
    // The pool's token stops a job that hasn't started; the listing's stops one that has.
    countJobs_.insert_or_assign(name, CountJob{std::move(job), cancel});
}

void DirectoryModel::CountJob::cancel() const {
    job->cancel();
    token.cancel();
}

void DirectoryModel::cancelCountsExcept(const std::vector<std::string> &keep) {
    std::erase_if(countJobs_, [&](const auto &item) {
        if (std::ranges::find(keep, item.first) != keep.end()) {
            return false;
        }
        item.second.cancel();
        return true;
    });
}

}  // namespace filepan::app
