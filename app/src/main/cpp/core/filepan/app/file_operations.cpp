// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/file_operations.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iterator>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "filepan/app/app.hpp"
#include "filepan/app/app_state.hpp"
#include "filepan/app/file_dialogs.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/places.hpp"
#include "filepan/cancellation.hpp"
#include "filepan/decision_promise.hpp"
#include "filepan/app/properties_sheet.hpp"

namespace filepan::app {

using namespace ui;
using namespace ui::components;
namespace fs = std::filesystem;

namespace {

std::string inQuotes(std::string_view name) { return "“" + std::string(name) + "”"; }

// "3 items" or "“a.pdf”".
std::string what(const std::vector<vfs::Location> &items) {
    return items.size() == 1 ? inQuotes(items.front().name()) : formatCount(items.size());
}

std::string folderLabel(const vfs::Location &folder) {
    if (folder.isRoot()) {
        if (const vfs::Location *archive = folder.archiveFile()) {
            return inQuotes(archive->name());
        }
        return folder.isLocal() ? std::string("/") : folder.serverId();
    }
    return std::string(folder.name());
}

std::string lower(std::string text) {
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// The verb for "Couldn't copy".
std::string failVerb(jobs::JobKind kind) {
    switch (kind) {
    case jobs::JobKind::kCopy:
        return "copy";
    case jobs::JobKind::kMove:
        return "move";
    case jobs::JobKind::kDelete:
        return "delete";
    case jobs::JobKind::kMakeDirectory:
        return "create the folder";
    case jobs::JobKind::kCreateFile:
        return "create the file";
    case jobs::JobKind::kRename:
        return "rename";
    case jobs::JobKind::kExtractArchive:
        return "extract";
    case jobs::JobKind::kCreateArchive:
        return "compress";
    case jobs::JobKind::kChecksum:
        return "hash";
    case jobs::JobKind::kImport:
        return "save";
    case jobs::JobKind::kVerifyArchive:
        return "test";
    }
    return "finish";
}

std::string cacheKey(const vfs::Location &location, const vfs::Entry &entry) {
    const std::string text = location.toString() + "|" + std::to_string(entry.modifiedTime.seconds) + "|" +
                             std::to_string(entry.size);
    char key[24];
    std::snprintf(key, sizeof(key), "%016zx", std::hash<std::string>{}(text));
    return key;
}

// On a worker: removes the oldest cache folders until the rest fit, keeping those in use.
void trimCache(const fs::path &root, const std::unordered_set<std::string> &keep) {
    struct Item {
        fs::path path;
        fs::file_time_type time;
        std::uintmax_t bytes = 0;
    };
    std::vector<Item> items;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        Item item{it->path(), it->last_write_time(ec)};
        for (fs::recursive_directory_iterator inner(it->path(), ec), last; !ec && inner != last; inner.increment(ec)) {
            if (inner->is_regular_file(ec)) {
                item.bytes += inner->file_size(ec);
            }
        }
        ec.clear();
        items.push_back(std::move(item));
    }
    std::ranges::sort(items, [](const Item &a, const Item &b) { return a.time < b.time; });
    std::uintmax_t total = 0;
    for (const Item &item : items) {
        total += item.bytes;
    }
    std::size_t count = items.size();
    for (const Item &item : items) {
        if (total <= FileOperations::kCacheBytes && count <= FileOperations::kCacheEntries) {
            break;
        }
        if (keep.contains(item.path.filename().string())) {
            continue;
        }
        fs::remove_all(item.path, ec);
        total -= item.bytes;
        --count;
    }
}

}  // namespace

FileOperations::FileOperations(runtime::AppServices &services, AppState &state, Overlays &overlays)
    : services_(services), state_(state), overlays_(overlays), jobs_(services), decisions_(services, overlays) {
    jobs_.onFinished = [this](const JobView &view, const JobMeta &meta) { onJobFinished(view, meta); };
    // A job starting clears the message showing: "Copied" is old news once the next copy runs.
    jobs_.onSubmitted = [this] { overlays_.snackbars().clear(); };
    decisions_.onNeedsPassword = [this](jobs::JobId id) {
        if (auto found = extracts_.find(id); found != extracts_.end()) {
            found->second.askAgain = true;
        }
    };
    services_.shares.setListener([this](runtime::SharedItems share) { receiveShare(std::move(share)); });
    services_.documentRequests.setListener(
            [this](runtime::DocumentRequest request) { receiveDocumentRequest(std::move(request)); });
}

FileOperations::~FileOperations() {
    services_.shares.setListener({});
    services_.documentRequests.setListener({});
    if (backHandler_ != 0) {
        services_.root.removeBackHandler(backHandler_);
    }
    alive_.reset();
}

void FileOperations::attach(AppContext &context) {
    context_ = &context;
    // Older than the App's own handler, so it only runs when back leaves FilePan: what was shared
    // and not saved goes then, and an app waiting for files hears none were chosen.
    backHandler_ = services_.root.addBackHandler([this] {
        cancelPick();
        if (!sharedItems_.empty()) {
            clearShared();
        }
        return false;
    });
    // Questions left waiting while the app was away come back with it.
    std::weak_ptr<bool> alive = alive_;
    context.overlays.snackbars().build([&] {
        context.overlays.snackbars().bind([this, alive, resumed = services_.resumed] {
            const bool on = resumed();
            untrack([&] {
                if (on && alive.lock()) {
                    decisions_.unsnooze();
                }
            });
        });
    });
}

std::string FileOperations::cacheDir() const {
    return services_.filesDir.empty() ? std::string() : services_.filesDir + "/cache/open";
}

void FileOperations::notify(std::string text, std::string action, std::function<void()> onAction) {
    overlays_.showSnackbar({.text = std::move(text), .action = std::move(action), .onAction = std::move(onAction)});
}

void FileOperations::onJobFinished(const JobView &view, const JobMeta &meta) {
    const jobs::JobProgress &p = view.progress;
    // A question its job was waiting on goes with it.
    decisions_.refresh();
    if (unannounced_.erase(view.id) > 0) {
        return;
    }
    switch (p.state) {
    case jobs::JobState::kSucceeded:
        if (!meta.doneText.empty()) {
            std::string text = meta.doneText;
            if (p.itemsSkipped > 0) {
                text += " · " + std::to_string(p.itemsSkipped) + " skipped";
            }
            notify(std::move(text));
        }
        break;
    case jobs::JobState::kFailed: {
        std::string text = "Couldn't " + failVerb(p.kind);
        if (p.error) {
            text += ": " + lower(describeError(*p.error).substr(0, 1)) + describeError(*p.error).substr(1);
        }
        notify(std::move(text), "Details", [this] { showJobs(); });
        break;
    }
    case jobs::JobState::kCancelled:
        notify(std::string(jobVerb(p.kind)) + " cancelled");
        break;
    default:
        break;
    }
}

// --- Clipboard ---

void FileOperations::copy(const FileTargets &targets) {
    if (targets.entries.empty()) {
        return;
    }
    const auto items = targets.locations();
    clipboard.set({items, false});
    state_.clearSelection();
    notify(what(items) + " to copy · open a folder and paste");
}

void FileOperations::cut(const FileTargets &targets) {
    if (targets.entries.empty()) {
        return;
    }
    if (vfs::Backend *backend = services_.backends.backendFor(targets.directory);
        backend != nullptr && !backend->capabilities().has(vfs::Capability::kWrite)) {
        notify("These can only be copied from here");
        return;
    }
    const auto items = targets.locations();
    clipboard.set({items, true});
    state_.clearSelection();
    notify(what(items) + " to move · open the destination and paste");
}

void FileOperations::clearClipboard() { clipboard.set({}); }

void FileOperations::paste(const vfs::Location &directory) {
    const Clipboard clip = clipboard.peek();
    if (clip.items.empty()) {
        notify("Nothing to paste");
        return;
    }
    if (vfs::Backend *backend = services_.backends.backendFor(directory);
        backend == nullptr || !backend->capabilities().has(vfs::Capability::kWrite)) {
        notify("Can't paste here: this folder is read-only");
        return;
    }
    std::vector<vfs::Location> sources;
    for (const vfs::Location &item : clip.items) {
        if (item.isAncestorOf(directory)) {
            notify("Can't paste " + inQuotes(item.name()) + " into itself");
            return;
        }
        // Moving something to where it is does nothing.
        if (!clip.cut || item.parent() != directory) {
            sources.push_back(item);
        }
    }
    if (sources.empty()) {
        clearClipboard();
        notify("Already here");
        return;
    }
    std::vector<vfs::Location> changes{directory};
    for (const vfs::Location &source : sources) {
        if (auto parent = source.parent(); parent && clip.cut && std::ranges::find(changes, *parent) == changes.end()) {
            changes.push_back(*parent);
        }
    }
    const std::string items = what(sources);
    const std::string into = folderLabel(directory);
    if (clip.cut) {
        std::vector<std::optional<vfs::Location>> targets;
        for (const vfs::Location &source : sources) {
            targets.emplace_back(directory.child(source.name()));
        }
        jobs_.submit(jobs::MoveJob{sources, directory},
                     {.title = "Move " + items + " to " + into,
                      .doneText = "Moved " + items,
                      .changes = changes,
                      .onFinished = [this, alive = std::weak_ptr<bool>(alive_), sources,
                                     targets](const jobs::JobProgress &) {
                          if (alive.lock()) {
                              syncUserItems(sources, targets);
                          }
                      }});
        clearClipboard();
    } else {
        jobs_.submit(jobs::CopyJob{sources, directory},
                     {.title = "Copy " + items + " to " + into, .doneText = "Copied " + items, .changes = changes});
    }
}

namespace {

// A dock stamp: tap it to act on the folder shown, × to drop what it holds.
struct DockStamp {
    std::shared_ptr<Box> row;
    std::shared_ptr<Box> button;
    std::shared_ptr<Pressable> pressable;
};

DockStamp dockStamp(const char *name, Prop<std::string> text, std::string icon, std::function<void()> onTap,
                    std::string clearLabel, std::function<void()> onClear) {
    auto row = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 2});
    row->setDebugName(name);
    auto button = Box::create({.justify = Justify::center, .minHeight = 48});
    button->appendChild(Stamp(std::move(text), StampTone::accent, std::move(icon)));
    auto pressable = makePressable(*button, {.onTap = std::move(onTap)});
    button->setOverlayHook([pressable](PaintContext &context) {
        if (pressable->pressed.peek()) {
            const gfx::Rect b = context.bounds();
            const float h = context.px(20);
            context.list.fill(gfx::RoundedRect{{b.x, b.y + (b.height - h) / 2, b.width, h},
                                               gfx::CornerRadii::uniform(context.px(4))},
                              gfx::Color{0, 0, 0, 0.18f});
        }
    });
    row->appendChild(button);
    row->appendChild(IconButton::create({.icon = "close",
                                         .label = std::move(clearLabel),
                                         .style = IconButtonStyle::plain,
                                         .size = 32,
                                         .onTap = std::move(onClear)}));
    return {row, button, pressable};
}

}  // namespace

