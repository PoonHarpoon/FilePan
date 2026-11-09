// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "filepan/jobs/job.hpp"

// Files and text shared to FilePan (Android's ACTION_SEND and ACTION_SEND_MULTIPLE), for the user
// to save into a folder:
//
//   shell      opens each shared document at once (its read grant ends with the activity, an open
//              descriptor doesn't) and hands the descriptors over
//   any thread AppHost::shareReceived(items) -> UI thread
//   UI thread  ShareInbox::deliver -> the app's listener, which offers to save them
//
// Another app asking FilePan for a document (ACTION_CREATE_DOCUMENT, or ACTION_GET_CONTENT and
// ACTION_OPEN_DOCUMENT) comes the same way, through AppHost::documentRequested, and the app
// answers with Platform::answerDocumentRequest.
namespace filepan::runtime {

struct SharedItems {
    // Each owns its descriptor: what nobody saves is closed with it.
    std::vector<jobs::ImportItem> items;
    // Shared by another app, which the user goes back to once they're saved.
    bool fromOtherApp = false;
};

struct DocumentRequest {
    enum class Kind : std::uint8_t {
        // A new document for the caller to write: the user picks a folder and a name.
        create,
        // Existing files for the caller to read: the user picks them.
        pick,
        // The caller went away without an answer; what it asked for goes.
        ended,
    };
    Kind kind = Kind::ended;
    // create: the caller's suggested name.
    std::string name;
    // create: the type the caller will write. pick: the types it takes ("image/*" for any image),
    // empty for any.
    std::vector<std::string> mimeTypes;
    // pick: several may be picked.
    bool multiple = false;
};

// UI thread only.
template <typename T>
class Inbox {
public:
    using Listener = std::function<void(T value)>;

    // What came before a listener did goes to it at once. An empty listener detaches.
    void setListener(Listener listener) {
        listener_ = std::move(listener);
        if (listener_ && waiting_) {
            T value = std::move(*waiting_);
            waiting_.reset();
            listener_(std::move(value));
        }
    }

    // Kept for a listener while there is none, replacing an older one.
    void deliver(T value) {
        if (listener_) {
            listener_(std::move(value));
        } else {
            waiting_ = std::move(value);
        }
    }

private:
    Listener listener_;
    std::optional<T> waiting_;
};

using ShareInbox = Inbox<SharedItems>;
using DocumentRequestInbox = Inbox<DocumentRequest>;

}  // namespace filepan::runtime
