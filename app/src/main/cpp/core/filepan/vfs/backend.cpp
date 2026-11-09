// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/vfs/backend.hpp"

#include <cerrno>
#include <utility>

namespace filepan::vfs {

Result<std::string> Backend::readLink(const Location &) { return fail(ENOTSUP); }

Result<void> Backend::createSymlink(const Location &, std::string_view) { return fail(ENOTSUP); }

Result<void> Backend::setTimes(const Location &, std::optional<Timestamp>,
                               std::optional<Timestamp>) {
    return fail(ENOTSUP);
}

Result<void> Backend::chmod(const Location &, std::uint32_t) { return fail(ENOTSUP); }

Result<std::unique_ptr<WatchHandle>> Backend::watch(const Location &, WatchCallback) {
    return fail(ENOTSUP);
}

void SimpleBackendResolver::set(BackendKind kind, std::shared_ptr<Backend> backend) {
    backends_[static_cast<std::size_t>(kind)] = std::move(backend);
}

Backend *SimpleBackendResolver::backendFor(const Location &location) {
    return backends_[static_cast<std::size_t>(location.kind())].get();
}

}  // namespace filepan::vfs