NodePtr FileOperations::createDockStamp(std::function<std::optional<vfs::Location>()> here) {
    // Shown while either has something; hidden, it takes no place in the dock.
    auto stamps = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 2});
    stamps->setDebugName("DockStamps");

    auto share = dockStamp(
            "ShareStamp", [this] { return std::to_string(shared().size()) + " to save"; }, "save",
            [this, here] {
                if (auto folder = here()) {
                    saveShared(*folder);
                }
            },
            "Don't save the shared items", [this] { clearShared(); });
    share.row->bind([this, row = share.row.get(), button = share.button.get(), pressable = share.pressable] {
        const std::vector<std::string> &names = shared();
        (void)pressable->pressed();
        untrack([&] {
            row->updateStyle([&](Style &s) { s.display = !names.empty(); });
            button->markPaintDirty();
            button->updateSemantics([&](Semantics &s) {
                s.role = Role::button;
                s.label = "Save " + (names.size() == 1 ? inQuotes(names.front()) : formatCount(names.size())) + " here";
                s.mergeDescendants = true;
            });
        });
    });
    stamps->appendChild(share.row);

    auto clip = dockStamp(
            "ClipboardStamp",
            [this] {
                const Clipboard &c = clipboard();
                return std::to_string(c.items.size()) + (c.cut ? " to move" : " to paste");
            },
            "content_paste",
            [this, here] {
                if (auto folder = here()) {
                    this->paste(*folder);
                }
            },
            "Clear the clipboard", [this] { clearClipboard(); });
    clip.row->bind([this, row = clip.row.get(), paste = clip.button.get(), pressable = clip.pressable] {
        const Clipboard &c = clipboard();
        (void)pressable->pressed();
        untrack([&] {
            row->updateStyle([&](Style &s) { s.display = !c.items.empty(); });
            paste->markPaintDirty();
            paste->updateSemantics([&](Semantics &s) {
                s.role = Role::button;
                s.label = (c.cut ? "Move " : "Paste ") + formatCount(c.items.size()) + " here";
                s.mergeDescendants = true;
            });
        });
    });
    stamps->appendChild(clip.row);

    // Another app waiting for files: a tap on one picks it; the stamp only says so.
    auto pickStamp = dockStamp(
            "PickStamp", [this] { return picking() && picking()->multiple ? "Choose files" : "Choose a file"; },
            "attach_file",
            [this] {
                const bool several = picking.peek() && picking.peek()->multiple;
                notify(several ? "Tap a file to attach it, or long-press to choose several" : "Tap a file to attach it");
            },
            "Don't attach anything", [this] { cancelPick(); });
    pickStamp.row->bind([this, row = pickStamp.row.get(), button = pickStamp.button.get(),
                         pressable = pickStamp.pressable] {
        const bool on = picking().has_value();
        (void)pressable->pressed();
        untrack([&] {
            row->updateStyle([&](Style &s) { s.display = on; });
            button->markPaintDirty();
            button->updateSemantics([&](Semantics &s) {
                s.role = Role::button;
                s.label = "Choose files to attach";
                s.mergeDescendants = true;
            });
        });
    });
    stamps->appendChild(pickStamp.row);

    stamps->bind([this, stamps = stamps.get()] {
        const bool any = !shared().empty() || !clipboard().items.empty() || picking().has_value();
        untrack([&] { stamps->updateStyle([&](Style &s) { s.display = any; }); });
    });
    return stamps;
}

