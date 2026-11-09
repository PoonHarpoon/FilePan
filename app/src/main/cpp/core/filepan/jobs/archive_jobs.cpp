// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Extract, create and verify archives, and checksums, over the core's archive and checksum
// modules. Local files take the core's direct paths where it has them; anything else goes
// through backend streams.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "filepan/archive_stream.hpp"
#include "filepan/checksum.hpp"
#include "filepan/create_archive.hpp"
#include "filepan/extract_archive.hpp"
#include "filepan/verify_archive.hpp"
#include "filepan/jobs/job_context.hpp"
#include "filepan/vfs/archive_backend.hpp"

namespace filepan::jobs {

namespace {

using vfs::Backend;
using vfs::EntryType;
using vfs::Location;

constexpr std::size_t kCopyBufferSize = 1024 * 1024;

// The entry's path relative to the target (absolute paths are made relative), or nullopt if it
// would escape it with "..". Empty for the archive's root.
std::optional<std::string> sanitize(std::string_view path) {
    std::string result;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const auto component = path.substr(start, end - start);
        if (component == "..") {
            return std::nullopt;
        }
        if (!component.empty() && component != ".") {
            if (!result.empty()) {
                result.push_back('/');
            }
            result.append(component);
        }
        start = end + 1;
    }
    return result;
}

Location resolve(const Location &directory, std::string_view relative) {
    Location location = directory;
    std::size_t start = 0;
    while (start < relative.size()) {
        auto end = relative.find('/', start);
        if (end == std::string_view::npos) {
            end = relative.size();
        }
        location = location.child(relative.substr(start, end - start));
        start = end + 1;
    }
    return location;
}

// mkdir -p below base.
Result<void> makeDirectories(Backend &backend, const Location &base, const Location &directory) {
    if (directory == base || !base.isAncestorOf(directory)) {
        return {};
    }
    const auto existing = backend.stat(directory);
    if (existing) {
        return existing->type == EntryType::kDirectory ? Result<void>()
                                                       : fail(ENOTDIR, directory.path());
    }
    if (existing.error().code != ENOENT) {
        return std::unexpected(existing.error());
    }
    if (auto parent = makeDirectories(backend, base, *directory.parent()); !parent) {
        return parent;
    }
    const auto made = backend.makeDirectory(directory, 0777);
    return made || made.error().code == EEXIST ? Result<void>() : made;
}

EntryType typeOfHeader(const ArchiveEntryHeader &header) {
    if (!header.hardlink.empty()) {
        return EntryType::kFile;
    }
    switch (header.mode & S_IFMT) {
        case 0:
        case S_IFREG: return EntryType::kFile;
        case S_IFDIR: return EntryType::kDirectory;
        case S_IFLNK: return EntryType::kSymlink;
        default: return EntryType::kOther;
    }
}

// An archive opened for reading: straight from a local file, or through its backend's stream.
struct OpenedArchive {
    std::unique_ptr<vfs::InputStream> stream;
    std::unique_ptr<vfs::StreamArchiveSource> source;
    std::unique_ptr<ArchiveReader> reader;

    [[nodiscard]] Error error(std::string_view context) const {
        if (source && source->error()) {
            return *source->error();
        }
        const auto error = reader->error();
        std::string message(context);
        if (!error.message.empty()) {
            message.append(": ").append(error.message);
        }
        return {error.code > 0 ? error.code : EIO, std::move(message)};
    }
};

Result<OpenedArchive> openArchive(Backend &backend, const Location &archive,
                                  const std::vector<std::string> &passwords) {
    OpenedArchive opened;
    ArchiveError error;
    if (archive.isLocal()) {
        opened.reader = ArchiveReader::openFile(archive.path(), passwords, error);
    } else {
        auto stream = backend.openRead(archive);
        if (!stream) {
            return std::unexpected(stream.error());
        }
        opened.stream = std::move(*stream);
        opened.source = std::make_unique<vfs::StreamArchiveSource>(*opened.stream);
        opened.reader = ArchiveReader::openSource(
            *opened.source, backend.capabilities().has(vfs::Capability::kSeekableRead), passwords,
            error);
        if (!opened.reader && opened.source->error()) {
            return std::unexpected(*opened.source->error());
        }
    }
    if (!opened.reader) {
        return fail(error.code > 0 ? error.code : EIO, archive.toString() + ": " + error.message);
    }
    return opened;
}

// The extraction of one archive's planned items through the target backend's streams, in one
// pass over the archive.
class StreamExtractor {
public:
    StreamExtractor(JobContext &context, Backend &target, ArchiveReader &reader)
        : context_(context), target_(target), reader_(reader), buffer_(kCopyBufferSize) {}

