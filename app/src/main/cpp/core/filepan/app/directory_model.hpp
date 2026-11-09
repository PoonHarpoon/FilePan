// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "filepan/app/settings.hpp"
#include "filepan/cancellation.hpp"
#include "filepan/error.hpp"
#include "filepan/ui/reactive.hpp"
#include "filepan/ui/ui_loop.hpp"
#include "filepan/vfs/backend.hpp"
#include "filepan/vfs/sort.hpp"

// One folder's contents for the browsing screen, kept current:
//
// - Listing streams: the backend's batches are listed on the I/O pool and appended on the UI
//   thread as they arrive, so a huge folder shows its first entries at once. Loading another
//   folder (or destroying the model) cancels the listing.
// - Once listed, the folder is watched (where the backend can): changed names are stat'ed on the
//   I/O pool and patched in; a rescan relists; a deleted folder becomes an error.
// - visible() is the entries after the hidden-file rule (dot files and what the user hid) and the filter, sorted with the
//   settings' order and collator. version bumps whenever it changes.
// - Folder item counts are computed lazily, on request, and can be cancelled.
//
// UI thread only; work on the pool never touches the model, it posts back.
namespace filepan::app {

class DirectoryModel : public std::enable_shared_from_this<DirectoryModel> {
public:
    enum class Status : std::uint8_t { idle, loading, loaded, error };

    DirectoryModel(ui::UiLoop &loop, ui::IoPool &io, vfs::BackendResolver &backends,
                   const vfs::NameCollator *collator = nullptr);
    ~DirectoryModel();
    DirectoryModel(const DirectoryModel &) = delete;
    DirectoryModel &operator=(const DirectoryModel &) = delete;

    // Starts listing location, dropping what was there.
    void load(const vfs::Location &location);
    // Lists the same folder again, keeping the entries until the new listing replaces them.
    void refresh();
    // Stops listing and watching.
    void stop();

    struct Options {
        SortSettings sort;
        bool showHidden = false;
        std::string filter;  // Case-insensitive substring of the name; empty for all.
        // Names here the user hid (user_items.hpp), sorted: left out like dot files unless
        // showHidden.
        std::vector<std::string> hiddenNames;

        bool operator==(const Options &) const = default;
    };
    void setOptions(Options options);
    [[nodiscard]] const Options &options() const { return options_; }

    ui::Signal<Status> status{Status::idle};
    ui::Signal<std::optional<Error>> error;
    // Bumps when visible() changes.
    ui::Signal<std::uint64_t> version{0};

    [[nodiscard]] const std::optional<vfs::Location> &location() const { return location_; }
    [[nodiscard]] const std::vector<vfs::Entry> &visible() const { return visible_; }
    // How many of visible() are folders: with folders first, they lead.
    [[nodiscard]] std::size_t folderCount() const { return folders_; }
    // Everything listed, hidden files included.
    [[nodiscard]] std::size_t totalCount() const { return entries_.size(); }
    [[nodiscard]] std::optional<std::size_t> indexOf(const std::string &name) const;
    // Whether the user hid this name here (Options::hiddenNames).
    [[nodiscard]] bool userHidden(const std::string &name) const;
    [[nodiscard]] bool watching() const { return watch_ != nullptr; }

    // Folder item counts (all entries, hidden ones included).
    [[nodiscard]] std::optional<std::uint64_t> itemCount(const std::string &name) const;
    // Counts the named folder's items on the pool unless known or pending; the result bumps
    // countVersion.
    void requestCount(const std::string &name);
    // Cancels pending counts except for these names.
    void cancelCountsExcept(const std::vector<std::string> &keep);
    ui::Signal<std::uint64_t> countVersion{0};
    [[nodiscard]] std::size_t pendingCounts() const { return countJobs_.size(); }

private:
    void startListing(bool keepEntries);
    void appendBatch(std::uint64_t generation, std::vector<vfs::Entry> batch);
    void listingDone(std::uint64_t generation, Result<void> result);
    void startWatch(std::uint64_t generation);
    void onChange(std::uint64_t generation, std::vector<std::string> names, bool rescan, bool gone);
    void applyStats(std::uint64_t generation, std::vector<std::pair<std::string, std::optional<vfs::Entry>>> stats);
    void scheduleRebuild();
    void rebuild();

    ui::UiLoop &loop_;
    ui::IoPool &io_;
    vfs::BackendResolver &backends_;
    const vfs::NameCollator *collator_;

    std::optional<vfs::Location> location_;
    std::uint64_t generation_ = 0;
    CancellationToken listCancel_;
    std::shared_ptr<ui::Cancellation> listJob_;
    std::vector<vfs::Entry> entries_;
    // While a refresh relists: the entries collected so far, swapped in when it's done.
    std::optional<std::vector<vfs::Entry>> incoming_;
    std::unique_ptr<vfs::WatchHandle> watch_;
    std::shared_ptr<ui::Cancellation> watchJob_;

    Options options_;
    std::vector<vfs::Entry> visible_;
    std::size_t folders_ = 0;
    std::unordered_map<std::string, std::size_t> visibleIndex_;
    bool rebuildPosted_ = false;

    std::unordered_map<std::string, std::uint64_t> counts_;
    struct CountJob {
        std::shared_ptr<ui::Cancellation> job;
        CancellationToken token;
        void cancel() const;
    };
    std::unordered_map<std::string, CountJob> countJobs_;
};

}  // namespace filepan::app
