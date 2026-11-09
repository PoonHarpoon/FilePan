// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Copy, move, delete, make directory, create file, rename, and saving shared files (import).

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "filepan/copy_file.hpp"
#include "filepan/jobs/job_context.hpp"
#include "filepan/move.hpp"
#include "filepan/vfs/local_backend.hpp"

namespace filepan::jobs {

namespace {

using vfs::Backend;
using vfs::Entry;
using vfs::EntryType;
using vfs::Location;

struct TreeSize {
    std::uint64_t bytes = 0;
    std::uint64_t items = 0;
};

// Counts a tree without following symlinks; only regular files have bytes to copy. What can't be
// listed counts as itself, and the copy reports the error when it gets there.
Result<TreeSize> measure(JobContext &context, Backend &backend, const Location &location,
                         const Entry &entry) {
    TreeSize size{entry.type == EntryType::kFile ? entry.size : 0, 1};
    if (entry.type != EntryType::kDirectory) {
        return size;
    }
    std::vector<Entry> directories;
    const auto listed = backend.list(
        location, {},
        [&](std::vector<Entry> batch) {
            for (auto &child : batch) {
                ++size.items;
                if (child.type == EntryType::kFile) {
                    size.bytes += child.size;
                } else if (child.type == EntryType::kDirectory) {
                    directories.push_back(std::move(child));
                }
            }
        },
        context.cancel());
    if (!listed && listed.error().code == ECANCELED) {
        return std::unexpected(listed.error());
    }
    for (const auto &directory : directories) {
        const auto child = measure(context, backend, location.child(directory.name), directory);
        if (!child) {
            return child;
        }
        size.bytes += child->bytes;
        size.items += child->items - 1;  // The directory itself was counted when listed.
    }
    return size;
}

const char *describe(CopyFileStage stage) {
    switch (stage) {
        case CopyFileStage::kOpenSource: return "open";
        case CopyFileStage::kOpenTarget: return "create";
        case CopyFileStage::kCopyData: return "copy";
        case CopyFileStage::kCloseTarget: return "write";
        default: return "copy";
    }
}

// Writes what read gives, until it gives 0 bytes, into a new file at target, reporting the bytes
// as done. On failure they're taken back and the partial file removed. read takes a buffer and
// returns Result<std::size_t>, like InputStream::read.
template <typename Read>
Result<void> writeNewFile(JobContext &context, Backend &targetBackend, const Location &target,
                          std::uint32_t permissions, std::vector<std::uint8_t> &buffer, Read &&read) {
    std::uint64_t copied = 0;
    auto result = [&]() -> Result<void> {
        auto output = targetBackend.openWrite(
            target, {.mode = vfs::WriteMode::kCreateNew, .permissions = permissions});
        if (!output) {
            return std::unexpected(output.error());
        }
        buffer.resize(1024 * 1024);
        while (true) {
            if (context.isCancelled()) {
                return fail(ECANCELED);
            }
            const Result<std::size_t> length = read(std::span(buffer));
            if (!length) {
                return std::unexpected(length.error());
            }
            if (*length == 0) {
                break;
            }
            if (auto written = (*output)->write(std::span(buffer).first(*length)); !written) {
                return written;
            }
            copied += *length;
            context.addDone(*length, 0);
        }
        return (*output)->close();
    }();
    if (!result) {
        context.undoBytes(copied);
        // Remove the partial copy, unless the target was never created.
        if (result.error().code != EEXIST) {
            (void)targetBackend.remove(target, false, nullptr, CancellationToken());
        }
    }
    return result;
}

class Transfer {
public:
    Transfer(JobContext &context, bool move) : context_(context), move_(move) {}

