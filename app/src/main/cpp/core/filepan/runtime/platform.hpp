// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "filepan/gfx/geometry.hpp"
#include "filepan/runtime/document_picker.hpp"
#include "filepan/ui/text_input.hpp"
#include "filepan/vfs/document_backend.hpp"
#include "filepan/vfs/security_key.hpp"

// The services native code asks of the platform: permissions, storage volumes, intents, the
// clipboard, the IME, accessibility, the jobs notification, haptics, the network, the document
// picker, security keys and finishing the activity.
// On Android, jni/ implements it by calling the Kotlin shell; host tests use a fake.
//
// AppHost calls every method on the UI thread but apkIcon (the I/O pool's), so an implementation
// may keep per-thread state (the JNI one attaches the UI thread to the VM once, and each I/O
// thread on its first apkIcon). Methods must not block on the Android
// main thread: the main thread may itself be waiting for the UI thread (surface handoff).
namespace filepan::runtime {

struct StorageVolume {
    std::string path;         // The mount point, e.g. /storage/emulated/0; empty if unmounted.
    std::string description;  // User-visible, e.g. "Internal shared storage".
    bool primary = false;
    bool removable = false;
    std::string state;  // Environment.MEDIA_*: "mounted", "mounted_ro", "unmounted", ...
    std::uint64_t totalBytes = 0;
    std::uint64_t freeBytes = 0;

    bool operator==(const StorageVolume &) const = default;
};

// What the IME needs: the editing value (UTF-8 byte offsets) and how to configure the keyboard.
struct TextInputState {
    ui::TextEditingValue value;
    ui::TextInputConfig config;
    // Which field's input this is: it changes whenever focus moves to another field, and the
    // platform tags the IME's edits with it so an edit meant for an earlier field is dropped.
    std::uint64_t session = 0;

    bool operator==(const TextInputState &) const = default;
};

// Values match Android's HapticFeedbackConstants where one exists; the shell maps the rest.
enum class HapticKind : std::uint8_t {
    tap,        // VIRTUAL_KEY
    longPress,  // LONG_PRESS
    tick,       // CLOCK_TICK: a range selection or a snap point.
    confirm,    // CONFIRM
    reject,     // REJECT
    dragStart,  // DRAG_START
};

// What the jobs notification shows; built by AppHost from the JobQueue's progress.
struct JobSummary {
    std::uint32_t activeJobs = 0;
    std::string title;  // "Copying 3 items"
    std::string text;   // "456 MiB / 1.1 GiB · " and the current item, or "2 waiting".
    // 0..1000, or -1 while indeterminate (scanning, unknown totals).
    std::int32_t progressPermille = -1;

    bool operator==(const JobSummary &) const = default;
};

class Platform {
public:
    virtual ~Platform() = default;

    // MANAGE_EXTERNAL_STORAGE. requestAllFilesAccess opens the system settings page; the answer
    // shows up as hasAllFilesAccess() after the activity resumes.
    virtual bool hasAllFilesAccess() = 0;
    virtual void requestAllFilesAccess() = 0;

    virtual std::vector<StorageVolume> storageVolumes() = 0;

    // Hands files to other apps through FilePan's content provider.
    virtual void openFile(const std::string &path, const std::string &mimeType) = 0;
    virtual void shareFiles(const std::vector<std::string> &paths,
                            const std::vector<std::string> &mimeTypes) = 0;
    // The answer to AppHost::documentRequested: the documents for the app that asked, writable
    // for one it creates; none when the user cancelled. Android hands back content URIs and
    // closes the activity it asked through. The default does nothing.
    virtual void answerDocumentRequest(const std::vector<std::string> &paths,
                                       const std::vector<std::string> &mimeTypes, bool writable) {
        (void)paths;
        (void)mimeTypes;
        (void)writable;
    }

    virtual void setClipboardText(const std::string &text) = 0;
    // The clipboard's text, as UTF-8; nullopt when it holds none. The default has none.
    virtual std::optional<std::string> clipboardText() { return std::nullopt; }

    // The IME. caret is in window px, for the IME's cursor anchor.
    virtual void showIme(gfx::Rect caret, const TextInputState &state) = 0;
    virtual void hideIme() = 0;
    // The text changed from the app side; the IME's view of it must be updated.
    virtual void updateIme(const TextInputState &state) = 0;

    // A semantics update in SemanticsTree's wire format. Only valid during the call.
    virtual void updateSemantics(std::span<const std::uint8_t> bytes) = 0;
    virtual void announce(const std::string &text) = 0;

