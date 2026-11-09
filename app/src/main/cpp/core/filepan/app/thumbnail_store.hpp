// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// Thumbnails on disk: one file per thumbnail in a directory of its own, between the GPU LRU and
// decoding (thumbnails.hpp). The app keeps two: a persistent one for local files under the cache
// directory, which Android may clear under storage pressure, and a temporary one for remote
// files, wiped at startup, when the app goes to the background and when a remote is left.
//
// A file is named after a 64-bit FNV-1a hash of its key ("<hash>.fpt"), prefixed with a hash of
// its group ("<group>-<hash>.fpt") when it has one, so a remote's files can be removed together.
// It holds, little-endian:
//
//   0   4  magic "FPTN"
//   4   2  version (1)
//   6   2  reserved, 0
//   8   4  width
//   12  4  height
//   16  4  key length
//   20  4  payload length
//   24     the key, then the payload: the RGBA8 pixels as one zstd frame with its content size
//          and checksum
//
// The key in the header rejects hash collisions. A file that's truncated, corrupt or of another
// version is a miss, and deleted. Files are written to a temporary name and renamed into place.
//
// Last access is the file's mtime: set when it's written and on every hit, so no index is kept.
// sweep() removes the files not accessed within the decay time, then the least recently accessed
// ones until the directory fits the size cap. Every method may be called from any thread; call
// them on I/O threads, never the UI thread.
namespace filepan::app {

struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // Non-premultiplied RGBA8, tightly packed.
};

struct ThumbnailStoreLimits {
    // The size cap, in bytes of the files; 0 has none.
    std::uint64_t maxBytes = 0;
    // Files not accessed for this long are removed by a sweep; 0 keeps them.
    std::int64_t maxAgeSeconds = 0;

    bool operator==(const ThumbnailStoreLimits &) const = default;
};

class ThumbnailStore {
public:
    // Wall-clock nanoseconds since the epoch: what last access is measured in.
    using Clock = std::function<std::int64_t()>;

    // The directory is created on the first write. clock defaults to the system clock.
    explicit ThumbnailStore(std::string directory, ThumbnailStoreLimits limits = {}, Clock clock = {});

    [[nodiscard]] const std::string &directory() const { return directory_; }
    void setLimits(ThumbnailStoreLimits limits);
    [[nodiscard]] ThumbnailStoreLimits limits() const;

    // The thumbnail stored for key, marking it accessed; nullopt on a miss.
    std::optional<DecodedImage> read(std::string_view key, std::string_view group = {});
    // Stores image for key. False if it couldn't, or another write of the same key is under way.
    bool write(std::string_view key, const DecodedImage &image, std::string_view group = {});
    // Removes what's past the decay time, then the least recently accessed files over the cap,
    // and recounts the usage. Concurrent sweeps run one after the other.
    void sweep();
    // Removes every thumbnail.
    void clear();
    // Removes every thumbnail written with this group.
    void removeGroup(std::string_view group);

    // An estimate of the files' total size, exact after a sweep or clear.
    [[nodiscard]] std::uint64_t usage() const { return usage_.load(std::memory_order_relaxed); }
    // The estimate is over the cap.
    [[nodiscard]] bool overLimit() const;
    // Where key's file is (for tests).
    [[nodiscard]] std::string pathFor(std::string_view key, std::string_view group = {}) const;

    // The file format, apart from the file system.
    static std::vector<std::uint8_t> encode(std::string_view key, const DecodedImage &image);
    // nullopt if bytes isn't a valid thumbnail file for key; *corrupt says whether it's invalid
    // (rather than another key's).
    static std::optional<DecodedImage> decode(std::span<const std::uint8_t> bytes, std::string_view key,
                                              bool *corrupt = nullptr);

private:
    [[nodiscard]] std::int64_t now() const;
    void subtract(std::uint64_t bytes);

    std::string directory_;
    Clock clock_;
    mutable std::mutex mutex_;
    ThumbnailStoreLimits limits_;
    std::unordered_set<std::string> writing_;
    std::mutex sweepMutex_;
    std::atomic<std::uint64_t> usage_{0};
    std::atomic<std::uint64_t> counter_{0};
};

// FNV-1a, 64-bit: stable across builds and platforms, for file names.
std::uint64_t stableHash(std::string_view text);

}  // namespace filepan::app
