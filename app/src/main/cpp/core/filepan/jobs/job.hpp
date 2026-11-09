// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "filepan/checksum.hpp"
#include "filepan/create_archive.hpp"
#include "filepan/error.hpp"
#include "filepan/unique_fd.hpp"
#include "filepan/verify_archive.hpp"
#include "filepan/vfs/location.hpp"

namespace filepan::jobs {

using JobId = std::uint64_t;

// Copies sources into targetDirectory, recursively, keeping their names. Works across backends
// through streams, and uses copyFile() when both ends are local.
struct CopyJob {
    std::vector<vfs::Location> sources;
    vfs::Location targetDirectory;
};

// Like CopyJob, but renames when source and target share a backend (falling back to copying and
// deleting across file systems) and deletes the sources after copying otherwise.
struct MoveJob {
    std::vector<vfs::Location> sources;
    vfs::Location targetDirectory;
};

// Deletes each target recursively.
struct DeleteJob {
    std::vector<vfs::Location> targets;
};

struct MakeDirectoryJob {
    vfs::Location location;
};

struct CreateFileJob {
    vfs::Location location;
};

struct RenameJob {
    vfs::Location source;
    std::string newName;
    // A file already at the new name is replaced without asking; a folder there still asks.
    bool replace = false;
};

// Extracts a whole archive into a directory, on any backends. The archive file may itself be
// inside an archive or on a server.
struct ExtractArchiveJob {
    vfs::Location archive;
    vfs::Location targetDirectory;
    std::vector<std::string> passwords;
};

// Archives sources into a new archive file, on any backends.
struct CreateArchiveJob {
    std::vector<vfs::Location> sources;
    vfs::Location target;
    CreateArchiveOptions options;
};

// Reads a whole archive, on any backend, to find damage; it writes nothing. What it found is in
// the JobResult. An encrypted entry with no (or the wrong) password fails it with EACCES.
struct VerifyArchiveJob {
    vfs::Location archive;
    std::vector<std::string> passwords;
};

struct ChecksumJob {
    vfs::Location file;
    std::vector<ChecksumAlgorithm> algorithms;
};

// One file to save: an open file another app shared (Android's ACTION_SEND), or shared text.
struct ImportItem {
    std::string name;
    // What the sharing app said, if anything; the copy reads to the end either way.
    std::optional<std::uint64_t> size{};
    // Read from wherever it is (a pipe works too); empty for text.
    UniqueFd fd{};
    std::string text{};
    // A file already at its name is replaced without asking; a folder there still asks.
    bool replace = false;
};

// Saves items into targetDirectory under their names, as new files. The job owns their files
// and closes them as it goes, so it keeps working while the app is in the background.
struct ImportJob {
    std::vector<ImportItem> items;
    vfs::Location targetDirectory;
};

// The order matches JobKind.
using JobSpec = std::variant<CopyJob, MoveJob, DeleteJob, MakeDirectoryJob, CreateFileJob,
                             RenameJob, ExtractArchiveJob, CreateArchiveJob, ChecksumJob,
                             ImportJob, VerifyArchiveJob>;

enum class JobKind : std::uint8_t {
    kCopy,
    kMove,
    kDelete,
    kMakeDirectory,
    kCreateFile,
    kRename,
    kExtractArchive,
    kCreateArchive,
    kChecksum,
    kImport,
    kVerifyArchive,
};

inline JobKind kindOf(const JobSpec &spec) { return static_cast<JobKind>(spec.index()); }

enum class JobState : std::uint8_t {
    kQueued,
    kRunning,
    // Blocked until the DecisionProvider answers a conflict or an error.
    kWaitingForDecision,
    // Done, possibly with skipped items.
    kSucceeded,
    kFailed,
    kCancelled,
};

inline bool isFinished(JobState state) {
    return state == JobState::kSucceeded || state == JobState::kFailed
           || state == JobState::kCancelled;
}

struct JobProgress {
    JobId id = 0;
    JobKind kind = JobKind::kCopy;
    JobState state = JobState::kQueued;
    // Still counting what there is to do; the totals are growing.
    bool scanning = false;
    // A total of 0 means unknown, e.g. for a rename.
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
    std::uint64_t itemsDone = 0;
    std::uint64_t itemsTotal = 0;
    // Included in itemsDone.
    std::uint64_t itemsSkipped = 0;
    // The path being worked on, for display.
    std::string currentItem;
    // Smoothed over recent updates; 0 until known.
    double bytesPerSecond = 0;
    std::optional<std::chrono::seconds> eta;
    // Why the job failed, for kFailed.
    std::optional<Error> error;
};

// What a finished job produced beyond its progress.
struct JobResult {
    // For ChecksumJob: raw digests indexed by ChecksumAlgorithm, empty where not requested.
    std::array<std::vector<std::uint8_t>, kChecksumAlgorithmCount> checksums;
    // For VerifyArchiveJob: the headers read, and the damage found; none for an intact archive.
    std::uint64_t archiveEntries = 0;
    std::vector<ArchiveProblem> archiveProblems;
};

}  // namespace filepan::jobs
