// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "filepan/vfs/local_backend.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;
using namespace filepan::vfs;
using filepan::CancellationToken;
using filepan::test::TempDir;

namespace {

void write(const fs::path &path, const std::string &content) {
    std::ofstream(path, std::ios::binary) << content;
}

std::map<std::string, Entry> listAll(LocalBackend &backend, const fs::path &path,
                                     ListOptions options = {}, int *batches = nullptr) {
    std::map<std::string, Entry> entries;
    const auto result = backend.list(
        Location::local(path.string()), options,
        [&](std::vector<Entry> batch) {
            EXPECT(!batch.empty());
            if (batches) ++*batches;
            for (auto &entry : batch) {
                EXPECT(!entries.contains(entry.name));
                entries.emplace(entry.name, std::move(entry));
            }
        },
        CancellationToken());
    EXPECT(result);
    return entries;
}

// Listing matches std::filesystem, across several getdents64 buffers and batches.
void testListingMatchesFilesystem() {
    TempDir temp;
    for (int i = 0; i < 3000; ++i) {
        write(temp.path() / ("file-with-a-longish-name-" + std::to_string(i)), std::string(i % 7, 'x'));
    }
    fs::create_directory(temp.path() / "dir");
    LocalBackend backend;
    int batches = 0;
    const auto entries = listAll(backend, temp.path(), {}, &batches);
    EXPECT(batches > 1);

    std::size_t count = 0;
    for (const auto &item : fs::directory_iterator(temp.path())) {
        ++count;
        const auto name = item.path().filename().string();
        const auto found = entries.find(name);
        EXPECT(found != entries.end());
        const Entry &entry = found->second;
        EXPECT(entry.hasStat);
        struct stat st {};
        EXPECT(lstat(item.path().c_str(), &st) == 0);
        EXPECT(entry.mode == st.st_mode);
        EXPECT(entry.inode == st.st_ino);
        EXPECT(entry.uid == st.st_uid && entry.gid == st.st_gid);
        EXPECT(entry.modifiedTime.seconds == st.st_mtim.tv_sec);
        EXPECT(entry.modifiedTime.nanoseconds == st.st_mtim.tv_nsec);
        if (item.is_directory()) {
            EXPECT(entry.type == EntryType::kDirectory && entry.isDirectoryLike());
            EXPECT(entry.mimeType().kind == FileKind::kDirectory);
        } else {
            EXPECT(entry.type == EntryType::kFile && entry.targetType == EntryType::kFile);
            EXPECT(entry.size == item.file_size());
        }
        EXPECT(!entry.hidden);
    }
    EXPECT(count == entries.size());

    // Statting on several threads gives the same entries as on one.
    fs::create_symlink("dir", temp.path() / "link");
    fs::create_symlink("missing", temp.path() / "broken");
    const auto serial = listAll(backend, temp.path(), {.statThreads = 1});
    const auto parallel = listAll(backend, temp.path(), {.statThreads = 8});
    EXPECT(serial.size() == parallel.size() && serial.size() == count + 2);
    for (const auto &[name, entry] : serial) {
        const Entry &other = parallel.at(name);
        EXPECT(entry.type == other.type && entry.targetType == other.targetType);
        EXPECT(entry.hasStat == other.hasStat && entry.brokenLink == other.brokenLink);
        EXPECT(entry.size == other.size && entry.mode == other.mode);
        EXPECT(entry.inode == other.inode && entry.modifiedTime == other.modifiedTime);
        EXPECT(entry.linkTarget == other.linkTarget && entry.hidden == other.hidden);
    }
    EXPECT(parallel.at("link").isDirectoryLike() && parallel.at("broken").brokenLink);

    // Without stat, only names and types.
    const auto bare = listAll(backend, temp.path(), {.withStat = false});
    EXPECT(bare.size() == count + 2);
    EXPECT(!bare.at("dir").hasStat);
    EXPECT(bare.at("dir").type == EntryType::kDirectory);
}

void testHiddenAndSymlinks() {
    TempDir temp;
    write(temp.path() / ".hidden", "h");
    write(temp.path() / "photo.jpg", "jpeg");
    fs::create_directory(temp.path() / "dir");
    fs::create_symlink("photo.jpg", temp.path() / "file-link");
    fs::create_symlink("dir", temp.path() / "dir-link");
    fs::create_symlink("missing", temp.path() / "broken");
    fs::create_symlink("loop", temp.path() / "loop");
    LocalBackend backend;
    const auto entries = listAll(backend, temp.path());
    EXPECT(entries.size() == 7);

    EXPECT(entries.at(".hidden").hidden);
    EXPECT(!entries.at("photo.jpg").hidden);
    EXPECT(entries.at("photo.jpg").mimeType().type == "image/jpeg");

    const Entry &fileLink = entries.at("file-link");
    EXPECT(fileLink.type == EntryType::kSymlink && fileLink.targetType == EntryType::kFile);
    EXPECT(fileLink.linkTarget == "photo.jpg" && !fileLink.brokenLink);
    EXPECT(fileLink.size == 4);  // The target's size.

    const Entry &dirLink = entries.at("dir-link");
    EXPECT(dirLink.targetType == EntryType::kDirectory && dirLink.isDirectoryLike());

    const Entry &broken = entries.at("broken");
    EXPECT(broken.type == EntryType::kSymlink && broken.brokenLink);
    EXPECT(broken.linkTarget == "missing" && broken.targetType == EntryType::kOther);
    EXPECT(broken.mimeType().type == kGenericMimeType);
    EXPECT(entries.at("loop").brokenLink);

    // stat follows, lstat doesn't.
    auto followed = backend.stat(Location::local((temp.path() / "file-link").string()));
    EXPECT(followed && followed->type == EntryType::kFile && followed->name == "file-link");
    auto link = backend.lstat(Location::local((temp.path() / "file-link").string()));
    EXPECT(link && link->type == EntryType::kSymlink && link->linkTarget == "photo.jpg");
    auto brokenStat = backend.stat(Location::local((temp.path() / "broken").string()));
    EXPECT(!brokenStat && brokenStat.error().code == ENOENT);
    auto brokenLstat = backend.lstat(Location::local((temp.path() / "broken").string()));
    EXPECT(brokenLstat && brokenLstat->brokenLink);
}

void testListErrorsAndCancel() {
    TempDir temp;
    LocalBackend backend;
    const auto noop = [](std::vector<Entry>) {};
    auto result = backend.list(Location::local((temp.path() / "missing").string()), {}, noop,
                               CancellationToken());
    EXPECT(!result && result.error().code == ENOENT);
    write(temp.path() / "file", "");
    result = backend.list(Location::local((temp.path() / "file").string()), {}, noop,
                          CancellationToken());
    EXPECT(!result && result.error().code == ENOTDIR);

    for (int i = 0; i < 2000; ++i) {
        write(temp.path() / std::to_string(i), "");
    }
    CancellationToken cancel;
    int batches = 0;
    result = backend.list(
        Location::local(temp.path().string()), {.minBatchSize = 1},
        [&](std::vector<Entry>) {
            ++batches;
            cancel.cancel();
        },
        cancel);
    EXPECT(!result && result.error().code == ECANCELED);
    EXPECT(batches == 1);
}

void testOperations() {
    TempDir temp;
    LocalBackend backend;
    const auto root = Location::local(temp.path().string());
    const auto dir = root.child("dir");
    EXPECT(backend.makeDirectory(dir));
    EXPECT(backend.makeDirectory(dir).error().code == EEXIST);
    const auto file = dir.child("file.txt");
    EXPECT(backend.createFile(file));
    EXPECT(backend.createFile(file).error().code == EEXIST);

    {
        auto out = backend.openWrite(file, {.mode = WriteMode::kReplace});
        EXPECT(out);
        const std::string text = "hello, world";
        EXPECT((*out)->write({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()}));
        EXPECT((*out)->size().value() == text.size());
        EXPECT((*out)->close());
    }
    EXPECT(backend.openWrite(file, {}).error().code == EEXIST);
    {
        auto in = backend.openRead(file);
        EXPECT(in && (*in)->size().value() == 12);
        EXPECT((*in)->seek(7, SeekOrigin::kBegin).value() == 7);
        std::uint8_t buffer[16];
        auto length = (*in)->read(buffer);
        EXPECT(length && *length == 5 && std::string(buffer, buffer + 5) == "world");
        EXPECT((*in)->read(buffer).value() == 0);
    }

    EXPECT(backend.setTimes(file, std::nullopt, Timestamp{1'000'000'000, 5}));
    auto entry = backend.lstat(file);
    EXPECT(entry->modifiedTime == (Timestamp{1'000'000'000, 5}));
    EXPECT(backend.chmod(file, 0600));
    EXPECT((backend.lstat(file)->mode & 07777) == 0600);

    const auto renamed = dir.child("renamed.txt");
    EXPECT(backend.rename(file, renamed, false));
    EXPECT(backend.createFile(file));
    EXPECT(backend.rename(file, renamed, false).error().code == EEXIST);
    EXPECT(backend.rename(file, renamed, true));
    EXPECT(backend.lstat(file).error().code == ENOENT);

    const auto link = dir.child("link");
    EXPECT(backend.createSymlink(link, "renamed.txt"));
    EXPECT(backend.readLink(link).value() == "renamed.txt");

    // Non-recursive removal of a non-empty directory fails; recursive works.
    EXPECT(!backend.remove(dir, false, nullptr, CancellationToken()));
    const auto removed = backend.remove(dir, true, nullptr, CancellationToken());
    EXPECT(removed && removed->removedCount == 3 && removed->skippedCount == 0);
    EXPECT(!fs::exists(temp.path() / "dir"));

    EXPECT(backend.capabilities().has(Capability::kWatch));
}

// Errors inside a recursive removal reach the callbacks, which can skip.
void testRemoveErrors() {
    if (geteuid() == 0) {
        return;  // Permissions don't stop root.
    }
    TempDir temp;
    fs::create_directories(temp.path() / "tree" / "locked");
    write(temp.path() / "tree" / "locked" / "file", "");
    write(temp.path() / "tree" / "other", "");
    fs::permissions(temp.path() / "tree" / "locked", fs::perms(0555));

    class Callbacks : public RemoveCallbacks {
    public:
        bool onProgress(std::uint64_t, std::string_view) override { return true; }
        ErrorAction onError(std::string_view path, const filepan::Error &error) override {
            EXPECT(error.code == EACCES);
            EXPECT(path.ends_with("locked/file"));
            ++errors;
            return errors == 1 ? ErrorAction::kRetry : ErrorAction::kSkip;
        }
        int errors = 0;
    } callbacks;
    LocalBackend backend;
    const auto result = backend.remove(Location::local((temp.path() / "tree").string()), true,
                                       &callbacks, CancellationToken());
    EXPECT(result && callbacks.errors == 2);
    EXPECT(result->removedCount == 1 && result->skippedCount == 3);
    EXPECT(fs::exists(temp.path() / "tree" / "locked" / "file"));
    EXPECT(!fs::exists(temp.path() / "tree" / "other"));
    fs::permissions(temp.path() / "tree" / "locked", fs::perms(0755));
}

}  // namespace

int main() {
    testListingMatchesFilesystem();
    testHiddenAndSymlinks();
    testListErrorsAndCancel();
    testOperations();
    testRemoveErrors();
    return 0;
}
