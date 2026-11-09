// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <string>
#include <vector>

#include "filepan/app/settings.hpp"

// Document trees on the storage home (experimental, behind Settings::documentTrees): adding one
// through the system's folder picker, and removing it again.
namespace filepan::app {

struct AppContext;

// Whether the home shows trees: the setting is on and the platform has them.
bool documentTreesAvailable(const AppContext &context);
// Opens the system's folder picker; the folder picked is saved as a tree and opened.
void addDocumentTree(AppContext &context);
// Forgets the tree and gives up its grant.
void removeDocumentTree(AppContext &context, const std::string &id);
// A tree's id: its display name, made unique among trees ("Music", "Music 2").
std::string newTreeId(const std::string &name, const std::vector<DocumentTree> &trees);

}  // namespace filepan::app