    Result<void> run(const std::vector<Location> &sources, const Location &targetDirectory) {
        auto targetBackend = context_.backendFor(targetDirectory);
        if (!targetBackend) {
            return std::unexpected(targetBackend.error());
        }
        scanned_ = needsScan(sources, targetDirectory, **targetBackend);
        if (scanned_) {
            context_.setScanning(true);
            for (const auto &source : sources) {
                auto backend = context_.backendFor(source);
                if (!backend) {
                    return std::unexpected(backend.error());
                }
                const auto entry = (*backend)->lstat(source);
                if (!entry) {
                    context_.addTotals(0, 1);
                    continue;
                }
                const auto size = measure(context_, **backend, source, *entry);
                if (!size) {
                    return std::unexpected(size.error());
                }
                context_.addTotals(size->bytes, size->items);
            }
            context_.setScanning(false);
        } else {
            context_.addTotals(0, sources.size());
        }
        for (const auto &source : sources) {
            if (source.isRoot()) {
                return fail(EINVAL, "can't move or copy a root: " + source.toString());
            }
            const auto step = transfer(source, targetDirectory.child(source.name()));
            if (!step) {
                return std::unexpected(step.error());
            }
        }
        return {};
    }

private:
    // A move that renames needs no totals; scan unless every source can be renamed.
    bool needsScan(const std::vector<Location> &sources, const Location &targetDirectory,
                   Backend &targetBackend) {
        if (!move_) {
            return true;
        }
        const auto targetEntry = targetBackend.stat(targetDirectory);
        for (const auto &source : sources) {
            if (!source.sameRoot(targetDirectory)) {
                return true;
            }
            if (source.isLocal()) {
                // Another file system means copying.
                const auto entry = targetBackend.lstat(source);
                if (entry && targetEntry && entry->device != targetEntry->device) {
                    return true;
                }
            }
        }
        return false;
    }

    // What skipping or renaming this entry accounts for.
    Result<TreeSize> treeSize(Backend &backend, const Location &location, const Entry &entry) {
        if (!scanned_) {
            return TreeSize{0, 1};
        }
        return measure(context_, backend, location, entry);
    }

    Result<Step> skipTree(Backend &backend, const Location &location, const Entry &entry) {
        const auto size = treeSize(backend, location, entry);
        if (!size) {
            return std::unexpected(size.error());
        }
        context_.addSkipped(size->bytes, size->items);
        return Step::kSkipped;
    }

    Result<Step> skipItem(std::uint64_t bytes = 0) {
        context_.addSkipped(bytes, 1);
        return Step::kSkipped;
    }

