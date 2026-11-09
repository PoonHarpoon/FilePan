// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <unistd.h>

#include <utility>

namespace filepan {

// Owns a file descriptor and closes it, unless it was released first. -1 is none.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}
    UniqueFd &operator=(UniqueFd &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    [[nodiscard]] int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

    // Gives the descriptor up without closing it.
    int release() { return std::exchange(fd_, -1); }
    void reset(int fd = -1) {
        if (fd_ >= 0) {
            // Not retried on EINTR: on Linux the descriptor is gone either way.
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

}  // namespace filepan
