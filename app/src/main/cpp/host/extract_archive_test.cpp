// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>

#include "filepan/extract_archive.hpp"
#include "test_util.hpp"

using filepan::ExtractCallbacks;
using filepan::ExtractItem;
using filepan::test::TempDir;
namespace fs = std::filesystem;

namespace {

struct Source {
    std::string name;
    std::string content;
    std::string linkTarget;  // Non-empty for a symbolic link.
    bool isDirectory = false;
};

std::string read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string randomBytes(std::size_t size, unsigned seed) {
    std::mt19937 random(seed);
    std::string bytes(size, '\0');
    for (auto &byte : bytes) {
        byte = static_cast<char>(random() % 7);  // Compressible, but not trivially.
    }
    return bytes;
}

// Writes an archive with libarchive; format and filter are libarchive names.
void writeArchive(const fs::path &path, const std::string &format, const std::string &filter,
                  const std::vector<Source> &sources, const char *password = nullptr) {
    archive *a = archive_write_new();
    EXPECT(archive_write_set_format_by_name(a, format.c_str()) == ARCHIVE_OK);
    if (!filter.empty()) {
        EXPECT(archive_write_add_filter_by_name(a, filter.c_str()) == ARCHIVE_OK);
    }
    if (password) {
        EXPECT(archive_write_set_options(a, "zip:encryption=aes256") == ARCHIVE_OK);
        EXPECT(archive_write_set_passphrase(a, password) == ARCHIVE_OK);
    }
    EXPECT(archive_write_open_filename(a, path.c_str()) == ARCHIVE_OK);
    for (const auto &source : sources) {
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, source.name.c_str());
        archive_entry_set_mtime(entry, 1'700'000'000, 0);
        if (source.isDirectory) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_perm(entry, 0755);
        } else if (!source.linkTarget.empty()) {
            archive_entry_set_filetype(entry, AE_IFLNK);
            archive_entry_set_symlink(entry, source.linkTarget.c_str());
            archive_entry_set_perm(entry, 0777);
        } else {
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0640);
            archive_entry_set_size(entry, static_cast<la_int64_t>(source.content.size()));
        }
        EXPECT(archive_write_header(a, entry) == ARCHIVE_OK);
        if (!source.content.empty()) {
            EXPECT(archive_write_data(a, source.content.data(), source.content.size())
                   == static_cast<la_ssize_t>(source.content.size()));
        }
        archive_entry_free(entry);
    }
    EXPECT(archive_write_close(a) == ARCHIVE_OK);
    archive_write_free(a);
}

class Recorder : public ExtractCallbacks {
public:
    std::uint64_t progress = 0;
    std::vector<Stage> errors;
    std::function<bool(const ExtractItem *)> skip = [](const ExtractItem *) { return true; };

    bool onProgress(std::uint64_t writtenSize) override {
        progress += writtenSize;
        return true;
    }

    bool onError(const ExtractItem *item, Stage stage, int error,
                 const std::string &message) override {
        std::fprintf(stderr, "  error: item %lld stage %d errno %d %s\n",
                     item ? static_cast<long long>(item->index) : -1LL, static_cast<int>(stage),
                     error, message.c_str());
        errors.push_back(stage);
        return skip(item);
    }
};

// Reads each entry's position, the way the Kotlin side indexes an archive before planning.
std::map<std::string, std::uint64_t> readIndices(const fs::path &archivePath) {
    archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    EXPECT(archive_read_open_filename(a, archivePath.c_str(), 10240) == ARCHIVE_OK);
    std::map<std::string, std::uint64_t> indices;
    archive_entry *entry;
    for (std::uint64_t index = 0; archive_read_next_header(a, &entry) == ARCHIVE_OK; ++index) {
        indices.emplace(archive_entry_pathname(entry), index);
    }
    archive_read_free(a);
    return indices;
}

// Plans every non-directory entry.
std::vector<ExtractItem> planAll(const fs::path &archivePath, const std::vector<Source> &sources,
                                 const fs::path &target) {
    const auto indices = readIndices(archivePath);
    std::vector<ExtractItem> items;
    for (const auto &source : sources) {
        if (source.isDirectory) {
            fs::create_directories(target / source.name);
            continue;
        }
        items.push_back({indices.at(source.name),
                         source.linkTarget.empty() ? ExtractItem::Type::kFile
                                                   : ExtractItem::Type::kSymbolicLink,
                         source.linkTarget.empty()
                             ? static_cast<std::int64_t>(source.content.size())
                             : 0,
                         (target / source.name).string()});
    }
    std::sort(items.begin(), items.end(),
              [](const ExtractItem &a, const ExtractItem &b) { return a.index < b.index; });
    return items;
}

const std::vector<Source> kSources = {
    {"dir/", "", "", true},
    {"dir/a.txt", "hello", ""},
    {"dir/big.bin", randomBytes(3 * 1024 * 1024 + 17, 1), ""},
    {"dir/empty", "", ""},
    {"dir/link", "", "a.txt"},
    {"top.txt", "top", ""},
};