    Result<Step> transfer(const Location &source, Location target) {
        context_.setCurrent(source);
        auto sourceBackendResult = context_.backendFor(source);
        auto targetBackendResult = context_.backendFor(target);
        if (!sourceBackendResult || !targetBackendResult) {
            return std::unexpected(!sourceBackendResult ? sourceBackendResult.error()
                                                        : targetBackendResult.error());
        }
        Backend &sourceBackend = **sourceBackendResult;
        Backend &targetBackend = **targetBackendResult;

        const auto stat = context_.attempt(source, "read",
                                           [&] { return sourceBackend.lstat(source); });
        if (!stat) {
            return std::unexpected(stat.error());
        }
        if (!*stat) {
            return skipItem();
        }
        const Entry entry = std::move(**stat);
        const bool isDirectory = entry.type == EntryType::kDirectory;

        if (isDirectory && source.isAncestorOf(target, false)) {
            const auto refused = context_.attempt(source, move_ ? "move" : "copy", [&] {
                return Result<void>(fail(EINVAL, "can't put a directory inside itself"));
            });
            if (!refused) {
                return std::unexpected(refused.error());
            }
            return skipTree(sourceBackend, source, entry);
        }

        bool merge = false;
        if (const auto existing = targetBackend.lstat(target)) {
            if (source == target) {
                if (move_) {
                    // Already there.
                    const auto size = treeSize(sourceBackend, source, entry);
                    if (!size) {
                        return std::unexpected(size.error());
                    }
                    context_.addDone(size->bytes, size->items);
                    return Step::kDone;
                }
                // Copying a file onto itself makes a copy next to it.
                auto free = freeLocation(targetBackend, target, isDirectory);
                if (!free) {
                    return std::unexpected(free.error());
                }
                target = std::move(*free);
            } else {
                const bool canMerge = isDirectory && existing->type == EntryType::kDirectory;
                const auto decision = context_.resolveConflict(ConflictRequest{
                    .source = source,
                    .target = target,
                    .sourceEntry = entry,
                    .targetEntry = *existing,
                    .canMerge = canMerge,
                });
                if (!decision) {
                    return fail(ECANCELED);
                }
                switch (decision->action) {
                    case ConflictAction::kSkip:
                        return skipTree(sourceBackend, source, entry);
                    case ConflictAction::kKeepBoth: {
                        auto free = freeLocation(targetBackend, target, isDirectory);
                        if (!free) {
                            return std::unexpected(free.error());
                        }
                        target = std::move(*free);
                        break;
                    }
                    case ConflictAction::kMerge:
                        merge = true;
                        break;
                    case ConflictAction::kReplace: {
                        const auto removed = context_.attempt(target, "replace", [&] {
                            return targetBackend.remove(target, true, nullptr, context_.cancel());
                        });
                        if (!removed) {
                            return std::unexpected(removed.error());
                        }
                        if (!*removed) {
                            return skipTree(sourceBackend, source, entry);
                        }
                        break;
                    }
                    case ConflictAction::kAbort:
                        return fail(ECANCELED);
                }
            }
        }

        if (move_ && !merge && source.sameRoot(target)) {
            const auto moved = moveWithin(sourceBackend, source, target, entry);
            if (!moved || *moved) {
                return moved ? Result<Step>(**moved) : std::unexpected(moved.error());
            }
            // The backend can't rename between these two; copy and delete instead.
        }

        Result<Step> step = Step::kDone;
        switch (entry.type) {
            case EntryType::kDirectory:
                return copyDirectory(sourceBackend, targetBackend, source, target, entry, merge);
            case EntryType::kSymlink:
                step = copySymlink(sourceBackend, targetBackend, source, target);
                break;
            case EntryType::kFile:
                step = copyRegularFile(sourceBackend, targetBackend, source, target, entry);
                break;
            case EntryType::kOther: {
                const auto refused = context_.attempt(source, move_ ? "move" : "copy", [&] {
                    return Result<void>(fail(ENOTSUP, "special file"));
                });
                if (!refused) {
                    return std::unexpected(refused.error());
                }
                return skipItem();
            }
        }
        if (step && *step == Step::kDone && move_) {
            const auto removed = context_.attempt(source, "remove", [&] {
                return sourceBackend.remove(source, false, nullptr, context_.cancel());
            });
            if (!removed) {
                return std::unexpected(removed.error());
            }
        }
        return step;
    }

    // Renames within a backend. Returns nullopt when it needs copying instead.
    Result<std::optional<Step>> moveWithin(Backend &backend, const Location &source,
                                           const Location &target, const Entry &entry) {
        class Callbacks : public MoveCallbacks {
        public:
            Callbacks(JobContext &context, bool countItems)
                : context_(context), countItems_(countItems) {}
            bool onProgress(std::uint64_t size, std::uint64_t count, std::string_view) override {
                bytes += size;
                items += countItems_ ? count : 0;
                context_.addDone(size, countItems_ ? count : 0);
                return !context_.isCancelled();
            }
            JobContext &context_;
            bool countItems_;
            std::uint64_t bytes = 0;
            std::uint64_t items = 0;
        } callbacks(context_, scanned_);

        bool copied = false;
        const auto moved = context_.attempt(source, "move", [&]() -> Result<bool> {
            if (source.isLocal()) {
                const auto result = movePath(source.path(), target.path(), {}, callbacks);
                if (!result) {
                    context_.undoBytes(std::exchange(callbacks.bytes, 0));
                    return std::unexpected(result.error());
                }
                copied = *result == MoveMethod::kCopied;
                return true;
            }
            const auto result = backend.rename(source, target, false);
            if (!result && (result.error().code == EXDEV || result.error().code == ENOTSUP)) {
                return false;
            }
            return result ? Result<bool>(true) : std::unexpected(result.error());
        });
        if (!moved) {
            return std::unexpected(moved.error());
        }
        if (!*moved) {
            const auto skipped = skipTree(backend, source, entry);
            return skipped ? Result<std::optional<Step>>(*skipped)
                           : std::unexpected(skipped.error());
        }
        if (!**moved) {
            return std::nullopt;
        }
        if (copied && scanned_) {
            // movePath() reported every entry as it copied.
            return Step::kDone;
        }
        if (copied) {
            context_.addDone(0, 1);
            return Step::kDone;
        }
        const auto size = treeSize(backend, source, entry);
        if (!size) {
            return std::unexpected(size.error());
        }
        context_.addDone(size->bytes, size->items);
        return Step::kDone;
    }

