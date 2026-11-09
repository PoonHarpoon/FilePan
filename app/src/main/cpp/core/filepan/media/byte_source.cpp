// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/media/byte_source.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <utility>

namespace filepan::media {

MediaSource MediaSource::file(std::string path) {
    return {std::move(path), nullptr};
}

MediaSource MediaSource::stream(std::shared_ptr<ByteSource> bytes) {
    return {{}, std::move(bytes)};
}

namespace {

class FileByteSource final : public ByteSource {
public:
    explicit FileByteSource(int fd) : fd_(fd) {}
    ~FileByteSource() override { ::close(fd_); }
    FileByteSource(const FileByteSource &) = delete;
    FileByteSource &operator=(const FileByteSource &) = delete;

    MediaResult<std::uint64_t> size() override {
        struct stat st{};
        if (::fstat(fd_, &st) != 0) {
            return std::unexpected(MediaError{std::string("Couldn't read the file: ") + std::strerror(errno)});
        }
        return static_cast<std::uint64_t>(st.st_size);
    }

    MediaResult<std::size_t> readAt(std::uint64_t offset, std::span<std::uint8_t> buffer) override {
        std::size_t done = 0;
        while (done < buffer.size()) {
            if (cancelled_.load(std::memory_order_acquire)) {
                return std::unexpected(MediaError{"Cancelled"});
            }
            const ssize_t read =
                    ::pread(fd_, buffer.data() + done, buffer.size() - done, static_cast<off_t>(offset + done));
            if (read < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(MediaError{std::string("Couldn't read the file: ") + std::strerror(errno)});
            }
            if (read == 0) {
                break;  // EOF.
            }
            done += static_cast<std::size_t>(read);
        }
        return done;
    }

    void cancel() override { cancelled_.store(true, std::memory_order_release); }

private:
    int fd_;
    std::atomic<bool> cancelled_{false};
};

}  // namespace

std::shared_ptr<ByteSource> fileByteSource(const std::string &path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return nullptr;
    }
    return std::make_shared<FileByteSource>(fd);
}

}  // namespace filepan::media