// --- Shared to FilePan ---

void FileOperations::receiveShare(runtime::SharedItems share) {
    if (share.items.empty()) {
        return;
    }
    std::vector<std::string> names;
    for (const jobs::ImportItem &item : share.items) {
        names.push_back(item.name);
    }
    // An app waiting for a document to be created hears it was cancelled.
    dropCreate(true);
    // An older share nobody saved is dropped, and its files closed.
    sharedItems_ = std::move(share.items);
    sharedFromOtherApp_ = share.fromOtherApp;
    shared.set(names);
    // The stamp is on the dock's idle row.
    state_.clearSelection();
    notify((names.size() == 1 ? inQuotes(names.front()) : formatCount(names.size())) +
           " to save · open a folder and save here");
}

void FileOperations::receiveDocumentRequest(runtime::DocumentRequest request) {
    // A request replaces an older one, which the shell cancelled when it finished its activity; one
    // that ended without an answer just goes.
    if (createMimeType_) {
        dropCreate(false);
        sharedItems_.clear();
        shared.set({});
    }
    picking.set(std::nullopt);
    switch (request.kind) {
    case runtime::DocumentRequest::Kind::ended:
        return;
    case runtime::DocumentRequest::Kind::pick:
        picking.set(PickRequest{.mimeTypes = std::move(request.mimeTypes), .multiple = request.multiple});
        state_.clearSelection();
        notify(request.multiple ? "Choose files to attach" : "Choose a file to attach");
        return;
    case runtime::DocumentRequest::Kind::create:
        break;
    }
    // It takes the share stamp's place, dropping an unsaved share.
    sharedItems_.clear();
    sharedItems_.push_back({.name = request.name});
    sharedFromOtherApp_ = false;
    createMimeType_ = request.mimeTypes.empty() ? std::string("application/octet-stream") : request.mimeTypes.front();
    shared.set({request.name});
    state_.clearSelection();
    notify(inQuotes(request.name) + " to save · open a folder and save here");
}

