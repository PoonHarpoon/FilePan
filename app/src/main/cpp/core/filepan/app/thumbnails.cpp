// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/thumbnails.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>

#ifdef __ANDROID__
#include <android/bitmap.h>
#include <android/data_space.h>
#include <android/imagedecoder.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace filepan::app {

void unpremultiply(std::vector<std::uint8_t> &rgba) {
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const unsigned a = rgba[i + 3];
        if (a == 0 || a == 255) {
            continue;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            rgba[i + c] = static_cast<std::uint8_t>(std::min(255u, (rgba[i + c] * 255u + a / 2) / a));
        }
    }
}

#ifdef __ANDROID__

std::optional<DecodedImage> decodeThumbnail(const std::string &path, int sizePx) {
    if (sizePx <= 0) {
        return std::nullopt;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::nullopt;
    }
    AImageDecoder *decoder = nullptr;
    const int created = AImageDecoder_createFromFd(fd, &decoder);
    ::close(fd);  // The decoder keeps its own reference.
    if (created != ANDROID_IMAGE_DECODER_SUCCESS || decoder == nullptr) {
        return std::nullopt;
    }
    struct Delete {
        AImageDecoder *decoder;
        ~Delete() { AImageDecoder_delete(decoder); }
    } guard{decoder};

    const AImageDecoderHeaderInfo *info = AImageDecoder_getHeaderInfo(decoder);
    const int width = AImageDecoderHeaderInfo_getWidth(info);
    const int height = AImageDecoderHeaderInfo_getHeight(info);
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    const bool opaque = AImageDecoderHeaderInfo_getAlphaFlags(info) == ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE;
    AImageDecoder_setAndroidBitmapFormat(decoder, ANDROID_BITMAP_FORMAT_RGBA_8888);
    // Premultiplied (the default): the decoder refuses to scale unpremultiplied output of an
    // image that may be translucent, which failed every large PNG or WebP. Undone below.
    // The UI composites in sRGB; wide-gamut photos are mapped into it.
    AImageDecoder_setDataSpace(decoder, ADATASPACE_SRGB);
    // Scale so the shorter side is sizePx (never up), then crop the center square.
    const float scale = std::min(1.0f, static_cast<float>(sizePx) / static_cast<float>(std::min(width, height)));
    const int scaledWidth = std::max(1, static_cast<int>(static_cast<float>(width) * scale + 0.5f));
    const int scaledHeight = std::max(1, static_cast<int>(static_cast<float>(height) * scale + 0.5f));
    if (AImageDecoder_setTargetSize(decoder, scaledWidth, scaledHeight) != ANDROID_IMAGE_DECODER_SUCCESS) {
        return std::nullopt;
    }
    const int side = std::min(scaledWidth, scaledHeight);
    ARect crop{(scaledWidth - side) / 2, (scaledHeight - side) / 2, 0, 0};
    crop.right = crop.left + side;
    crop.bottom = crop.top + side;
    if (AImageDecoder_setCrop(decoder, crop) != ANDROID_IMAGE_DECODER_SUCCESS) {
        return std::nullopt;
    }
    const std::size_t stride = AImageDecoder_getMinimumStride(decoder);
    std::vector<std::uint8_t> buffer(stride * static_cast<std::size_t>(side));
    if (AImageDecoder_decodeImage(decoder, buffer.data(), stride, buffer.size()) != ANDROID_IMAGE_DECODER_SUCCESS) {
        return std::nullopt;
    }
    DecodedImage image{side, side, {}};
    const std::size_t row = static_cast<std::size_t>(side) * 4;
    if (stride == row) {
        image.rgba = std::move(buffer);
    } else {
        image.rgba.resize(row * static_cast<std::size_t>(side));
        for (int y = 0; y < side; ++y) {
            std::copy_n(buffer.data() + static_cast<std::size_t>(y) * stride, row,
                        image.rgba.data() + static_cast<std::size_t>(y) * row);
        }
    }
    if (!opaque) {
        unpremultiply(image.rgba);
    }
    return image;
}