    Result<Step> copyDirectory(Backend &sourceBackend, Backend &targetBackend,
                               const Location &source, const Location &target,
                               const Entry &entry, bool merge) {
        if (!merge) {
            const auto made = context_.attempt(target, "create directory", [&] {
                // Writable for us while its contents arrive; the mode is applied afterwards.
                return targetBackend.makeDirectory(target, (entry.mode & 0777) | 0700);
            });
            if (!made) {
                return std::unexpected(made.error());
            }
            if (!*made) {
                return skipTree(sourceBackend, source, entry);
            }
        }
        std::vector<std::string> names;
        const auto listed = context_.attempt(source, "list", [&] {
            names.clear();
            return sourceBackend.list(
                source, {.withStat = false},
                [&](std::vector<Entry> batch) {
                    for (auto &child : batch) {
                        names.push_back(std::move(child.name));
                    }
                },
                context_.cancel());
        });
        if (!listed) {
            return std::unexpected(listed.error());
        }
        if (!*listed) {
            // Its contents stay behind, and so does the source directory.
            return skipTree(sourceBackend, source, entry);
        }
        if (!scanned_) {
            context_.addTotals(0, names.size());
        }
        bool skippedAny = false;
        for (const auto &name : names) {
            const auto step = transfer(source.child(name), target.child(name));
            if (!step) {
                return step;
            }
            skippedAny |= *step == Step::kSkipped;
        }
        // Best effort, like copyFile()'s attributes.
        if (!merge) {
            if (targetBackend.capabilities().has(vfs::Capability::kChmod) && entry.hasStat) {
                (void)targetBackend.chmod(target, entry.mode & 07777);
            }
            if (targetBackend.capabilities().has(vfs::Capability::kSetTimes) && entry.hasStat) {
                (void)targetBackend.setTimes(target, std::nullopt, entry.modifiedTime);
            }
        }
        context_.addDone(0, 1);
        if (move_ && !skippedAny) {
            const auto removed = context_.attempt(source, "remove", [&] {
                return sourceBackend.remove(source, false, nullptr, context_.cancel());
            });
            if (!removed) {
                return std::unexpected(removed.error());
            }
        }
        return skippedAny ? Step::kSkipped : Step::kDone;
    }

    Result<Step> copySymlink(Backend &sourceBackend, Backend &targetBackend,
                             const Location &source, const Location &target) {
        const auto linkTarget =
            context_.attempt(source, "read link", [&] { return sourceBackend.readLink(source); });
        if (!linkTarget) {
            return std::unexpected(linkTarget.error());
        }
        if (!*linkTarget) {
            return skipItem();
        }
        const auto created = context_.attempt(target, "create link", [&] {
            return targetBackend.createSymlink(target, **linkTarget);
        });
        if (!created) {
            return std::unexpected(created.error());
        }
        if (!*created) {
            return skipItem();
        }
        context_.addDone(0, 1);
        return Step::kDone;
    }

    Result<Step> copyRegularFile(Backend &sourceBackend, Backend &targetBackend,
                                 const Location &source, const Location &target,
                                 const Entry &entry) {
        const auto copied = context_.attempt(source, "copy", [&]() -> Result<void> {
            if (source.isLocal() && target.isLocal()) {
                return copyLocal(source, target);
            }
            return copyStream(sourceBackend, targetBackend, source, target, entry);
        });
        if (!copied) {
            return std::unexpected(copied.error());
        }
        if (!*copied) {
            return skipItem(entry.size);
        }
        context_.addDone(0, 1);
        return Step::kDone;
    }

