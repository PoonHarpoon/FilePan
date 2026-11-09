// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "filepan/media/byte_source.hpp"
#include "filepan/media/image_source.hpp"
#include "filepan/vfs/backend.hpp"

// The viewer's reads of files on a server.
//
//   RemoteByteSource  A media::ByteSource over a stream from Backend::openReadParallel(), opened
//                     on first use (from the reading thread, never the UI thread). It reads in
//                     512 KiB blocks and keeps them: in memory while a shared budget allows, then
//                     in an unlinked temp file, so seeking back never fetches again. After two
//                     misses in a row it reads the next 4 blocks ahead on a worker thread, while
//                     it's active. Readers waiting on a block another thread is fetching wait for
//                     it rather than fetching it twice.
//   RemoteMediaCache  The viewer's sources: one per item near the current one, each prefetched
//                     a few at a time, nearest first.
//
// A lost connection reads as "Lost the connection to the server".
namespace filepan::app {

namespace detail {
struct RemoteSourceState;
}

// How many bytes of blocks RemoteByteSources keep in memory between them. Past it, the farthest
// source (by RemoteByteSource::setDistance) moves its least recently used blocks to disk, or
// drops them without a spill directory.
class RemoteMemoryBudget {
public:
    explicit RemoteMemoryBudget(std::size_t bytes) : limit_(bytes) {}
    RemoteMemoryBudget(const RemoteMemoryBudget &) = delete;
    RemoteMemoryBudget &operator=(const RemoteMemoryBudget &) = delete;

    [[nodiscard]] std::size_t limit() const { return limit_; }
    [[nodiscard]] std::size_t used() const { return used_.load(); }

private:
    friend struct detail::RemoteSourceState;
    friend class RemoteByteSource;
    void attach(const std::shared_ptr<detail::RemoteSourceState> &source);
    void add(std::size_t bytes) { used_ += bytes; }
    void release(std::size_t bytes) { used_ -= bytes; }
    // Evicts until used() is within the limit (or nothing more can go).
    void enforce();

    const std::size_t limit_;
    std::atomic<std::size_t> used_{0};
    std::mutex mutex_;
    std::mutex enforcing_;
    std::vector<std::weak_ptr<detail::RemoteSourceState>> sources_;
};

struct RemoteByteSourceOptions {
    // Where evicted blocks go, in a file unlinked at once; empty drops them instead.
    std::string spillDirectory;
    // Shared with other sources; null gives this one 16 MB of its own.
    std::shared_ptr<RemoteMemoryBudget> budget;
};

// A part of a file to prefetch: length bytes from offset, or the last length bytes.
struct ByteRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    bool fromEnd = false;
};

class RemoteByteSource final : public media::ByteSource, public std::enable_shared_from_this<RemoteByteSource> {
public:
    static constexpr std::size_t kBlockSize = 512 << 10;
    // Blocks read ahead once reads go in sequence.
    static constexpr std::size_t kReadAhead = 4;

    // backends must outlive the source, and so must the backend serving location.
    static std::shared_ptr<RemoteByteSource> create(vfs::BackendResolver &backends, vfs::Location location,
                                                    RemoteByteSourceOptions options = {});
    // Cancels; a fetch under way finishes on its worker in the background.
    ~RemoteByteSource() override;
    RemoteByteSource(const RemoteByteSource &) = delete;
    RemoteByteSource &operator=(const RemoteByteSource &) = delete;

    media::MediaResult<std::uint64_t> size() override;
    media::MediaResult<std::size_t> readAt(std::uint64_t offset, std::span<std::uint8_t> buffer) override;
    // Permanent: blocked and later reads fail, and the worker stops.
    void cancel() override;
    // Inactive, it reads nothing ahead (a paused video, a page not shown); prefetches go on.
    void setActive(bool active) override;
    [[nodiscard]] bool waiting() const override;