namespace {

// decodeThumbnail's scaling, cropping and conversion, for a decoder made from a buffer. Kept apart
// from decodeThumbnail for now; it could share this.
std::optional<DecodedImage> decodeWith(AImageDecoder *decoder, int sizePx) {
    const AImageDecoderHeaderInfo *info = AImageDecoder_getHeaderInfo(decoder);
    const int width = AImageDecoderHeaderInfo_getWidth(info);
    const int height = AImageDecoderHeaderInfo_getHeight(info);
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    const bool opaque = AImageDecoderHeaderInfo_getAlphaFlags(info) == ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE;
    AImageDecoder_setAndroidBitmapFormat(decoder, ANDROID_BITMAP_FORMAT_RGBA_8888);
    AImageDecoder_setDataSpace(decoder, ADATASPACE_SRGB);
    const float scale = std::min(1.0f, static_cast<float>(sizePx) / static_cast<float>(std::min(width, height)));
    const int scaledWidth = std::max(1, static_cast<int>(static_cast<float>(width) * scale + 0.5f));
    const int scaledHeight = std::max(1, static_cast<int>(static_cast<float>(height) * scale + 0.5f));
    if (AImageDecoder_setTargetSize(decoder, scaledWidth, scaledHeight) != ANDROID_IMAGE_DECODER_SUCCESS) {
        return std::nullopt;
    }
    const int side = std::min(scaledWidth, scaledHeight);
    ARect crop{(scaledWidth - side) / 2, (scaledHeight - side) / 2, 0, 0};
    crop.right = crop.left + side;
    crop.bottom = crop.top + side;
    if (AImageDecoder_setCrop(decoder, crop) != ANDROID_IMAGE_DECODER_SUCCESS) {
        return std::nullopt;
    }
    const std::size_t stride = AImageDecoder_getMinimumStride(decoder);
    std::vector<std::uint8_t> buffer(stride * static_cast<std::size_t>(side));
    if (AImageDecoder_decodeImage(decoder, buffer.data(), stride, buffer.size()) != ANDROID_IMAGE_DECODER_SUCCESS) {
        return std::nullopt;
    }
    DecodedImage image{side, side, {}};
    const std::size_t row = static_cast<std::size_t>(side) * 4;
    if (stride == row) {
        image.rgba = std::move(buffer);
    } else {
        image.rgba.resize(row * static_cast<std::size_t>(side));
        for (int y = 0; y < side; ++y) {
            std::copy_n(buffer.data() + static_cast<std::size_t>(y) * stride, row,
                        image.rgba.data() + static_cast<std::size_t>(y) * row);
        }
    }
    if (!opaque) {
        unpremultiply(image.rgba);
    }
    return image;
}

}  // namespace

std::optional<DecodedImage> decodeThumbnailBuffer(std::span<const std::uint8_t> bytes, int sizePx) {
    if (sizePx <= 0 || bytes.empty()) {
        return std::nullopt;
    }
    AImageDecoder *decoder = nullptr;
    // The decoder reads the bytes in place, and goes before they do.
    if (AImageDecoder_createFromBuffer(bytes.data(), bytes.size(), &decoder) != ANDROID_IMAGE_DECODER_SUCCESS ||
        decoder == nullptr) {
        return std::nullopt;
    }
    struct Delete {
        AImageDecoder *decoder;
        ~Delete() { AImageDecoder_delete(decoder); }
    } guard{decoder};
    return decodeWith(decoder, sizePx);
}

#else

std::optional<DecodedImage> decodeThumbnail(const std::string &, int) { return std::nullopt; }

std::optional<DecodedImage> decodeThumbnailBuffer(std::span<const std::uint8_t>, int) { return std::nullopt; }

#endif

