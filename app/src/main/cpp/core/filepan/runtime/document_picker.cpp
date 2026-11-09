// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/runtime/document_picker.hpp"

#include <utility>

#include "filepan/runtime/platform.hpp"

namespace filepan::runtime {

bool DocumentPicker::pick(DocumentPickOptions options, Callback done) {
    cancel();
    const std::uint64_t request = ++request_;
    if (!platform_.pickDocument(request, options)) {
        return false;
    }
    done_ = std::move(done);
    return true;
}

void DocumentPicker::deliver(std::uint64_t request, PickedDocument document) {
    if (request != request_ || !done_) {
        return;
    }
    Callback done = std::exchange(done_, nullptr);
    done(std::move(document));
}

void DocumentPicker::cancel() {
    if (done_) {
        Callback done = std::exchange(done_, nullptr);
        done(PickedDocument{.status = PickedDocument::Status::cancelled});
    }
}

bool DocumentSaver::save(DocumentSaveOptions options, Callback done) {
    cancel();
    const std::uint64_t request = ++request_;
    if (!platform_.saveDocument(request, options)) {
        return false;
    }
    done_ = std::move(done);
    return true;
}

void DocumentSaver::deliver(std::uint64_t request, SavedDocument document) {
    if (request != request_ || !done_) {
        return;
    }
    Callback done = std::exchange(done_, nullptr);
    done(std::move(document));
}

void DocumentSaver::cancel() {
    if (done_) {
        Callback done = std::exchange(done_, nullptr);
        done(SavedDocument{.status = SavedDocument::Status::cancelled});
    }
}

bool DocumentTreePicker::pick(Callback done) {
    cancel();
    const std::uint64_t request = ++request_;
    if (!platform_.openDocumentTree(request)) {
        return false;
    }
    done_ = std::move(done);
    return true;
}

void DocumentTreePicker::deliver(std::uint64_t request, PickedTree tree) {
    if (request != request_ || !done_) {
        return;
    }
    Callback done = std::exchange(done_, nullptr);
    done(std::move(tree));
}

void DocumentTreePicker::cancel() {
    if (done_) {
        Callback done = std::exchange(done_, nullptr);
        done(PickedTree{.status = PickedTree::Status::cancelled});
    }
}

}  // namespace filepan::runtime