    // A source for one player: its cancel() fails only its own reads, so the blocks stay for the
    // next player of the same file. It keeps this source alive.
    std::shared_ptr<media::ByteSource> view();

    // Fetches ranges on the worker, after any read-ahead; done(ok) runs there once they're all
    // in (true), or on the first error, cancel() or stopPrefetch() (false). Blocks already in
    // are skipped. Once the whole file is in, the stream is closed.
    void prefetch(std::vector<ByteRange> ranges, std::function<void(bool ok)> done = {});
    // Drops queued prefetches (their done(false) runs); the block being fetched finishes.
    void stopPrefetch();
    // How far it is from what's shown, for the budget: the farthest gives up memory first.
    void setDistance(std::size_t distance);

    // For tests: blocks in memory and on disk, and whether the stream is open.
    [[nodiscard]] std::size_t blocksInMemory() const;
    [[nodiscard]] std::size_t blocksOnDisk() const;
    [[nodiscard]] bool streamOpen() const;

private:
    class View;
    explicit RemoteByteSource(std::shared_ptr<detail::RemoteSourceState> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::RemoteSourceState> state_;
};

// The viewer's spill directory, under the app's cache directory.
inline constexpr const char *kRemoteSpillDir = "/viewer-remote";

// Removes what a run that was killed left in a spill directory.
void clearRemoteSpill(const std::string &directory);

struct RemoteMediaItem {
    // A local one gets no source.
    vfs::Location location;
    bool video = false;
    // From the listing.
    std::uint64_t size = 0;
};

struct RemoteMediaCacheOptions {
    // Items prefetched each side of the current one.
    std::size_t around = 2;
    // How much of a video's start and end to prefetch; with no head, videos aren't prefetched.
    std::uint64_t videoHeadBytes = 8ull << 20;
    std::uint64_t videoTailBytes = 1ull << 20;
    // Larger images aren't prefetched (nor shown).
    std::uint64_t maxImageBytes = media::kMaxStreamedImageBytes;
    // Prefetches running at once, each on its own source and so its own pooled connection.
    std::size_t concurrency = 3;
    std::size_t memoryBytes = 64ull << 20;
    std::string spillDirectory;
};

namespace detail {
struct RemoteCacheState;
}

// The viewer's sources: one RemoteByteSource per item in [index - around, index + around],
// prefetched (images whole, the start and end of videos) in the order index, index + 1,
// index - 1, index + 2... with at most concurrency at once. Moving the index drops the sources
// more than around + 1 away, so a step back and forth fetches nothing again; the farthest give
// up memory first. Call it on one thread (the UI thread).
class RemoteMediaCache {
public:
    RemoteMediaCache(vfs::BackendResolver &backends, std::vector<RemoteMediaItem> items,
                     RemoteMediaCacheOptions options);
    // Stops prefetching; sources players still hold carry on.
    ~RemoteMediaCache();
    RemoteMediaCache(const RemoteMediaCache &) = delete;
    RemoteMediaCache &operator=(const RemoteMediaCache &) = delete;

    void setIndex(std::size_t index);
    // Paused (mobile data mode on a metered network), only the current item is fetched ahead, and
    // only an image: neighbours' prefetches stop, and start again when it's resumed.
    void setPaused(bool paused);
    // A view of items[index]'s source for a player, made now if there's none; null for a local
    // item.
    std::shared_ptr<media::ByteSource> open(std::size_t index);

    // For tests: items[index]'s source if it's kept, the items in the order their prefetches
    // started, how many run now, and whether items[index]'s is done.
    [[nodiscard]] std::shared_ptr<RemoteByteSource> source(std::size_t index) const;
    [[nodiscard]] std::vector<std::size_t> started() const;
    [[nodiscard]] std::size_t running() const;
    [[nodiscard]] bool prefetched(std::size_t index) const;
    [[nodiscard]] const RemoteMemoryBudget &budget() const;

private:
    std::shared_ptr<detail::RemoteCacheState> state_;
};

}  // namespace filepan::app