namespace {

// What a request's I/O work hands back to the UI thread.
struct Loaded {
    std::optional<DecodedImage> image;
    // The disk tier went past its cap with this write.
    bool overLimit = false;
    std::uint64_t diskUsage = 0;
    // A remote file's bytes were read but didn't decode (as opposed to not being read).
    bool undecodable = false;
};

constexpr std::size_t kFetchChunk = 64 * 1024;

// A remote file's bytes, read in chunks so a cancelled request stops between them. nullopt if
// cancelled, unreadable, or over maxBytes (0: no limit).
std::optional<std::vector<std::uint8_t>> fetch(vfs::BackendResolver &backends, const vfs::Location &location,
                                               std::uint64_t maxBytes, const ui::Cancellation &token) {
    vfs::Backend *backend = backends.backendFor(location);
    if (backend == nullptr || token.cancelled()) {
        return std::nullopt;
    }
    auto stream = backend->openRead(location);
    if (!stream) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    while (true) {
        if (token.cancelled()) {
            return std::nullopt;
        }
        const std::size_t at = bytes.size();
        bytes.resize(at + kFetchChunk);
        auto got = (*stream)->read(std::span(bytes).subspan(at));
        if (!got) {
            return std::nullopt;
        }
        bytes.resize(at + *got);
        if (*got == 0) {
            return bytes;
        }
        if (maxBytes != 0 && bytes.size() > maxBytes) {
            return std::nullopt;
        }
    }
}

}  // namespace

ThumbnailSource thumbnailSource(const vfs::Location &folder, const vfs::Entry &entry) {
    if (entry.targetType != vfs::EntryType::kFile) {
        return ThumbnailSource::none;
    }
    const vfs::FileKind kind = entry.mimeType().kind;
    if (folder.isLocal()) {
        return kind == vfs::FileKind::kImage || kind == vfs::FileKind::kVideo || kind == vfs::FileKind::kApk
                       ? ThumbnailSource::local
                       : ThumbnailSource::none;
    }
    if (folder.kind() == vfs::BackendKind::kSftp && kind == vfs::FileKind::kImage) {
        return ThumbnailSource::remote;
    }
    return ThumbnailSource::none;
}

bool isIconThumbnail(vfs::FileKind kind) { return kind == vfs::FileKind::kApk; }

std::string thumbnailKey(const vfs::Location &location, const vfs::Entry &entry) {
    return (location.isLocal() ? location.path() : location.toString()) + "|" +
           std::to_string(entry.modifiedTime.seconds) + "|" + std::to_string(entry.size);
}

ThumbnailCache::ThumbnailCache(ui::IoPool &io, gfx::vulkan::ImageStore &images, ImageDecoder decoder,
                               std::size_t capacity)
    : ThumbnailCache(io, images, std::move(decoder), ThumbnailCacheOptions{.capacity = capacity}) {}

ThumbnailCache::ThumbnailCache(ui::IoPool &io, gfx::vulkan::ImageStore &images, ImageDecoder decoder,
                               ThumbnailCacheOptions options)
    : io_(io),
      images_(images),
      decoder_(std::move(decoder)),
      backends_(options.backends),
      bufferDecoder_(std::move(options.bufferDecoder)) {
    now_ = options.retryClock ? std::move(options.retryClock) : [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
    };
    local_.capacity = std::max<std::size_t>(options.capacity, 1);
    remote_.capacity = std::max<std::size_t>(options.remoteCapacity, 1);
    if (!options.diskDirectory.empty()) {
        disk_ = std::make_shared<ThumbnailStore>(std::move(options.diskDirectory), options.diskLimits, options.clock);
        sweepDisk();
    }
    if (!options.remoteDirectory.empty()) {
        remoteDisk_ = std::make_shared<ThumbnailStore>(std::move(options.remoteDirectory),
                                                       ThumbnailStoreLimits{.maxBytes = options.remoteTempBytes},
                                                       options.clock);
        // Left by a run that was killed or crashed.
        io_.execute([store = remoteDisk_] { store->clear(); });
    }
}