void FileOperations::dropCreate(bool answer) {
    if (createMimeType_ && answer) {
        services_.platform.answerDocumentRequest({}, {}, false);
    }
    createMimeType_.reset();
}

void FileOperations::clearShared() {
    dropCreate(true);
    sharedItems_.clear();
    sharedFromOtherApp_ = false;
    shared.set({});
}

void FileOperations::saveShared(const vfs::Location &directory) {
    if (sharedItems_.empty()) {
        notify("Nothing to save");
        return;
    }
    // The app that asked writes the document itself, through a file on this device.
    if (createMimeType_ && !directory.isLocal()) {
        notify("Can't save here: choose a folder on this device");
        return;
    }
    if (vfs::Backend *backend = services_.backends.backendFor(directory);
        backend == nullptr || !backend->capabilities().has(vfs::Capability::kWrite)) {
        notify("Can't save here: this folder is read-only");
        return;
    }
    if (sharedItems_.size() > 1) {
        submitShared(directory);
        return;
    }
    const std::string name = sharedItems_.front().name;
    showNameDialog(services_, overlays_,
                   {.title = "Save",
                    .action = "Save",
                    .initial = name,
                    .folder = directory,
                    .allowReplace = true,
                    .onSubmit = [this, directory](const std::string &name, bool replace) {
                        // Dropped while the dialog was up.
                        if (sharedItems_.size() != 1) {
                            return;
                        }
                        sharedItems_.front().name = name;
                        sharedItems_.front().replace = replace;
                        submitShared(directory);
                    }});
}

void FileOperations::submitShared(const vfs::Location &directory) {
    std::vector<jobs::ImportItem> items = std::exchange(sharedItems_, {});
    const bool leave = std::exchange(sharedFromOtherApp_, false);
    const std::optional<std::string> create = std::exchange(createMimeType_, std::nullopt);
    const std::string path = create ? directory.child(items.front().name).path() : std::string();
    shared.set({});
    const std::string label = items.size() == 1 ? inQuotes(items.front().name) : formatCount(items.size());
    const std::string into = folderLabel(directory);
    jobs_.submit(jobs::ImportJob{std::move(items), directory},
                 {.title = "Save " + label + " to " + into,
                  .doneText = "Saved " + label + " to " + into,
                  .changes = {directory},
                  // Back to the app that shared them, once they're safely here; or, for an app
                  // that asked for a document, the empty file to write, or nothing if it failed.
                  .onFinished = [platform = &services_.platform, leave, create,
                                 path](const jobs::JobProgress &progress) {
                      const bool saved = progress.state == jobs::JobState::kSucceeded;
                      if (create && saved) {
                          platform->answerDocumentRequest({path}, {*create}, true);
                      } else if (create) {
                          platform->answerDocumentRequest({}, {}, false);
                      } else if (leave && saved) {
                          platform->finishActivity();
                      }
                  }});
}

// --- Asking first ---

void FileOperations::remove(const FileTargets &targets) { removeFrom({targets}, {}); }

void FileOperations::removeFrom(const std::vector<FileTargets> &groups, std::function<void()> onFinished) {
    std::vector<vfs::Location> items;
    std::vector<vfs::Entry> entries;
    std::vector<vfs::Location> directories;
    for (const FileTargets &targets : groups) {
        if (targets.entries.empty()) {
            continue;
        }
        if (vfs::Backend *backend = services_.backends.backendFor(targets.directory);
            backend == nullptr || !backend->capabilities().has(vfs::Capability::kWrite)) {
            notify("Can't delete here: this folder is read-only");
            return;
        }
        std::ranges::copy(targets.locations(), std::back_inserter(items));
        std::ranges::copy(targets.entries, std::back_inserter(entries));
        directories.push_back(targets.directory);
    }
    if (items.empty()) {
        return;
    }
    showDeleteSheet(overlays_, entries, [this, items, directories, onFinished = std::move(onFinished)] {
        state_.clearSelection();
        jobs_.submit(jobs::DeleteJob{items},
                     {.title = "Delete " + what(items),
                      .doneText = "Deleted " + what(items),
                      .changes = directories,
                      .onFinished = [this, alive = std::weak_ptr<bool>(alive_), items,
                                     onFinished](const jobs::JobProgress &) {
                          if (alive.lock()) {
                              syncUserItems(items, std::vector<std::optional<vfs::Location>>(items.size()));
                          }
                          if (onFinished) {
                              onFinished();
                          }
                      }});
    });
}