    // Returns ECANCELED when cancelled; any other failure is the item's, for the caller to
    // offer skipping.
    Result<void> extract(const ArchiveEntryHeader &header, EntryType type,
                         const Location &target) {
        if (type == EntryType::kSymlink) {
            return target_.createSymlink(target, header.symlink);
        }
        std::uint64_t copied = 0;
        auto result = [&]() -> Result<void> {
            auto output = target_.openWrite(
                target, {.mode = vfs::WriteMode::kCreateNew,
                         .permissions = (header.mode & 0777) ? (header.mode & 0777) : 0666});
            if (!output) {
                return std::unexpected(output.error());
            }
            const auto original = extracted_.find(header.hardlink);
            if (!header.hardlink.empty() && header.size == 0 && original != extracted_.end()) {
                // A hard link's data is in the entry it links to, extracted earlier.
                auto input = target_.openRead(original->second);
                if (!input) {
                    return std::unexpected(input.error());
                }
                while (true) {
                    auto length = (*input)->read(buffer_);
                    if (!length) {
                        return std::unexpected(length.error());
                    }
                    if (*length == 0) {
                        break;
                    }
                    if (auto written = (*output)->write(std::span(buffer_).first(*length));
                        !written) {
                        return written;
                    }
                }
                return (*output)->close();
            }
            while (true) {
                if (context_.isCancelled()) {
                    return fail(ECANCELED);
                }
                const std::int64_t length = reader_.readData(buffer_.data(), buffer_.size());
                if (length < 0) {
                    const auto error = reader_.error();
                    const bool password = (header.flags & ArchiveReader::kEntryEncrypted)
                                          && error.code != ENOMEM;
                    return fail(password ? EACCES : (error.code > 0 ? error.code : EIO),
                                error.message);
                }
                if (length == 0) {
                    break;
                }
                if (auto written = (*output)->write(
                        std::span(buffer_).first(static_cast<std::size_t>(length)));
                    !written) {
                    return written;
                }
                copied += static_cast<std::uint64_t>(length);
                context_.addDone(static_cast<std::uint64_t>(length), 0);
            }
            return (*output)->close();
        }();
        if (!result) {
            context_.undoBytes(copied);
            if (result.error().code != EEXIST) {
                (void)target_.remove(target, false, nullptr, CancellationToken());
            }
            return result;
        }
        if ((header.flags & ArchiveReader::kEntryMtimeSet)
            && target_.capabilities().has(vfs::Capability::kSetTimes)) {
            (void)target_.setTimes(target, std::nullopt,
                                   vfs::Timestamp{header.mtime,
                                                  static_cast<std::uint32_t>(header.mtimeNsec)});
        }
        extracted_.emplace(header.pathname, target);
        return {};
    }

private:
    JobContext &context_;
    Backend &target_;
    ArchiveReader &reader_;
    std::vector<std::uint8_t> buffer_;
    std::map<std::string, Location> extracted_;
};

}  // namespace

