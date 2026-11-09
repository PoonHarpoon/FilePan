// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/document_trees.hpp"

#include <algorithm>
#include <utility>

#include "filepan/app/app.hpp"
#include "filepan/runtime/app_host.hpp"

namespace filepan::app {

bool documentTreesAvailable(const AppContext &context) {
    return context.state.settings.peek().documentTrees && context.services.documentTrees != nullptr &&
           context.services.treePicker != nullptr;
}

std::string newTreeId(const std::string &name, const std::vector<DocumentTree> &trees) {
    std::string base;
    for (const char c : name) {
        // Not a path separator, nor the control characters a provider might pass on.
        base += c == '/' || static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
    }
    if (base.find_first_not_of(' ') == std::string::npos) {
        base = "Storage";
    }
    const auto taken = [&](const std::string &id) {
        return std::ranges::any_of(trees, [&](const DocumentTree &tree) { return tree.id == id; });
    };
    std::string id = base;
    for (int n = 2; taken(id); ++n) {
        id = base + " " + std::to_string(n);
    }
    return id;
}

void addDocumentTree(AppContext &context) {
    if (!documentTreesAvailable(context)) {
        return;
    }
    AppContext *ctx = &context;
    const bool asked = context.services.treePicker->pick(
            [ctx, alive = context.lifetime](runtime::PickedTree picked) {
                if (alive.expired()) {
                    return;
                }
                using Status = runtime::PickedTree::Status;
                if (picked.status == Status::cancelled) {
                    return;
                }
                if (picked.status != Status::picked || picked.uri.empty()) {
                    ctx->notify("Couldn't keep access to that folder");
                    return;
                }
                std::string id;
                ctx->state.updateSettings([&](Settings &s) {
                    // The same folder picked again keeps its place.
                    const auto same = std::ranges::find(s.trees, picked.uri, &DocumentTree::uri);
                    if (same != s.trees.end()) {
                        id = same->id;
                        return;
                    }
                    id = newTreeId(picked.name, s.trees);
                    s.trees.push_back({id, picked.uri});
                });
                ctx->state.navigate(vfs::Location::document(id));
            });
    if (!asked) {
        context.notify("There's no folder picker here");
    }
}

void removeDocumentTree(AppContext &context, const std::string &id) {
    std::string uri;
    context.state.updateSettings([&](Settings &s) {
        const auto at = std::ranges::find(s.trees, id, &DocumentTree::id);
        if (at != s.trees.end()) {
            uri = at->uri;
            s.trees.erase(at);
        }
    });
    if (!uri.empty()) {
        context.services.platform.releaseDocumentTree(uri);
        context.notify("Removed " + id);
    }
}

}  // namespace filepan::app