void FileOperations::rename(const FileTargets &targets) {
    if (targets.entries.size() != 1) {
        return;
    }
    const vfs::Entry &entry = targets.entries.front();
    const vfs::Location source = targets.directory.child(entry.name);
    const vfs::Location directory = targets.directory;
    showNameDialog(services_, overlays_,
                   {.title = "Rename",
                    .action = "Rename",
                    .initial = entry.name,
                    .directory = entry.isDirectoryLike(),
                    .original = entry.name,
                    .folder = directory,
                    .allowReplace = true,
                    .onSubmit = [this, source, directory](const std::string &name, bool replace) {
                        state_.clearSelection();
                        jobs_.submit(jobs::RenameJob{source, name, replace},
                                     {.title = "Rename " + inQuotes(source.name()) + " to " + inQuotes(name),
                                      .changes = {directory},
                                      .onFinished = [this, alive = std::weak_ptr<bool>(alive_), source,
                                                     target = directory.child(name)](const jobs::JobProgress &) {
                                          if (alive.lock()) {
                                              syncUserItems({source}, {target});
                                          }
                                      }});
                    }});
}

void FileOperations::newFolder(const vfs::Location &directory) {
    showNameDialog(services_, overlays_,
                   {.title = "New folder",
                    .action = "Create",
                    .initial = "New folder",
                    .directory = true,
                    .folder = directory,
                    .onSubmit = [this, directory](const std::string &name, bool) {
                        jobs_.submit(jobs::MakeDirectoryJob{directory.child(name)},
                                     {.title = "New folder " + inQuotes(name), .changes = {directory}});
                    }});
}

void FileOperations::newFile(const vfs::Location &directory) {
    showNameDialog(services_, overlays_,
                   {.title = "New file",
                    .action = "Create",
                    .initial = "untitled.txt",
                    .folder = directory,
                    .onSubmit = [this, directory](const std::string &name, bool) {
                        jobs_.submit(jobs::CreateFileJob{directory.child(name)},
                                     {.title = "New file " + inQuotes(name), .changes = {directory}});
                    }});
}

void FileOperations::properties(const FileTargets &targets) {
    std::string label;
    if (context_ != nullptr) {
        for (const Crumb &crumb : crumbsFor(targets.directory, context_->volumes.peek())) {
            label += (label.empty() ? "" : " / ") + crumb.label;
        }
    }
    showProperties({.services = services_,
                    .overlays = overlays_,
                    .jobs = jobs_,
                    .locationLabel = label},
                   targets);
}

void FileOperations::extract(const FileTargets &targets) {
    std::vector<vfs::Entry> archives;
    for (const vfs::Entry &entry : targets.entries) {
        if (!entry.isDirectoryLike() && entry.mimeType().kind == vfs::FileKind::kArchive) {
            archives.push_back(entry);
        }
    }
    if (archives.empty()) {
        notify("Nothing here is an archive");
        return;
    }
    const vfs::Location directory = targets.directory;
    if (vfs::Backend *backend = services_.backends.backendFor(directory);
        backend == nullptr || !backend->capabilities().has(vfs::Capability::kWrite)) {
        notify("Can't extract here: this folder is read-only");
        return;
    }
    showExtractSheet(overlays_, archives, directory, [this, archives, directory](const ExtractChoice &choice) {
        state_.clearSelection();
        extractInto(archives, directory, choice.newFolder, {});
    });
}

void FileOperations::extractInto(const std::vector<vfs::Entry> &archives, const vfs::Location &directory, bool newFolder,
                                 const std::string &password) {
    std::weak_ptr<bool> alive = alive_;
    for (const vfs::Entry &entry : archives) {
        const vfs::Location archive = directory.child(entry.name);
        const vfs::Location target = newFolder ? directory.child(extractFolderName(entry.name)) : directory;
        std::vector<std::string> passwords;
        if (!password.empty()) {
            passwords.push_back(password);
        }
        const jobs::JobId id = jobs_.submit(
                jobs::ExtractArchiveJob{archive, target, passwords},
                {.title = "Extract " + inQuotes(entry.name),
                 .doneText = "Extracted " + inQuotes(entry.name),
                 .changes = {directory},
                 .onFinished = [this, alive](const jobs::JobProgress &p) {
                     if (alive.lock()) {
                         askPasswordIfLocked(p);
                     }
                 }});
        extracts_.insert_or_assign(id, PendingExtract{.entry = entry,
                                                      .directory = directory,
                                                      .newFolder = newFolder,
                                                      .hadPassword = !password.empty()});
    }
}