Result<void> runExtractArchive(JobContext &context, const ExtractArchiveJob &job) {
    auto archiveBackendResult = context.backendFor(job.archive);
    auto targetBackendResult = context.backendFor(job.targetDirectory);
    if (!archiveBackendResult || !targetBackendResult) {
        return std::unexpected(!archiveBackendResult ? archiveBackendResult.error()
                                                     : targetBackendResult.error());
    }
    Backend &archiveBackend = **archiveBackendResult;
    Backend &backend = **targetBackendResult;
    const bool local = job.archive.isLocal() && job.targetDirectory.isLocal();

    // Plan from the headers: which entries go where, and what they total.
    context.setScanning(true);
    context.setCurrent(job.archive);
    auto opened = openArchive(archiveBackend, job.archive, job.passwords);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    struct Planned {
        std::uint64_t index;
        EntryType type;
        std::int64_t size;
        std::string path;
    };
    std::vector<Planned> planned;
    std::uint64_t skipped = 0;
    ArchiveEntryHeader header;
    for (std::uint64_t index = 0;; ++index) {
        const auto status = opened->reader->readHeader(header);
        if (status == ArchiveReader::Status::kError) {
            return std::unexpected(opened->error(job.archive.toString()));
        }
        if (status == ArchiveReader::Status::kEnd) {
            break;
        }
        if ((index & 255) == 0 && context.isCancelled()) {
            return fail(ECANCELED);
        }
        const auto relative = sanitize(header.pathname);
        const auto type = typeOfHeader(header);
        if (!relative || relative->empty() || type == EntryType::kOther) {
            skipped += type == EntryType::kOther || !relative;
            continue;
        }
        planned.push_back({index, type, header.size, *relative});
        context.addTotals(
            type == EntryType::kFile ? static_cast<std::uint64_t>(std::max<std::int64_t>(header.size, 0))
                                     : 0,
            1);
    }
    *opened = OpenedArchive{};
    context.addTotals(0, skipped);
    context.addSkipped(0, skipped);
    context.setScanning(false);

    // The target may be a new folder named after the archive; its parent must exist.
    const auto created = context.attempt(job.targetDirectory, "create directory", [&] {
        const auto made = backend.makeDirectory(job.targetDirectory, 0777);
        return made || made.error().code == EEXIST ? Result<void>() : made;
    });
    if (!created) {
        return std::unexpected(created.error());
    }

    // Directories first, and conflicts settled before the single pass over the archive.
    std::vector<ExtractItem> items;
    std::vector<Location> targets;
    for (const auto &entry : planned) {
        Location target = resolve(job.targetDirectory, entry.path);
        context.setCurrent(target);
        const auto parents = context.attempt(target, "create directory", [&] {
            return makeDirectories(backend, job.targetDirectory,
                                   entry.type == EntryType::kDirectory ? target : *target.parent());
        });
        if (!parents) {
            return std::unexpected(parents.error());
        }
        if (!*parents) {
            context.addSkipped(0, 1);
            continue;
        }
        if (entry.type == EntryType::kDirectory) {
            context.addDone(0, 1);
            continue;
        }
        if (const auto existing = backend.lstat(target)) {
            vfs::Entry incoming;
            incoming.name = std::string(target.name());
            incoming.type = entry.type;
            incoming.targetType = entry.type;
            incoming.size = static_cast<std::uint64_t>(std::max<std::int64_t>(entry.size, 0));
            const auto decision = context.resolveConflict(ConflictRequest{
                .source = Location::archive(job.archive, entry.path),
                .target = target,
                .sourceEntry = incoming,
                .targetEntry = *existing,
                .canMerge = false,
            });
            if (!decision) {
                return fail(ECANCELED);
            }
            if (decision->action == ConflictAction::kSkip) {
                context.addSkipped(incoming.size, 1);
                continue;
            }
            if (decision->action == ConflictAction::kKeepBoth) {
                auto free = freeLocation(backend, target, false);
                if (!free) {
                    return std::unexpected(free.error());
                }
                target = std::move(*free);
            } else {
                const auto removed = context.attempt(target, "replace", [&] {
                    return backend.remove(target, true, nullptr, context.cancel());
                });
                if (!removed) {
                    return std::unexpected(removed.error());
                }
                if (!*removed) {
                    context.addSkipped(incoming.size, 1);
                    continue;
                }
            }
        }
        items.push_back({entry.index,
                         entry.type == EntryType::kSymlink ? ExtractItem::Type::kSymbolicLink
                                                           : ExtractItem::Type::kFile,
                         entry.size, target.path()});
        targets.push_back(std::move(target));
    }

    context.setCurrent(job.archive);
    if (!local) {
        auto archive = openArchive(archiveBackend, job.archive, job.passwords);
        if (!archive) {
            return std::unexpected(archive.error());
        }
        StreamExtractor extractor(context, backend, *archive->reader);
        std::size_t next = 0;
        for (std::uint64_t index = 0; next < items.size(); ++index) {
            const auto status = archive->reader->readHeader(header);
            if (status == ArchiveReader::Status::kEnd) {
                return fail(EIO, "the archive changed while extracting: " + job.archive.toString());
            }
            if (status == ArchiveReader::Status::kError) {
                return std::unexpected(archive->error(job.archive.toString()));
            }
            if (items[next].index != index) {
                continue;
            }
            const ExtractItem &item = items[next];
            const Location &target = targets[next];
            ++next;
            context.setCurrent(target);
            const auto type = typeOfHeader(header);
            const bool matches =
                (item.type == ExtractItem::Type::kSymbolicLink) == (type == EntryType::kSymlink)
                && (item.size < 0 || header.size == item.size);
            auto extracted = matches ? extractor.extract(header, type, target)
                                     : Result<void>(fail(EIO, "the archive changed"));
            if (extracted) {
                context.addDone(0, 1);
                continue;
            }
            if (extracted.error().code == ECANCELED || context.isCancelled()) {
                return fail(ECANCELED);
            }
            // The archive is read in one pass, so an entry can't be redone.
            if (context.resolveError(target, extracted.error(), "extract", false)
                == ErrorAction::kAbort) {
                return std::unexpected(extracted.error());
            }
            context.addSkipped(0, 1);
        }
        return {};
    }

    class Callbacks : public ExtractCallbacks {
    public:
        Callbacks(JobContext &context, const Location &archive)
            : context_(context), archive_(archive) {}
        bool onProgress(std::uint64_t writtenSize) override {
            context_.addDone(writtenSize, 0);
            return !context_.isCancelled();
        }
        bool onError(const ExtractItem *item, Stage stage, int error,
                     const std::string &message) override {
            const auto location = item ? Location::local(item->target) : archive_;
            // libarchive reports a missing passphrase as its own -1; say EACCES, as the
            // streamed path does.
            if (stage == Stage::kPasswordRequired) {
                error = EACCES;
            } else if (error <= 0) {
                error = EIO;
            }
            // The archive is read in one pass, so an entry can't be redone.
            const auto action =
                context_.resolveError(location, Error{error, message}, "extract", false);
            if (action == ErrorAction::kAbort) {
                return false;
            }
            if (item) {
                context_.addSkipped(0, 1);
            }
            return true;
        }
        JobContext &context_;
        const Location &archive_;
    } callbacks(context, job.archive);
    const auto result = extractArchive(job.archive.path(), job.passwords, items, callbacks);
    if (result.error) {
        return fail(result.error, "extract " + job.archive.path());
    }
    context.addDone(0, result.extractedCount);
    return {};
}

