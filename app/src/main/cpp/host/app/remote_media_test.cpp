// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The viewer's remote reads (remote_media.hpp) against an in-memory server: the block cache,
// spilling to disk and reading back, read-ahead and pausing it, cancelling a blocked read, and
// errors; and the viewer's cache of them: its window, prefetch order and concurrency, and memory.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fstream>
#include <optional>
#include <random>

#include "app/fake_media_backend.hpp"
#include "filepan/app/remote_media.hpp"
#include "filepan/vfs/sftp_backend.hpp"
#include "test_util.hpp"
#ifdef FILEPAN_SSHD
#include "sshd_harness.hpp"
#endif

using namespace filepan;
using namespace filepan::test;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBlock = app::RemoteByteSource::kBlockSize;

bool eventually(const std::function<bool()> &pred, int timeoutMs = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

std::vector<std::uint8_t> readRange(media::ByteSource &source, std::uint64_t offset, std::size_t length) {
    std::vector<std::uint8_t> out(length);
    auto read = source.readAt(offset, out);
    EXPECT(read.has_value());
    out.resize(read.value_or(0));
    return out;
}

std::vector<std::uint8_t> slice(const std::vector<std::uint8_t> &data, std::size_t offset, std::size_t length) {
    return {data.begin() + static_cast<std::ptrdiff_t>(offset),
            data.begin() + static_cast<std::ptrdiff_t>(std::min(data.size(), offset + length))};
}

struct Server {
    FakeMediaBackend backend;
    FakeMediaResolver resolver{backend};

    Server() {
        backend.add("/a.mp4", 6 * kBlock + 1000);
        backend.add("/b.jpg", 3 * kBlock, 3);
    }
    std::shared_ptr<app::RemoteByteSource> open(const std::string &path, app::RemoteByteSourceOptions options = {}) {
        return app::RemoteByteSource::create(resolver, vfs::Location::sftp("nas", path), std::move(options));
    }
};

void testHitsAndMisses() {
    Server server;
    const auto data = server.backend.file("/a.mp4");
    auto source = server.open("/a.mp4");
    source->setActive(false);  // No read-ahead here.
    // Nothing is opened until it's read.
    EXPECT(server.backend.parallelOpens() == 0 && !source->streamOpen());
    EXPECT(source->size() && *source->size() == data.size());
    EXPECT(server.backend.parallelOpens() == 1 && source->streamOpen());

    // A read within one block fetches it; reading it again doesn't.
    EXPECT(readRange(*source, 100, 1000) == slice(data, 100, 1000));
    const int reads = server.backend.reads();
    EXPECT(reads >= 1 && source->blocksInMemory() == 1);
    EXPECT(readRange(*source, 5000, 20) == slice(data, 5000, 20));
    EXPECT(server.backend.reads() == reads);
    // Across a block boundary: the next one too. Far away: just that one.
    EXPECT(readRange(*source, kBlock - 10, 20) == slice(data, kBlock - 10, 20));
    EXPECT(source->blocksInMemory() == 2);
    EXPECT(readRange(*source, 4 * kBlock + 7, 9) == slice(data, 4 * kBlock + 7, 9));
    EXPECT(source->blocksInMemory() == 3);
    // Past the end: short, then nothing.
    EXPECT(readRange(*source, data.size() - 10, 100) == slice(data, data.size() - 10, 10));
    EXPECT(readRange(*source, data.size(), 100).empty());
    EXPECT(server.backend.parallelOpens() == 1);
}

void testSpillAndReadBack() {
    Server server;
    TempDir dir;
    const std::string spill = (dir.path() / "viewer-remote").string();
    const auto data = server.backend.file("/a.mp4");
    // Two blocks in memory; the rest go to disk.
    auto budget = std::make_shared<app::RemoteMemoryBudget>(2 * kBlock);
    auto source = server.open("/a.mp4", {.spillDirectory = spill, .budget = budget});
    source->setActive(false);  // No read-ahead here.
    EXPECT(readRange(*source, 0, data.size()) == data);
    EXPECT(source->blocksInMemory() <= 2 && source->blocksOnDisk() >= 5);
    EXPECT(budget->used() <= budget->limit());
    // The spill file is unlinked: nothing to see in its directory.
    EXPECT(fs::exists(spill) && fs::is_empty(spill));
    // Reading it all again comes from memory and disk.
    const int reads = server.backend.reads();
    EXPECT(readRange(*source, 0, data.size()) == data);
    EXPECT(readRange(*source, 3 * kBlock - 5, 10) == slice(data, 3 * kBlock - 5, 10));
    EXPECT(server.backend.reads() == reads);

    // Without a spill directory, evicted blocks are fetched again.
    auto dropping = server.open("/a.mp4", {.budget = std::make_shared<app::RemoteMemoryBudget>(kBlock)});
    dropping->setActive(false);
    EXPECT(readRange(*dropping, 0, 3 * kBlock) == slice(data, 0, 3 * kBlock));
    EXPECT(dropping->blocksInMemory() == 1 && dropping->blocksOnDisk() == 0);
    const int before = server.backend.reads();
    EXPECT(readRange(*dropping, 0, 10) == slice(data, 0, 10));
    EXPECT(server.backend.reads() > before);

    // A run that was killed leaves files there; they're cleared at the next start.
    std::fclose(std::fopen((fs::path(spill) / "left").c_str(), "w"));
    app::clearRemoteSpill(spill);
    EXPECT(fs::is_empty(spill));
}

void testReadAhead() {
    Server server;
    const auto data = server.backend.file("/a.mp4");
    auto source = server.open("/a.mp4");
    // One miss isn't a sequence.
    EXPECT(readRange(*source, 0, 100) == slice(data, 0, 100));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT(source->blocksInMemory() == 1);
    // The next block missed too: the 4 after it come in by themselves.
    EXPECT(readRange(*source, kBlock, 100) == slice(data, kBlock, 100));
    EXPECT(eventually([&] { return source->blocksInMemory() == 6; }));
    // Reading on keeps it ahead, up to the end.
    EXPECT(readRange(*source, 2 * kBlock, 100) == slice(data, 2 * kBlock, 100));
    EXPECT(eventually([&] { return source->blocksInMemory() == 7; }));
    const int reads = server.backend.reads();
    EXPECT(readRange(*source, 0, data.size()) == data);
    EXPECT(server.backend.reads() == reads);

    // Inactive (paused, or not on screen), it reads only what's asked for.
    auto paused = server.open("/a.mp4");
    paused->setActive(false);
    EXPECT(readRange(*paused, 0, 100) == slice(data, 0, 100));
    EXPECT(readRange(*paused, kBlock, 100) == slice(data, kBlock, 100));
    EXPECT(readRange(*paused, 2 * kBlock, 100) == slice(data, 2 * kBlock, 100));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT(paused->blocksInMemory() == 3);
    // Active again, reading on brings it back.
    paused->setActive(true);
    EXPECT(readRange(*paused, 3 * kBlock, 100) == slice(data, 3 * kBlock, 100));
    EXPECT(eventually([&] { return paused->blocksInMemory() == 7; }));
}

void testCancel() {
    Server server;
    auto source = server.open("/a.mp4");
    EXPECT(source->size().has_value());
    server.backend.hold(true);
    auto first = source->view();
    auto second = source->view();
    // The first view's read is fetching at the gate; the second waits for that block.
    std::atomic<int> firstDone{0};
    std::atomic<int> secondDone{0};
    std::thread a([&] {
        std::vector<std::uint8_t> out(10);
        firstDone = first->readAt(0, out).has_value() ? 1 : -1;
    });
    EXPECT(eventually([&] { return server.backend.waiting() == 1; }));
    std::thread b([&] {
        std::vector<std::uint8_t> out(10);
        auto read = second->readAt(5, out);
        secondDone = read ? 1 : (read.error().message == "Cancelled" ? -1 : -2);
    });
    EXPECT(eventually([&] { return second->waiting() && first->waiting() && source->waiting(); }));
    // Cancelling the second view lets it go at once, while the fetch is still held.
    second->cancel();
    EXPECT(eventually([&] { return secondDone != 0; }));
    EXPECT(secondDone == -1 && firstDone == 0);
    b.join();
    // Cancelling a view fails only its own reads.
    std::vector<std::uint8_t> out(10);
    EXPECT(!second->readAt(0, out));
    server.backend.hold(false);
    a.join();
    EXPECT(firstDone == 1 && !first->waiting() && !source->waiting());
    EXPECT(first->readAt(0, out).has_value());

    // Cancelling the source fails a read waiting on a held fetch, and every read after.
    auto other = server.open("/b.jpg");
    EXPECT(other->size().has_value());
    server.backend.hold(true);
    std::atomic<int> fetching{0};
    std::atomic<int> waiting{0};
    std::thread c([&] {
        std::vector<std::uint8_t> buffer(10);
        fetching = other->readAt(0, buffer) ? 1 : -1;
    });
    EXPECT(eventually([&] { return server.backend.waiting() == 1; }));
    std::thread d([&] {
        std::vector<std::uint8_t> buffer(10);
        waiting = other->readAt(0, buffer) ? 1 : -1;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    other->cancel();
    EXPECT(eventually([&] { return waiting != 0; }) && waiting == -1);
    d.join();
    EXPECT(!other->readAt(0, out) && !other->size());
    server.backend.hold(false);
    c.join();
    // Prefetches asked for after that fail at once.
    bool ok = true;
    other->prefetch({{.offset = 0, .length = 10}}, [&](bool done) { ok = done; });
    EXPECT(!ok);
}

void testErrors() {
    Server server;
    std::vector<std::uint8_t> out(10);
    // A dropped connection.
    auto source = server.open("/a.mp4");
    EXPECT(source->size().has_value());
    server.backend.fail(ECONNRESET);
    auto read = source->readAt(0, out);
    EXPECT(!read && read.error().message == "Lost the connection to the server");
    // It reads again once the server's back.
    server.backend.fail(0);
    EXPECT(source->readAt(0, out).has_value());
    // A file that's gone.
    auto missing = server.open("/gone.jpg");
    auto size = missing->size();
    EXPECT(!size && size.error().message.starts_with("Couldn't read the file"));
    // A server that can't be reached at all.
    server.backend.fail(ECONNREFUSED);
    auto unreachable = server.open("/b.jpg");
    size = unreachable->size();
    EXPECT(!size && size.error().message.starts_with("Couldn't connect to the server"));
}

void testPrefetch() {
    Server server;
    const auto data = server.backend.file("/b.jpg");
    auto source = server.open("/b.jpg");
    source->setActive(false);
    // Fetched whole on the worker; then the stream closes.
    std::atomic<int> done{0};
    source->prefetch({{.offset = 0, .length = data.size()}}, [&](bool ok) { done = ok ? 1 : -1; });
    EXPECT(eventually([&] { return done != 0; }) && done == 1);
    EXPECT(source->blocksInMemory() == 3 && !source->streamOpen());
    const int reads = server.backend.reads();
    EXPECT(readRange(*source, 0, data.size()) == data);
    EXPECT(server.backend.reads() == reads && server.backend.parallelOpens() == 1);

    // A video's head and tail: blocks 0-1 and the last one.
    auto video = server.open("/a.mp4");
    video->setActive(false);
    done = 0;
    video->prefetch({{.offset = 0, .length = 2 * kBlock}, {.length = 1000, .fromEnd = true}},
                    [&](bool ok) { done = ok ? 1 : -1; });
    EXPECT(eventually([&] { return done != 0; }) && done == 1);
    EXPECT(video->blocksInMemory() == 3 && video->streamOpen());
    // A failed one says so.
    server.backend.fail(ECONNRESET);
    done = 0;
    video->prefetch({{.offset = 3 * kBlock, .length = kBlock}}, [&](bool ok) { done = ok ? 1 : -1; });
    EXPECT(eventually([&] { return done != 0; }) && done == -1);
    server.backend.fail(0);
    // stopPrefetch drops what's queued behind a held fetch.
    server.backend.hold(true);
    std::atomic<int> first{0};
    std::atomic<int> second{0};
    video->prefetch({{.offset = 3 * kBlock, .length = kBlock}}, [&](bool ok) { first = ok ? 1 : -1; });
    video->prefetch({{.offset = 4 * kBlock, .length = kBlock}}, [&](bool ok) { second = ok ? 1 : -1; });
    EXPECT(eventually([&] { return server.backend.waiting() == 1; }));
    video->stopPrefetch();
    EXPECT(first == -1 && second == -1);
    server.backend.hold(false);
    EXPECT(eventually([&] { return video->blocksInMemory() == 4; }));
}

// Ten items: a local one, then images of 2 blocks, but 5 and 9, videos of 20.
struct Folder {
    FakeMediaBackend backend;
    FakeMediaResolver resolver{backend};
    std::vector<app::RemoteMediaItem> items;

    Folder() {
        items.push_back({vfs::Location::local("/sdcard/x.jpg"), false, 10});
        for (std::size_t i = 1; i < 10; ++i) {
            const bool video = i == 5 || i == 9;
            const std::string path = "/m/" + std::to_string(i) + (video ? ".mp4" : ".jpg");
            const std::size_t size = video ? 20 * kBlock : 2 * kBlock;
            backend.add(path, size, static_cast<std::uint8_t>(i));
            items.push_back({vfs::Location::sftp("nas", path), video, size});
        }
    }
};

void testCacheWindow() {
    Folder folder;
    TempDir dir;
    // Room in memory for 5 blocks.
    app::RemoteMediaCache cache(folder.resolver, folder.items,
                                {.around = 2, .videoHeadBytes = kBlock, .videoTailBytes = kBlock, .concurrency = 2,
                                 .memoryBytes = 5 * kBlock, .spillDirectory = dir.path().string()});
    // A local item has no source.
    EXPECT(cache.open(0) == nullptr);

    // Held at the server: two start, the current one and the next.
    folder.backend.hold(true);
    cache.setIndex(4);
    EXPECT(cache.started() == (std::vector<std::size_t>{4, 5}) && cache.running() == 2);
    EXPECT(eventually([&] { return folder.backend.waiting() == 2; }));
    folder.backend.hold(false);
    // Then the rest of the window, nearest first, never more than two at once.
    EXPECT(eventually([&] { return cache.started().size() == 5 && cache.running() == 0; }));
    EXPECT(cache.started() == (std::vector<std::size_t>{4, 5, 3, 6, 2}));
    EXPECT(folder.backend.mostRunning() <= 2);
    EXPECT(folder.backend.parallelOpens() == 5);
    for (std::size_t i = 2; i <= 6; ++i) {
        EXPECT(cache.prefetched(i));
    }
    // Images whole; a video's first and last blocks.
    EXPECT(cache.source(5)->blocksInMemory() + cache.source(5)->blocksOnDisk() == 2);
    // Over the budget, the farthest went to disk; what's shown stays in memory.
    EXPECT(cache.budget().used() <= cache.budget().limit());
    EXPECT(cache.source(4)->blocksInMemory() == 2);
    EXPECT(cache.source(2)->blocksInMemory() == 0 && cache.source(2)->blocksOnDisk() == 2);
    // Fetched images closed their streams.
    EXPECT(!cache.source(4)->streamOpen());

    // A step on: only the one coming into the window is opened and fetched; the one now 3
    // away is kept.
    const auto kept = cache.source(5);
    cache.setIndex(5);
    EXPECT(eventually([&] { return cache.started().size() == 6 && cache.running() == 0; }));
    EXPECT(cache.started().back() == 7 && folder.backend.parallelOpens() == 6);
    EXPECT(cache.source(5) == kept && cache.source(2) != nullptr);
    // Reading what's been fetched needs no stream.
    const int reads = folder.backend.reads();
    auto view = cache.open(4);
    std::vector<std::uint8_t> data(2 * kBlock);
    EXPECT(view->readAt(0, data) && data == folder.backend.file("/m/4.jpg"));
    EXPECT(folder.backend.reads() == reads && folder.backend.parallelOpens() == 6);
    // And back: nothing new at all.
    cache.setIndex(4);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT(cache.started().size() == 6 && folder.backend.parallelOpens() == 6);
    // Two steps on, those more than 3 away go.
    cache.setIndex(6);
    EXPECT(cache.source(2) == nullptr && cache.source(3) != nullptr);
    EXPECT(eventually([&] { return cache.started().size() == 7 && cache.running() == 0; }));
    EXPECT(cache.started().back() == 8);
    // Its view outlives the cache.
    view.reset();
    EXPECT(eventually([&] { return cache.budget().used() <= cache.budget().limit(); }));
}

void testCacheSettings() {
    Folder folder;
    // Nothing each side, no video: just the current image.
    {
        app::RemoteMediaCache cache(folder.resolver, folder.items, {.around = 0, .videoHeadBytes = 0});
        cache.setIndex(5);
        cache.setIndex(4);
        EXPECT(eventually([&] { return cache.running() == 0 && cache.prefetched(4); }));
        EXPECT(cache.started() == (std::vector<std::size_t>{4}));
        // The video got a source when it was current, but nothing was fetched.
        EXPECT(cache.source(5) != nullptr && cache.source(5)->blocksInMemory() == 0);
    }
    // Images over the largest shown aren't fetched.
    app::RemoteMediaCache cache(folder.resolver, folder.items, {.around = 1, .maxImageBytes = kBlock});
    cache.setIndex(2);
    EXPECT(eventually([&] { return cache.running() == 0; }));
    EXPECT(cache.started().empty() && cache.prefetched(2));
    // Destroyed while prefetching, at the gate.
    folder.backend.hold(true);
    auto busy = std::make_unique<app::RemoteMediaCache>(folder.resolver, folder.items, app::RemoteMediaCacheOptions{});
    busy->setIndex(3);
    EXPECT(eventually([&] { return folder.backend.waiting() > 0; }));
    busy.reset();
    folder.backend.hold(false);
    EXPECT(eventually([&] { return folder.backend.waiting() == 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// Mobile data mode: paused, only the current image is fetched ahead; neighbours' prefetches stop
// and start over once it's resumed.
void testCachePaused() {
    Folder folder;
    app::RemoteMediaCache cache(folder.resolver, folder.items,
                                {.around = 2, .videoHeadBytes = kBlock, .videoTailBytes = kBlock, .concurrency = 2});
    cache.setPaused(true);
    cache.setIndex(4);
    EXPECT(eventually([&] { return cache.running() == 0 && cache.prefetched(4); }));
    EXPECT(cache.started() == (std::vector<std::size_t>{4}));
    cache.setPaused(false);
    EXPECT(eventually([&] { return cache.started().size() == 5 && cache.running() == 0; }));
    // Paused while the next is held at the server: it stops, the current one finishes.
    folder.backend.hold(true);
    cache.setIndex(7);
    EXPECT(eventually([&] { return folder.backend.waiting() == 2; }));
    EXPECT(cache.started().back() == 8);
    cache.setPaused(true);
    folder.backend.hold(false);
    EXPECT(eventually([&] { return cache.running() == 0 && cache.prefetched(7); }));
    EXPECT(!cache.prefetched(8) && !cache.prefetched(9));
    // Resumed: the rest of the window.
    cache.setPaused(false);
    EXPECT(eventually([&] { return cache.running() == 0 && cache.prefetched(8) && cache.prefetched(9); }));
}

#ifdef FILEPAN_SSHD
// Accepts the host key for the session; asks for nothing else.
class AcceptHostKey final : public vfs::SftpPromptProvider {
public:
    void requestHostKey(vfs::HostKeyPrompt, DecisionPromise<vfs::HostKeyDecision> promise) override {
        std::thread([promise] { promise.fulfil(vfs::HostKeyDecision::kAcceptOnce); }).detach();
    }
    void requestCredential(vfs::CredentialPrompt, DecisionPromise<std::optional<std::string>> promise) override {
        promise.fulfil(std::nullopt);
    }
};

// Over a real sshd: reads at random offsets and sizes, from two players at once, match the file.
void testSftp() {
    TempDir dir;
    Sshd sshd(dir.path());
    auto sftp = std::make_shared<vfs::SftpBackend>(std::make_shared<AcceptHostKey>());
    vfs::SftpServerConfig config;
    config.id = "test";
    config.server.host = "127.0.0.1";
    config.server.port = sshd.port();
    config.server.username = getenv("USER") ? getenv("USER") : "root";
    config.server.authentication.type = vfs::SftpAuthentication::Type::kPrivateKey;
    config.server.authentication.privateKey = read(dir.path() / "client_key");
    config.timeoutSeconds = 10;
    sftp->setServer(config);
    vfs::SimpleBackendResolver backends;
    backends.set(vfs::BackendKind::kSftp, sftp);

    std::vector<std::uint8_t> data(5 * kBlock + 12345);
    std::mt19937 random(7);
    for (auto &byte : data) {
        byte = static_cast<std::uint8_t>(random());
    }
    const fs::path file = dir.path() / "clip.mp4";
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char *>(data.data()),
                                                static_cast<std::streamsize>(data.size()));

    TempDir spill;
    auto source = app::RemoteByteSource::create(
            backends, vfs::Location::sftp("test", file.string()),
            {.spillDirectory = spill.path().string(), .budget = std::make_shared<app::RemoteMemoryBudget>(2 * kBlock)});
    EXPECT(source->size() && *source->size() == data.size());
    // No read-ahead, so that no fetch outlives the backend at the end.
    source->setActive(false);
    std::atomic<int> mismatches{0};
    const auto player = [&](unsigned seed) {
        auto view = source->view();
        std::mt19937 rng(seed);
        for (int i = 0; i < 60; ++i) {
            const std::uint64_t offset = rng() % data.size();
            const std::size_t length = 1 + rng() % (kBlock + kBlock / 2);
            if (readRange(*view, offset, length) != slice(data, offset, length)) {
                ++mismatches;
            }
        }
    };
    std::thread a(player, 1);
    std::thread b(player, 2);
    a.join();
    b.join();
    EXPECT(mismatches == 0);
    EXPECT(source->blocksInMemory() <= 2 && source->blocksOnDisk() > 0);
    source.reset();
}
#endif

}  // namespace

int main() {
    testHitsAndMisses();
    testSpillAndReadBack();
    testReadAhead();
    testCancel();
    testErrors();
    testPrefetch();
    testCacheWindow();
    testCacheSettings();
    testCachePaused();
#ifdef FILEPAN_SSHD
    testSftp();
#endif
    std::printf("remote_media_test passed\n");
    return 0;
}
