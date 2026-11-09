// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "filepan/ui/node.hpp"
#include "filepan/vfs/entry.hpp"
#include "filepan/vfs/location.hpp"

// What the dock and menus ask for when the user acts on files. The browsing screen only
// describes the request (which folder, which entries); an implementation does the work, usually
// by queueing a job on the JobQueue and showing sheets for names, conflicts and progress.
//
// The app installs FileOperations (file_operations.hpp) unless AppOptions::actions gives another;
// StubFileActions says "Not yet" to everything.
namespace filepan::app {

struct AppContext;

struct FileTargets {
    vfs::Location directory;
    std::vector<vfs::Entry> entries;  // The selection, or the one entry a menu was opened on.

    [[nodiscard]] std::vector<vfs::Location> locations() const;
};

// Another app waiting for files to read (ACTION_GET_CONTENT, ACTION_OPEN_DOCUMENT): the types it
// takes ("image/*" for any image; empty for any) and whether several may go.
struct PickRequest {
    std::vector<std::string> mimeTypes;
    bool multiple = false;
    friend bool operator==(const PickRequest &, const PickRequest &) = default;
};

enum class FileAction : std::uint8_t {
    copy,
    cut,
    paste,
    remove,
    rename,
    newFolder,
    newFile,
    share,
    properties,
    extract,
    verify,
    compress,
    openWith,
};

// "Copy", "Delete"...: menu labels.
const char *fileActionLabel(FileAction action);
const char *fileActionIcon(FileAction action);

class FileActions {
public:
    virtual ~FileActions() = default;

    // Puts the targets on the app's clipboard for paste.
    virtual void copy(const FileTargets &targets) = 0;
    virtual void cut(const FileTargets &targets) = 0;
    // Into directory, from the clipboard.
    virtual void paste(const vfs::Location &directory) = 0;
    virtual void remove(const FileTargets &targets) = 0;
    // Deletes from several folders at once, each group's entries in its directory, after the same
    // confirmation as remove. onFinished runs on the UI thread once the job has ended, however it
    // ended; not at all if the user cancels. Does nothing by default.
    virtual void removeFrom(const std::vector<FileTargets> &groups, std::function<void()> onFinished) {
        (void)groups;
        (void)onFinished;
    }
    // One entry.
    virtual void rename(const FileTargets &targets) = 0;
    virtual void newFolder(const vfs::Location &directory) = 0;
    virtual void newFile(const vfs::Location &directory) = 0;
    virtual void share(const FileTargets &targets) = 0;
    virtual void properties(const FileTargets &targets) = 0;
    // Archives into their folder (or a chosen one).
    virtual void extract(const FileTargets &targets) = 0;
    // Reads archives through to find damage, without extracting them.
    virtual void verify(const FileTargets &targets) = 0;
    virtual void compress(const FileTargets &targets) = 0;
    // One file, with the app chooser.
    virtual void openWith(const FileTargets &targets) = 0;

    // Whether the clipboard holds anything to paste.
    [[nodiscard]] virtual bool canPaste() const { return false; }

    // Opens one file with the app the system picks for its type. Local files go straight to
    // Platform::openFile; others (in archives, on servers) are copied out first.
    virtual void open(const FileTargets &targets) { (void)targets; }

    // Tracked: the app waiting for files, if any; a file tap picks for it then. pick hands it the
    // files in targets. The defaults never wait.
    [[nodiscard]] virtual std::optional<PickRequest> pickRequest() const { return std::nullopt; }
    virtual void pick(const FileTargets &targets) { (void)targets; }

    // Called once the App's context exists.
    virtual void attach(AppContext &context) { (void)context; }
    // What the browsing screen hosts for the actions: the job data band (it places itself above
    // the dock) and a stamp for the dock's idle row (the clipboard); null for none. here is the
    // folder the screen shows.
    virtual ui::NodePtr createBand() { return nullptr; }
    virtual ui::NodePtr createDockStamp(std::function<std::optional<vfs::Location>()> here) {
        (void)here;
        return nullptr;
    }
    // Tracked: the band is up, so lists clear it too.
    [[nodiscard]] virtual bool bandShown() const { return false; }
    // Tracked: bumps when a job finished; changedFolders() are the folders it touched. Screens
    // showing one they can't watch refresh it.
    [[nodiscard]] virtual std::uint64_t changes() const { return 0; }
    [[nodiscard]] virtual std::vector<vfs::Location> changedFolders() const { return {}; }

    // Dispatches by kind; directory-only actions use targets.directory.
    void perform(FileAction action, const FileTargets &targets);
};

// Every action tells the user it isn't there yet.
class StubFileActions final : public FileActions {
public:
    explicit StubFileActions(std::function<void(const std::string &message)> notify) : notify_(std::move(notify)) {}

    void copy(const FileTargets &) override { notYet(); }
    void cut(const FileTargets &) override { notYet(); }
    void paste(const vfs::Location &) override { notYet(); }
    void remove(const FileTargets &) override { notYet(); }
    void rename(const FileTargets &) override { notYet(); }
    void newFolder(const vfs::Location &) override { notYet(); }
    void newFile(const vfs::Location &) override { notYet(); }
    void share(const FileTargets &) override { notYet(); }
    void properties(const FileTargets &) override { notYet(); }
    void extract(const FileTargets &) override { notYet(); }
    void verify(const FileTargets &) override { notYet(); }
    void compress(const FileTargets &) override { notYet(); }
    void openWith(const FileTargets &) override { notYet(); }
    void open(const FileTargets &) override { notYet(); }

private:
    void notYet() {
        if (notify_) {
            notify_("Not yet");
        }
    }

    std::function<void(const std::string &)> notify_;
};

}  // namespace filepan::app