void FileOperations::verify(const FileTargets &targets) {
    std::vector<vfs::Entry> archives;
    for (const vfs::Entry &entry : targets.entries) {
        if (!entry.isDirectoryLike() && entry.mimeType().kind == vfs::FileKind::kArchive) {
            archives.push_back(entry);
        }
    }
    if (archives.empty()) {
        notify("Nothing here is an archive");
        return;
    }
    state_.clearSelection();
    verifyArchives(archives, targets.directory, {});
}

void FileOperations::verifyArchives(const std::vector<vfs::Entry> &archives, const vfs::Location &directory,
                                    const std::string &password) {
    std::weak_ptr<bool> alive = alive_;
    for (const vfs::Entry &entry : archives) {
        std::vector<std::string> passwords;
        if (!password.empty()) {
            passwords.push_back(password);
        }
        // Its own snackbar says what it found; failing and cancelling get the usual ones.
        const jobs::JobId id = jobs_.submit(
                jobs::VerifyArchiveJob{directory.child(entry.name), passwords},
                {.title = "Test " + inQuotes(entry.name),
                 .onFinished = [this, alive, name = entry.name](const jobs::JobProgress &p) {
                     if (!alive.lock() || askPasswordIfLocked(p) || p.state != jobs::JobState::kSucceeded) {
                         return;
                     }
                     const auto result = services_.jobs.result(p.id);
                     if (!result || p.itemsSkipped > 0) {
                         // Skipped from an error sheet.
                         notify("Didn't test " + inQuotes(name));
                         return;
                     }
                     const std::uint64_t entries = result->archiveEntries;
                     if (result->archiveProblems.empty()) {
                         notify(inQuotes(name) + " is intact" + (entries > 0 ? " · " + formatCount(entries) : ""));
                         return;
                     }
                     const std::size_t count = result->archiveProblems.size();
                     notify(inQuotes(name) + " is damaged · " + std::to_string(count) + (count == 1 ? " problem" : " problems"),
                            "Details", [this, name, entries, problems = result->archiveProblems] {
                                showArchiveProblemsSheet(overlays_, name, entries, problems);
                            });
                 }});
        extracts_.insert_or_assign(
                id, PendingExtract{.entry = entry, .directory = directory, .hadPassword = !password.empty(), .verify = true});
    }
}

bool FileOperations::askPasswordIfLocked(const jobs::JobProgress &p) {
    const auto found = extracts_.find(p.id);
    if (found == extracts_.end()) {
        return false;
    }
    const PendingExtract extract = found->second;
    extracts_.erase(found);
    // Encrypted and no (or the wrong) password: ask for one, once the job has stopped.
    const bool locked = p.state == jobs::JobState::kFailed && p.error && p.error->code == EACCES;
    if (!extract.askAgain && !locked) {
        return false;
    }
    // The password sheet says it all; no snackbar about the stop.
    unannounced_.insert(p.id);
    std::weak_ptr<bool> alive = alive_;
    services_.loop.post([this, alive, extract] {
        if (alive.lock()) {
            askPassword(extract);
        }
    });
    return true;
}

void FileOperations::askPassword(const PendingExtract &extract) {
    if (services_.prompts == nullptr) {
        notify("Couldn't " + std::string(extract.verify ? "test " : "extract ") + inQuotes(extract.entry.name) +
               ": it needs a password");
        return;
    }
    // The archive password sheet, as for browsing into it; its answer waits on the pool.
    auto [promise, future] = makeDecision<std::optional<std::string>>();
    services_.prompts->requestPassword(
            {.archive = extract.directory.child(extract.entry.name), .entryPath = "/", .retry = extract.hadPassword},
            std::move(promise));
    auto waiting = std::make_shared<DecisionFuture<std::optional<std::string>>>(std::move(future));
    std::weak_ptr<bool> alive = alive_;
    services_.io.submit(
            [waiting] {
                const CancellationToken never;
                const auto answer = waiting->wait(never);
                return answer && *answer ? **answer : std::string();
            },
            [this, alive, extract](std::string password) {
                if (!alive.lock() || password.empty()) {
                    return;
                }
                if (extract.verify) {
                    verifyArchives({extract.entry}, extract.directory, password);
                } else {
                    extractInto({extract.entry}, extract.directory, extract.newFolder, password);
                }
            });
}

bool shouldDeleteSourcesAfterCompress(const jobs::JobProgress &progress) {
    return progress.state == jobs::JobState::kSucceeded && progress.itemsSkipped == 0;
}