ThumbnailCache::~ThumbnailCache() {
    alive_->cancel();
    for (auto &[key, token] : pending_) {
        token->cancel();
    }
    for (Tier *tier : {&local_, &remote_}) {
        for (const Entry &entry : tier->lru) {
            images_.remove(entry.id);
        }
    }
}

void ThumbnailCache::setEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled) {
        cancelExcept({});
    }
}

gfx::ImageId ThumbnailCache::get(const std::string &key) {
    if (!enabled_) {
        return 0;
    }
    for (Tier *tier : {&local_, &remote_}) {
        const auto found = tier->entries.find(key);
        if (found == tier->entries.end()) {
            continue;
        }
        if (tier == &remote_ && !remoteEnabled_) {
            return 0;
        }
        tier->lru.splice(tier->lru.begin(), tier->lru, found->second);
        return found->second->id;
    }
    return 0;
}

void ThumbnailCache::request(const std::string &key, const std::string &path, int sizePx,
                             ThumbnailPersistence persistence, std::function<void(gfx::ImageId)> done) {
    if (!enabled_ || !decoder_ || local_.entries.contains(key) || pending_.contains(key) || failed(key)) {
        return;
    }
    // Requests are one per key at a time (pending_), and the store skips a write of a key that's
    // already being written, so a key is never written twice at once.
    std::shared_ptr<ThumbnailStore> disk = persistence == ThumbnailPersistence::disk ? disk_ : nullptr;
    auto token = std::make_shared<ui::Cancellation>();
    pending_[key] = token;
    io_.submit(
            [decoder = decoder_, disk, token, key, path, sizePx]() -> Loaded {
                Loaded loaded;
                if (token->cancelled()) {
                    return loaded;
                }
                if (disk) {
                    loaded.image = disk->read(key);
                }
                if (!loaded.image) {
                    loaded.image = decoder(path, sizePx);
                    if (disk && loaded.image && disk->write(key, *loaded.image)) {
                        loaded.overLimit = disk->overLimit();
                    }
                }
                if (disk) {
                    loaded.diskUsage = disk->usage();
                }
                return loaded;
            },
            [this, alive = alive_, token, key, disk, done = std::move(done)](Loaded loaded) {
                if (alive->cancelled()) {
                    return;
                }
                // Even a cancelled request may have written to the disk tier.
                if (disk) {
                    diskUsage.set(loaded.diskUsage);
                    if (loaded.overLimit) {
                        sweepDisk();
                    }
                }
                if (token->cancelled()) {
                    return;
                }
                pending_.erase(key);
                if (!loaded.image || !upload(local_, key, *loaded.image, {}, done)) {
                    recordFailure(key, false);
                }
            });
}

void ThumbnailCache::requestRemote(const std::string &key, const vfs::Location &location, std::uint64_t size,
                                   int sizePx, std::function<void(gfx::ImageId)> done) {
    if (!enabled_ || !remoteEnabled_ || backends_ == nullptr || !bufferDecoder_ ||
        (remoteMaxBytes_ != 0 && size > remoteMaxBytes_) || remote_.entries.contains(key) ||
        pending_.contains(key) || failed(key)) {
        return;
    }
    std::string group = remoteGroup(location);
    auto token = std::make_shared<ui::Cancellation>();
    pending_[key] = token;
    pendingGroup_[key] = group;
    remoteGroups_.insert(group);
    Fetch job{key, location, sizePx, std::move(group), token, std::move(done)};
    if (!remoteDisk_) {
        fetchQueue_.push_back(std::move(job));
        pumpFetches();
        return;
    }
    // The temporary file first: it doesn't count against the fetches.
    io_.submit(
            [store = remoteDisk_, token, key, group = job.group]() -> std::optional<DecodedImage> {
                if (token->cancelled()) {
                    return std::nullopt;
                }
                return store->read(key, group);
            },
            [this, alive = alive_, job = std::move(job)](std::optional<DecodedImage> image) mutable {
                if (alive->cancelled() || job.token->cancelled()) {
                    return;
                }
                if (image) {
                    pending_.erase(job.key);
                    pendingGroup_.erase(job.key);
                    upload(remote_, job.key, *image, job.group, job.done);
                    return;
                }
                fetchQueue_.push_back(std::move(job));
                pumpFetches();
            });
}