namespace {

// Writes an archive through an OutputStream.
class StreamSink : public ArchiveSink {
public:
    explicit StreamSink(vfs::OutputStream &output) : output_(output) {}
    bool write(const void *buffer, std::size_t size) override {
        auto written = output_.write({static_cast<const std::uint8_t *>(buffer), size});
        if (!written) {
            error = written.error();
        }
        return written.has_value();
    }
    std::optional<Error> error;

private:
    vfs::OutputStream &output_;
};

// Adds sources read through their backends to an ArchiveWriter.
class StreamArchiver {
public:
    StreamArchiver(JobContext &context, ArchiveWriter &writer)
        : context_(context), writer_(writer), buffer_(kCopyBufferSize) {}

    // Returns ECANCELED when cancelled or aborted, and the archive's error when it fails.
    Result<void> add(Backend &backend, const Location &location, const std::string &entryName) {
        if (context_.isCancelled()) {
            return fail(ECANCELED);
        }
        context_.setCurrent(location);
        const auto entry = backend.lstat(location);
        if (!entry) {
            return sourceError(location, entry.error());
        }
        ArchiveWriter::Entry header;
        header.pathname = entryName;
        header.mode = entry->mode;
        if ((header.mode & S_IFMT) == 0) {
            header.mode |= entry->type == EntryType::kDirectory ? S_IFDIR
                           : entry->type == EntryType::kSymlink ? S_IFLNK
                                                                : S_IFREG;
        }
        if ((header.mode & 07777) == 0) {
            header.mode |= entry->type == EntryType::kDirectory ? 0755 : 0644;
        }
        if (entry->hasStat) {
            header.hasMtime = true;
            header.mtime = entry->modifiedTime.seconds;
            header.mtimeNsec = entry->modifiedTime.nanoseconds;
            header.hasOwner = header.hasGroup = true;
            header.uid = entry->uid;
            header.gid = entry->gid;
        }
        std::unique_ptr<vfs::InputStream> input;
        switch (entry->type) {
            case EntryType::kFile: {
                auto opened = backend.openRead(location);
                if (!opened) {
                    return sourceError(location, opened.error());
                }
                input = std::move(*opened);
                header.size = static_cast<std::int64_t>(entry->size);
                break;
            }
            case EntryType::kSymlink: {
                auto target = backend.readLink(location);
                if (!target) {
                    return sourceError(location, target.error());
                }
                header.hasSymlink = true;
                header.symlink = std::move(*target);
                break;
            }
            case EntryType::kDirectory:
                break;
            case EntryType::kOther:
                // Devices, FIFOs and sockets can't be archived meaningfully.
                return sourceError(location, Error{ENOTSUP, location.toString()});
        }
        if (!writer_.writeHeader(header)) {
            return archiveError();
        }
        if (input) {
            std::uint64_t written = 0;
            while (written < entry->size) {
                if (context_.isCancelled()) {
                    return fail(ECANCELED);
                }
                const auto length = input->read(buffer_);
                if (!length) {
                    // The header is written, so the archive can't skip this entry any more.
                    context_.resolveError(location, length.error(), "add to archive", false);
                    return std::unexpected(length.error());
                }
                if (*length == 0) {
                    break;
                }
                if (!writer_.writeData(buffer_.data(), *length)) {
                    return archiveError();
                }
                written += *length;
                context_.addDone(*length, 0);
            }
        }
        context_.addDone(0, 1);
        if (entry->type != EntryType::kDirectory) {
            return {};
        }
        std::vector<std::string> names;
        const auto listed = backend.list(
            location, {.withStat = false},
            [&](std::vector<vfs::Entry> batch) {
                for (auto &child : batch) {
                    names.push_back(std::move(child.name));
                }
            },
            context_.cancel());
        if (!listed) {
            return sourceError(location, listed.error(), false);
        }
        // Stable, predictable archive contents regardless of listing order.
        std::ranges::sort(names);
        for (const auto &name : names) {
            if (auto added = add(backend, location.child(name), entryName + "/" + name); !added) {
                return added;
            }
        }
        return {};
    }