    // copyFile(): copy_file_range or sendfile, with mode, times and user xattrs.
    Result<void> copyLocal(const Location &source, const Location &target) {
        class Callbacks : public CopyFileCallbacks {
        public:
            explicit Callbacks(JobContext &context) : context_(context) {}
            bool onProgress(std::uint64_t size) override {
                copied += size;
                context_.addDone(size, 0);
                return !context_.isCancelled();
            }
            JobContext &context_;
            std::uint64_t copied = 0;
        } callbacks(context_);
        const CopyFileOptions options{
            .replaceExisting = false,
            .noFollowLinks = true,
            .copyAttributes = false,
            .progressInterval = std::chrono::milliseconds(50),
        };
        const auto result =
            copyFile(source.path().c_str(), target.path().c_str(), options, callbacks);
        if (result.stage == CopyFileStage::kNone) {
            return {};
        }
        context_.undoBytes(callbacks.copied);
        if (result.stage == CopyFileStage::kAborted) {
            return fail(ECANCELED);
        }
        return fail(result.error, std::string(describe(result.stage)) + ": " + source.path());
    }

    Result<void> copyStream(Backend &sourceBackend, Backend &targetBackend,
                            const Location &source, const Location &target, const Entry &entry) {
        auto input = sourceBackend.openRead(source);
        if (!input) {
            return std::unexpected(input.error());
        }
        const auto result = writeNewFile(
            context_, targetBackend, target, entry.hasStat ? (entry.mode & 0777) | 0600 : 0666,
            buffer_, [&](std::span<std::uint8_t> buffer) { return (*input)->read(buffer); });
        if (!result) {
            return result;
        }
        if (entry.hasStat && targetBackend.capabilities().has(vfs::Capability::kSetTimes)) {
            (void)targetBackend.setTimes(target, std::nullopt, entry.modifiedTime);
        }
        return {};
    }

