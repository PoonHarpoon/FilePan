// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Delete, make directory, create file, rename, archives (and verifying them), checksums, import,
// and keep-both naming.

#include <archive.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <string_view>

#include "filepan/jobs/job_context.hpp"
#include "job_test_util.hpp"

using namespace filepan::jobs;
using namespace filepan::test;
using filepan::vfs::Location;

namespace {

JobProgress finalProgress(JobQueue &queue, JobId id) {
    queue.waitUntilIdle();
    const auto progress = queue.progress(id);
    EXPECT(progress && isFinished(progress->state));
    return *progress;
}

void testKeepBothNames() {
    EXPECT(keepBothName("a.txt", false, 1) == "a (1).txt");
    EXPECT(keepBothName("a (1).txt", false, 2) == "a (2).txt");
    EXPECT(keepBothName("a (x).txt", false, 1) == "a (x) (1).txt");
    EXPECT(keepBothName("a.tar.gz", false, 1) == "a (1).tar.gz");
    // Only archives have compound extensions; a compressed PDF is a gz file.
    EXPECT(keepBothName("a.pdf.gz", false, 1) == "a.pdf (1).gz");
    EXPECT(keepBothName("noext", false, 3) == "noext (3)");
    EXPECT(keepBothName(".bashrc", false, 1) == ".bashrc (1)");
    EXPECT(keepBothName("photos.2024", true, 1) == "photos.2024 (1)");
    EXPECT(keepBothName("photos (9)", true, 10) == "photos (10)");
}

void testDelete() {
    TempDir temp;
    fs::create_directories(temp.path() / "tree" / "a" / "b");
    writeFile(temp.path() / "tree" / "a" / "b" / "file", "x");
    writeFile(temp.path() / "tree" / "top", "y");
    writeFile(temp.path() / "single", "z");
    auto decisions = std::make_shared<ScriptedDecisions>();
    JobQueue queue(makeBackends(temp.path()), decisions, nullptr);
    const auto id = queue.submit(DeleteJob{{local(temp.path() / "tree"), local(temp.path() / "single")}});
    const auto progress = finalProgress(queue, id);
    EXPECT(progress.state == JobState::kSucceeded);
    EXPECT(progress.itemsTotal == 6 && progress.itemsDone == 6);
    EXPECT(fs::is_empty(temp.path()));

    // A missing target asks; skipping it still succeeds.
    decisions->errorAnswer = {ErrorAction::kSkip, false};
    const auto missing = queue.submit(DeleteJob{{local(temp.path() / "missing")}});
    const auto skipped = finalProgress(queue, missing);
    EXPECT(skipped.state == JobState::kSucceeded && skipped.itemsSkipped == 1);
    EXPECT(decisions->errors.size() == 1 && decisions->errors[0].error.code == ENOENT);
    EXPECT(decisions->errors[0].operation == "delete");
}

void testCreateAndRename() {
    TempDir temp;
    auto decisions = std::make_shared<ScriptedDecisions>();
    JobQueue queue(makeBackends(temp.path()), decisions, nullptr);
    EXPECT(finalProgress(queue, queue.submit(MakeDirectoryJob{local(temp.path() / "dir")})).state
           == JobState::kSucceeded);
    EXPECT(fs::is_directory(temp.path() / "dir"));
    EXPECT(finalProgress(queue, queue.submit(CreateFileJob{local(temp.path() / "dir" / "f")})).state
           == JobState::kSucceeded);
    EXPECT(fs::is_regular_file(temp.path() / "dir" / "f"));

    // Exists: the error decision decides; abort fails with EEXIST.
    const auto again = finalProgress(queue, queue.submit(MakeDirectoryJob{local(temp.path() / "dir")}));
    EXPECT(again.state == JobState::kFailed && again.error->code == EEXIST);

    // Rename, then rename onto an existing name with keep-both.
    EXPECT(finalProgress(queue, queue.submit(RenameJob{local(temp.path() / "dir" / "f"), "g.txt"})).state
           == JobState::kSucceeded);
    EXPECT(fs::exists(temp.path() / "dir" / "g.txt") && !fs::exists(temp.path() / "dir" / "f"));
    writeFile(temp.path() / "dir" / "h.txt", "h");
    decisions->conflictAnswers["g.txt"] = {ConflictAction::kKeepBoth, false};
    EXPECT(finalProgress(queue, queue.submit(RenameJob{local(temp.path() / "dir" / "h.txt"), "g.txt"})).state
           == JobState::kSucceeded);
    EXPECT(readFile(temp.path() / "dir" / "g (1).txt") == "h");
    // Replace.
    decisions->conflictAnswers["g.txt"] = {ConflictAction::kReplace, false};
    EXPECT(finalProgress(queue, queue.submit(RenameJob{local(temp.path() / "dir" / "g (1).txt"), "g.txt"})).state
           == JobState::kSucceeded);
    EXPECT(readFile(temp.path() / "dir" / "g.txt") == "h");
    // Replace, decided up front: no question.
    writeFile(temp.path() / "dir" / "i.txt", "i");
    const std::size_t asked = decisions->conflicts.size();
    EXPECT(finalProgress(queue, queue.submit(RenameJob{local(temp.path() / "dir" / "i.txt"), "g.txt", true})).state
           == JobState::kSucceeded);
    EXPECT(readFile(temp.path() / "dir" / "g.txt") == "i" && !fs::exists(temp.path() / "dir" / "i.txt"));
    EXPECT(decisions->conflicts.size() == asked);
    // A folder there still asks.
    fs::create_directory(temp.path() / "dir" / "sub");
    decisions->conflictAnswers["sub"] = {ConflictAction::kSkip, false};
    EXPECT(finalProgress(queue, queue.submit(RenameJob{local(temp.path() / "dir" / "g.txt"), "sub", true})).state
           == JobState::kSucceeded);
    EXPECT(decisions->conflicts.size() == asked + 1);
    EXPECT(fs::is_directory(temp.path() / "dir" / "sub") && fs::exists(temp.path() / "dir" / "g.txt"));
    // Invalid names fail without asking.
    const auto invalid = finalProgress(queue, queue.submit(RenameJob{local(temp.path() / "dir" / "g.txt"), "a/b"}));
    EXPECT(invalid.state == JobState::kFailed && invalid.error->code == EINVAL);
}

void testArchives() {
    TempDir temp;
    fs::create_directories(temp.path() / "src" / "sub");
    writeFile(temp.path() / "src" / "one.txt", "one");
    writeFile(temp.path() / "src" / "sub" / "two.txt", "two two");
    fs::create_symlink("one.txt", temp.path() / "src" / "link");
    auto decisions = std::make_shared<ScriptedDecisions>();
    JobQueue queue(makeBackends(temp.path()), decisions, nullptr);

    const auto archive = temp.path() / "out.tar.gz";
    const auto created = finalProgress(queue, queue.submit(CreateArchiveJob{
        {local(temp.path() / "src")}, local(archive), {ARCHIVE_FORMAT_TAR, ARCHIVE_FILTER_GZIP, "", 0}}));
    EXPECT(created.state == JobState::kSucceeded);
    EXPECT(created.itemsTotal == 5 && created.bytesTotal == 3 + 7);
    EXPECT(fs::file_size(archive) > 0);

    // Extracting into an empty directory, then again with a conflict on one file.
    fs::create_directory(temp.path() / "x");
    const auto extracted = finalProgress(queue, queue.submit(ExtractArchiveJob{
        local(archive), local(temp.path() / "x"), {}}));
    EXPECT(extracted.state == JobState::kSucceeded);
    EXPECT(extracted.itemsDone == extracted.itemsTotal && extracted.itemsTotal == 5);
    EXPECT(extracted.bytesDone == 10);
    EXPECT(readFile(temp.path() / "x" / "src" / "sub" / "two.txt") == "two two");
    EXPECT(fs::read_symlink(temp.path() / "x" / "src" / "link") == "one.txt");

    writeFile(temp.path() / "x" / "src" / "one.txt", "changed");
    decisions->conflictAnswers["one.txt"] = {ConflictAction::kKeepBoth, false};
    decisions->conflictAnswers["two.txt"] = {ConflictAction::kSkip, false};
    decisions->conflictAnswers["link"] = {ConflictAction::kReplace, false};
    const auto again = finalProgress(queue, queue.submit(ExtractArchiveJob{
        local(archive), local(temp.path() / "x"), {}}));
    EXPECT(again.state == JobState::kSucceeded && again.itemsSkipped == 1);
    EXPECT(readFile(temp.path() / "x" / "src" / "one.txt") == "changed");
    EXPECT(readFile(temp.path() / "x" / "src" / "one (1).txt") == "one");
    EXPECT(decisions->conflicts.size() == 3);

    // A target folder that doesn't exist yet is created.
    const auto fresh = finalProgress(queue, queue.submit(ExtractArchiveJob{
        local(archive), local(temp.path() / "fresh"), {}}));
    EXPECT(fresh.state == JobState::kSucceeded);
    EXPECT(readFile(temp.path() / "fresh" / "src" / "sub" / "two.txt") == "two two");

    // An encrypted zip without its password: the error says EACCES, as the streamed path does.
    const auto locked = temp.path() / "locked.zip";
    EXPECT(finalProgress(queue, queue.submit(CreateArchiveJob{
        {local(temp.path() / "src" / "one.txt")}, local(locked), {ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE, "pw", 0}})).state
           == JobState::kSucceeded);
    decisions->errors.clear();
    const auto denied = finalProgress(queue, queue.submit(ExtractArchiveJob{
        local(locked), local(temp.path() / "locked"), {}}));
    EXPECT(denied.state != JobState::kSucceeded && decisions->errors.size() == 1);
    EXPECT(decisions->errors[0].error.code == EACCES);
    EXPECT(finalProgress(queue, queue.submit(ExtractArchiveJob{
        local(locked), local(temp.path() / "unlocked"), {"pw"}})).state == JobState::kSucceeded);
    EXPECT(readFile(temp.path() / "unlocked" / "one.txt") == "one");

    // Creating onto an existing archive asks too.
    decisions->conflictAnswers["out.tar.gz"] = {ConflictAction::kKeepBoth, false};
    EXPECT(finalProgress(queue, queue.submit(CreateArchiveJob{
        {local(temp.path() / "src")}, local(archive), {ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE, "", 0}})).state
           == JobState::kSucceeded);
    EXPECT(fs::exists(temp.path() / "out (1).tar.gz"));

    // Off the local backend, through streams: a remote archive into a local directory, a remote
    // archive of remote sources, and that one extracted onto the remote side.
    fs::create_directory(temp.path() / "y");
    const auto fromRemote = finalProgress(queue, queue.submit(ExtractArchiveJob{
        FakeRemoteBackend::at("/out.tar.gz"), local(temp.path() / "y"), {}}));
    EXPECT(fromRemote.state == JobState::kSucceeded && fromRemote.bytesDone == 10);
    EXPECT(fromRemote.itemsDone == 5 && fromRemote.itemsSkipped == 0);
    EXPECT(readFile(temp.path() / "y" / "src" / "sub" / "two.txt") == "two two");
    EXPECT(fs::read_symlink(temp.path() / "y" / "src" / "link") == "one.txt");

    const auto remoteArchive = finalProgress(queue, queue.submit(CreateArchiveJob{
        {FakeRemoteBackend::at("/src")}, FakeRemoteBackend::at("/remote.tar.xz"),
        {ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_XZ, "", 0}}));
    EXPECT(remoteArchive.state == JobState::kSucceeded);
    EXPECT(remoteArchive.itemsDone == 5 && remoteArchive.bytesDone == 10);
    fs::create_directory(temp.path() / "z");
    const auto remoteToRemote = finalProgress(queue, queue.submit(ExtractArchiveJob{
        FakeRemoteBackend::at("/remote.tar.xz"), FakeRemoteBackend::at("/z"), {}}));
    EXPECT(remoteToRemote.state == JobState::kSucceeded);
    EXPECT(readFile(temp.path() / "z" / "src" / "one.txt") == "one");
    EXPECT(readFile(temp.path() / "z" / "src" / "sub" / "two.txt") == "two two");
    EXPECT(fs::read_symlink(temp.path() / "z" / "src" / "link") == "one.txt");

    // A missing remote archive fails with the stream's error.
    const auto missing = finalProgress(queue, queue.submit(ExtractArchiveJob{
        FakeRemoteBackend::at("/missing.zip"), local(temp.path() / "y"), {}}));
    EXPECT(missing.state == JobState::kFailed && missing.error->code == ENOENT);
}

void testChecksum() {
    TempDir temp;
    writeFile(temp.path() / "abc", "abc");
    auto decisions = std::make_shared<ScriptedDecisions>();
    JobQueue queue(makeBackends(temp.path()), decisions, nullptr);
    const auto id = queue.submit(ChecksumJob{local(temp.path() / "abc"),
                                             {filepan::ChecksumAlgorithm::kSha256,
                                              filepan::ChecksumAlgorithm::kCrc32}});
    const auto progress = finalProgress(queue, id);
    EXPECT(progress.state == JobState::kSucceeded && progress.bytesDone == 3);
    const auto result = queue.result(id);
    EXPECT(result);
    const auto &sha256 = result->checksums[static_cast<int>(filepan::ChecksumAlgorithm::kSha256)];
    EXPECT(sha256.size() == 32 && sha256[0] == 0xba && sha256[1] == 0x78 && sha256[31] == 0xad);
    EXPECT(result->checksums[static_cast<int>(filepan::ChecksumAlgorithm::kCrc32)].size() == 4);
    EXPECT(result->checksums[static_cast<int>(filepan::ChecksumAlgorithm::kMd5)].empty());

    // The same file through a remote backend's stream.
    const auto remoteId = queue.submit(ChecksumJob{FakeRemoteBackend::at("/abc"),
                                                   {filepan::ChecksumAlgorithm::kSha256}});
    EXPECT(finalProgress(queue, remoteId).state == JobState::kSucceeded);
    EXPECT(queue.result(remoteId)->checksums[static_cast<int>(filepan::ChecksumAlgorithm::kSha256)]
           == sha256);
    const auto missing = finalProgress(
        queue, queue.submit(ChecksumJob{FakeRemoteBackend::at("/none"),
                                        {filepan::ChecksumAlgorithm::kSha256}}));
    EXPECT(missing.state == JobState::kFailed && missing.error->code == ENOENT);
}

void testVerifyArchive() {
    TempDir temp;
    fs::create_directories(temp.path() / "src" / "sub");
    writeFile(temp.path() / "src" / "one.txt", "one");
    writeFile(temp.path() / "src" / "sub" / "two.txt", "two two");
    auto decisions = std::make_shared<ScriptedDecisions>();
    JobQueue queue(makeBackends(temp.path()), decisions, nullptr);
    const auto archive = temp.path() / "out.tar.gz";
    EXPECT(finalProgress(queue, queue.submit(CreateArchiveJob{
        {local(temp.path() / "src")}, local(archive), {ARCHIVE_FORMAT_TAR, ARCHIVE_FILTER_GZIP, "", 0}})).state
           == JobState::kSucceeded);

    // Intact, here and through a remote backend's stream; it reads every byte once.
    for (const auto &location : {local(archive), FakeRemoteBackend::at("/out.tar.gz")}) {
        const auto id = queue.submit(VerifyArchiveJob{location, {}});
        const auto progress = finalProgress(queue, id);
        EXPECT(progress.state == JobState::kSucceeded && progress.kind == JobKind::kVerifyArchive);
        EXPECT(progress.bytesDone == fs::file_size(archive) && progress.itemsDone == 1);
        const auto result = queue.result(id);
        EXPECT(result && result->archiveEntries == 4 && result->archiveProblems.empty());
    }

    // A wrong CRC-32 in the gzip trailer is damage the job reports, and still succeeds.
    const std::string bytes = readFile(archive);
    std::string damaged = bytes;
    damaged[damaged.size() - 8] = static_cast<char>(damaged[damaged.size() - 8] ^ 1);
    writeFile(temp.path() / "damaged.tar.gz", damaged);
    const auto damagedId = queue.submit(VerifyArchiveJob{local(temp.path() / "damaged.tar.gz"), {}});
    EXPECT(finalProgress(queue, damagedId).state == JobState::kSucceeded);
    const auto problems = queue.result(damagedId)->archiveProblems;
    EXPECT(problems.size() == 1 && problems[0].path.empty());

    // An encrypted zip asks for its password with EACCES, as extracting does.
    const auto locked = temp.path() / "locked.zip";
    EXPECT(finalProgress(queue, queue.submit(CreateArchiveJob{
        {local(temp.path() / "src" / "one.txt")}, local(locked), {ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE, "pw", 0}})).state
           == JobState::kSucceeded);
    decisions->errors.clear();
    const auto denied = finalProgress(queue, queue.submit(VerifyArchiveJob{local(locked), {}}));
    EXPECT(denied.state == JobState::kFailed && denied.error->code == EACCES);
    EXPECT(decisions->errors.size() == 1 && decisions->errors[0].operation == "verify");
    EXPECT(decisions->errors[0].jobKind == JobKind::kVerifyArchive);
    const auto unlocked = queue.submit(VerifyArchiveJob{local(locked), {"pw"}});
    EXPECT(finalProgress(queue, unlocked).state == JobState::kSucceeded);
    EXPECT(queue.result(unlocked)->archiveProblems.empty());

    // A missing archive fails with the stream's error.
    const auto missing = finalProgress(
        queue, queue.submit(VerifyArchiveJob{FakeRemoteBackend::at("/none.zip"), {}}));
    EXPECT(missing.state == JobState::kFailed && missing.error->code == ENOENT);
}

// A pipe's ends, closed with it.
struct Pipe {
    Pipe() {
        int fds[2];
        EXPECT(pipe2(fds, O_CLOEXEC) == 0);
        read = filepan::UniqueFd(fds[0]);
        write = filepan::UniqueFd(fds[1]);
    }
    void send(std::string_view data) const {
        EXPECT(::write(write.get(), data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    }
    // Whether every copy of the read end is closed: writing then fails with EPIPE.
    [[nodiscard]] bool readerClosed() const {
        return ::write(write.get(), "x", 1) == -1 && errno == EPIPE;
    }
    filepan::UniqueFd read;
    filepan::UniqueFd write;
};

ImportItem fileItem(const fs::path &path, std::string name) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    EXPECT(fd != -1);
    return {.name = std::move(name), .size = fs::file_size(path), .fd = filepan::UniqueFd(fd)};
}

ImportItem textItem(std::string name, std::string text) {
    return {.name = std::move(name), .text = std::move(text)};
}

void testImport() {
    // Writing to a closed pipe fails instead of killing the test.
    signal(SIGPIPE, SIG_IGN);
    TempDir temp;
    fs::create_directories(temp.path() / "in");
    fs::create_directories(temp.path() / "out");
    writeFile(temp.path() / "in" / "photo.jpg", std::string(300'000, 'p'));
    auto decisions = std::make_shared<ScriptedDecisions>();
    JobQueue queue(makeBackends(temp.path()), decisions, nullptr);

    // A file and text, both with known sizes: progress goes by bytes.
    std::vector<ImportItem> items;
    items.push_back(fileItem(temp.path() / "in" / "photo.jpg", "photo.jpg"));
    items.push_back(textItem("note.txt", "hello"));
    const auto sized = finalProgress(queue, queue.submit(ImportJob{std::move(items), local(temp.path() / "out")}));
    EXPECT(sized.state == JobState::kSucceeded);
    EXPECT(sized.bytesTotal == 300'005 && sized.bytesDone == 300'005);
    EXPECT(sized.itemsTotal == 2 && sized.itemsDone == 2 && sized.itemsSkipped == 0);
    EXPECT(readFile(temp.path() / "out" / "photo.jpg") == std::string(300'000, 'p'));
    EXPECT(readFile(temp.path() / "out" / "note.txt") == "hello");

    // A pipe with no size, and text: no byte total, so progress goes by items.
    Pipe pipe;
    pipe.send("streamed");
    pipe.write.reset();
    std::vector<ImportItem> streamed;
    streamed.push_back({.name = "stream.bin", .fd = std::move(pipe.read)});
    streamed.push_back(textItem("more.txt", "more"));
    const auto unsized = finalProgress(queue, queue.submit(ImportJob{std::move(streamed), local(temp.path() / "out")}));
    EXPECT(unsized.state == JobState::kSucceeded);
    EXPECT(unsized.bytesTotal == 0 && unsized.itemsTotal == 2 && unsized.itemsDone == 2);
    EXPECT(readFile(temp.path() / "out" / "stream.bin") == "streamed");
    EXPECT(readFile(temp.path() / "out" / "more.txt") == "more");

    // Onto a remote backend, through its streams.
    std::vector<ImportItem> remote;
    remote.push_back(textItem("remote.txt", "far"));
    EXPECT(finalProgress(queue, queue.submit(ImportJob{std::move(remote), FakeRemoteBackend::at("/out")})).state
           == JobState::kSucceeded);
    EXPECT(readFile(temp.path() / "out" / "remote.txt") == "far");

    // A name that can't be a file is an error the user can skip but not retry.
    std::vector<ImportItem> invalid;
    invalid.push_back(textItem("a/b", "x"));
    const auto refused = finalProgress(queue, queue.submit(ImportJob{std::move(invalid), local(temp.path() / "out")}));
    EXPECT(refused.state == JobState::kFailed && refused.error->code == EINVAL);
    EXPECT(decisions->errors.size() == 1 && decisions->errors[0].operation == "save");
    EXPECT(!decisions->errors[0].canRetry);
    decisions->errorAnswer = {ErrorAction::kSkip, false};
    std::vector<ImportItem> skipped;
    skipped.push_back(textItem("..", "x"));
    skipped.push_back(textItem("ok.txt", "ok"));
    const auto partly = finalProgress(queue, queue.submit(ImportJob{std::move(skipped), local(temp.path() / "out")}));
    EXPECT(partly.state == JobState::kSucceeded && partly.itemsDone == 2 && partly.itemsSkipped == 1);
    EXPECT(readFile(temp.path() / "out" / "ok.txt") == "ok");
}

void testImportConflicts() {
    TempDir temp;
    writeFile(temp.path() / "note.txt", "old");
    writeFile(temp.path() / "keep.txt", "old");
    writeFile(temp.path() / "skip.txt", "old");
    auto decisions = std::make_shared<ScriptedDecisions>();
    decisions->conflictAnswers["note.txt"] = {ConflictAction::kKeepBoth, false};
    decisions->conflictAnswers["keep.txt"] = {ConflictAction::kReplace, false};
    decisions->conflictAnswers["skip.txt"] = {ConflictAction::kSkip, false};
    JobQueue queue(makeBackends(temp.path()), decisions, nullptr);
    std::vector<ImportItem> items;
    items.push_back(textItem("note.txt", "new"));
    items.push_back(textItem("keep.txt", "new"));
    items.push_back(textItem("skip.txt", "new"));
    const auto progress = finalProgress(queue, queue.submit(ImportJob{std::move(items), local(temp.path())}));
    EXPECT(progress.state == JobState::kSucceeded && progress.itemsDone == 3 && progress.itemsSkipped == 1);
    EXPECT(readFile(temp.path() / "note.txt") == "old" && readFile(temp.path() / "note (1).txt") == "new");
    EXPECT(readFile(temp.path() / "keep.txt") == "new");
    EXPECT(readFile(temp.path() / "skip.txt") == "old");
    // The sheet shows the incoming file as it will be.
    EXPECT(decisions->conflicts.size() == 3);
    EXPECT(decisions->conflicts[0].sourceEntry.name == "note.txt" && decisions->conflicts[0].sourceEntry.size == 3);
    EXPECT(decisions->conflicts[0].jobKind == JobKind::kImport && !decisions->conflicts[0].canMerge);

    // Aborting stops the job.
    decisions->conflictAnswers["note.txt"] = {ConflictAction::kAbort, false};
    std::vector<ImportItem> again;
    again.push_back(textItem("note.txt", "newer"));
    again.push_back(textItem("fresh.txt", "x"));
    const auto aborted = finalProgress(queue, queue.submit(ImportJob{std::move(again), local(temp.path())}));
    EXPECT(aborted.state == JobState::kCancelled);
    EXPECT(!fs::exists(temp.path() / "fresh.txt"));

    // Replace, decided up front: no question.
    const std::size_t asked = decisions->conflicts.size();
    std::vector<ImportItem> replacing;
    replacing.push_back(textItem("skip.txt", "replaced"));
    replacing.back().replace = true;
    const auto replaced = finalProgress(queue, queue.submit(ImportJob{std::move(replacing), local(temp.path())}));
    EXPECT(replaced.state == JobState::kSucceeded && replaced.itemsSkipped == 0);
    EXPECT(readFile(temp.path() / "skip.txt") == "replaced");
    EXPECT(decisions->conflicts.size() == asked);
}

void testImportCancel() {
    signal(SIGPIPE, SIG_IGN);
    TempDir temp;
    auto decisions = std::make_shared<ScriptedDecisions>();
    ProgressLog log;
    JobQueue queue(makeBackends(temp.path()), decisions, log.observer(),
                   {.maxConcurrentJobs = 1, .progressInterval = std::chrono::milliseconds(0)});
    Pipe running;
    Pipe waiting;
    std::vector<ImportItem> first;
    first.push_back({.name = "partial.bin", .fd = std::move(running.read)});
    const auto id = queue.submit(ImportJob{std::move(first), local(temp.path())});
    std::vector<ImportItem> second;
    second.push_back({.name = "never.bin", .fd = std::move(waiting.read)});
    const auto queued = queue.submit(ImportJob{std::move(second), local(temp.path())});

    // A job cancelled before it runs closes its files at once.
    EXPECT(queue.cancel(queued));
    EXPECT(waiting.readerClosed());

    // Cancelled halfway: the partial file goes, and so does the pipe.
    running.send("first half");
    EXPECT(log.waitFor([&](const JobProgress &p) { return p.id == id && p.bytesDone >= 10; }));
    EXPECT(fs::exists(temp.path() / "partial.bin"));
    EXPECT(queue.cancel(id));
    // Unblocks the read if it was waiting; the reader may already be gone.
    (void)::write(running.write.get(), "second half", 11);
    const auto progress = finalProgress(queue, id);
    EXPECT(progress.state == JobState::kCancelled && progress.bytesDone == 0);
    EXPECT(!fs::exists(temp.path() / "partial.bin") && !fs::exists(temp.path() / "never.bin"));
    EXPECT(running.readerClosed());
}

}  // namespace

int main() {
    testKeepBothNames();
    testDelete();
    testCreateAndRename();
    testArchives();
    testChecksum();
    testVerifyArchive();
    testImport();
    testImportConflicts();
    testImportCancel();
    return 0;
}