    // Jobs became active (start the foreground service) or all finished (stop it).
    virtual void jobsActive(bool active) = 0;
    virtual void updateJobNotification(const JobSummary &summary) = 0;

    virtual void performHaptic(HapticKind kind) = 0;

    // Back at the root: let the system do what back does there.
    virtual void finishActivity() = 0;

    // POST_NOTIFICATIONS, for the jobs notification. requestNotificationPermission shows the
    // system's dialog (which may not show once it's been refused); openNotificationSettings the app's
    // notification settings. The answer shows up as hasNotificationPermission() after the activity
    // resumes. The defaults have it granted and do nothing.
    virtual bool hasNotificationPermission() { return true; }
    virtual void requestNotificationPermission() {}
    virtual void openNotificationSettings() {}

    // Whether the default network is metered (mobile data, a phone's hotspot); later changes
    // come in through AppHost::setNetwork. The default is unmetered.
    virtual bool isNetworkMetered() { return false; }

    // Dark icons on the status and navigation bars, for a light theme; light icons otherwise. The
    // default does nothing.
    virtual void setLightSystemBars(bool light) { (void)light; }

    // Opens a web link in the browser. The default does nothing.
    virtual void openUrl(const std::string &url) { (void)url; }

    // Encrypts a secret (a saved password) with a key that never leaves the platform's key store,
    // so the blob can be written to disk; unprotectSecret reverses it. nullopt when it can't (no
    // key store, or a blob from another key): the defaults have none, and secrets then aren't
    // saved. Android uses an AES-GCM key in the Android Keystore.
    virtual std::optional<std::vector<std::uint8_t>> protectSecret(std::span<const std::uint8_t> plain) {
        (void)plain;
        return std::nullopt;
    }
    virtual std::optional<std::vector<std::uint8_t>> unprotectSecret(std::span<const std::uint8_t> blob) {
        (void)blob;
        return std::nullopt;
    }

    // The app icon of the APK file at path, drawn into a sizePx square: sizePx * sizePx * 4
    // bytes of premultiplied RGBA8 (as Android draws), tightly packed. nullopt when there's none
    // (the default). Unlike the other methods, it's called on I/O pool threads, several at once
    // maybe, and may block for a while: it parses the package.
    virtual std::optional<std::vector<std::uint8_t>> apkIcon(const std::string &path, int sizePx) {
        (void)path;
        (void)sizePx;
        return std::nullopt;
    }

    // Shows the system's document picker for one document; the shell answers with
    // AppHost::documentPicked(request, ...), from any thread, exactly once per request it took.
    // False if there's no picker (the default), and then no answer comes. Android opens
    // ACTION_OPEN_DOCUMENT and reads the picked document's bytes without keeping a grant to it.
    // See document_picker.hpp.
    virtual bool pickDocument(std::uint64_t request, const DocumentPickOptions &options) {
        (void)request;
        (void)options;
        return false;
    }

    // Shows the system's picker for where to save one document; the shell writes options.bytes
    // there and answers with AppHost::documentSaved(request, ...), from any thread, exactly once
    // per request it took. False if there's no picker (the default), and then no answer comes.
    // Android opens ACTION_CREATE_DOCUMENT.
    virtual bool saveDocument(std::uint64_t request, const DocumentSaveOptions &options) {
        (void)request;
        (void)options;
        return false;
    }

    // --- Document trees ---

    // The calls behind document trees (kDocument locations), asked for once when the app starts;
    // null if there are none (the default). Its methods run on I/O pool threads. Android's goes
    // through ContentResolver and DocumentsContract.
    virtual std::shared_ptr<vfs::DocumentProvider> documentProvider() { return nullptr; }

    // Shows the system's picker for a folder to keep (ACTION_OPEN_DOCUMENT_TREE); the shell keeps
    // a grant to it and answers with AppHost::documentTreePicked(request, ...), from any thread,
    // exactly once per request it took. False if there's no picker (the default).
    virtual bool openDocumentTree(std::uint64_t request) {
        (void)request;
        return false;
    }
    // Gives up the grant to a tree picked earlier. The default does nothing.
    virtual void releaseDocumentTree(const std::string &uri) { (void)uri; }

    // --- Security keys ---

    // Finds the FIDO security key the user presents when an SFTP "sk-" key signs in, asked for
    // once when the app starts; null if there's no way to (the default). Its start() and stop()
    // run on I/O threads.
    virtual std::shared_ptr<vfs::SecurityKeySource> securityKeySource() { return nullptr; }
};

}  // namespace filepan::runtime
