// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "filepan/app/thumbnail_store.hpp"
#include "filepan/gfx/display_list.hpp"
#include "filepan/gfx/vulkan/image_store.hpp"
#include "filepan/ui/reactive.hpp"
#include "filepan/ui/ui_loop.hpp"
#include "filepan/vfs/backend.hpp"
#include "filepan/vfs/entry.hpp"
#include "filepan/vfs/location.hpp"

// Thumbnails: decoded on the I/O pool at the size they're shown, uploaded through the renderer's
// ImageStore, and kept in a small LRU. Rows ask for theirs when bound and the list cancels the
// requests of rows that scrolled away.
//
// Below the LRU is a disk tier (thumbnail_store.hpp), if the cache has a directory for it. Each
// request says whether it may use it: one that may reads the stored thumbnail on the I/O pool
// before decoding, and stores what it decodes. The disk tier is swept on the I/O pool when the
// cache starts, when its limits change, and when writes take it past its size cap.
//
// Remote files (requestRemote) never touch that tier. Their bytes are read through the vfs on the
// I/O pool, at most kRemoteThumbnailFetches at a time so thumbnails don't crowd out the listing
// and transfers on the same connection, and decoded from memory. They're kept in a small LRU of
// their own, backed by temporary files in a separate directory so scrolling back doesn't fetch
// again: wiped when the cache starts, when the app goes to the background (dropRemoteTemp), and
// per remote once no tab shows it (keepRemoteGroups), with a small size cap as a backstop.
namespace filepan::app {

// Decodes a local image file to a square-ish thumbnail whose shorter side is sizePx (cropped to
// the center). Runs on an I/O thread. nullopt when it can't (not an image, or no decoder).
using ImageDecoder = std::function<std::optional<DecodedImage>(const std::string &path, int sizePx)>;

// AImageDecoder on Android; on a host there is no decoder and this returns nullopt.
std::optional<DecodedImage> decodeThumbnail(const std::string &path, int sizePx);

// The same, from an encoded image in memory (a remote file's bytes).
using BufferDecoder = std::function<std::optional<DecodedImage>(std::span<const std::uint8_t> bytes, int sizePx)>;
std::optional<DecodedImage> decodeThumbnailBuffer(std::span<const std::uint8_t> bytes, int sizePx);

// Premultiplied RGBA8 to the non-premultiplied the ImageStore and DecodedImage take, in place.
// Fully transparent pixels keep their (zero) color.
void unpremultiply(std::vector<std::uint8_t> &rgba);

// Where a file's thumbnail comes from, if it has one: local images, videos and APKs (their app
// icon) are decoded from their path and kept on disk; images on an SFTP server are fetched and
// kept in memory and temporary files. Not remote videos or APKs (their decoders take local
// paths), nor anything in an archive, whose entries may want a password or a decompression from
// the start each.
enum class ThumbnailSource : std::uint8_t { none, local, remote };
ThumbnailSource thumbnailSource(const vfs::Location &folder, const vfs::Entry &entry);

// Whether a file kind's thumbnail is an icon with its own transparent edges, drawn whole (an
// APK's), rather than a picture.
bool isIconThumbnail(vfs::FileKind kind);

// Whether a request's thumbnail may be read from and written to the disk tier.
enum class ThumbnailPersistence : std::uint8_t {
    disk,
    memoryOnly,
};

// Remote thumbnails: GPU images kept, the temporary files' cap, and fetches at a time.
inline constexpr std::size_t kRemoteThumbnailsInMemory = 32;
inline constexpr std::uint64_t kRemoteThumbnailTempBytes = 64ull << 20;
inline constexpr int kRemoteThumbnailFetches = 2;

// A local thumbnail that failed is tried this many times in all, the next one no sooner than
// kThumbnailRetryMs later.
inline constexpr int kThumbnailAttempts = 2;
inline constexpr std::int64_t kThumbnailRetryMs = 30'000;

struct ThumbnailCacheOptions {
    // GPU images kept.
    std::size_t capacity = 256;
    // The disk tier's directory; empty has none.
    std::string diskDirectory;
    ThumbnailStoreLimits diskLimits;
    // The disk tier's notion of now (tests).
    ThumbnailStore::Clock clock;
    // Monotonic milliseconds, for when a failed thumbnail may be tried again (tests); null is the
    // steady clock.
    std::function<std::int64_t()> retryClock;

