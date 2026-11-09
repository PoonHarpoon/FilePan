// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/vfs/save.hpp"

#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <string_view>

namespace filepan::vfs {

namespace {

Result<void> writeAll(Backend &backend, const Location &location, std::span<const std::uint8_t> bytes,
                      const OpenWriteOptions &options) {
    auto stream = backend.openWrite(location, options);
    if (!stream) {
        return std::unexpected(stream.error());
    }
    if (!bytes.empty()) {
        if (auto written = (*stream)->write(bytes); !written) {
            return written;
        }
    }
    return (*stream)->close();
}

// ".notes.txt.filepan-1a2b3c4d": hidden, and recognizably ours if it's ever left behind.
std::string temporaryName(std::string_view name) {
    static thread_local std::mt19937 random{std::random_device{}()};
    char suffix[16];
    std::snprintf(suffix, sizeof suffix, "%08x", static_cast<unsigned>(random()));
    return "." + std::string(name) + ".filepan-" + suffix;
}

}  // namespace

Result<Entry> saveFile(Backend &backend, const Location &location, std::span<const std::uint8_t> bytes,
                       std::optional<FileStamp> expected) {
    if (location.isRoot()) {
        return fail(EISDIR);
    }
    std::optional<Entry> existing;
    if (auto entry = backend.lstat(location)) {
        existing = std::move(*entry);
    } else if (entry.error().code != ENOENT) {
        return std::unexpected(entry.error());
    }
    if (existing && existing->type == EntryType::kSymlink) {
        // Whether the link's target changed, as the reader saw it.
        auto target = backend.stat(location);
        if (!target) {
            return std::unexpected(target.error());
        }
        if (expected && FileStamp::of(*target) != *expected) {
            return fail(kSaveConflict, "The file changed since it was opened");
        }
    } else if (expected && (!existing || FileStamp::of(*existing) != *expected)) {
        return fail(kSaveConflict, existing ? "The file changed since it was opened" : "The file is gone");
    }
    if (existing && existing->isDirectoryLike()) {
        return fail(EISDIR);
    }

    const bool inPlace = existing && (existing->type == EntryType::kSymlink || existing->linkCount > 1);
    if (inPlace) {
        if (auto written = writeAll(backend, location, bytes, {.mode = WriteMode::kReplace}); !written) {
            return std::unexpected(written.error());
        }
        return backend.stat(location);
    }

    const std::uint32_t permissions = existing && existing->mode != 0 ? existing->mode & 07777 : 0666;
    const Location temporary = *location.withName(temporaryName(location.name()));
    if (auto written = writeAll(backend, temporary, bytes, {.mode = WriteMode::kCreateNew, .permissions = permissions});
        !written) {
        (void)backend.remove(temporary, false, nullptr, CancellationToken());
        return std::unexpected(written.error());
    }
    // The umask may have taken bits the old file had.
    if (existing && existing->mode != 0 && backend.capabilities().has(Capability::kChmod)) {
        (void)backend.chmod(temporary, permissions);
    }
    if (auto renamed = backend.rename(temporary, location, true); !renamed) {
        (void)backend.remove(temporary, false, nullptr, CancellationToken());
        return std::unexpected(renamed.error());
    }
    return backend.stat(location);
}

}  // namespace filepan::vfs
