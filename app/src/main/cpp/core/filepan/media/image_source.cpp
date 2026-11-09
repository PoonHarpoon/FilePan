// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/media/image_source.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>

#ifdef __ANDROID__
#include <android/bitmap.h>
#include <android/data_space.h>
#include <android/imagedecoder.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace filepan::media {

DecodeLimits decodeLimitsFor(int screenWidth, int screenHeight, int maxTexture) {
    const int texture = maxTexture > 0 ? maxTexture : 4096;
    const int longSide = std::max({screenWidth, screenHeight, 1});
    DecodeLimits limits;
    limits.maxDimension = std::clamp(longSide * 2, 1, texture);
    limits.maxAnimatedDimension = std::min(limits.maxDimension, DecodeLimits{}.maxAnimatedDimension);
    return limits;
}

std::pair<int, int> fitWithin(int width, int height, int maxDimension) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    const int longer = std::max(width, height);
    if (maxDimension <= 0 || longer <= maxDimension) {
        return {width, height};
    }
    const double scale = static_cast<double>(maxDimension) / longer;
    const auto scaled = [&](int side) {
        return std::clamp(static_cast<int>(std::lround(side * scale)), 1, maxDimension);
    };
    return {scaled(width), scaled(height)};
}

#ifdef __ANDROID__

namespace {

std::string decoderError(const char *what, int result) {
    const char *reason = AImageDecoder_resultToString(result);
    return std::string(what) + ": " + (reason != nullptr ? reason : std::to_string(result));
}

class AndroidImageSource final : public ImageSource {
public:
    static MediaResult<std::unique_ptr<ImageSource>> open(const MediaSource &source, const DecodeLimits &limits) {
        std::unique_ptr<AndroidImageSource> self(new AndroidImageSource);
        int created = ANDROID_IMAGE_DECODER_SUCCESS;
        if (source.bytes) {
            auto read = readAll(*source.bytes);
            if (!read) {
                return std::unexpected(read.error());
            }
            // The decoder reads from the buffer for as long as it lives.
            self->encoded_ = std::move(*read);
            created = AImageDecoder_createFromBuffer(self->encoded_.data(), self->encoded_.size(), &self->decoder_);
        } else {
            const int fd = ::open(source.path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                return std::unexpected(MediaError{std::string("Couldn't open the file: ") + std::strerror(errno)});
            }
            created = AImageDecoder_createFromFd(fd, &self->decoder_);
            ::close(fd);  // The decoder keeps its own reference.
        }
        if (created != ANDROID_IMAGE_DECODER_SUCCESS || self->decoder_ == nullptr) {
            return std::unexpected(MediaError{decoderError("Not an image this device can decode", created)});
        }
        if (auto prepared = self->prepare(limits); !prepared) {
            return std::unexpected(prepared.error());
        }
        return self;
    }

    ~AndroidImageSource() override {
        if (decoder_ != nullptr) {
            AImageDecoder_delete(decoder_);
        }
    }

    [[nodiscard]] const ImageInfo &info() const override { return info_; }

    MediaResult<std::optional<ImageFrame>> next() override {
        if (started_) {
            if (!info_.animated) {
                return std::nullopt;
            }
            // The frame just shown asked for its area to be cleared before the next one.
            if (clearRect_) {
                clear(*clearRect_);
                clearRect_.reset();
            }
            const int advanced = AImageDecoder_advanceFrame(decoder_);
            if (advanced == ANDROID_IMAGE_DECODER_FINISHED) {
                return std::nullopt;
            }
            if (advanced != ANDROID_IMAGE_DECODER_SUCCESS) {
                return std::unexpected(MediaError{decoderError("Couldn't decode the next frame", advanced)});
            }
        }
        started_ = true;
        const int decoded = AImageDecoder_decodeImage(decoder_, canvas_.data(), stride_, canvas_.size());
        // A truncated file still decodes what it has, the rest transparent: show that.
        if (decoded != ANDROID_IMAGE_DECODER_SUCCESS && decoded != ANDROID_IMAGE_DECODER_INCOMPLETE) {
            return std::unexpected(MediaError{decoderError("Couldn't decode the image", decoded)});
        }
        ImageFrame frame{info_.decodedWidth, info_.decodedHeight, {}, 0};
        if (info_.animated) {
            AImageDecoderFrameInfo *frameInfo = AImageDecoderFrameInfo_create();
            if (frameInfo != nullptr && AImageDecoder_getFrameInfo(decoder_, frameInfo) == ANDROID_IMAGE_DECODER_SUCCESS) {
                frame.durationNs = AImageDecoderFrameInfo_getDuration(frameInfo);
                if (AImageDecoderFrameInfo_getDisposeOp(frameInfo) == ANDROID_IMAGE_DECODER_DISPOSE_OP_BACKGROUND) {
                    clearRect_ = AImageDecoderFrameInfo_getFrameRect(frameInfo);
                }
            }
            AImageDecoderFrameInfo_delete(frameInfo);
        }
        copyOut(frame);
        return frame;
    }

    MediaResult<void> rewind() override {
        if (!info_.animated) {
            started_ = false;
            return {};
        }
        const int rewound = AImageDecoder_rewind(decoder_);
        if (rewound != ANDROID_IMAGE_DECODER_SUCCESS) {
            return std::unexpected(MediaError{decoderError("Couldn't restart the animation", rewound)});
        }
        started_ = false;
        clearRect_.reset();
        std::fill(canvas_.begin(), canvas_.end(), std::uint8_t{0});
        return {};
    }

private:
    AndroidImageSource() = default;