void FileOperations::compress(const FileTargets &targets) {
    if (targets.entries.empty()) {
        return;
    }
    const vfs::Location directory = targets.directory;
    if (vfs::Backend *backend = services_.backends.backendFor(directory);
        backend == nullptr || !backend->capabilities().has(vfs::Capability::kWrite)) {
        notify("Can't compress here: this folder is read-only");
        return;
    }
    const auto sources = targets.locations();
    std::weak_ptr<bool> alive = alive_;
    showCompressSheet(services_, overlays_, targets.entries, directory, [this, alive, sources, directory](const CompressChoice &choice) {
        state_.clearSelection();
        jobs_.submit(jobs::CreateArchiveJob{sources, directory.child(choice.name), choice.options},
                     {.title = "Compress " + what(sources) + " into " + inQuotes(choice.name),
                      .doneText = "Compressed into " + inQuotes(choice.name),
                      .changes = {directory},
                      .onFinished = [this, alive, sources, directory, deleteSources = choice.deleteSources](
                                            const jobs::JobProgress &p) {
                          if (!deleteSources || !alive.lock()) {
                              return;
                          }
                          if (!shouldDeleteSourcesAfterCompress(p)) {
                              if (p.state == jobs::JobState::kSucceeded) {
                                  notify("Kept the originals: some items were skipped");
                              }
                              return;
                          }
                          jobs_.submit(jobs::DeleteJob{sources},
                                       {.title = "Deleting originals",
                                        .doneText = "Deleted the originals",
                                        .changes = {directory},
                                        .onFinished = [this, alive, sources](const jobs::JobProgress &) {
                                            if (alive.lock()) {
                                                syncUserItems(sources, std::vector<std::optional<vfs::Location>>(
                                                                               sources.size()));
                                            }
                                        }});
                      }});
    });
}

// --- Keeping favorites and hidden items in step ---

void FileOperations::syncUserItems(std::vector<vfs::Location> sources,
                                   std::vector<std::optional<vfs::Location>> targets) {
    const Settings &settings = state_.settings.peek();
    const auto marked = [&](const vfs::Location &source) {
        return std::ranges::any_of(settings.favorites,
                                   [&](const Favorite &f) { return source.isAncestorOf(f.location); }) ||
               std::ranges::any_of(settings.hiddenItems,
                                   [&](const vfs::Location &item) { return source.isAncestorOf(item); });
    };
    if (std::ranges::none_of(sources, marked)) {
        return;
    }
    // Only what's really gone from where it was: a skipped or failed item stays put.
    std::weak_ptr<bool> alive = alive_;
    vfs::BackendResolver *backends = &services_.backends;
    services_.io.submit(
            [backends, sources] {
                std::vector<bool> gone;
                for (const vfs::Location &source : sources) {
                    vfs::Backend *backend = backends->backendFor(source);
                    const auto stat = backend != nullptr ? backend->lstat(source) : Result<vfs::Entry>(fail(EIO, ""));
                    gone.push_back(!stat && stat.error().code == ENOENT);
                }
                return gone;
            },
            [this, alive, sources, targets](std::vector<bool> gone) {
                if (!alive.lock()) {
                    return;
                }
                std::vector<vfs::Location> deleted;
                for (std::size_t i = 0; i < sources.size(); ++i) {
                    if (!gone[i]) {
                        continue;
                    }
                    if (targets[i]) {
                        state_.itemMoved(sources[i], *targets[i]);
                    } else {
                        deleted.push_back(sources[i]);
                    }
                }
                if (!deleted.empty()) {
                    state_.itemsGone(deleted);
                }
            });
}

// --- Handing files to other apps ---

void FileOperations::cached(const FileTargets &targets, std::function<void(std::vector<std::string>)> done) {
    std::vector<std::string> paths(targets.entries.size());
    struct Miss {
        std::size_t index;
        vfs::Location source;
        std::string key;
    };
    std::vector<Miss> misses;
    for (std::size_t i = 0; i < targets.entries.size(); ++i) {
        const vfs::Location location = targets.directory.child(targets.entries[i].name);
        if (location.isLocal()) {
            paths[i] = location.path();
        } else {
            misses.push_back({i, location, cacheKey(location, targets.entries[i])});
        }
    }
    if (misses.empty()) {
        done(std::move(paths));
        return;
    }
    const std::string root = cacheDir();
    if (root.empty()) {
        notify("FilePan has no cache to copy this into");
        return;
    }
    for (const Miss &miss : misses) {
        paths[miss.index] = root + "/" + miss.key + "/" + std::string(miss.source.name());
    }
    std::vector<std::uint64_t> sizes;
    for (const Miss &miss : misses) {
        sizes.push_back(targets.entries[miss.index].size);
    }
    std::weak_ptr<bool> alive = alive_;
    auto shared = std::make_shared<std::function<void(std::vector<std::string>)>>(std::move(done));
    // On the pool: trim, and find which are already there.
    services_.io.submit(
            [root, misses, sizes, paths] {
                std::unordered_set<std::string> keep;
                for (const Miss &miss : misses) {
                    keep.insert(miss.key);
                }
                std::error_code ec;
                fs::create_directories(root, ec);
                trimCache(root, keep);
                std::vector<bool> hit(misses.size());
                for (std::size_t i = 0; i < misses.size(); ++i) {
                    const fs::path dir = fs::path(root) / misses[i].key;
                    const fs::path file = paths[misses[i].index];
                    if (fs::is_regular_file(file, ec) && fs::file_size(file, ec) == sizes[i]) {
                        hit[i] = true;
                        fs::last_write_time(dir, fs::file_time_type::clock::now(), ec);
                        continue;
                    }
                    // A partial copy from before goes.
                    fs::remove_all(dir, ec);
                    fs::create_directories(dir, ec);
                }
                return hit;
            },
            [this, alive, root, misses, paths, shared](std::vector<bool> hit) {
                if (!alive.lock()) {
                    return;
                }
                auto left = std::make_shared<std::size_t>(0);
                auto failed = std::make_shared<bool>(false);
                for (std::size_t i = 0; i < misses.size(); ++i) {
                    if (!hit[i]) {
                        ++*left;
                    }
                }
                if (*left == 0) {
                    (*shared)(paths);
                    return;
                }
                for (std::size_t i = 0; i < misses.size(); ++i) {
                    if (hit[i]) {
                        continue;
                    }
                    const vfs::Location target = vfs::Location::local(root + "/" + misses[i].key);
                    jobs_.submit(jobs::CopyJob{{misses[i].source}, target},
                                 {.title = "Prepare " + inQuotes(misses[i].source.name()),
                                  .onFinished = [left, failed, shared, paths](const jobs::JobProgress &p) {
                                      *failed = *failed || p.state != jobs::JobState::kSucceeded;
                                      if (--*left == 0 && !*failed) {
                                          (*shared)(paths);
                                      }
                                  }});
                }
            });
}