    // Remote thumbnails are read through these; null has none.
    vfs::BackendResolver *backends = nullptr;
    BufferDecoder bufferDecoder = decodeThumbnailBuffer;
    // Their temporary files' directory; empty keeps them in memory only.
    std::string remoteDirectory;
    std::size_t remoteCapacity = kRemoteThumbnailsInMemory;
    std::uint64_t remoteTempBytes = kRemoteThumbnailTempBytes;
};

// A thumbnail's cache key: the file's path (a local one) or full location, mtime and size, so a
// changed file gets a new one.
std::string thumbnailKey(const vfs::Location &location, const vfs::Entry &entry);

class ThumbnailCache {
public:
    ThumbnailCache(ui::IoPool &io, gfx::vulkan::ImageStore &images, ImageDecoder decoder = decodeThumbnail,
                   std::size_t capacity = 256);
    ThumbnailCache(ui::IoPool &io, gfx::vulkan::ImageStore &images, ImageDecoder decoder,
                   ThumbnailCacheOptions options);
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache &) = delete;
    ThumbnailCache &operator=(const ThumbnailCache &) = delete;

    // The cached image for key (and marks it recently used), or 0.
    gfx::ImageId get(const std::string &key);
    // Decodes path for key unless it's cached, pending, or failed before; then calls done with
    // the new image on the UI thread (not if cancelled first). With ThumbnailPersistence::disk
    // the disk tier is tried first, and given what's decoded.
    void request(const std::string &key, const std::string &path, int sizePx, ThumbnailPersistence persistence,
                 std::function<void(gfx::ImageId)> done);
    // The same for a file read through the vfs (an SFTP server): never the disk tier, only the
    // remote LRU and its temporary files. Files over the remote size limit are skipped; size is
    // the listing's, and the read stops past the limit too.
    void requestRemote(const std::string &key, const vfs::Location &location, std::uint64_t size, int sizePx,
                       std::function<void(gfx::ImageId)> done);
    void cancel(const std::string &key);
    // Cancels every pending request whose key isn't in keep: the rows still on screen.
    void cancelExcept(const std::unordered_set<std::string> &keep);
    // Off (the thumbnails setting): get() gives 0 and request() does nothing, so the disk tier is
    // neither read nor written; pending requests are cancelled. Both tiers are kept for when
    // they're turned on again.
    void setEnabled(bool enabled);
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] std::size_t pending() const { return pending_.size(); }
    [[nodiscard]] std::size_t cached() const { return local_.entries.size() + remote_.entries.size(); }
    [[nodiscard]] std::size_t cachedRemote() const { return remote_.entries.size(); }

    // The disk tier's size cap and decay time; a change sweeps it.
    void setDiskLimits(ThumbnailStoreLimits limits);
    // Empties the disk tier, then calls done on the UI thread.
    void clearDisk(std::function<void()> done = {});
    // The disk tier's size in bytes, as of its last sweep or write. Tracked.
    ui::Signal<std::uint64_t> diskUsage{0};
    // Null without a disk tier.
    [[nodiscard]] ThumbnailStore *diskStore() const { return disk_.get(); }

    // Remote thumbnails on or off, and the largest file fetched for one (0: no limit). Off
    // cancels their pending requests, and get() gives 0 for them.
    void setRemote(bool enabled, std::uint64_t maxBytes);
    [[nodiscard]] bool remoteEnabled() const { return remoteEnabled_; }
    // Which remote a location's thumbnails belong to: the server, or the archive file.
    static std::string remoteGroup(const vfs::Location &location);
    // Forgets the remotes not in open (no tab shows them): their images, pending requests and
    // temporary files.
    void keepRemoteGroups(const std::unordered_set<std::string> &open);
    // Removes every remote temporary file (the app went to the background).
    void dropRemoteTemp();
    // Null without remote temporary files.
    [[nodiscard]] ThumbnailStore *remoteStore() const { return remoteDisk_.get(); }

