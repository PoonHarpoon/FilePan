// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/thumbnail_store.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <tuple>

#include "filepan/eintr.hpp"

namespace filepan::app {

namespace {

constexpr std::uint8_t kMagic[4] = {'F', 'P', 'T', 'N'};
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeaderSize = 24;
constexpr std::string_view kSuffix = ".fpt";
constexpr std::string_view kTemporaryMarker = ".tmp.";
// Thumbnails are a few hundred pixels a side; anything bigger isn't ours.
constexpr std::uint32_t kMaxSide = 4096;
constexpr std::uint32_t kMaxKey = 64 * 1024;
constexpr std::uint64_t kMaxFile = 64ull << 20;
// A temporary file this old was left by a write that never finished.
constexpr std::int64_t kStaleTemporaryNs = 3600ll * 1'000'000'000;
constexpr int kZstdLevel = 3;

void put16(std::vector<std::uint8_t> &out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t> &out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

std::uint32_t get32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

std::string hex64(std::uint64_t v) {
    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(v));
    return buffer;
}

std::int64_t mtimeNs(const struct stat &st) {
    return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
}

struct timespec toTimespec(std::int64_t ns) {
    return {static_cast<time_t>(ns / 1'000'000'000), static_cast<long>(ns % 1'000'000'000)};
}

bool writeAll(int fd, std::span<const std::uint8_t> data) {
    while (!data.empty()) {
        const ssize_t written = FILEPAN_RETRY(::write(fd, data.data(), data.size()));
        if (written <= 0) {
            return false;
        }
        data = data.subspan(static_cast<std::size_t>(written));
    }
    return true;
}

// Calls visit(name, stat) for every regular file in directory.
template <typename Visit>
void forEachFile(const std::string &directory, Visit visit) {
    DIR *dir = ::opendir(directory.c_str());
    if (dir == nullptr) {
        return;
    }
    const int dirFd = ::dirfd(dir);
    while (const dirent *entry = ::readdir(dir)) {
        const std::string_view name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        struct stat st {};
        if (::fstatat(dirFd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        visit(std::string(name), st);
    }
    ::closedir(dir);
}

}  // namespace

std::uint64_t stableHash(std::string_view text) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const char c : text) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

ThumbnailStore::ThumbnailStore(std::string directory, ThumbnailStoreLimits limits, Clock clock)
    : directory_(std::move(directory)), clock_(std::move(clock)), limits_(limits) {}

void ThumbnailStore::setLimits(ThumbnailStoreLimits limits) {
    std::lock_guard lock(mutex_);
    limits_ = limits;
}

ThumbnailStoreLimits ThumbnailStore::limits() const {
    std::lock_guard lock(mutex_);
    return limits_;
}

bool ThumbnailStore::overLimit() const {
    const std::uint64_t cap = limits().maxBytes;
    return cap != 0 && usage() > cap;
}

std::int64_t ThumbnailStore::now() const {
    if (clock_) {
        return clock_();
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
}

void ThumbnailStore::subtract(std::uint64_t bytes) {
    std::uint64_t current = usage_.load(std::memory_order_relaxed);
    while (!usage_.compare_exchange_weak(current, current > bytes ? current - bytes : 0, std::memory_order_relaxed)) {
    }
}

std::string ThumbnailStore::pathFor(std::string_view key, std::string_view group) const {
    std::string name = hex64(stableHash(key));
    if (!group.empty()) {
        name = hex64(stableHash(group)) + "-" + name;
    }
    return directory_ + "/" + name + std::string(kSuffix);
}

std::vector<std::uint8_t> ThumbnailStore::encode(std::string_view key, const DecodedImage &image) {
    const std::size_t raw = image.rgba.size();
    std::vector<std::uint8_t> out;
    if (image.width <= 0 || image.height <= 0 || static_cast<std::uint32_t>(image.width) > kMaxSide ||
        static_cast<std::uint32_t>(image.height) > kMaxSide ||
        raw != static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4 ||
        key.size() > kMaxKey) {
        return out;
    }
    ZSTD_CCtx *context = ZSTD_createCCtx();
    if (context == nullptr) {
        return out;
    }
    ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, kZstdLevel);
    ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 1);
    ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 1);
    const std::size_t bound = ZSTD_compressBound(raw);
    out.reserve(kHeaderSize + key.size() + bound);
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    put16(out, kVersion);
    put16(out, 0);
    put32(out, static_cast<std::uint32_t>(image.width));
    put32(out, static_cast<std::uint32_t>(image.height));
    put32(out, static_cast<std::uint32_t>(key.size()));
    put32(out, 0);  // The payload length, below.
    out.insert(out.end(), key.begin(), key.end());
    const std::size_t at = out.size();
    out.resize(at + bound);
    const std::size_t compressed = ZSTD_compress2(context, out.data() + at, bound, image.rgba.data(), raw);
    ZSTD_freeCCtx(context);
    if (ZSTD_isError(compressed)) {
        out.clear();
        return out;
    }
    out.resize(at + compressed);
    for (int i = 0; i < 4; ++i) {
        out[20 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(compressed >> (8 * i));
    }
    return out;
}

std::optional<DecodedImage> ThumbnailStore::decode(std::span<const std::uint8_t> bytes, std::string_view key,
                                                   bool *corrupt) {
    const auto bad = [&]() -> std::optional<DecodedImage> {
        if (corrupt != nullptr) {
            *corrupt = true;
        }
        return std::nullopt;
    };
    if (corrupt != nullptr) {
        *corrupt = false;
    }
    if (bytes.size() < kHeaderSize || !std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin()) ||
        (bytes[4] | bytes[5] << 8) != kVersion) {
        return bad();
    }
    const std::uint32_t width = get32(&bytes[8]);
    const std::uint32_t height = get32(&bytes[12]);
    const std::uint32_t keyLength = get32(&bytes[16]);
    const std::uint32_t payload = get32(&bytes[20]);
    if (width == 0 || height == 0 || width > kMaxSide || height > kMaxSide || keyLength > kMaxKey ||
        bytes.size() != kHeaderSize + keyLength + static_cast<std::size_t>(payload)) {
        return bad();
    }
    const std::string_view stored(reinterpret_cast<const char *>(bytes.data()) + kHeaderSize, keyLength);
    if (stored != key) {
        return std::nullopt;  // Another key with the same hash.
    }
    const std::uint8_t *frame = bytes.data() + kHeaderSize + keyLength;
    const std::size_t raw = static_cast<std::size_t>(width) * height * 4;
    if (ZSTD_getFrameContentSize(frame, payload) != raw) {
        return bad();
    }
    DecodedImage image{static_cast<int>(width), static_cast<int>(height), std::vector<std::uint8_t>(raw)};
    const std::size_t got = ZSTD_decompress(image.rgba.data(), raw, frame, payload);
    if (ZSTD_isError(got) || got != raw) {
        return bad();
    }
    return image;
}