void ThumbnailCache::pumpFetches() {
    while (fetching_ < kRemoteThumbnailFetches && !fetchQueue_.empty()) {
        Fetch job = std::move(fetchQueue_.front());
        fetchQueue_.pop_front();
        if (job.token->cancelled()) {
            continue;  // Scrolled away while it waited.
        }
        ++fetching_;
        io_.submit(
                [backends = backends_, decoder = bufferDecoder_, store = remoteDisk_, maxBytes = remoteMaxBytes_,
                 token = job.token, key = job.key, location = job.location, sizePx = job.sizePx,
                 group = job.group]() -> Loaded {
                    Loaded loaded;
                    auto bytes = fetch(*backends, location, maxBytes, *token);
                    if (!bytes || token->cancelled()) {
                        return loaded;
                    }
                    loaded.image = decoder(*bytes, sizePx);
                    if (!loaded.image) {
                        loaded.undecodable = true;
                    } else if (store && store->write(key, *loaded.image, group)) {
                        loaded.overLimit = store->overLimit();
                    }
                    return loaded;
                },
                [this, alive = alive_, job = std::move(job)](Loaded loaded) mutable {
                    if (alive->cancelled()) {
                        return;
                    }
                    --fetching_;
                    pumpFetches();
                    if (loaded.overLimit) {
                        sweepRemote();
                    }
                    if (job.token->cancelled()) {
                        return;
                    }
                    pending_.erase(job.key);
                    pendingGroup_.erase(job.key);
                    if (loaded.image ? !upload(remote_, job.key, *loaded.image, job.group, job.done)
                                     : loaded.undecodable) {
                        recordFailure(job.key, true);
                    }
                });
    }
}

bool ThumbnailCache::failed(const std::string &key) const {
    const auto found = failed_.find(key);
    return found != failed_.end() &&
           (found->second.attempts >= kThumbnailAttempts || now_() < found->second.retryAtMs);
}

void ThumbnailCache::recordFailure(const std::string &key, bool final) {
    Failure &failure = failed_[key];
    failure.attempts = final ? kThumbnailAttempts : failure.attempts + 1;
    failure.retryAtMs = now_() + kThumbnailRetryMs;
}

bool ThumbnailCache::upload(Tier &tier, const std::string &key, DecodedImage &image, const std::string &group,
                            const std::function<void(gfx::ImageId)> &done) {
    if (image.width <= 0 || image.height <= 0) {
        return false;
    }
    const gfx::ImageId id = images_.add(image.width, image.height, std::move(image.rgba));
    if (id == 0) {
        return false;
    }
    insert(tier, key, id, group);
    if (done) {
        done(id);
    }
    return true;
}

void ThumbnailCache::insert(Tier &tier, const std::string &key, gfx::ImageId id, std::string group) {
    tier.lru.push_front({key, id, std::move(group)});
    tier.entries[key] = tier.lru.begin();
    while (tier.lru.size() > tier.capacity) {
        const Entry &old = tier.lru.back();
        images_.remove(old.id);
        tier.entries.erase(old.key);
        tier.lru.pop_back();
    }
}

void ThumbnailCache::cancel(const std::string &key) {
    if (const auto found = pending_.find(key); found != pending_.end()) {
        found->second->cancel();
        pending_.erase(found);
        pendingGroup_.erase(key);
    }
}

void ThumbnailCache::cancelExcept(const std::unordered_set<std::string> &keep) {
    std::erase_if(pending_, [&](const auto &item) {
        if (keep.contains(item.first)) {
            return false;
        }
        item.second->cancel();
        pendingGroup_.erase(item.first);
        return true;
    });
}

