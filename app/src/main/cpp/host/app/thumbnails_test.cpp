// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The thumbnail cache: its disk tier (the file format, hits after a restart, invalidation by key,
// LRU and decay eviction, corrupt files) and remote thumbnails (temporary files only, the memory
// budget, the size limit, cancellation), with fake decoders (the host has no image decoder) and a
// fake backend, on a temporary directory.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "filepan/app/thumbnail_store.hpp"
#include "filepan/app/thumbnails.hpp"
#include "filepan/error.hpp"
#include "filepan/vfs/backend.hpp"
#include "test_util.hpp"

using namespace filepan;
using namespace filepan::test;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::int64_t kDay = 86400 * kSecond;

std::int64_t realNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

// Noise, so zstd can't shrink it much and file sizes are predictable.
app::DecodedImage noise(int side, unsigned seed) {
    std::mt19937 random(seed);
    app::DecodedImage image{side, side, std::vector<std::uint8_t>(static_cast<std::size_t>(side * side * 4))};
    for (auto &byte : image.rgba) {
        byte = static_cast<std::uint8_t>(random());
    }
    return image;
}

std::size_t fileCount(const fs::path &dir) {
    if (!fs::exists(dir)) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(fs::directory_iterator(dir), fs::directory_iterator()));
}

template <typename Condition>
bool pumpUntil(ui::UiLoop &loop, Condition condition) {
    for (int i = 0; i < 1000; ++i) {
        loop.runPending();
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void testFormat() {
    const app::DecodedImage image = noise(7, 1);
    const std::vector<std::uint8_t> bytes = app::ThumbnailStore::encode("key|1|2", image);
    EXPECT(bytes.size() > 24 && bytes[0] == 'F' && bytes[1] == 'P' && bytes[2] == 'T' && bytes[3] == 'N');
    bool corrupt = true;
    auto decoded = app::ThumbnailStore::decode(bytes, "key|1|2", &corrupt);
    EXPECT(decoded && !corrupt && decoded->width == 7 && decoded->height == 7 && decoded->rgba == image.rgba);
    // Another key with the same file name: a miss, but not corrupt.
    EXPECT(!app::ThumbnailStore::decode(bytes, "other", &corrupt) && !corrupt);
    // Truncated, or a flipped payload byte (the zstd checksum catches it).
    std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - 3);
    EXPECT(!app::ThumbnailStore::decode(truncated, "key|1|2", &corrupt) && corrupt);
    std::vector<std::uint8_t> flipped = bytes;
    flipped[flipped.size() - 6] ^= 0x5A;
    EXPECT(!app::ThumbnailStore::decode(flipped, "key|1|2", &corrupt) && corrupt);
    // Images that don't add up aren't written.
    EXPECT(app::ThumbnailStore::encode("k", {2, 2, std::vector<std::uint8_t>(15)}).empty());
    // The hash is stable: file names survive a rebuild.
    EXPECT(app::stableHash("") == 0xcbf29ce484222325ull && app::stableHash("a") == 0xaf63dc4c8601ec8cull);
}

void testRestartHitAndInvalidation() {
    TempDir dir;
    const std::string disk = (dir.path() / "thumbnails").string();
    std::atomic<int> decoded{0};
    const app::ImageDecoder decoder = [&](const std::string &path, int) -> std::optional<app::DecodedImage> {
        ++decoded;
        return noise(8, static_cast<unsigned>(path.size()));
    };
    const app::ThumbnailCacheOptions options{.diskDirectory = disk,
                                             .diskLimits = {.maxBytes = 1 << 20, .maxAgeSeconds = 3 * 86400}};
    ui::UiLoop loop;
    ui::IoPool io(loop, 2);
    gfx::vulkan::ImageStore images;
    {
        app::ThumbnailCache cache(io, images, decoder, options);
        int done = 0;
        cache.request("/a.jpg|100|5", "/a.jpg", 8, app::ThumbnailPersistence::disk, [&](gfx::ImageId) { ++done; });
        EXPECT(pumpUntil(loop, [&] { return done == 1; }));
        EXPECT(decoded == 1 && fileCount(disk) == 1);
        EXPECT(pumpUntil(loop, [&] { return cache.diskUsage.peek() > 0; }));
    }
    {
        // A new cache (the app restarted) finds it on disk without decoding.
        app::ThumbnailCache cache(io, images, decoder, options);
        int done = 0;
        cache.request("/a.jpg|100|5", "/a.jpg", 8, app::ThumbnailPersistence::disk, [&](gfx::ImageId) { ++done; });
        EXPECT(pumpUntil(loop, [&] { return done == 1; }));
        EXPECT(decoded == 1 && cache.get("/a.jpg|100|5") != 0);
        // The file changed (its mtime is in the key): decoded again, and stored beside the old one
        // until a sweep ages it out.
        cache.request("/a.jpg|101|5", "/a.jpg", 8, app::ThumbnailPersistence::disk, [&](gfx::ImageId) { ++done; });
        EXPECT(pumpUntil(loop, [&] { return done == 2; }));
        EXPECT(decoded == 2 && fileCount(disk) == 2);
        // Memory only: never read from or written to the disk.
        cache.request("/b.jpg|1|1", "/b.jpg", 8, app::ThumbnailPersistence::memoryOnly,
                      [&](gfx::ImageId) { ++done; });
        EXPECT(pumpUntil(loop, [&] { return done == 3; }));
        EXPECT(decoded == 3 && fileCount(disk) == 2);
        // Off: the disk is neither read nor written, and kept.
        cache.setEnabled(false);
        cache.request("/c.jpg|1|1", "/c.jpg", 8, app::ThumbnailPersistence::disk, [&](gfx::ImageId) { ++done; });
        for (int i = 0; i < 20; ++i) {
            loop.runPending();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        EXPECT(done == 3 && decoded == 3 && fileCount(disk) == 2);
        // Clearing empties it.
        cache.setEnabled(true);
        bool cleared = false;
        cache.clearDisk([&] { cleared = true; });
        EXPECT(pumpUntil(loop, [&] { return cleared; }));
        EXPECT(fileCount(disk) == 0 && cache.diskUsage.peek() == 0);
    }
}

void testLruEviction() {
    TempDir dir;
    const std::string disk = (dir.path() / "t").string();
    std::int64_t now = realNow();
    app::ThumbnailStore store(disk, {}, [&] { return now; });
    for (int i = 0; i < 5; ++i) {
        now += kSecond;
        EXPECT(store.write("k" + std::to_string(i), noise(32, static_cast<unsigned>(i))));
    }
    const std::uint64_t one = fs::file_size(store.pathFor("k0"));
    EXPECT(store.usage() >= 5 * one - 5 * 64 && store.usage() <= 5 * one + 5 * 64);
    // k0 is read: it's now the most recently used.
    now += kSecond;
    EXPECT(store.read("k0"));
    // Room for three: k1 and k2, the least recently accessed, go.
    store.setLimits({.maxBytes = 3 * one + one / 2});
    EXPECT(store.overLimit());
    store.sweep();
    EXPECT(fs::exists(store.pathFor("k0")) && !fs::exists(store.pathFor("k1")) && !fs::exists(store.pathFor("k2")));
    EXPECT(fs::exists(store.pathFor("k3")) && fs::exists(store.pathFor("k4")));
    EXPECT(store.usage() <= 3 * one + one / 2 && !store.overLimit());

    // Through the cache: writes past the cap start one sweep, which brings it back under.
    ui::UiLoop loop;
    ui::IoPool io(loop, 3);
    gfx::vulkan::ImageStore images;
    TempDir other;
    const std::string cacheDisk = (other.path() / "t").string();
    app::ThumbnailCache cache(io, images,
                              [](const std::string &path, int) -> std::optional<app::DecodedImage> {
                                  return noise(32, static_cast<unsigned>(path.size() * 31 + path.back()));
                              },
                              app::ThumbnailCacheOptions{.diskDirectory = cacheDisk,
                                                         .diskLimits = {.maxBytes = 4 * one}});
    int done = 0;
    for (int i = 0; i < 12; ++i) {
        const std::string key = "/p" + std::to_string(i);
        cache.request(key, key, 32, app::ThumbnailPersistence::disk, [&](gfx::ImageId) { ++done; });
    }
    EXPECT(pumpUntil(loop, [&] { return done == 12; }));
    EXPECT(pumpUntil(loop, [&] {
        return cache.diskStore()->usage() <= 4 * one && cache.diskUsage.peek() == cache.diskStore()->usage();
    }));
    EXPECT(fileCount(cacheDisk) <= 4);
}

void testDecay() {
    TempDir dir;
    const std::string disk = (dir.path() / "t").string();
    std::int64_t now = realNow() - 200 * kDay;
    app::ThumbnailStore store(disk, {.maxAgeSeconds = 3 * 86400}, [&] { return now; });
    EXPECT(store.write("old", noise(4, 1)) && store.write("viewed", noise(4, 2)));
    now += 2 * kDay;
    EXPECT(store.read("viewed"));
    now += 2 * kDay;  // "old" is 4 days unseen, "viewed" 2.
    store.sweep();
    EXPECT(!fs::exists(store.pathFor("old")) && fs::exists(store.pathFor("viewed")));
    // Never: nothing ages out.
    now += 300 * kDay;  // Past the real clock too, for the temporary file below.
    store.setLimits({.maxAgeSeconds = 0});
    store.sweep();
    EXPECT(fs::exists(store.pathFor("viewed")));
    // A temporary file from a write that never finished goes once it's stale.
    const fs::path stale = fs::path(disk) / "0123456789abcdef.fpt.tmp.1.1";
    std::ofstream(stale) << "x";
    store.sweep();
    EXPECT(!fs::exists(stale));

    // Through the cache: the startup sweep drops what decayed while it wasn't running.
    std::int64_t clock = now + 10 * kDay;
    ui::UiLoop loop;
    ui::IoPool io(loop, 1);
    gfx::vulkan::ImageStore images;
    app::ThumbnailCache cache(io, images, {},
                              app::ThumbnailCacheOptions{.diskDirectory = disk,
                                                         .diskLimits = {.maxAgeSeconds = 86400},
                                                         .clock = [&] { return clock; }});
    EXPECT(pumpUntil(loop, [&] { return !fs::exists(store.pathFor("viewed")); }));
}

void testCorruptFiles() {
    TempDir dir;
    const std::string disk = (dir.path() / "t").string();
    app::ThumbnailStore store(disk);
    EXPECT(store.write("a", noise(16, 3)));
    const std::string path = store.pathFor("a");
    // Truncated: a miss, and deleted.
    fs::resize_file(path, fs::file_size(path) - 10);
    EXPECT(!store.read("a") && !fs::exists(path));
    // Garbage: the same.
    std::ofstream(path) << "not a thumbnail at all, just some bytes";
    EXPECT(!store.read("a") && !fs::exists(path));
    // Empty: the same.
    std::ofstream(path).close();
    EXPECT(!store.read("a") && !fs::exists(path));
    // Another key's file under this name (a hash collision): a miss, but kept.
    EXPECT(store.write("b", noise(16, 4)));
    fs::copy_file(store.pathFor("b"), path);
    EXPECT(!store.read("a") && fs::exists(path));
    // Writing replaces it.
    EXPECT(store.write("a", noise(16, 5)) && store.read("a"));

    // Through the cache: a corrupt file is decoded again and rewritten.
    std::ofstream(path, std::ios::trunc) << "garbage";
    ui::UiLoop loop;
    ui::IoPool io(loop, 1);
    gfx::vulkan::ImageStore images;
    int decoded = 0;
    app::ThumbnailCache cache(io, images,
                              [&](const std::string &, int) -> std::optional<app::DecodedImage> {
                                  ++decoded;
                                  return noise(16, 6);
                              },
                              app::ThumbnailCacheOptions{.diskDirectory = disk});
    int done = 0;
    cache.request("a", "/a", 16, app::ThumbnailPersistence::disk, [&](gfx::ImageId) { ++done; });
    EXPECT(pumpUntil(loop, [&] { return done == 1; }));
    EXPECT(decoded == 1 && store.read("a") && store.read("a")->rgba == noise(16, 6).rgba);
}

// --- Remote thumbnails ---

// Serves files from memory for every location, a few KB per read, and can hold reads at a gate.
class FakeBackend final : public vfs::Backend {
public:
    std::map<std::string, std::vector<std::uint8_t>> files;  // By path.
    std::atomic<int> opens{0};
    std::map<std::string, int> reads;  // By path; guarded by mutex.
    std::mutex mutex;
    std::condition_variable condition;
    bool held = false;

    void hold(bool on) {
        {
            std::lock_guard lock(mutex);
            held = on;
        }
        condition.notify_all();
    }
    int readsOf(const std::string &path) {
        std::lock_guard lock(mutex);
        return reads[path];
    }

    [[nodiscard]] vfs::BackendKind kind() const override { return vfs::BackendKind::kSftp; }
    [[nodiscard]] vfs::Capabilities capabilities() const override { return {}; }
    Result<void> list(const vfs::Location &, const vfs::ListOptions &, const vfs::ListCallback &,
                      const CancellationToken &) override {
        return fail(ENOTSUP);
    }
    Result<vfs::Entry> stat(const vfs::Location &) override { return fail(ENOTSUP); }
    Result<vfs::Entry> lstat(const vfs::Location &) override { return fail(ENOTSUP); }
    Result<void> makeDirectory(const vfs::Location &, std::uint32_t) override { return fail(EROFS); }
    Result<void> createFile(const vfs::Location &, std::uint32_t) override { return fail(EROFS); }
    Result<void> rename(const vfs::Location &, const vfs::Location &, bool) override { return fail(EROFS); }
    Result<vfs::RemoveStats> remove(const vfs::Location &, bool, vfs::RemoveCallbacks *,
                                    const CancellationToken &) override {
        return fail(EROFS);
    }
    Result<std::unique_ptr<vfs::OutputStream>> openWrite(const vfs::Location &,
                                                         const vfs::OpenWriteOptions &) override {
        return fail(EROFS);
    }
    Result<std::unique_ptr<vfs::InputStream>> openRead(const vfs::Location &location) override;
};

class FakeStream final : public vfs::InputStream {
public:
    FakeStream(FakeBackend &backend, std::string path) : backend_(backend), path_(std::move(path)) {}

    Result<std::size_t> read(std::span<std::uint8_t> buffer) override {
        std::unique_lock lock(backend_.mutex);
        backend_.condition.wait(lock, [&] { return !backend_.held; });
        ++backend_.reads[path_];
        const std::vector<std::uint8_t> &data = backend_.files[path_];
        const std::size_t n = std::min({buffer.size(), data.size() - at_, std::size_t{4096}});
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(at_), n, buffer.begin());
        at_ += n;
        return n;
    }
    Result<std::uint64_t> seek(std::int64_t, vfs::SeekOrigin) override { return fail(ESPIPE); }
    Result<std::uint64_t> size() override { return backend_.files[path_].size(); }

private:
    FakeBackend &backend_;
    std::string path_;
    std::size_t at_ = 0;
};

Result<std::unique_ptr<vfs::InputStream>> FakeBackend::openRead(const vfs::Location &location) {
    ++opens;
    std::lock_guard lock(mutex);
    if (!files.contains(location.path())) {
        return fail(ENOENT);
    }
    return std::unique_ptr<vfs::InputStream>(std::make_unique<FakeStream>(*this, location.path()));
}

class FakeResolver final : public vfs::BackendResolver {
public:
    explicit FakeResolver(FakeBackend &backend) : backend_(backend) {}
    vfs::Backend *backendFor(const vfs::Location &) override { return &backend_; }

private:
    FakeBackend &backend_;
};

struct Remote {
    TempDir dir;
    std::string disk = (dir.path() / "thumbnails").string();
    std::string temp = (dir.path() / "thumbnails-remote").string();
    FakeBackend backend;
    FakeResolver resolver{backend};
    std::atomic<int> localDecodes{0};
    ui::UiLoop loop;
    ui::IoPool io{loop, 4};
    gfx::vulkan::ImageStore images;

    // withTemp false keeps remote thumbnails in memory only, so requests queue in the order made.
    std::unique_ptr<app::ThumbnailCache> make(std::size_t remoteCapacity = 4, bool withTemp = true) {
        return std::make_unique<app::ThumbnailCache>(
                io, images,
                [this](const std::string &, int) -> std::optional<app::DecodedImage> {
                    ++localDecodes;
                    return noise(4, 1);
                },
                app::ThumbnailCacheOptions{
                        .diskDirectory = disk,
                        .backends = &resolver,
                        // The "image" is its bytes' size: any bytes decode.
                        .bufferDecoder = [](std::span<const std::uint8_t> bytes,
                                            int) -> std::optional<app::DecodedImage> {
                            return noise(4, static_cast<unsigned>(bytes.size()));
                        },
                        .remoteDirectory = withTemp ? temp : std::string(),
                        .remoteCapacity = remoteCapacity,
                });
    }
    vfs::Location add(const std::string &path, std::size_t size) {
        std::lock_guard lock(backend.mutex);
        backend.files[path] = std::vector<std::uint8_t>(size, 7);
        return vfs::Location::sftp("nas", path);
    }
    // A request as the list makes one, counting completions in done.
    void request(app::ThumbnailCache &cache, const vfs::Location &location, int &done,
                 std::optional<std::uint64_t> size = std::nullopt) {
        vfs::Entry entry;
        entry.size = size ? *size : backend.files[location.path()].size();
        entry.modifiedTime.seconds = 1;
        cache.requestRemote(app::thumbnailKey(location, entry), location, entry.size, 16,
                            [&done](gfx::ImageId) { ++done; });
    }
    static std::string key(const vfs::Location &location, std::uint64_t size) {
        vfs::Entry entry;
        entry.size = size;
        entry.modifiedTime.seconds = 1;
        return app::thumbnailKey(location, entry);
    }
};

void testRemoteTempOnly() {
    Remote r;
    // Leftovers from a killed run are wiped at startup.
    fs::create_directories(r.temp);
    std::ofstream(fs::path(r.temp) / "0123456789abcdef-0123456789abcdef.fpt") << "old";
    auto cache = r.make(2);
    EXPECT(pumpUntil(r.loop, [&] { return fileCount(r.temp) == 0; }));
    cache->setRemote(true, 10 << 20);

    const vfs::Location a = r.add("/a.jpg", 5000);
    const vfs::Location b = r.add("/b.jpg", 6000);
    const vfs::Location c = r.add("/c.jpg", 7000);
    int done = 0;
    r.request(*cache, a, done);
    EXPECT(pumpUntil(r.loop, [&] { return done == 1; }));
    // In memory and a temporary file; never the persistent directory, never the path decoder.
    EXPECT(cache->get(Remote::key(a, 5000)) != 0 && fileCount(r.temp) == 1);
    EXPECT(fileCount(r.disk) == 0 && r.localDecodes == 0);
    EXPECT(app::thumbnailKey(a, {}).starts_with("sftp:"));

    // The memory budget: two of three stay; the third comes back from its file, not the server.
    r.request(*cache, b, done);
    r.request(*cache, c, done);
    EXPECT(pumpUntil(r.loop, [&] { return done == 3; }));
    EXPECT(cache->cachedRemote() == 2 && cache->get(Remote::key(a, 5000)) == 0 && fileCount(r.temp) == 3);
    const int opens = r.backend.opens;
    r.request(*cache, a, done);
    EXPECT(pumpUntil(r.loop, [&] { return done == 4; }));
    EXPECT(r.backend.opens == opens && cache->cachedRemote() == 2 && cache->get(Remote::key(a, 5000)) != 0);
    // Local thumbnails have their own budget: not pushed out by remote ones, nor the reverse.
    cache->request("/local|1|1", "/local", 16, app::ThumbnailPersistence::disk, [&](gfx::ImageId) { ++done; });
    EXPECT(pumpUntil(r.loop, [&] { return done == 5; }));
    EXPECT(cache->cachedRemote() == 2 && cache->get("/local|1|1") != 0 && fileCount(r.disk) == 1);

    // Going to the background drops the files, not the images.
    cache->dropRemoteTemp();
    EXPECT(pumpUntil(r.loop, [&] { return fileCount(r.temp) == 0; }));
    EXPECT(cache->cachedRemote() == 2);
    // Leaving the server drops both.
    r.request(*cache, r.add("/d.jpg", 100), done);
    EXPECT(pumpUntil(r.loop, [&] { return done == 6 && fileCount(r.temp) == 1; }));
    cache->keepRemoteGroups({"sftp:other"});
    EXPECT(cache->cachedRemote() == 0 && cache->get(Remote::key(b, 6000)) == 0);
    EXPECT(pumpUntil(r.loop, [&] { return fileCount(r.temp) == 0; }));
    // The local thumbnail stays.
    EXPECT(cache->get("/local|1|1") != 0 && fileCount(r.disk) == 1);
}

void testRemoteLimits() {
    Remote r;
    auto cache = r.make();
    cache->setRemote(true, 4000);
    // Over the limit by the listing's size: not fetched at all.
    const vfs::Location big = r.add("/big.jpg", 5000);
    int done = 0;
    r.request(*cache, big, done);
    EXPECT(cache->pending() == 0 && r.backend.opens == 0);
    // The listing said it was small, but it isn't: the read stops past the limit.
    r.request(*cache, big, done, 100);
    EXPECT(pumpUntil(r.loop, [&] { return cache->pending() == 0; }));
    EXPECT(done == 0 && r.backend.opens == 1 && r.backend.readsOf("/big.jpg") <= 2);
    // Within a raised limit it's fetched (not marked failed before).
    cache->setRemote(true, 10 << 20);
    r.request(*cache, big, done, 100);
    EXPECT(pumpUntil(r.loop, [&] { return done == 1; }));
    // Off: nothing is requested, and what's cached isn't shown.
    cache->setRemote(false, 10 << 20);
    EXPECT(cache->get(Remote::key(big, 100)) == 0);
    r.request(*cache, r.add("/small.jpg", 10), done);
    EXPECT(cache->pending() == 0);
    // Thumbnails off altogether: the same.
    cache->setRemote(true, 10 << 20);
    cache->setEnabled(false);
    r.request(*cache, r.add("/small.jpg", 10), done);
    EXPECT(cache->pending() == 0);
}

void testRemoteCancellation() {
    Remote r;
    auto cache = r.make(4, false);
    cache->setRemote(true, 10 << 20);
    const vfs::Location a = r.add("/a.jpg", 640 * 1024);
    const vfs::Location b = r.add("/b.jpg", 8000);
    const vfs::Location c = r.add("/c.jpg", 8000);
    r.backend.hold(true);
    int done = 0;
    r.request(*cache, a, done);
    r.request(*cache, b, done);
    r.request(*cache, c, done);
    // Two fetches at a time: the third waits.
    EXPECT(pumpUntil(r.loop, [&] { return r.backend.opens == 2; }));
    for (int i = 0; i < 20; ++i) {
        r.loop.runPending();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT(r.backend.opens == 2 && cache->pending() == 3);
    // a scrolls away mid-transfer, c before it starts; b stays on screen.
    cache->cancelExcept({Remote::key(b, 8000)});
    r.backend.hold(false);
    EXPECT(pumpUntil(r.loop, [&] { return done == 1; }));
    for (int i = 0; i < 50; ++i) {
        r.loop.runPending();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT(done == 1 && cache->get(Remote::key(b, 8000)) != 0 && cache->pending() == 0);
    // c was never opened, and a stopped after the read it was in (640 KB is 160 reads).
    EXPECT(r.backend.opens == 2 && r.backend.readsOf("/c.jpg") == 0 && r.backend.readsOf("/a.jpg") <= 2);
    // Cancelled isn't failed: asking again fetches it.
    r.request(*cache, c, done);
    EXPECT(pumpUntil(r.loop, [&] { return done == 2; }));
}

}  // namespace

int main() {
    testFormat();
    testRestartHitAndInvalidation();
    testLruEviction();
    testDecay();
    testCorruptFiles();
    testRemoteTempOnly();
    testRemoteLimits();
    testRemoteCancellation();
    std::puts("thumbnails_test: OK");
    return 0;
}