std::optional<DecodedImage> ThumbnailStore::read(std::string_view key, std::string_view group) {
    const std::string path = pathFor(key, group);
    const int fd = FILEPAN_RETRY(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd < 0) {
        return std::nullopt;
    }
    struct stat st {};
    std::vector<std::uint8_t> bytes;
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && static_cast<std::uint64_t>(st.st_size) <= kMaxFile;
    if (ok) {
        bytes.resize(static_cast<std::size_t>(st.st_size));
        std::size_t at = 0;
        while (at < bytes.size()) {
            const ssize_t got = FILEPAN_RETRY(::read(fd, bytes.data() + at, bytes.size() - at));
            if (got <= 0) {
                break;
            }
            at += static_cast<std::size_t>(got);
        }
        bytes.resize(at);
    }
    bool corrupt = !ok;
    std::optional<DecodedImage> image;
    if (ok) {
        image = decode(bytes, key, &corrupt);
    }
    if (image) {
        // The access time is the mtime: atime is often off (noatime) or coarse (relatime).
        const struct timespec times[2] = {{0, UTIME_OMIT}, toTimespec(now())};
        ::futimens(fd, times);
    }
    ::close(fd);
    if (corrupt && ::unlink(path.c_str()) == 0) {
        subtract(static_cast<std::uint64_t>(st.st_size));
    }
    return image;
}

