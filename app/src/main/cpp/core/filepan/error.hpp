// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cerrno>
#include <expected>
#include <string>
#include <utility>

namespace filepan {

// An errno-style failure, with optional context such as the path or a library's message. New
// core APIs return Result<T>; older ones keep their callback-and-errno conventions.
struct Error {
    int code = 0;
    std::string message;

    [[nodiscard]] bool isCancelled() const { return code == ECANCELED; }
};

template <typename T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> fail(int code, std::string message = {}) {
    return std::unexpected(Error{code, std::move(message)});
}

// Uses the current errno.
inline std::unexpected<Error> failErrno(std::string message = {}) {
    return fail(errno, std::move(message));
}

}  // namespace filepan