    std::optional<Error> sinkError;

private:
    // A source that can't be read: skipped, or aborting.
    Result<void> sourceError(const Location &location, const Error &error, bool countItem = true) {
        if (error.code == ECANCELED || context_.isCancelled()) {
            return fail(ECANCELED);
        }
        if (context_.resolveError(location, error, "add to archive", false)
            == ErrorAction::kAbort) {
            return fail(ECANCELED);
        }
        if (countItem) {
            context_.addSkipped(0, 1);
        }
        return {};
    }

    Result<void> archiveError() {
        if (sinkError) {
            return std::unexpected(*sinkError);
        }
        const auto error = writer_.error();
        return fail(error.code > 0 ? error.code : EIO, error.message);
    }

    JobContext &context_;
    ArchiveWriter &writer_;
    std::vector<std::uint8_t> buffer_;
};

}  // namespace

Result<void> runCreateArchive(JobContext &context, const CreateArchiveJob &job) {
    auto targetBackendResult = context.backendFor(job.target);
    if (!targetBackendResult) {
        return std::unexpected(targetBackendResult.error());
    }
    Backend &backend = **targetBackendResult;
    bool local = job.target.isLocal();
    std::vector<CreateArchiveSource> sources;
    context.setScanning(true);
    for (const auto &source : job.sources) {
        auto sourceBackendResult = context.backendFor(source);
        if (!sourceBackendResult) {
            return std::unexpected(sourceBackendResult.error());
        }
        Backend &sourceBackend = **sourceBackendResult;
        local = local && source.isLocal();
        sources.push_back({source.path(), std::string(source.name())});
        // Totals by walking the tree, as the copy does.
        std::vector<Location> pending{source};
        while (!pending.empty()) {
            const Location location = std::move(pending.back());
            pending.pop_back();
            const auto entry = sourceBackend.lstat(location);
            context.addTotals(entry && entry->type == EntryType::kFile ? entry->size : 0, 1);
            if (!entry || entry->type != EntryType::kDirectory) {
                continue;
            }
            (void)sourceBackend.list(
                location, {.withStat = false},
                [&](std::vector<vfs::Entry> batch) {
                    for (const auto &child : batch) {
                        pending.push_back(location.child(child.name));
                    }
                },
                context.cancel());
            if (context.isCancelled()) {
                return fail(ECANCELED);
            }
        }
    }
    context.setScanning(false);

    Location target = job.target;
    if (const auto existing = backend.lstat(target)) {
        vfs::Entry incoming;
        incoming.name = std::string(target.name());
        incoming.type = incoming.targetType = EntryType::kFile;
        const auto decision = context.resolveConflict(ConflictRequest{
            .source = target,
            .target = target,
            .sourceEntry = incoming,
            .targetEntry = *existing,
            .canMerge = false,
        });
        if (!decision) {
            return fail(ECANCELED);
        }
        if (decision->action == ConflictAction::kSkip) {
            context.tracker().update([](JobProgress &progress) {
                progress.itemsSkipped += progress.itemsTotal - progress.itemsDone;
                progress.itemsDone = progress.itemsTotal;
                progress.bytesDone = progress.bytesTotal;
            });
            return {};
        }
        if (decision->action == ConflictAction::kKeepBoth) {
            auto free = freeLocation(backend, target, false);
            if (!free) {
                return std::unexpected(free.error());
            }
            target = std::move(*free);
        } else {
            const auto removed = backend.remove(target, true, nullptr, context.cancel());
            if (!removed) {
                return std::unexpected(removed.error());
            }
        }
    }

    if (!local) {
        auto result = [&]() -> Result<void> {
            auto output = backend.openWrite(target, {.mode = vfs::WriteMode::kCreateNew});
            if (!output) {
                return std::unexpected(output.error());
            }
            StreamSink sink(**output);
            ArchiveError archiveError;
            auto writer = ArchiveWriter::open(sink, job.options, archiveError);
            if (!writer) {
                return sink.error ? std::unexpected(*sink.error)
                                  : fail(archiveError.code > 0 ? archiveError.code : EIO,
                                         archiveError.message);
            }
            StreamArchiver archiver(context, *writer);
            for (const auto &source : job.sources) {
                Backend &sourceBackend = **context.backendFor(source);
                auto added = archiver.add(sourceBackend, source, std::string(source.name()));
                if (!added) {
                    if (sink.error) {
                        return std::unexpected(*sink.error);
                    }
                    return added;
                }
            }
            if (!writer->close()) {
                if (sink.error) {
                    return std::unexpected(*sink.error);
                }
                const auto error = writer->error();
                return fail(error.code > 0 ? error.code : EIO, error.message);
            }
            return (*output)->close();
        }();
        if (!result) {
            (void)backend.remove(target, false, nullptr, CancellationToken());
        }
        return result;
    }

    class Callbacks : public CreateArchiveCallbacks {
    public:
        explicit Callbacks(JobContext &context) : context_(context) {}
        bool onProgress(std::uint64_t readSize, std::uint64_t entryCount,
                        std::string_view currentPath) override {
            context_.addDone(readSize, entryCount);
            context_.tracker().update([&](JobProgress &progress) {
                progress.currentItem = currentPath;
            });
            return !context_.isCancelled();
        }
        bool onSourceError(std::string_view path, int error) override {
            const auto action = context_.resolveError(Location::local(path),
                                                      Error{error, std::string(path)},
                                                      "add to archive", false);
            if (action == ErrorAction::kAbort) {
                return false;
            }
            context_.addSkipped(0, 1);
            return true;
        }
        JobContext &context_;
    } callbacks(context);
    const auto result = createArchive(target.path(), job.options, sources, callbacks);
    if (result.error) {
        (void)backend.remove(target, false, nullptr, CancellationToken());
        return fail(result.error, result.message);
    }
    return {};
}

