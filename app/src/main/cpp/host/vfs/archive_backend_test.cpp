// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The archive backend over zip, tar.gz and 7z fixtures made with createArchive().

#include <atomic>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <archive.h>
#include <sys/stat.h>

#include "filepan/archive_stream.hpp"
#include "filepan/create_archive.hpp"
#include "filepan/jobs/job_queue.hpp"
#include "filepan/vfs/archive_backend.hpp"
#include "filepan/vfs/local_backend.hpp"
#include "jobs/job_test_util.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;
using namespace filepan::vfs;
using filepan::CancellationToken;
using filepan::DecisionPromise;
using filepan::test::TempDir;

namespace {

void write(const fs::path &path, const std::string &content) {
    std::ofstream(path, std::ios::binary) << content;
}

std::string bigContent() {
    std::string content(700 * 1024 + 3, '\0');
    std::uint32_t state = 7;
    for (auto &c : content) {
        state = state * 1103515245 + 12345;
        c = static_cast<char>(state >> 16);
    }
    return content;
}

class NoCallbacks : public filepan::CreateArchiveCallbacks {
public:
    bool onProgress(std::uint64_t, std::uint64_t, std::string_view) override { return true; }
    bool onSourceError(std::string_view, int) override { return false; }
};

// tree/ with docs/readme.txt, big.bin, an empty directory and a symlink.
fs::path makeTree(const fs::path &root) {
    const fs::path tree = root / "tree";
    fs::create_directories(tree / "docs");
    fs::create_directories(tree / "empty");
    write(tree / "docs" / "readme.txt", "hello archive");
    write(tree / "big.bin", bigContent());
    fs::create_symlink("docs/readme.txt", tree / "link");
    return tree;
}

fs::path makeArchive(const fs::path &tree, const fs::path &target, int format, int filter,
                     const std::string &password = {}) {
    NoCallbacks callbacks;
    const auto result =
        filepan::createArchive(target.string(), {format, filter, password, 1},
                               {{(tree / "docs").string(), "docs"},
                                {(tree / "big.bin").string(), "big.bin"},
                                {(tree / "empty").string(), "empty"},
                                {(tree / "link").string(), "link"}},
                               callbacks);
    EXPECT(result.error == 0);
    return target;
}

std::string readAll(InputStream &stream) {
    std::string data;
    std::vector<std::uint8_t> buffer(100'000);
    while (true) {
        const auto length = stream.read(buffer);
        EXPECT(length);
        if (*length == 0) {
            return data;
        }
        data.append(reinterpret_cast<const char *>(buffer.data()), *length);
    }
}

std::map<std::string, Entry> listAll(Backend &backend, const Location &directory,
                                     ListOptions options = {}) {
    std::map<std::string, Entry> entries;
    const auto result = backend.list(
        directory, options,
        [&](std::vector<Entry> batch) {
            for (auto &entry : batch) {
                entries.emplace(entry.name, std::move(entry));
            }
        },
        CancellationToken());
    if (!result) {
        std::fprintf(stderr, "list %s: %d %s\n", directory.toString().c_str(), result.error().code,
                     result.error().message.c_str());
    }
    EXPECT(result);
    return entries;
}

// Answers password requests from a script and records them.
class ScriptedPasswords : public PasswordProvider {
public:
    void requestPassword(PasswordRequest request,
                         DecisionPromise<std::optional<std::string>> promise) override {
        std::lock_guard lock(mutex);
        requests.push_back(request);
        if (answers.empty()) {
            promise.fulfil(std::nullopt);
            return;
        }
        // Answer from another thread, as a UI would.
        auto answer = answers.front();
        answers.pop_front();
        std::thread([promise, answer] { promise.fulfil(answer); }).detach();
    }

    std::mutex mutex;
    std::deque<std::optional<std::string>> answers;
    std::vector<PasswordRequest> requests;
};

struct Fixture {
    std::shared_ptr<SimpleBackendResolver> resolver = std::make_shared<SimpleBackendResolver>();
    std::shared_ptr<ScriptedPasswords> passwords = std::make_shared<ScriptedPasswords>();
    std::shared_ptr<ArchiveBackend> archive;

    explicit Fixture(ArchiveBackendOptions options = {.revalidateInterval = {}}) {
        resolver->set(BackendKind::kLocal, std::make_shared<LocalBackend>());
        archive = std::make_shared<ArchiveBackend>(*resolver, passwords, options);
        resolver->set(BackendKind::kArchive, archive);
    }
};

void checkFormat(int format, int filter, const char *extension) {
    TempDir temp;
    const auto tree = makeTree(temp.path());
    const auto file = makeArchive(tree, temp.path() / (std::string("a") + extension), format, filter);
    Fixture fixture;
    ArchiveBackend &backend = *fixture.archive;
    const Location root = Location::archive(Location::local(file.string()));

    const auto entries = listAll(backend, root);
    // 7z stores symbolic links as files.
    const bool hasLinks = format != ARCHIVE_FORMAT_7ZIP;
    EXPECT(entries.size() == 4u);
    EXPECT(entries.at("docs").type == EntryType::kDirectory);
    EXPECT(entries.at("empty").isDirectoryLike());
    EXPECT(entries.at("big.bin").type == EntryType::kFile);
    EXPECT(entries.at("big.bin").size == bigContent().size());
    EXPECT(entries.at("big.bin").hasStat && entries.at("big.bin").modifiedTime.seconds > 0);
    if (hasLinks) {
        const auto &link = entries.at("link");
        EXPECT(link.type == EntryType::kSymlink && link.linkTarget == "docs/readme.txt");
        EXPECT(link.targetType == EntryType::kFile && !link.brokenLink);
        EXPECT(link.size == 13);
    }
    // Without stat: names and types only.
    const auto bare = listAll(backend, root, {.withStat = false});
    EXPECT(bare.size() == 4u && !bare.at("big.bin").hasStat);

    const auto docs = listAll(backend, root.child("docs"));
    EXPECT(docs.size() == 1u && docs.contains("readme.txt"));
    EXPECT(listAll(backend, root.child("empty")).empty());

    const auto stat = backend.stat(root.child("docs").child("readme.txt"));
    EXPECT(stat && stat->size == 13 && stat->name == "readme.txt");
    EXPECT((stat->mode & S_IFMT) == S_IFREG);
    EXPECT(backend.stat(root.child("missing")).error().code == ENOENT);
    EXPECT(backend.list(root.child("big.bin"), {}, [](auto) {}, CancellationToken()).error().code
           == ENOTDIR);
    if (hasLinks) {
        const auto followed = backend.stat(root.child("link"));
        EXPECT(followed && followed->type == EntryType::kFile && followed->name == "link");
        EXPECT(backend.lstat(root.child("link"))->type == EntryType::kSymlink);
        EXPECT(*backend.readLink(root.child("link")) == "docs/readme.txt");
        auto viaLink = backend.openRead(root.child("link"));
        EXPECT(viaLink && readAll(**viaLink) == "hello archive");
    }

    // Reading, and seeking both ways.
    auto stream = backend.openRead(root.child("big.bin"));
    EXPECT(stream);
    const std::string content = bigContent();
    EXPECT(*(*stream)->size() == content.size());
    EXPECT(readAll(**stream) == content);
    EXPECT(*(*stream)->seek(500'000, SeekOrigin::kBegin) == 500'000);
    std::vector<std::uint8_t> chunk(10);
    EXPECT(*(*stream)->read(chunk) == 10);
    EXPECT(std::string(chunk.begin(), chunk.end()) == content.substr(500'000, 10));
    EXPECT(*(*stream)->seek(-20, SeekOrigin::kEnd) == content.size() - 20);
    EXPECT(readAll(**stream) == content.substr(content.size() - 20));
    EXPECT(backend.openRead(root.child("docs")).error().code == EISDIR);

    // Read-only.
    EXPECT(backend.makeDirectory(root.child("new")).error().code == EROFS);
    EXPECT(backend.createFile(root.child("new")).error().code == EROFS);
    EXPECT(backend.openWrite(root.child("new"), {}).error().code == EROFS);
    EXPECT(backend.remove(root.child("big.bin"), false, nullptr, CancellationToken()).error().code
           == EROFS);
    EXPECT(backend.rename(root.child("big.bin"), root.child("x"), false).error().code == EROFS);
    EXPECT(!backend.capabilities().has(Capability::kWrite));
    EXPECT(fixture.passwords->requests.empty());
}

void testFormats() {
    checkFormat(ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE, ".zip");
    checkFormat(ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_GZIP, ".tar.gz");
    checkFormat(ARCHIVE_FORMAT_7ZIP, ARCHIVE_FILTER_NONE, ".7z");
}

class FileSink : public filepan::ArchiveSink {
public:
    explicit FileSink(const fs::path &path) : out_(path, std::ios::binary) {}
    bool write(const void *buffer, std::size_t size) override {
        out_.write(static_cast<const char *>(buffer), static_cast<std::streamsize>(size));
        return true;
    }

private:
    std::ofstream out_;
};

void writeEntries(const fs::path &path, int format, int filter,
                  const std::vector<std::pair<std::string, std::string>> &files) {
    FileSink sink(path);
    filepan::ArchiveError error;
    auto writer = filepan::ArchiveWriter::open(sink, {format, filter, {}, 1}, error);
    EXPECT(writer);
    for (const auto &[name, content] : files) {
        filepan::ArchiveWriter::Entry entry;
        entry.pathname = name;
        entry.mode = S_IFREG | 0644;
        entry.size = static_cast<std::int64_t>(content.size());
        EXPECT(writer->writeHeader(entry));
        EXPECT(writer->writeData(content.data(), content.size()));
    }
    EXPECT(writer->close());
}

// Directories that only appear in their entries' paths.
void testSynthesizedDirectories() {
    TempDir temp;
    const auto file = temp.path() / "s.tar";
    writeEntries(file, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_NONE,
                 {{"a/b/c.txt", "c"}, {"./a/d.txt", "dd"}, {"/top.txt", "t"}, {"x/y/", ""}});
    Fixture fixture;
    const Location root = Location::archive(Location::local(file.string()));
    const auto top = listAll(*fixture.archive, root);
    EXPECT(top.size() == 3u && top.at("a").type == EntryType::kDirectory);
    EXPECT(top.contains("top.txt") && top.contains("x"));
    const auto a = listAll(*fixture.archive, root.child("a"));
    EXPECT(a.size() == 2u && a.at("b").type == EntryType::kDirectory && a.at("d.txt").size == 2);
    EXPECT(listAll(*fixture.archive, root.child("a").child("b")).contains("c.txt"));
    EXPECT(fixture.archive->stat(root.child("x").child("y"))->type == EntryType::kDirectory);
    const auto statA = fixture.archive->stat(root.child("a"));
    EXPECT(statA && statA->hasStat && (statA->mode & S_IFMT) == S_IFDIR);
    auto stream = fixture.archive->openRead(root.child("a").child("b").child("c.txt"));
    EXPECT(stream && readAll(**stream) == "c");

    // The index follows the archive file when it changes.
    writeEntries(file, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_NONE,
                 {{"only/one.txt", "1"}, {"two.txt", "22"}});
    const auto changed = listAll(*fixture.archive, root);
    EXPECT(changed.size() == 2u && changed.contains("only") && changed.contains("two.txt"));
    EXPECT(fixture.archive->stat(root.child("a")).error().code == ENOENT);
}

void testEncryptedZip() {
    TempDir temp;
    const auto tree = makeTree(temp.path());
    const auto file =
        makeArchive(tree, temp.path() / "secret.zip", ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE,
                    "s3cret");
    const Location archiveFile = Location::local(file.string());
    const Location readme = Location::archive(archiveFile, "/docs/readme.txt");

    {
        Fixture fixture;
        // Listing needs no password: zip encrypts data, not names.
        EXPECT(listAll(*fixture.archive, Location::archive(archiveFile)).size() == 4u);
        EXPECT(fixture.passwords->requests.empty());

        // A wrong password, then the right one.
        fixture.passwords->answers = {"nope", "s3cret"};
        auto stream = fixture.archive->openRead(readme);
        EXPECT(stream && readAll(**stream) == "hello archive");
        EXPECT(fixture.passwords->requests.size() == 2u);
        EXPECT(!fixture.passwords->requests[0].retry && fixture.passwords->requests[1].retry);
        EXPECT(fixture.passwords->requests[0].archive == archiveFile);
        EXPECT(fixture.passwords->requests[0].entryPath == "/docs/readme.txt");

        // Remembered for the session: another entry needs no prompt.
        auto big = fixture.archive->openRead(Location::archive(archiveFile, "/big.bin"));
        EXPECT(big && readAll(**big) == bigContent());
        EXPECT(fixture.passwords->requests.size() == 2u);

        // Until forgotten; then giving up fails with EACCES.
        fixture.archive->forgetPasswords(archiveFile);
        const auto declined = fixture.archive->openRead(readme);
        EXPECT(!declined && declined.error().code == EACCES);
        EXPECT(fixture.passwords->requests.size() == 3u);
    }
    {
        // Wrong every time: the attempts run out.
        Fixture fixture({.revalidateInterval = {}, .maxPasswordAttempts = 3});
        fixture.passwords->answers = {"a", "b", "c", "d", "e"};
        const auto result = fixture.archive->openRead(readme);
        EXPECT(!result && result.error().code == EACCES);
        EXPECT(fixture.passwords->requests.size() == 3u);
    }
    {
        // A password supplied up front; and no provider at all.
        Fixture fixture;
        fixture.archive->addPassword(archiveFile, "s3cret");
        auto stream = fixture.archive->openRead(readme);
        EXPECT(stream && readAll(**stream) == "hello archive");
        EXPECT(fixture.passwords->requests.empty());
        SimpleBackendResolver resolver;
        resolver.set(BackendKind::kLocal, std::make_shared<LocalBackend>());
        ArchiveBackend bare(resolver);
        EXPECT(bare.openRead(readme).error().code == EACCES);
    }
}

// An archive inside an archive, through Location nesting.
void testNested() {
    TempDir temp;
    const auto inner = temp.path() / "inner.tar.gz";
    writeEntries(inner, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_GZIP,
                 {{"deep/file.txt", "nested content"}});
    const auto innerZip = temp.path() / "inner.zip";
    writeEntries(innerZip, ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE, {{"z.txt", "zip in zip"}});
    std::ifstream innerIn(inner, std::ios::binary);
    std::stringstream innerBytes;
    innerBytes << innerIn.rdbuf();
    std::ifstream zipIn(innerZip, std::ios::binary);
    std::stringstream zipBytes;
    zipBytes << zipIn.rdbuf();
    const auto outer = temp.path() / "outer.zip";
    writeEntries(outer, ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE,
                 {{"in/inner.tar.gz", innerBytes.str()}, {"inner.zip", zipBytes.str()}});

    Fixture fixture;
    const Location outerRoot = Location::archive(Location::local(outer.string()));
    const Location nested = Location::archive(outerRoot.child("in").child("inner.tar.gz"));
    EXPECT(listAll(*fixture.archive, nested).contains("deep"));
    auto stream = fixture.archive->openRead(nested.child("deep").child("file.txt"));
    EXPECT(stream && readAll(**stream) == "nested content");
    const Location nestedZip = Location::archive(outerRoot.child("inner.zip"), "/z.txt");
    auto zipStream = fixture.archive->openRead(nestedZip);
    EXPECT(zipStream && readAll(**zipStream) == "zip in zip");
}

void testWatch() {
    TempDir temp;
    const auto file = temp.path() / "w.tar";
    writeEntries(file, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_NONE, {{"a.txt", "a"}});
    write(temp.path() / "unrelated", "u");
    Fixture fixture({.revalidateInterval = std::chrono::hours(1)});
    const Location root = Location::archive(Location::local(file.string()));
    EXPECT(listAll(*fixture.archive, root).contains("a.txt"));
    std::atomic<int> changes = 0;
    auto handle = fixture.archive->watch(root, [&](const DirectoryChange &change) {
        EXPECT(change.rescan && change.path == "/");
        ++changes;
    });
    EXPECT(handle);
    writeEntries(file, ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_NONE, {{"b.txt", "b"}});
    for (int i = 0; i < 200 && changes == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT(changes > 0);
    // The index was dropped even though the revalidate interval hasn't passed.
    EXPECT(listAll(*fixture.archive, root).contains("b.txt"));
}

filepan::jobs::JobProgress runJob(filepan::jobs::JobQueue &queue, filepan::jobs::JobSpec spec) {
    const auto id = queue.submit(std::move(spec));
    queue.waitUntilIdle();
    const auto progress = queue.progress(id);
    EXPECT(progress && filepan::jobs::isFinished(progress->state));
    if (progress->error) {
        std::fprintf(stderr, "job failed: %d %s\n", progress->error->code,
                     progress->error->message.c_str());
    }
    return *progress;
}

// Jobs out of archives: extracting one nested in another, and copying entries out, encrypted
// ones included.
void testJobs() {
    using namespace filepan::jobs;
    TempDir temp;
    const auto tree = makeTree(temp.path());
    const auto zip = makeArchive(tree, temp.path() / "a.zip", ARCHIVE_FORMAT_ZIP,
                                 ARCHIVE_FILTER_NONE, "pw");
    const auto inner = makeArchive(tree, temp.path() / "inner.tar.gz",
                                   ARCHIVE_FORMAT_TAR_PAX_RESTRICTED, ARCHIVE_FILTER_GZIP);
    std::ifstream innerIn(inner, std::ios::binary);
    std::stringstream innerBytes;
    innerBytes << innerIn.rdbuf();
    const auto outer = temp.path() / "outer.zip";
    writeEntries(outer, ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE,
                 {{"nested/inner.tar.gz", innerBytes.str()}});

    Fixture fixture;
    auto decisions = std::make_shared<filepan::test::ScriptedDecisions>();
    JobQueue queue(fixture.resolver, decisions, nullptr);

    fs::create_directory(temp.path() / "x");
    const auto extracted = runJob(
        queue, ExtractArchiveJob{Location::archive(Location::local(outer.string()),
                                                   "/nested/inner.tar.gz"),
                                 Location::local((temp.path() / "x").string()), {}});
    EXPECT(extracted.state == JobState::kSucceeded);
    EXPECT(filepan::test::readFile(temp.path() / "x" / "big.bin") == bigContent());
    EXPECT(filepan::test::readFile(temp.path() / "x" / "docs" / "readme.txt") == "hello archive");
    EXPECT(fs::read_symlink(temp.path() / "x" / "link") == "docs/readme.txt");

    // Copy out of the encrypted zip, the password asked for once.
    fixture.passwords->answers = {"pw"};
    fs::create_directory(temp.path() / "y");
    const Location zipRoot = Location::archive(Location::local(zip.string()));
    const auto copied = runJob(queue, CopyJob{{zipRoot.child("docs"), zipRoot.child("big.bin"),
                                               zipRoot.child("link"), zipRoot.child("empty")},
                                              Location::local((temp.path() / "y").string())});
    EXPECT(copied.state == JobState::kSucceeded && copied.itemsSkipped == 0);
    EXPECT(copied.bytesDone == bigContent().size() + 13);
    EXPECT(filepan::test::readFile(temp.path() / "y" / "big.bin") == bigContent());
    EXPECT(filepan::test::readFile(temp.path() / "y" / "docs" / "readme.txt") == "hello archive");
    EXPECT(fs::read_symlink(temp.path() / "y" / "link") == "docs/readme.txt");
    EXPECT(fs::is_directory(temp.path() / "y" / "empty"));
    EXPECT(fixture.passwords->requests.size() == 1u);

    // Moving out of an archive copies, then fails to delete the read-only source.
    decisions->errorAnswer = {ErrorAction::kSkip, false};
    fs::create_directory(temp.path() / "z");
    const auto moved = runJob(queue, MoveJob{{zipRoot.child("big.bin")},
                                             Location::local((temp.path() / "z").string())});
    EXPECT(moved.state == JobState::kSucceeded);
    EXPECT(fs::exists(temp.path() / "z" / "big.bin"));
    EXPECT(!decisions->errors.empty() && decisions->errors.back().error.code == EROFS);
}

}  // namespace

int main() {
    EXPECT(std::setlocale(LC_CTYPE, "C.UTF-8"));
    testFormats();
    testSynthesizedDirectories();
    testEncryptedZip();
    testNested();
    testWatch();
    testJobs();
    std::puts("archive_backend_test: OK");
    return 0;
}