    // The whole file, a chunk at a time; a cancelled source fails the read part-way.
    static MediaResult<std::vector<std::uint8_t>> readAll(ByteSource &bytes) {
        auto size = bytes.size();
        if (!size) {
            return std::unexpected(size.error());
        }
        if (*size > kMaxStreamedImageBytes) {
            return std::unexpected(MediaError{"Too large to show here"});
        }
        constexpr std::size_t kChunk = 1 << 20;
        std::vector<std::uint8_t> data(static_cast<std::size_t>(*size));
        std::size_t done = 0;
        while (done < data.size()) {
            const std::size_t want = std::min(kChunk, data.size() - done);
            auto read = bytes.readAt(done, std::span(data).subspan(done, want));
            if (!read) {
                return std::unexpected(read.error());
            }
            done += *read;
            if (*read < want) {
                break;  // The file got shorter: decode what there is.
            }
        }
        data.resize(done);
        return data;
    }

    MediaResult<void> prepare(const DecodeLimits &limits) {
        const AImageDecoderHeaderInfo *header = AImageDecoder_getHeaderInfo(decoder_);
        info_.width = AImageDecoderHeaderInfo_getWidth(header);
        info_.height = AImageDecoderHeaderInfo_getHeight(header);
        if (info_.width <= 0 || info_.height <= 0) {
            return std::unexpected(MediaError{"The image has no size"});
        }
        opaque_ = AImageDecoderHeaderInfo_getAlphaFlags(header) == ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE;
        info_.animated = AImageDecoder_isAnimated(decoder_);
        if (info_.animated) {
            const std::int32_t repeat = AImageDecoder_getRepeatCount(decoder_);
            info_.repeatCount = repeat == ANDROID_IMAGE_DECODER_INFINITE ? -1 : std::max(repeat, 0);
        } else {
            info_.repeatCount = 0;
        }
        // Premultiplied (the default): unpremultiplied output can't always be scaled. copyOut
        // undoes it for the ImageStore, which only for translucent images costs anything.
        AImageDecoder_setAndroidBitmapFormat(decoder_, ANDROID_BITMAP_FORMAT_RGBA_8888);
        // The UI composites in sRGB; wide-gamut photos are mapped into it.
        AImageDecoder_setDataSpace(decoder_, ADATASPACE_SRGB);
        const auto [width, height] =
                fitWithin(info_.width, info_.height, info_.animated ? limits.maxAnimatedDimension : limits.maxDimension);
        if ((width != info_.width || height != info_.height) &&
            AImageDecoder_setTargetSize(decoder_, width, height) != ANDROID_IMAGE_DECODER_SUCCESS) {
            return std::unexpected(MediaError{"The image can't be decoded at a size this device can show"});
        }
        info_.decodedWidth = width;
        info_.decodedHeight = height;
        stride_ = AImageDecoder_getMinimumStride(decoder_);
        canvas_.assign(stride_ * static_cast<std::size_t>(height), 0);
        return {};
    }

    void clear(const ARect &rect) {
        const int left = std::clamp(rect.left, 0, info_.decodedWidth);
        const int right = std::clamp(rect.right, left, info_.decodedWidth);
        const int top = std::clamp(rect.top, 0, info_.decodedHeight);
        const int bottom = std::clamp(rect.bottom, top, info_.decodedHeight);
        for (int y = top; y < bottom; ++y) {
            std::fill_n(canvas_.data() + static_cast<std::size_t>(y) * stride_ + static_cast<std::size_t>(left) * 4,
                        static_cast<std::size_t>(right - left) * 4, std::uint8_t{0});
        }
    }

    // The canvas stays as decoded (the next frame builds on it); frames get a tight,
    // unpremultiplied copy.
    void copyOut(ImageFrame &frame) const {
        const std::size_t row = static_cast<std::size_t>(frame.width) * 4;
        frame.rgba.resize(row * static_cast<std::size_t>(frame.height));
        for (int y = 0; y < frame.height; ++y) {
            std::memcpy(frame.rgba.data() + static_cast<std::size_t>(y) * row,
                        canvas_.data() + static_cast<std::size_t>(y) * stride_, row);
        }
        if (opaque_) {
            return;
        }
        for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
            const unsigned a = frame.rgba[i + 3];
            if (a == 0 || a == 255) {
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                frame.rgba[i + c] = static_cast<std::uint8_t>(std::min(255u, (frame.rgba[i + c] * 255u + a / 2) / a));
            }
        }
    }

    std::vector<std::uint8_t> encoded_;  // A streamed file, which the decoder reads from.
    AImageDecoder *decoder_ = nullptr;
    ImageInfo info_;
    std::size_t stride_ = 0;
    std::vector<std::uint8_t> canvas_;
    bool opaque_ = false;
    bool started_ = false;
    std::optional<ARect> clearRect_;
};

}  // namespace

MediaResult<std::unique_ptr<ImageSource>> openImage(const MediaSource &source, const DecodeLimits &limits) {
    return AndroidImageSource::open(source, limits);
}

#else

MediaResult<std::unique_ptr<ImageSource>> openImage(const MediaSource &, const DecodeLimits &) {
    return std::unexpected(MediaError{"Images can only be decoded on Android"});
}

#endif

}  // namespace filepan::media
