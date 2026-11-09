// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "filepan/app/decision_sheets.hpp"
#include "filepan/app/file_actions.hpp"
#include "filepan/app/jobs_ui.hpp"
#include "filepan/runtime/app_host.hpp"
#include "filepan/runtime/shared_items.hpp"
#include "filepan/ui/ui_root.hpp"

// FileActions for real: every action becomes a job on the JobQueue, after a dialog or sheet
// where it needs one.
//
// - Copy and move put the targets on the app clipboard, which the dock shows as a stamp
//   (`3 TO PASTE`, tap to paste here, × to clear); paste copies or moves them into the folder.
// - Delete confirms in a sheet; rename, new folder and new file ask for a name in a dialog.
// - Extract and compress have their own sheets; properties too (properties_sheet.hpp). Testing an
//   archive needs none: a snackbar says whether it's intact, with the damage a tap away.
// - Share and open hand local files to other apps. Files in archives or on servers are first
//   copied into the app's cache (filesDir/cache/open, trimmed oldest first), as a job.
// - What other apps share to FilePan waits on a stamp of its own (`2 TO SAVE`, × to drop it);
//   saving it here asks for the name of a single item, then imports it as a job, and goes back
//   to the app that shared it once it's saved. Leaving FilePan with back drops it.
// - Jobs show on the data band and in the jobs sheet (jobs_ui.hpp); a finished one shows a
//   snackbar, a failed one an error snackbar with details. Their conflicts and errors become
//   sheets (decision_sheets.hpp).
namespace filepan::app {

class AppState;
class Overlays;

// Whether a finished compress job allows deleting its sources: it must have succeeded with
// nothing skipped, so every original is in the archive.
bool shouldDeleteSourcesAfterCompress(const jobs::JobProgress &progress);

struct Clipboard {
    std::vector<vfs::Location> items;
    // Move rather than copy.
    bool cut = false;

    bool operator==(const Clipboard &) const = default;
};

class FileOperations final : public FileActions {
public:
    // The cache keeps at most this much, oldest first out.
    static constexpr std::uint64_t kCacheBytes = 512ull << 20;
    static constexpr std::size_t kCacheEntries = 64;

    FileOperations(runtime::AppServices &services, AppState &state, Overlays &overlays);
    ~FileOperations() override;

    void copy(const FileTargets &targets) override;
    void cut(const FileTargets &targets) override;
    void paste(const vfs::Location &directory) override;
    void remove(const FileTargets &targets) override;
    void removeFrom(const std::vector<FileTargets> &groups, std::function<void()> onFinished) override;
    void rename(const FileTargets &targets) override;
    void newFolder(const vfs::Location &directory) override;
    void newFile(const vfs::Location &directory) override;
    void share(const FileTargets &targets) override;
    void properties(const FileTargets &targets) override;
    void extract(const FileTargets &targets) override;
    void verify(const FileTargets &targets) override;
    void compress(const FileTargets &targets) override;
    void openWith(const FileTargets &targets) override;
    void open(const FileTargets &targets) override;
    [[nodiscard]] bool canPaste() const override { return !clipboard.peek().items.empty(); }

    void attach(AppContext &context) override;
    ui::NodePtr createBand() override;
    ui::NodePtr createDockStamp(std::function<std::optional<vfs::Location>()> here) override;
    [[nodiscard]] bool bandShown() const override { return jobs_.active(); }
    [[nodiscard]] std::uint64_t changes() const override { return jobs_.changes(); }
    [[nodiscard]] std::vector<vfs::Location> changedFolders() const override { return jobs_.changed(); }

    // The jobs sheet; its waiting jobs can bring their question back.
    void showJobs();
    // The band's tap: a waiting question first, otherwise the jobs sheet.
    void onBandTap();

    ui::Signal<Clipboard> clipboard;
    void clearClipboard();

    // The names of what was shared to FilePan and not yet saved, or of the document another app
    // asked to create; empty for none.
    ui::Signal<std::vector<std::string>> shared;
    // Saves it into directory, asking for the name first when it's one item.
    void saveShared(const vfs::Location &directory);
    // Drops it, closing its files; an app waiting for its document hears it was cancelled.
    void clearShared();

    // The app waiting for files, if any.
    ui::Signal<std::optional<PickRequest>> picking;
    [[nodiscard]] std::optional<PickRequest> pickRequest() const override { return picking(); }
    // Copies the files not on this device out first.
    void pick(const FileTargets &targets) override;
    // Tells the app the user chose nothing.
    void cancelPick();

    [[nodiscard]] JobsModel &jobs() { return jobs_; }
    [[nodiscard]] DecisionSheets &decisions() { return decisions_; }
    [[nodiscard]] std::string cacheDir() const;

private:
    void notify(std::string text, std::string action = {}, std::function<void()> onAction = {});
    void onJobFinished(const JobView &view, const JobMeta &meta);
    void extractInto(const std::vector<vfs::Entry> &archives, const vfs::Location &directory, bool newFolder,
                     const std::string &password);
    void verifyArchives(const std::vector<vfs::Entry> &archives, const vfs::Location &directory,
                        const std::string &password);
    // An extraction or test, which may need a password.
    struct PendingExtract {
        vfs::Entry entry;
        vfs::Location directory;
        bool newFolder = true;
        bool hadPassword = false;
        // It hit an encrypted entry: ask for the password once it has stopped.
        bool askAgain = false;
        // A test rather than an extraction.
        bool verify = false;
    };
    // Once the job has stopped: if it was locked, asks for the password to run it again and
    // returns true, with no snackbar for the stop.
    bool askPasswordIfLocked(const jobs::JobProgress &p);
    void askPassword(const PendingExtract &extract);
    // After a rename, move or delete: favorites and hidden items at or below a source that's no
    // longer there follow it to targets[i], or go if that's nullopt (a delete).
    void syncUserItems(std::vector<vfs::Location> sources, std::vector<std::optional<vfs::Location>> targets);
    // Copies files that aren't local into the cache, then calls done with their local paths.
    void cached(const FileTargets &targets, std::function<void(std::vector<std::string> paths)> done);
    void receiveShare(runtime::SharedItems share);
    void receiveDocumentRequest(runtime::DocumentRequest request);
    // Forgets the document another app asked to create, answering that it was cancelled if answer.
    void dropCreate(bool answer);
    void submitShared(const vfs::Location &directory);

    runtime::AppServices &services_;
    AppState &state_;
    Overlays &overlays_;
    AppContext *context_ = nullptr;
    JobsModel jobs_;
    DecisionSheets decisions_;
    std::unordered_map<jobs::JobId, PendingExtract> extracts_;
    // Finished jobs that get no snackbar.
    std::unordered_set<jobs::JobId> unannounced_;
    // What was shared to FilePan, waiting for a folder.
    std::vector<jobs::ImportItem> sharedItems_;
    bool sharedFromOtherApp_ = false;
    // The type of the document another app asked to create, whose name is sharedItems_' one item.
    std::optional<std::string> createMimeType_;
    ui::UiRoot::BackHandlerId backHandler_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace filepan::app