void ThumbnailCache::setDiskLimits(ThumbnailStoreLimits limits) {
    if (!disk_ || disk_->limits() == limits) {
        return;
    }
    disk_->setLimits(limits);
    sweepDisk();
}

void ThumbnailCache::sweepDisk() {
    if (!disk_) {
        return;
    }
    if (sweeping_) {
        sweepAgain_ = true;
        return;
    }
    sweeping_ = true;
    io_.submit(
            [disk = disk_] {
                disk->sweep();
                return disk->usage();
            },
            [this, alive = alive_](std::uint64_t usage) {
                if (alive->cancelled()) {
                    return;
                }
                sweeping_ = false;
                diskUsage.set(usage);
                if (sweepAgain_) {
                    sweepAgain_ = false;
                    sweepDisk();
                }
            });
}

void ThumbnailCache::clearDisk(std::function<void()> done) {
    if (!disk_) {
        if (done) {
            done();
        }
        return;
    }
    io_.submit(
            [disk = disk_] {
                disk->clear();
                return disk->usage();
            },
            [this, alive = alive_, done = std::move(done)](std::uint64_t usage) {
                if (alive->cancelled()) {
                    return;
                }
                diskUsage.set(usage);
                if (done) {
                    done();
                }
            });
}

void ThumbnailCache::setRemote(bool enabled, std::uint64_t maxBytes) {
    remoteMaxBytes_ = maxBytes;
    if (remoteEnabled_ == enabled) {
        return;
    }
    remoteEnabled_ = enabled;
    if (!enabled) {
        for (const auto &[key, group] : pendingGroup_) {
            if (const auto found = pending_.find(key); found != pending_.end()) {
                found->second->cancel();
                pending_.erase(found);
            }
        }
        pendingGroup_.clear();
    }
}

std::string ThumbnailCache::remoteGroup(const vfs::Location &location) {
    switch (location.kind()) {
    case vfs::BackendKind::kLocal:
        return {};
    case vfs::BackendKind::kArchive:
        return "archive:" + location.archiveFile()->toString();
    case vfs::BackendKind::kSftp:
        return "sftp:" + location.serverId();
    case vfs::BackendKind::kDocument:
        return "document:" + location.serverId();
    }
    return {};
}

void ThumbnailCache::keepRemoteGroups(const std::unordered_set<std::string> &open) {
    std::vector<std::string> gone;
    for (const std::string &group : remoteGroups_) {
        if (!open.contains(group)) {
            gone.push_back(group);
        }
    }
    if (gone.empty()) {
        return;
    }
    const std::unordered_set<std::string> left(gone.begin(), gone.end());
    std::erase_if(pendingGroup_, [&](const auto &item) {
        if (!left.contains(item.second)) {
            return false;
        }
        if (const auto found = pending_.find(item.first); found != pending_.end()) {
            found->second->cancel();
            pending_.erase(found);
        }
        return true;
    });
    for (auto it = remote_.lru.begin(); it != remote_.lru.end();) {
        if (left.contains(it->group)) {
            images_.remove(it->id);
            remote_.entries.erase(it->key);
            it = remote_.lru.erase(it);
        } else {
            ++it;
        }
    }
    for (const std::string &group : gone) {
        remoteGroups_.erase(group);
        if (remoteDisk_) {
            io_.execute([store = remoteDisk_, group] { store->removeGroup(group); });
        }
    }
}

void ThumbnailCache::dropRemoteTemp() {
    if (remoteDisk_) {
        io_.execute([store = remoteDisk_] { store->clear(); });
    }
}

void ThumbnailCache::sweepRemote() {
    if (!remoteDisk_ || remoteSweeping_) {
        return;
    }
    remoteSweeping_ = true;
    io_.submit([store = remoteDisk_] { store->sweep(); },
               [this, alive = alive_] {
                   if (!alive->cancelled()) {
                       remoteSweeping_ = false;
                   }
               });
}

}  // namespace filepan::app
