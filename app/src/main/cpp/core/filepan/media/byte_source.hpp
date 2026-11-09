// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

// Where the viewer's decoders read a file from: a local path, which they open themselves, or a
// ByteSource for a file that streams (from an SFTP server, say).
namespace filepan::media {

struct MediaError {
    std::string message;
};

template <typename T>
using MediaResult = std::expected<T, MediaError>;

// A file's bytes, read at any offset.
class ByteSource {
public:
    virtual ~ByteSource() = default;

    virtual MediaResult<std::uint64_t> size() = 0;
    // Thread-safe: video and audio extractors read from different threads. Short reads only at
    // EOF; 0 at/after EOF.
    virtual MediaResult<std::size_t> readAt(std::uint64_t offset, std::span<std::uint8_t> buffer) = 0;
    // Makes blocked and future reads fail promptly (used on teardown).
    virtual void cancel() {}
    // Hint from the player: actively needed (playing / current page) or not; sources may pause
    // read-ahead.
    virtual void setActive(bool active) { (void)active; }
    // True while a read is blocked waiting on the network (for a buffering spinner).
    [[nodiscard]] virtual bool waiting() const { return false; }
};

struct MediaSource {
    std::string path;                   // For local files,
    std::shared_ptr<ByteSource> bytes;  // or for streamed ones.

    static MediaSource file(std::string path);
    static MediaSource stream(std::shared_ptr<ByteSource> bytes);
};

// pread on an fd (for tests and hosts); null if path can't be opened.
std::shared_ptr<ByteSource> fileByteSource(const std::string &path);

}  // namespace filepan::media