void FileOperations::share(const FileTargets &targets) {
    FileTargets files{targets.directory, {}};
    for (const vfs::Entry &entry : targets.entries) {
        if (entry.targetType == vfs::EntryType::kFile) {
            files.entries.push_back(entry);
        }
    }
    if (files.entries.empty()) {
        notify(targets.entries.empty() ? "Nothing to share" : "Folders can't be shared; share the files in them");
        return;
    }
    if (files.entries.size() < targets.entries.size()) {
        notify("Sharing the files; folders can't be shared");
    }
    std::vector<std::string> mimes;
    for (const vfs::Entry &entry : files.entries) {
        mimes.emplace_back(entry.mimeType().type);
    }
    cached(files, [this, mimes](std::vector<std::string> paths) { services_.platform.shareFiles(paths, mimes); });
}

void FileOperations::open(const FileTargets &targets) {
    if (targets.entries.size() != 1) {
        return;
    }
    const std::string mime(targets.entries.front().mimeType().type);
    cached(targets, [this, mime](std::vector<std::string> paths) { services_.platform.openFile(paths.front(), mime); });
}

void FileOperations::openWith(const FileTargets &targets) {
    if (targets.entries.size() != 1 || targets.entries.front().isDirectoryLike()) {
        return;
    }
    // A generic type makes Android show every app, not just the default for the type.
    cached(targets, [this](std::vector<std::string> paths) { services_.platform.openFile(paths.front(), "*/*"); });
}

namespace {

// Whether a file of type mime is one the asking app takes: any type when it named none, "*/*",
// "image/*" for every image, or the type itself.
bool accepts(const std::vector<std::string> &types, std::string_view mime) {
    if (types.empty()) {
        return true;
    }
    return std::ranges::any_of(types, [mime](std::string_view type) {
        if (type == "*/*" || type == mime) {
            return true;
        }
        return type.ends_with("/*") && mime.starts_with(type.substr(0, type.size() - 1));
    });
}

}  // namespace

void FileOperations::pick(const FileTargets &targets) {
    if (!picking.peek()) {
        return;
    }
    const PickRequest request = *picking.peek();
    FileTargets files{targets.directory, {}};
    std::vector<std::string> mimes;
    for (const vfs::Entry &entry : targets.entries) {
        if (entry.targetType != vfs::EntryType::kFile) {
            continue;
        }
        const std::string_view mime = entry.mimeType().type;
        if (!accepts(request.mimeTypes, mime)) {
            notify(inQuotes(entry.name) + " isn't a type the app takes");
            return;
        }
        files.entries.push_back(entry);
        mimes.emplace_back(mime);
    }
    if (files.entries.empty()) {
        notify("Folders can't be attached; choose files");
        return;
    }
    if (files.entries.size() > 1 && !request.multiple) {
        notify("The app takes one file");
        return;
    }
    state_.clearSelection();
    // Answered once the files are here, and only once.
    cached(files, [this, alive = std::weak_ptr<bool>(alive_), mimes](std::vector<std::string> paths) {
        if (alive.lock() && picking.peek()) {
            picking.set(std::nullopt);
            services_.platform.answerDocumentRequest(paths, mimes, false);
        }
    });
}

void FileOperations::cancelPick() {
    if (picking.peek()) {
        picking.set(std::nullopt);
        services_.platform.answerDocumentRequest({}, {}, false);
    }
}

// --- Jobs ---

NodePtr FileOperations::createBand() {
    return jobBand(jobs_, [this] { onBandTap(); });
}

void FileOperations::onBandTap() {
    for (const JobView &view : jobs_.list.peek()) {
        if (view.progress.state == jobs::JobState::kWaitingForDecision && decisions_.hasPending(view.id)) {
            decisions_.showFor(view.id);
            return;
        }
    }
    showJobs();
}

void FileOperations::showJobs() {
    overlays_.showSheet("Jobs", jobsSheet(jobs_, [this](jobs::JobId id) { decisions_.showFor(id); }));
}

}  // namespace filepan::app
