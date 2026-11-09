// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "filepan/create_archive.hpp"
#include "filepan/runtime/app_host.hpp"
#include "filepan/verify_archive.hpp"
#include "filepan/vfs/entry.hpp"
#include "filepan/vfs/location.hpp"

// The dialogs and sheets that ask before a file job: a name (rename, new folder, new file), a
// delete confirmation, and the extract and compress sheets. Each lists the target folder on the
// I/O pool so names can be checked as they're typed. And one after a job: what testing an
// archive found.
namespace filepan::app {

class Overlays;

// Why a name can't be used, or empty: empty, '/', "." and "..", or taken by another entry
// (original, the name being renamed, is allowed).
std::string nameError(std::string_view name, const std::vector<std::string> &existing, std::string_view original = {});
// The part of a file name before its extension: "report" for "report.pdf", "data" for
// "data.tar.zst"; the whole name for folders and dot files.
std::string_view nameStem(std::string_view name, bool directory);

struct NameDialog {
    std::string title;   // "Rename"
    std::string action;  // "Rename", "Create"
    std::string label = "Name";
    std::string initial;
    // Selects the stem (or all of it) to type over.
    bool directory = false;
    // Its own current name, which is allowed.
    std::string original;
    vfs::Location folder;
    // A name taken by a file can be replaced, with a switch under the field. Never for folders,
    // either way.
    bool allowReplace = false;
    // replace: replace the file already at name.
    std::function<void(const std::string &name, bool replace)> onSubmit;
};
void showNameDialog(runtime::AppServices &services, Overlays &overlays, NameDialog dialog);

// "Delete 3 items?": a sheet listing them, with Delete and Cancel.
void showDeleteSheet(Overlays &overlays, const std::vector<vfs::Entry> &entries, std::function<void()> onConfirm);

struct ExtractChoice {
    // Into a new folder named after the archive, rather than straight into its folder.
    bool newFolder = true;
};
// An encrypted archive's password is asked for once the job meets it, with the app's archive
// password sheet.
void showExtractSheet(Overlays &overlays, const std::vector<vfs::Entry> &archives, const vfs::Location &folder,
                      std::function<void(const ExtractChoice &choice)> onExtract);
// The folder an archive extracts into: its name without the archive extensions.
std::string extractFolderName(std::string_view archiveName);

// "“a.zip” is damaged": what a test found, each damaged entry (or the archive as a whole) with the
// reason, under a line saying how many of the entries read had a problem.
void showArchiveProblemsSheet(Overlays &overlays, const std::string &archiveName, std::uint64_t entries,
                              const std::vector<ArchiveProblem> &problems);

// The formats the compress sheet offers, in order.
struct ArchiveFormat {
    const char *label;      // "ZIP"
    const char *extension;  // ".zip"
    int format;             // ARCHIVE_FORMAT_*
    int filter;             // ARCHIVE_FILTER_*
    int levels[3];          // fast, normal, best, in the compressor's scale.
};
const std::vector<ArchiveFormat> &archiveFormats();

struct CompressChoice {
    std::string name;  // With the extension.
    CreateArchiveOptions options;
    // Delete the sources once the archive is complete.
    bool deleteSources = false;
};
void showCompressSheet(runtime::AppServices &services, Overlays &overlays, const std::vector<vfs::Entry> &entries,
                       const vfs::Location &folder, std::function<void(const CompressChoice &choice)> onCompress);

}  // namespace filepan::app