    JobContext &context_;
    bool move_;
    // Whether totals were counted up front; otherwise items are counted as they are found.
    bool scanned_ = false;
    std::vector<std::uint8_t> buffer_;
};

// The Location of a path reported from inside a backend's recursive operation.
Location locationOf(const Location &root, std::string_view path) { return root.withPath(path); }

bool isValidName(std::string_view name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos
           && name.find('\0') == std::string::npos;
}

}  // namespace

Result<void> runCopyOrMove(JobContext &context, const std::vector<Location> &sources,
                           const Location &targetDirectory, bool move) {
    return Transfer(context, move).run(sources, targetDirectory);
}

Result<void> runDelete(JobContext &context, const DeleteJob &job) {
    context.setScanning(true);
    for (const auto &target : job.targets) {
        auto backend = context.backendFor(target);
        if (!backend) {
            return std::unexpected(backend.error());
        }
        const auto entry = (*backend)->lstat(target);
        if (!entry) {
            context.addTotals(0, 1);
            continue;
        }
        const auto size = measure(context, **backend, target, *entry);
        if (!size) {
            return std::unexpected(size.error());
        }
        context.addTotals(0, size->items);
    }
    context.setScanning(false);

    for (const auto &target : job.targets) {
        Backend &backend = **context.backendFor(target);
        context.setCurrent(target);
        class Callbacks : public vfs::RemoveCallbacks {
        public:
            Callbacks(JobContext &context, const Location &root) : context_(context), root_(root) {}
            bool onProgress(std::uint64_t count, std::string_view) override {
                context_.addDone(0, count);
                return !context_.isCancelled();
            }
            ErrorAction onError(std::string_view path, const Error &error) override {
                switch (context_.resolveError(locationOf(root_, path), error, "delete")) {
                    case jobs::ErrorAction::kRetry: return ErrorAction::kRetry;
                    case jobs::ErrorAction::kSkip: return ErrorAction::kSkip;
                    case jobs::ErrorAction::kAbort: break;
                }
                return ErrorAction::kAbort;
            }
            JobContext &context_;
            const Location &root_;
        } callbacks(context, target);
        const auto removed = context.attempt(target, "delete", [&] {
            return backend.remove(target, true, &callbacks, context.cancel());
        });
        if (!removed) {
            return std::unexpected(removed.error());
        }
        if (!*removed) {
            context.addSkipped(0, 1);
        } else if ((*removed)->skippedCount) {
            context.addSkipped(0, (*removed)->skippedCount);
        }
    }
    return {};
}

namespace {

template <typename F>
Result<void> runSingle(JobContext &context, const Location &location, std::string_view operation,
                       F &&f) {
    context.addTotals(0, 1);
    context.setCurrent(location);
    auto backend = context.backendFor(location);
    if (!backend) {
        return std::unexpected(backend.error());
    }
    const auto done = context.attempt(location, operation, [&] { return f(**backend); });
    if (!done) {
        return std::unexpected(done.error());
    }
    if (*done) {
        context.addDone(0, 1);
    } else {
        context.addSkipped(0, 1);
    }
    return {};
}

}  // namespace

Result<void> runMakeDirectory(JobContext &context, const MakeDirectoryJob &job) {
    return runSingle(context, job.location, "create directory",
                     [&](Backend &backend) { return backend.makeDirectory(job.location, 0777); });
}

Result<void> runCreateFile(JobContext &context, const CreateFileJob &job) {
    return runSingle(context, job.location, "create file",
                     [&](Backend &backend) { return backend.createFile(job.location, 0666); });
}

Result<void> runRename(JobContext &context, const RenameJob &job) {
    if (!isValidName(job.newName) || job.source.isRoot()) {
        return fail(EINVAL, "invalid name: " + job.newName);
    }
    auto backendResult = context.backendFor(job.source);
    if (!backendResult) {
        return std::unexpected(backendResult.error());
    }
    Backend &backend = **backendResult;
    Location target = *job.source.withName(job.newName);
    context.addTotals(0, 1);
    context.setCurrent(job.source);
    if (target == job.source) {
        context.addDone(0, 1);
        return {};
    }
    bool replace = false;
    const auto source = backend.lstat(job.source);
    const auto existing = backend.lstat(target);
    if (source && existing) {
        if (source->device == existing->device && source->inode == existing->inode
            && source->inode != 0) {
            // A case-only rename on a case-insensitive file system finds the file itself.
            replace = true;
        } else if (job.replace && existing->type != EntryType::kDirectory) {
            replace = true;
        } else {
            const auto decision = context.resolveConflict(ConflictRequest{
                .source = job.source,
                .target = target,
                .sourceEntry = *source,
                .targetEntry = *existing,
                .canMerge = false,
            });
            if (!decision) {
                return fail(ECANCELED);
            }
            switch (decision->action) {
                case ConflictAction::kSkip:
                    context.addSkipped(0, 1);
                    return {};
                case ConflictAction::kKeepBoth: {
                    auto free = freeLocation(backend, target,
                                             source->type == EntryType::kDirectory);
                    if (!free) {
                        return std::unexpected(free.error());
                    }
                    target = std::move(*free);
                    break;
                }
                case ConflictAction::kReplace:
                case ConflictAction::kMerge:
                    replace = true;
                    break;
                case ConflictAction::kAbort:
                    return fail(ECANCELED);
            }
        }
    }
    const auto renamed = context.attempt(job.source, "rename",
                                         [&] { return backend.rename(job.source, target, replace); });
    if (!renamed) {
        return std::unexpected(renamed.error());
    }
    if (*renamed) {
        context.addDone(0, 1);
    } else {
        context.addSkipped(0, 1);
    }
    return {};
}

namespace {

// Saves one item into a new file at target. A file is read from its start where it can seek
// (so a retry starts over), through a copy of its descriptor that the stream closes.
Result<void> saveItem(JobContext &context, Backend &backend, const ImportItem &item,
                      const Location &target, std::vector<std::uint8_t> &buffer) {
    if (!item.fd) {
        std::size_t offset = 0;
        return writeNewFile(context, backend, target, 0666, buffer,
                            [&](std::span<std::uint8_t> chunk) -> Result<std::size_t> {
                                const std::size_t length =
                                    std::min(chunk.size(), item.text.size() - offset);
                                std::memcpy(chunk.data(), item.text.data() + offset, length);
                                offset += length;
                                return length;
                            });
    }
    // A pipe can't seek; it's read once.
    (void)lseek(item.fd.get(), 0, SEEK_SET);
    const int fd = fcntl(item.fd.get(), F_DUPFD_CLOEXEC, 0);
    if (fd == -1) {
        return failErrno("open: " + item.name);
    }
    const auto input = vfs::fdInputStream(fd);
    return writeNewFile(context, backend, target, 0666, buffer,
                        [&](std::span<std::uint8_t> chunk) { return input->read(chunk); });
}

}  // namespace

Result<void> runImport(JobContext &context, ImportJob &job) {
    // Closed when this returns, however it ends.
    std::vector<ImportItem> items = std::move(job.items);
    auto backendResult = context.backendFor(job.targetDirectory);
    if (!backendResult) {
        return std::unexpected(backendResult.error());
    }
    Backend &backend = **backendResult;

    // Bytes only when every size is known; otherwise progress goes by items.
    std::uint64_t total = 0;
    bool sized = true;
    for (const ImportItem &item : items) {
        const auto size = item.fd ? item.size : std::optional<std::uint64_t>(item.text.size());
        sized = sized && size.has_value();
        total += size.value_or(0);
    }
    context.addTotals(sized ? total : 0, items.size());

    std::vector<std::uint8_t> buffer;
    for (ImportItem &item : items) {
        const std::uint64_t size = !sized ? 0 : item.fd ? *item.size : item.text.size();
        Location target = job.targetDirectory.child(item.name);
        context.setCurrent(target);
        if (!isValidName(item.name)) {
            const Error error{EINVAL, "invalid name: " + item.name};
            if (context.resolveError(target, error, "save", false) != ErrorAction::kSkip) {
                return std::unexpected(error);
            }
            context.addSkipped(size, 1);
            continue;
        }

        if (const auto existing = backend.lstat(target)) {
            vfs::Entry incoming;
            incoming.name = item.name;
            incoming.type = incoming.targetType = EntryType::kFile;
            incoming.size = item.fd ? item.size.value_or(0) : item.text.size();
            const auto decision =
                item.replace && existing->type != EntryType::kDirectory
                    ? std::optional<ConflictDecision>(ConflictDecision{.action = ConflictAction::kReplace})
                    : context.resolveConflict(ConflictRequest{
                          .source = target,
                          .target = target,
                          .sourceEntry = incoming,
                          .targetEntry = *existing,
                          .canMerge = false,
                      });
            if (!decision) {
                return fail(ECANCELED);
            }
            bool skip = decision->action == ConflictAction::kSkip;
            if (decision->action == ConflictAction::kKeepBoth) {
                auto free = freeLocation(backend, target, false);
                if (!free) {
                    return std::unexpected(free.error());
                }
                target = std::move(*free);
            } else if (decision->action == ConflictAction::kReplace) {
                const auto removed = context.attempt(target, "replace", [&] {
                    return backend.remove(target, true, nullptr, context.cancel());
                });
                if (!removed) {
                    return std::unexpected(removed.error());
                }
                skip = !*removed;
            } else if (decision->action == ConflictAction::kAbort) {
                return fail(ECANCELED);
            }
            if (skip) {
                context.addSkipped(size, 1);
                item.fd.reset();
                continue;
            }
        }

        // What a pipe already gave can't be read again, so it can't be retried.
        const bool canRetry = !item.fd || lseek(item.fd.get(), 0, SEEK_CUR) != -1;
        bool skipped = false;
        while (true) {
            const auto saved = saveItem(context, backend, item, target, buffer);
            if (saved) {
                break;
            }
            if (saved.error().code == ECANCELED || context.isCancelled()) {
                return fail(ECANCELED);
            }
            const ErrorAction action = context.resolveError(target, saved.error(), "save", canRetry);
            if (action == ErrorAction::kAbort) {
                return std::unexpected(saved.error());
            }
            if (action == ErrorAction::kSkip) {
                skipped = true;
                break;
            }
        }
        if (skipped) {
            context.addSkipped(size, 1);
        } else {
            context.addDone(0, 1);
        }
        item.fd.reset();
    }
    return {};
}

}  // namespace filepan::jobs