private:
    struct Entry {
        std::string key;
        gfx::ImageId id = 0;
        std::string group;  // The remote's, for remote thumbnails.
    };
    // An LRU of GPU images, most recently used first.
    struct Tier {
        std::size_t capacity = 1;
        std::list<Entry> lru;
        std::unordered_map<std::string, std::list<Entry>::iterator> entries;
    };
    struct Fetch {
        std::string key;
        vfs::Location location;
        int sizePx = 0;
        std::string group;
        std::shared_ptr<ui::Cancellation> token;
        std::function<void(gfx::ImageId)> done;
    };

    void insert(Tier &tier, const std::string &key, gfx::ImageId id, std::string group = {});
    // Uploads image for key into tier and calls done; false if there's nothing to upload.
    bool upload(Tier &tier, const std::string &key, DecodedImage &image, const std::string &group,
                const std::function<void(gfx::ImageId)> &done);
    // Sweeps the disk tier, or again once the sweep under way is done.
    void sweepDisk();
    void sweepRemote();
    // Starts queued remote fetches while fewer than kRemoteThumbnailFetches run.
    void pumpFetches();

    ui::IoPool &io_;
    gfx::vulkan::ImageStore &images_;
    ImageDecoder decoder_;
    Tier local_;
    std::unordered_map<std::string, std::shared_ptr<ui::Cancellation>> pending_;
    // Keys whose decode gave nothing: not an image, or a video with no frame that decodes in time.
    // Not retried at once, so a slow video doesn't hold an I/O thread again on every scroll; but a
    // local file is asked for once more kThumbnailRetryMs after its first failure (when its row
    // is bound again), as a video grab may have timed out or found the decoders busy. A remote
    // file whose bytes didn't decode isn't. The keys carry the file's mtime and size, so a changed
    // file is retried anyway. A cancelled request isn't here: its result is dropped before it
    // reaches the cache. A remote file that couldn't be read isn't either: the connection may
    // come back.
    struct Failure {
        int attempts = 0;
        std::int64_t retryAtMs = 0;  // On now_'s clock.
    };
    [[nodiscard]] bool failed(const std::string &key) const;
    void recordFailure(const std::string &key, bool final);
    std::unordered_map<std::string, Failure> failed_;
    std::function<std::int64_t()> now_;
    bool enabled_ = true;
    // Shared with the I/O work, which may outlive the cache.
    std::shared_ptr<ThumbnailStore> disk_;
    bool sweeping_ = false;
    bool sweepAgain_ = false;

    // Remote thumbnails.
    vfs::BackendResolver *backends_ = nullptr;
    BufferDecoder bufferDecoder_;
    Tier remote_;
    std::shared_ptr<ThumbnailStore> remoteDisk_;
    bool remoteEnabled_ = true;
    std::uint64_t remoteMaxBytes_ = 0;
    std::deque<Fetch> fetchQueue_;
    int fetching_ = 0;
    bool remoteSweeping_ = false;
    // The remotes with thumbnails, and the group of each pending remote request.
    std::unordered_set<std::string> remoteGroups_;
    std::unordered_map<std::string, std::string> pendingGroup_;

    // Cancelled when the cache goes, so results that arrive later are dropped.
    std::shared_ptr<ui::Cancellation> alive_ = std::make_shared<ui::Cancellation>();
};

}  // namespace filepan::app