bool ThumbnailStore::write(std::string_view key, const DecodedImage &image, std::string_view group) {
    const std::string path = pathFor(key, group);
    {
        std::lock_guard lock(mutex_);
        if (!writing_.insert(path).second) {
            return false;
        }
    }
    struct Done {
        ThumbnailStore *self;
        const std::string &path;
        ~Done() {
            std::lock_guard lock(self->mutex_);
            self->writing_.erase(path);
        }
    } done{this, path};

    const std::vector<std::uint8_t> bytes = encode(key, image);
    if (bytes.empty()) {
        return false;
    }
    ::mkdir(directory_.c_str(), 0700);
    const std::string temporary = path + std::string(kTemporaryMarker) + std::to_string(::getpid()) + "." +
                                  std::to_string(counter_.fetch_add(1, std::memory_order_relaxed));
    const int fd = FILEPAN_RETRY(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    if (fd < 0) {
        return false;
    }
    bool ok = writeAll(fd, bytes);
    // Not synced: a file torn by a crash is caught as corrupt and dropped.
    if (ok) {
        const struct timespec stamp = toTimespec(now());
        const struct timespec times[2] = {stamp, stamp};
        ::futimens(fd, times);
    }
    ok = ::close(fd) == 0 && ok;
    struct stat old {};
    const bool replacing = ok && ::stat(path.c_str(), &old) == 0;
    if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    if (replacing) {
        subtract(static_cast<std::uint64_t>(old.st_size));
    }
    usage_.fetch_add(bytes.size(), std::memory_order_relaxed);
    return true;
}

void ThumbnailStore::sweep() {
    std::lock_guard sweeping(sweepMutex_);
    const ThumbnailStoreLimits limits = this->limits();
    const std::int64_t current = now();
    const std::int64_t maxAgeNs = limits.maxAgeSeconds > 0 ? limits.maxAgeSeconds * 1'000'000'000 : 0;
    std::vector<std::tuple<std::int64_t, std::uint64_t, std::string>> files;  // mtime, size, name
    std::uint64_t total = 0;
    forEachFile(directory_, [&](const std::string &name, const struct stat &st) {
        const std::string full = directory_ + "/" + name;
        if (name.find(kTemporaryMarker) != std::string::npos) {
            if (current - mtimeNs(st) > kStaleTemporaryNs) {
                ::unlink(full.c_str());
            }
            return;
        }
        if (!name.ends_with(kSuffix)) {
            return;
        }
        if (maxAgeNs != 0 && current - mtimeNs(st) > maxAgeNs) {
            ::unlink(full.c_str());
            return;
        }
        files.emplace_back(mtimeNs(st), static_cast<std::uint64_t>(st.st_size), name);
        total += static_cast<std::uint64_t>(st.st_size);
    });
    if (limits.maxBytes != 0 && total > limits.maxBytes) {
        std::ranges::sort(files);
        for (const auto &[mtime, size, name] : files) {
            if (total <= limits.maxBytes) {
                break;
            }
            if (::unlink((directory_ + "/" + name).c_str()) == 0 || errno == ENOENT) {
                total -= size;
            }
        }
    }
    usage_.store(total, std::memory_order_relaxed);
}

void ThumbnailStore::clear() {
    std::lock_guard sweeping(sweepMutex_);
    forEachFile(directory_, [&](const std::string &name, const struct stat &) {
        if (name.ends_with(kSuffix)) {
            ::unlink((directory_ + "/" + name).c_str());
        }
    });
    usage_.store(0, std::memory_order_relaxed);
}

void ThumbnailStore::removeGroup(std::string_view group) {
    if (group.empty()) {
        return;
    }
    const std::string prefix = hex64(stableHash(group)) + "-";
    std::lock_guard sweeping(sweepMutex_);
    forEachFile(directory_, [&](const std::string &name, const struct stat &st) {
        if (name.starts_with(prefix) && name.ends_with(kSuffix) && ::unlink((directory_ + "/" + name).c_str()) == 0) {
            subtract(static_cast<std::uint64_t>(st.st_size));
        }
    });
}

}  // namespace filepan::app
