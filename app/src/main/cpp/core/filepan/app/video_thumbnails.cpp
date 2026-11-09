// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/video_thumbnails.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "filepan/media/video_thumbnail.hpp"
#include "filepan/vfs/mime.hpp"

namespace filepan::app {

std::optional<DecodedImage> decodeVideoThumbnail(const std::string &path, int sizePx) {
    std::optional<media::RgbaImage> frame = media::grabVideoThumbnail(path, sizePx);
    if (!frame) {
        return std::nullopt;
    }
    // Opaque, so premultiplied or not is the same.
    return DecodedImage{frame->width, frame->height, std::move(frame->rgba)};
}

std::optional<DecodedImage> decodeApkThumbnail(runtime::Platform &platform, const std::string &path, int sizePx) {
    if (sizePx <= 0) {
        return std::nullopt;
    }
    std::optional<std::vector<std::uint8_t>> pixels = platform.apkIcon(path, sizePx);
    const auto expected = static_cast<std::size_t>(sizePx) * static_cast<std::size_t>(sizePx) * 4;
    if (!pixels || pixels->size() != expected) {
        return std::nullopt;
    }
    // Android draws premultiplied; an icon's edges are translucent.
    unpremultiply(*pixels);
    return DecodedImage{sizePx, sizePx, std::move(*pixels)};
}

ImageDecoder fileThumbnailDecoder(FileThumbnailSources sources) {
    return [sources = std::move(sources)](const std::string &path, int sizePx) -> std::optional<DecodedImage> {
        const std::string_view name = std::string_view(path).substr(path.find_last_of('/') + 1);
        const vfs::FileKind kind = vfs::mimeTypeForFileName(name).kind;
        const ImageDecoder &decoder = kind == vfs::FileKind::kVideo ? sources.video
                                      : kind == vfs::FileKind::kApk ? sources.apk
                                                                    : sources.image;
        if (!decoder) {
            return std::nullopt;
        }
        return decoder(path, sizePx);
    };
}

std::optional<DecodedImage> decodeFileThumbnail(const std::string &path, int sizePx) {
    static const ImageDecoder decoder = fileThumbnailDecoder();
    return decoder(path, sizePx);
}

}  // namespace filepan::app
