// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <optional>
#include <string>

#include "filepan/app/thumbnails.hpp"
#include "filepan/runtime/platform.hpp"

// Video and APK thumbnails for the browsing list, beside the image ones in thumbnails.hpp, and
// the app's decoder that picks between them by file kind.
namespace filepan::app {

// A frame of a local video as a thumbnail (media::grabVideoThumbnail), the same square as
// decodeThumbnail gives an image. nullopt off Android, or when no frame decodes within about 5 s.
std::optional<DecodedImage> decodeVideoThumbnail(const std::string &path, int sizePx);

// A local APK's app icon (Platform::apkIcon) as a sizePx square, uncropped, transparent around
// its shape. nullopt when the platform gives none (always, off Android).
std::optional<DecodedImage> decodeApkThumbnail(runtime::Platform &platform, const std::string &path, int sizePx);

// What fileThumbnailDecoder decodes each kind of file with; the defaults are the real ones. A
// null one gives that kind nothing.
struct FileThumbnailSources {
    ImageDecoder image = decodeThumbnail;
    ImageDecoder video = decodeVideoThumbnail;
    // Null: the app uses decodeApkThumbnail with its platform.
    ImageDecoder apk;
};

// The app's ImageDecoder: videos and APKs (by file name, as the list decides which files get
// thumbnails) by sources.video and sources.apk, everything else by sources.image.
ImageDecoder fileThumbnailDecoder(FileThumbnailSources sources = {});

// fileThumbnailDecoder with the real image and video sources, and no APK icons.
std::optional<DecodedImage> decodeFileThumbnail(const std::string &path, int sizePx);

}  // namespace filepan::app
