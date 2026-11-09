// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/file_actions.hpp"

namespace filepan::app {

std::vector<vfs::Location> FileTargets::locations() const {
    std::vector<vfs::Location> out;
    out.reserve(entries.size());
    for (const vfs::Entry &entry : entries) {
        out.push_back(directory.child(entry.name));
    }
    return out;
}

const char *fileActionLabel(FileAction action) {
    switch (action) {
    case FileAction::copy:
        return "Copy";
    case FileAction::cut:
        return "Move";
    case FileAction::paste:
        return "Paste";
    case FileAction::remove:
        return "Delete";
    case FileAction::rename:
        return "Rename";
    case FileAction::newFolder:
        return "New folder";
    case FileAction::newFile:
        return "New file";
    case FileAction::share:
        return "Share";
    case FileAction::properties:
        return "Properties";
    case FileAction::extract:
        return "Extract";
    case FileAction::verify:
        return "Test integrity";
    case FileAction::compress:
        return "Compress";
    case FileAction::openWith:
        return "Open with";
    }
    return "";
}

const char *fileActionIcon(FileAction action) {
    switch (action) {
    case FileAction::copy:
        return "content_copy";
    case FileAction::cut:
        return "drive_file_move";
    case FileAction::paste:
        return "content_paste";
    case FileAction::remove:
        return "delete";
    case FileAction::rename:
        return "drive_file_rename_outline";
    case FileAction::newFolder:
        return "create_new_folder";
    case FileAction::newFile:
        return "note_add";
    case FileAction::share:
        return "share";
    case FileAction::properties:
        return "info";
    case FileAction::extract:
        return "unarchive";
    case FileAction::verify:
        return "fact_check";
    case FileAction::compress:
        return "archive";
    case FileAction::openWith:
        return "open_in_new";
    }
    return "";
}

void FileActions::perform(FileAction action, const FileTargets &targets) {
    switch (action) {
    case FileAction::copy:
        return copy(targets);
    case FileAction::cut:
        return cut(targets);
    case FileAction::paste:
        return paste(targets.directory);
    case FileAction::remove:
        return remove(targets);
    case FileAction::rename:
        return rename(targets);
    case FileAction::newFolder:
        return newFolder(targets.directory);
    case FileAction::newFile:
        return newFile(targets.directory);
    case FileAction::share:
        return share(targets);
    case FileAction::properties:
        return properties(targets);
    case FileAction::extract:
        return extract(targets);
    case FileAction::verify:
        return verify(targets);
    case FileAction::compress:
        return compress(targets);
    case FileAction::openWith:
        return openWith(targets);
    }
}

}  // namespace filepan::app