Result<void> runChecksum(JobContext &context, const ChecksumJob &job, JobResult &result) {
    auto backendResult = context.backendFor(job.file);
    if (!backendResult) {
        return std::unexpected(backendResult.error());
    }
    Backend &backend = **backendResult;
    context.setCurrent(job.file);
    const auto entry = context.attempt(job.file, "read", [&] { return backend.stat(job.file); });
    if (!entry) {
        return std::unexpected(entry.error());
    }
    if (!*entry) {
        context.addTotals(0, 1);
        context.addSkipped(0, 1);
        return {};
    }
    context.addTotals((*entry)->size, 1);

    class Callbacks : public ChecksumCallbacks {
    public:
        explicit Callbacks(JobContext &context) : context_(context) {}
        bool onProgress(std::uint64_t size) override {
            hashed += size;
            context_.addDone(size, 0);
            return !context_.isCancelled();
        }
        JobContext &context_;
        std::uint64_t hashed = 0;
    } callbacks(context);
    // Reads a backend's stream, keeping its error, which the errno alone would lose.
    class StreamInput : public ChecksumInput {
    public:
        explicit StreamInput(vfs::InputStream &stream) : stream_(stream) {}
        std::int64_t read(std::uint8_t *buffer, std::size_t size) override {
            const auto length = stream_.read({buffer, size});
            if (!length) {
                error = length.error();
                return -(error->code > 0 ? error->code : EIO);
            }
            return static_cast<std::int64_t>(*length);
        }
        std::optional<Error> error;

    private:
        vfs::InputStream &stream_;
    };
    const auto computed = context.attempt(job.file, "read", [&]() -> Result<ChecksumResult> {
        callbacks.hashed = 0;
        ChecksumResult checksums;
        std::optional<Error> streamError;
        if (job.file.isLocal()) {
            checksums = computeChecksums(job.file.path(), job.algorithms, callbacks);
        } else {
            auto stream = backend.openRead(job.file);
            if (!stream) {
                return std::unexpected(stream.error());
            }
            StreamInput input(**stream);
            checksums = computeChecksums(input, job.algorithms, callbacks);
            streamError = input.error;
        }
        if (checksums.error) {
            context.undoBytes(callbacks.hashed);
            if (streamError && checksums.error != ECANCELED) {
                return std::unexpected(*streamError);
            }
            return fail(checksums.error, job.file.toString());
        }
        return checksums;
    });
    if (!computed) {
        return std::unexpected(computed.error());
    }
    if (!*computed) {
        context.addSkipped(0, 1);
        return {};
    }
    result.checksums = std::move((*computed)->digests);
    context.addDone(0, 1);
    return {};
}

