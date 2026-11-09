// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "filepan/error.hpp"

// Read-only files bundled with the app: the APK's assets/ on Android, a directory (normally
// app/src/main/assets) on a host. Paths are relative, e.g. "licenses/licenses.xml".
// Thread-safe; read on the I/O pool, not the UI thread.
namespace filepan::runtime {

class AssetSource {
public:
    virtual ~AssetSource() = default;
    // The whole file. ENOENT if there's no such asset.
    [[nodiscard]] virtual Result<std::string> read(std::string_view path) const = 0;
};

std::unique_ptr<AssetSource> createDirectoryAssetSource(std::string directory);

}  // namespace filepan::runtime

#ifdef __ANDROID__
struct AAssetManager;

namespace filepan::runtime {

// assetManager must outlive the source.
std::unique_ptr<AssetSource> createAndroidAssetSource(AAssetManager *assetManager);

}  // namespace filepan::runtime
#endif
