// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Picking one document with the system's picker (Android's ACTION_OPEN_DOCUMENT), for content the
// app needs once, such as an SSH key file: the shell reads the picked document's bytes and hands
// them over, and keeps no grant to it.
//
//   app (UI thread)  DocumentPicker::pick -> Platform::pickDocument(request, options)
//   shell            shows the picker; on the result, reads the bytes off its main thread
//   any thread       AppHost::documentPicked(request, document) -> UI thread
//   UI thread        DocumentPicker::deliver -> the pick's callback, once
namespace filepan::runtime {

struct DocumentPickOptions {
    // What the picker offers, as MIME types; "*/*" for anything.
    std::vector<std::string> mimeTypes{"*/*"};
    // A document longer than this isn't read: it comes back as tooLarge.
    std::size_t maxBytes = 1024 * 1024;
};

struct PickedDocument {
    enum class Status : std::uint8_t {
        picked = 0,
        cancelled = 1,  // Backed out of the picker, or a newer pick replaced this one.
        failed = 2,     // No picker, or the document couldn't be read.
        tooLarge = 3,   // Longer than maxBytes.
    };

    Status status = Status::cancelled;
    // The document's display name ("id_ed25519"), if the provider gave one.
    std::string name;
    // Its contents, when picked.
    std::string bytes;

    bool operator==(const PickedDocument &) const = default;
};

class Platform;

// The app's side: one pick at a time. UI thread only.
class DocumentPicker {
public:
    using Callback = std::function<void(PickedDocument)>;

    explicit DocumentPicker(Platform &platform) : platform_(platform) {}

    // Opens the picker; done runs once with the outcome, when the platform answers. A pick still
    // waiting is cancelled first (its callback runs now). False, with done never run, where the
    // platform has no picker.
    bool pick(DocumentPickOptions options, Callback done);
    // The platform's answer to request. Answers to replaced or unknown requests are dropped.
    void deliver(std::uint64_t request, PickedDocument document);
    // Cancels the waiting pick, if any (its screen is going).
    void cancel();

    [[nodiscard]] bool waiting() const { return static_cast<bool>(done_); }
    // The request the platform was last asked for; 0 before any.
    [[nodiscard]] std::uint64_t lastRequest() const { return request_; }

private:
    Platform &platform_;
    std::uint64_t request_ = 0;
    Callback done_;
};

// Saving one document with the system's picker (ACTION_CREATE_DOCUMENT), such as a settings
// backup: the user picks where and the name, and the shell writes the bytes there off its main
// thread. Same flow as picking, the other way.
struct DocumentSaveOptions {
    // The name the picker suggests.
    std::string name;
    std::string mimeType = "application/octet-stream";
    std::string bytes;
};

struct SavedDocument {
    enum class Status : std::uint8_t {
        saved = 0,
        cancelled = 1,  // Backed out of the picker, or a newer save replaced this one.
        failed = 2,     // No picker, or the document couldn't be written.
    };
    Status status = Status::cancelled;
    // The name it was saved under, if the provider gave one.
    std::string name;

    bool operator==(const SavedDocument &) const = default;
};

// The app's side: one save at a time. UI thread only.
class DocumentSaver {
public:
    using Callback = std::function<void(SavedDocument)>;

    explicit DocumentSaver(Platform &platform) : platform_(platform) {}

    // Opens the picker; done runs once with the outcome, when the platform answers. A save still
    // waiting is cancelled first (its callback runs now). False, with done never run, where the
    // platform has no picker.
    bool save(DocumentSaveOptions options, Callback done);
    // The platform's answer to request. Answers to replaced or unknown requests are dropped.
    void deliver(std::uint64_t request, SavedDocument document);
    void cancel();
    [[nodiscard]] bool waiting() const { return static_cast<bool>(done_); }
    // The request the platform was last asked for; 0 before any.
    [[nodiscard]] std::uint64_t lastRequest() const { return request_; }

private:
    Platform &platform_;
    std::uint64_t request_ = 0;
    Callback done_;
};

// Picking a folder to keep with the system's picker (ACTION_OPEN_DOCUMENT_TREE), for a document
// tree: the shell takes a persistable grant to it and hands over its URI. Same flow as picking.
struct PickedTree {
    enum class Status : std::uint8_t {
        picked = 0,
        cancelled = 1,  // Backed out of the picker, or a newer pick replaced this one.
        failed = 2,     // No picker, or no grant could be kept.
    };
    Status status = Status::cancelled;
    // The tree's URI, which the platform's DocumentProvider takes, and its display name.
    std::string uri;
    std::string name;

    bool operator==(const PickedTree &) const = default;
};

// The app's side: one pick at a time. UI thread only.
class DocumentTreePicker {
public:
    using Callback = std::function<void(PickedTree)>;

    explicit DocumentTreePicker(Platform &platform) : platform_(platform) {}

    // Opens the picker; done runs once with the outcome. A pick still waiting is cancelled first.
    // False, with done never run, where the platform has no picker.
    bool pick(Callback done);
    // The platform's answer to request. Answers to replaced or unknown requests are dropped.
    void deliver(std::uint64_t request, PickedTree tree);
    void cancel();
    [[nodiscard]] bool waiting() const { return static_cast<bool>(done_); }
    // The request the platform was last asked for; 0 before any.
    [[nodiscard]] std::uint64_t lastRequest() const { return request_; }

private:
    Platform &platform_;
    std::uint64_t request_ = 0;
    Callback done_;
};

}  // namespace filepan::runtime