void testRoundTrip(const std::string &format, const std::string &filter) {
    TempDir dir;
    const auto archivePath = dir.path() / "archive";
    // libarchive's 7z writer stores symbolic links as regular files.
    auto sources = kSources;
    if (format == "7zip") {
        std::erase_if(sources, [](const Source &source) { return !source.linkTarget.empty(); });
    }
    writeArchive(archivePath, format, filter, sources);
    const auto target = dir.path() / "out";
    fs::create_directory(target);
    Recorder recorder;
    const auto result =
        filepan::extractArchive(archivePath.string(), {}, planAll(archivePath, sources, target), recorder);
    if (result.error || !recorder.errors.empty()) {
        std::fprintf(stderr, "%s/%s failed\n", format.c_str(), filter.c_str());
    }
    EXPECT(result.error == 0);
    EXPECT(recorder.errors.empty());
    EXPECT(read(target / "dir/a.txt") == "hello");
    EXPECT(read(target / "dir/big.bin") == kSources[2].content);
    EXPECT(fs::exists(target / "dir/empty") && fs::file_size(target / "dir/empty") == 0);
    EXPECT(read(target / "top.txt") == "top");
    if (format != "7zip") {
        EXPECT(fs::is_symlink(target / "dir/link"));
        EXPECT(fs::read_symlink(target / "dir/link") == "a.txt");
    }
    struct stat st {};
    EXPECT(stat((target / "dir/a.txt").c_str(), &st) == 0);
    EXPECT(st.st_mtim.tv_sec == 1'700'000'000);
    EXPECT(recorder.progress >= kSources[2].content.size());
}

void testExtractsOnlyPlannedItems() {
    TempDir dir;
    const auto archivePath = dir.path() / "archive.zip";
    writeArchive(archivePath, "zip", "", kSources);
    const auto target = dir.path() / "out";
    fs::create_directory(target);
    Recorder recorder;
    const std::vector<ExtractItem> items = {
        {5, ExtractItem::Type::kFile, 3, (target / "renamed.txt").string()},
    };
    EXPECT(filepan::extractArchive(archivePath.string(), {}, items, recorder).extractedCount == 1);
    EXPECT(read(target / "renamed.txt") == "top");
    EXPECT(!fs::exists(target / "dir"));
}

void testReplacesExistingTargetWithoutFollowingLinks() {
    TempDir dir;
    const auto archivePath = dir.path() / "archive.tar";
    writeArchive(archivePath, "pax", "", kSources);
    const auto target = dir.path() / "out";
    fs::create_directory(target);
    std::ofstream(dir.path() / "victim") << "keep";
    fs::create_symlink(dir.path() / "victim", target / "top.txt");
    Recorder recorder;
    const std::vector<ExtractItem> items = {
        {5, ExtractItem::Type::kFile, 3, (target / "top.txt").string()},
    };
    EXPECT(filepan::extractArchive(archivePath.string(), {}, items, recorder).error == 0);
    EXPECT(!fs::is_symlink(target / "top.txt"));
    EXPECT(read(target / "top.txt") == "top");
    EXPECT(read(dir.path() / "victim") == "keep");
}

void testMismatchedPlanIsRejected() {
    TempDir dir;
    const auto archivePath = dir.path() / "archive.zip";
    writeArchive(archivePath, "zip", "", kSources);
    Recorder recorder;
    // Index 1 is "hello" (5 bytes), not 42 bytes.
    const std::vector<ExtractItem> items = {
        {1, ExtractItem::Type::kFile, 42, (dir.path() / "wrong").string()},
        {5, ExtractItem::Type::kFile, 3, (dir.path() / "right").string()},
    };
    const auto result = filepan::extractArchive(archivePath.string(), {}, items, recorder);
    EXPECT(result.error == 0);
    EXPECT(recorder.errors == std::vector{ExtractCallbacks::Stage::kEntryMismatch});
    EXPECT(!fs::exists(dir.path() / "wrong"));
    EXPECT(read(dir.path() / "right") == "top");
}

void testEncryptedZip() {
    TempDir dir;
    const auto archivePath = dir.path() / "secret.zip";
    const std::vector<Source> sources = {{"secret.txt", "classified", ""}};
    writeArchive(archivePath, "zip", "", sources, "hunter2");
    const std::vector<ExtractItem> items = {
        {0, ExtractItem::Type::kFile, 10, (dir.path() / "secret.txt").string()},
    };

    Recorder noPassword;
    filepan::extractArchive(archivePath.string(), {}, items, noPassword);
    EXPECT(noPassword.errors.size() == 1);
    EXPECT(!fs::exists(dir.path() / "secret.txt"));

    Recorder withPassword;
    EXPECT(filepan::extractArchive(archivePath.string(), {"wrong", "hunter2"}, items, withPassword)
               .error == 0);
    EXPECT(withPassword.errors.empty());
    EXPECT(read(dir.path() / "secret.txt") == "classified");
}

void testMissingArchive() {
    Recorder recorder;
    const std::vector<ExtractItem> items = {{0, ExtractItem::Type::kFile, -1, "/tmp/x"}};
    EXPECT(filepan::extractArchive("/nonexistent/archive.zip", {}, items, recorder).error == EIO);
    EXPECT(recorder.errors == std::vector{ExtractCallbacks::Stage::kOpenArchive});
}

}  // namespace

int main() {
    testRoundTrip("zip", "");
    testRoundTrip("pax", "gzip");
    testRoundTrip("pax", "zstd");
    testRoundTrip("pax", "xz");
    testRoundTrip("pax", "bzip2");
    testRoundTrip("pax", "lz4");
    testRoundTrip("7zip", "");
    testExtractsOnlyPlannedItems();
    testReplacesExistingTargetWithoutFollowingLinks();
    testMismatchedPlanIsRejected();
    testEncryptedZip();
    testMissingArchive();
    std::puts("extract_archive_test: OK");
    return 0;
}