Result<void> runVerifyArchive(JobContext &context, const VerifyArchiveJob &job, JobResult &result) {
    auto backendResult = context.backendFor(job.archive);
    if (!backendResult) {
        return std::unexpected(backendResult.error());
    }
    Backend &backend = **backendResult;
    context.setCurrent(job.archive);
    const auto entry =
        context.attempt(job.archive, "read", [&] { return backend.stat(job.archive); });
    if (!entry) {
        return std::unexpected(entry.error());
    }
    if (!*entry) {
        context.addTotals(0, 1);
        context.addSkipped(0, 1);
        return {};
    }
    context.addTotals((*entry)->size, 1);

    class Callbacks : public VerifyArchiveCallbacks {
    public:
        explicit Callbacks(JobContext &context) : context_(context) {}
        bool onProgress(std::uint64_t readSize, std::string_view currentPath) override {
            read += readSize;
            context_.addDone(readSize, 0);
            context_.tracker().update([&](JobProgress &progress) {
                progress.currentItem = currentPath;
            });
            return !context_.isCancelled();
        }
        JobContext &context_;
        std::uint64_t read = 0;
    } callbacks(context);
    // Always through the backend's stream, local files too: its errors tell a failing read from
    // a damaged archive.
    const auto verify = [&]() -> Result<VerifyArchiveResult> {
        callbacks.read = 0;
        auto stream = backend.openRead(job.archive);
        if (!stream) {
            return std::unexpected(stream.error());
        }
        vfs::StreamArchiveSource source(**stream);
        auto verified =
            verifyArchive(source, backend.capabilities().has(vfs::Capability::kSeekableRead),
                          job.passwords, callbacks);
        if (verified.error) {
            context.undoBytes(callbacks.read);
            if (verified.error == EIO && source.error()) {
                return std::unexpected(*source.error());
            }
            std::string message = job.archive.toString();
            if (!verified.message.empty()) {
                message.append(": ").append(verified.message);
            }
            return fail(verified.error, std::move(message));
        }
        return verified;
    };
    const auto verified = context.attempt(job.archive, "verify", verify);
    if (!verified) {
        return std::unexpected(verified.error());
    }
    if (!*verified) {
        context.addSkipped(0, 1);
        return {};
    }
    result.archiveEntries = (*verified)->entryCount;
    result.archiveProblems = std::move((*verified)->problems);
    context.addDone(0, 1);
    return {};
}

}  // namespace filepan::jobs
