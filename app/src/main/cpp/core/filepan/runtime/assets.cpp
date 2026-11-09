// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/runtime/assets.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

#include "filepan/eintr.hpp"

#ifdef __ANDROID__
#include <android/asset_manager.h>
#endif

namespace filepan::runtime {

namespace {

// Relative, with no ".." segments.
bool safePath(std::string_view path) {
    if (path.empty() || path.front() == '/') {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        if (path.substr(start, end - start) == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

class DirectoryAssetSource final : public AssetSource {
public:
    explicit DirectoryAssetSource(std::string directory) : directory_(std::move(directory)) {}

    Result<std::string> read(std::string_view path) const override {
        if (!safePath(path)) {
            return fail(EINVAL, std::string(path));
        }
        const std::string full = directory_ + "/" + std::string(path);
        const int fd = FILEPAN_RETRY(::open(full.c_str(), O_RDONLY | O_CLOEXEC));
        if (fd < 0) {
            return failErrno(full);
        }
        std::string out;
        char buffer[16384];
        while (true) {
            const ssize_t got = FILEPAN_RETRY(::read(fd, buffer, sizeof buffer));
            if (got < 0) {
                const int error = errno;
                ::close(fd);
                return fail(error, full);
            }
            if (got == 0) {
                break;
            }
            out.append(buffer, static_cast<std::size_t>(got));
        }
        ::close(fd);
        return out;
    }

private:
    std::string directory_;
};

#ifdef __ANDROID__
class AndroidAssetSource final : public AssetSource {
public:
    explicit AndroidAssetSource(AAssetManager *manager) : manager_(manager) {}

    Result<std::string> read(std::string_view path) const override {
        if (!safePath(path)) {
            return fail(EINVAL, std::string(path));
        }
        const std::string name(path);
        AAsset *asset = AAssetManager_open(manager_, name.c_str(), AASSET_MODE_BUFFER);
        if (asset == nullptr) {
            return fail(ENOENT, name);
        }
        const void *buffer = AAsset_getBuffer(asset);
        const auto length = static_cast<std::size_t>(AAsset_getLength64(asset));
        std::string out;
        if (buffer != nullptr) {
            out.assign(static_cast<const char *>(buffer), length);
        }
        AAsset_close(asset);
        if (buffer == nullptr) {
            return fail(EIO, name);
        }
        return out;
    }

private:
    AAssetManager *manager_;
};
#endif

}  // namespace

std::unique_ptr<AssetSource> createDirectoryAssetSource(std::string directory) {
    return std::make_unique<DirectoryAssetSource>(std::move(directory));
}

#ifdef __ANDROID__
std::unique_ptr<AssetSource> createAndroidAssetSource(AAssetManager *assetManager) {
    return std::make_unique<AndroidAssetSource>(assetManager);
}
#endif

}  // namespace filepan::runtime
