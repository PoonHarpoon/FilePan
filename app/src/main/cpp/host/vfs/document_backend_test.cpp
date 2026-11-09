// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "filepan/vfs/document_backend.hpp"
#include "test_util.hpp"
#include "vfs/fake_document_provider.hpp"

using namespace filepan;
using namespace filepan::vfs;
using filepan::test::FakeDocumentProvider;
using filepan::test::TempDir;
namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path &path, const std::string &text) { std::ofstream(path) << text; }

std::string readFile(const fs::path &path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> names(DocumentBackend &backend, const Location &directory) {
    std::vector<std::string> out;
    EXPECT(backend.list(directory, {.minBatchSize = 1},
                        [&](std::vector<Entry> batch) {
                            for (const Entry &entry : batch) {
                                out.push_back(entry.name);
                            }
                        },
                        {}));
    std::ranges::sort(out);
    return out;
}

std::string read(DocumentBackend &backend, const Location &location) {
    auto stream = backend.openRead(location);
    EXPECT(stream.has_value());
    std::string out;
    std::uint8_t buffer[7];
    while (true) {
        auto n = (*stream)->read(buffer);
        EXPECT(n.has_value());
        if (*n == 0) {
            return out;
        }
        out.append(reinterpret_cast<const char *>(buffer), *n);
    }
}

void write(DocumentBackend &backend, const Location &location, const std::string &text, WriteMode mode) {
    auto stream = backend.openWrite(location, {.mode = mode});
    EXPECT(stream.has_value());
    EXPECT((*stream)->write({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()}));
    EXPECT((*stream)->close());
}

}  // namespace

int main() {
    TempDir dir;
    const fs::path top = dir.path() / "Music";
    fs::create_directories(top / "Albums" / "Live");
    writeFile(top / "Albums" / "notes.txt", "liner notes");
    writeFile(top / ".hidden", "x");
    auto provider = std::make_shared<FakeDocumentProvider>();
    DocumentBackend backend(provider);
    backend.setTrees({{"Music", top.string()}});
    const Location root = Location::document("Music");

    // An unknown tree, and the root.
    EXPECT(backend.stat(Location::document("Gone")).error().code == ENOENT);
    auto rootEntry = backend.stat(root);
    EXPECT(rootEntry && rootEntry->type == EntryType::kDirectory);
    EXPECT(backend.capabilities().has(Capability::kWrite) && !backend.capabilities().has(Capability::kSymlinks));

    // Listing, and paths resolved through the listing; the ids are never paths.
    EXPECT((names(backend, root) == std::vector<std::string>{".hidden", "Albums"}));
    EXPECT((names(backend, root.child("Albums")) == std::vector<std::string>{"Live", "notes.txt"}));
    auto notes = backend.stat(root.child("Albums").child("notes.txt"));
    EXPECT(notes && notes->type == EntryType::kFile && notes->size == 11 && notes->name == "notes.txt");
    EXPECT(backend.stat(root.child("Albums").child("missing")).error().code == ENOENT);
    EXPECT(backend.stat(root.child("Albums").child("notes.txt").child("x")).error().code == ENOTDIR);
    // Resolved again from the cache, without asking the provider.
    const int before = provider->calls;
    EXPECT(backend.stat(root.child("Albums").child("notes.txt")));
    EXPECT(provider->calls == before);

    // Reading and writing through file descriptors.
    EXPECT(read(backend, root.child("Albums").child("notes.txt")) == "liner notes");
    EXPECT(backend.openRead(root.child("Albums")).error().code == EISDIR);
    write(backend, root.child("new.txt"), "fresh", WriteMode::kCreateNew);
    EXPECT(readFile(top / "new.txt") == "fresh");
    EXPECT(backend.openWrite(root.child("new.txt"), {.mode = WriteMode::kCreateNew}).error().code == EEXIST);
    write(backend, root.child("new.txt"), "hi", WriteMode::kReplace);
    EXPECT(readFile(top / "new.txt") == "hi");
    EXPECT(backend.openWrite(root.child("nope.txt"), {.mode = WriteMode::kExisting}).error().code == ENOENT);
    EXPECT(backend.stat(root.child("new.txt"))->size == 2);

    // Creating: a clash fails rather than getting the provider's "name (1)".
    EXPECT(backend.makeDirectory(root.child("Singles"), 0777));
    EXPECT(fs::is_directory(top / "Singles"));
    EXPECT(backend.makeDirectory(root.child("Singles"), 0777).error().code == EEXIST);
    EXPECT(backend.createFile(root.child("new.txt"), 0666).error().code == EEXIST);
    EXPECT(!fs::exists(top / "new.txt (1)"));
    EXPECT(backend.createFile(root.child("Singles").child("a.txt"), 0666));

    // Renaming in place, moving, and both; replacing; EXDEV without a provider move.
    EXPECT(backend.rename(root.child("new.txt"), root.child("renamed.txt"), false));
    EXPECT(fs::exists(top / "renamed.txt") && !fs::exists(top / "new.txt"));
    EXPECT(backend.stat(root.child("new.txt")).error().code == ENOENT);
    EXPECT(backend.rename(root.child("renamed.txt"), root.child("Singles").child("b.txt"), false));
    EXPECT(readFile(top / "Singles" / "b.txt") == "hi");
    EXPECT(backend.rename(root.child("Singles").child("b.txt"), root.child("Singles").child("a.txt"), false).error().code ==
           EEXIST);
    EXPECT(backend.rename(root.child("Singles").child("b.txt"), root.child("Singles").child("a.txt"), true));
    EXPECT(readFile(top / "Singles" / "a.txt") == "hi" && !fs::exists(top / "Singles" / "b.txt"));
    provider->supportsMove = false;
    EXPECT(backend.rename(root.child("Singles").child("a.txt"), root.child("a.txt"), false).error().code == EXDEV);
    EXPECT(backend.rename(root.child("Singles"), Location::document("Other", "/Singles"), false).error().code == EXDEV);
    provider->supportsMove = true;
    // A folder moved keeps what's in it, and what was cached below its old path is forgotten.
    EXPECT(backend.rename(root.child("Albums"), root.child("Singles").child("Albums"), false));
    EXPECT(backend.stat(root.child("Albums").child("notes.txt")).error().code == ENOENT);
    EXPECT(read(backend, root.child("Singles").child("Albums").child("notes.txt")) == "liner notes");

    // Removing: not a non-empty folder without recursion, never the root, everything with it.
    EXPECT(backend.remove(root.child("Singles"), false, nullptr, {}).error().code == ENOTEMPTY);
    EXPECT(backend.remove(root, true, nullptr, {}).error().code == EBUSY);
    auto removed = backend.remove(root.child("Singles"), true, nullptr, {});
    EXPECT(removed && removed->removedCount == 5 && !fs::exists(top / "Singles"));
    EXPECT((names(backend, root) == std::vector<std::string>{".hidden"}));
    // Changed behind its back: a listing brings the cache up to date.
    writeFile(top / "outside.txt", "o");
    EXPECT((names(backend, root) == std::vector<std::string>{".hidden", "outside.txt"}));

    // A tree dropped from the list is gone.
    backend.setTrees({});
    EXPECT(backend.stat(root.child(".hidden")).error().code == ENOENT);
    return 0;
}
