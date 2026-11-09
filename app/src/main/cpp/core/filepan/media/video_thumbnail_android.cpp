// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The Android video frame grab for thumbnails (see video_thumbnail.hpp). Independent of the
// playback backend: its own extractor (the video track only, so no audio is read or decoded),
// codec and image reader, all released before it returns.
//
// Two ways to the frame, the second only if the first gives nothing:
//  - surface: the codec renders into an AImageReader of CPU-readable YUV_420_888 images, which
//    the codec fills in whatever layout it likes and AImage describes. No KEY_COLOR_FORMAT: a
//    decoder with a surface picks its own.
//  - buffer: no surface, KEY_COLOR_FORMAT asks for flexible YUV, and the output buffer's format
//    (MediaImage2, or stride, slice height and color format) says where the planes are. The way
//    MediaMetadataRetriever has long done it, and the one left when a vendor's decoder can't
//    write to a CPU-readable surface, or writes P010 to it.

#ifdef __ANDROID__

#include <android/hardware_buffer.h>
#include <fcntl.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "filepan/media/video_thumbnail.hpp"
#include "filepan/runtime/log.hpp"

namespace filepan::media {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::int64_t kDequeueTimeoutUs = 5'000;
constexpr auto kImagePoll = std::chrono::milliseconds(2);
// The surface's share of the deadline, so a stuck surface leaves the buffer way time to run.
constexpr double kSurfaceShare = 0.6;

std::counting_semaphore<kMaxConcurrentGrabs> grabSlots(kMaxConcurrentGrabs);

// Why a way gave no frame: the step and the media_status_t (or dequeue result) it failed with.
struct Failure {
    const char *step = "none";
    int status = 0;
};

struct Turn {
    Turn() { grabSlots.acquire(); }
    ~Turn() { grabSlots.release(); }
    Turn(const Turn &) = delete;
    Turn &operator=(const Turn &) = delete;
};

struct File {
    int fd = -1;
    AMediaExtractor *extractor = nullptr;
    AMediaFormat *format = nullptr;  // The video track's.
    std::string mime;
    std::int32_t width = 0;
    std::int32_t height = 0;
    int rotation = 0;
    std::int64_t durationUs = 0;
    YuvColor color;

    File() = default;
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    ~File() {
        if (format != nullptr) {
            AMediaFormat_delete(format);
        }
        if (extractor != nullptr) {
            AMediaExtractor_delete(extractor);
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

// One way's codec and reader, released in order: the image before its reader, the codec before
// the reader it renders into.
struct Session {
    AImageReader *reader = nullptr;
    AMediaCodec *codec = nullptr;
    AMediaFormat *format = nullptr;  // The configured copy.
    bool started = false;
    AImage *image = nullptr;

    Session() = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    ~Session() {
        if (image != nullptr) {
            AImage_delete(image);
        }
        if (codec != nullptr) {
            if (started) {
                AMediaCodec_stop(codec);
            }
            AMediaCodec_delete(codec);
        }
        if (reader != nullptr) {
            AImageReader_delete(reader);
        }
        if (format != nullptr) {
            AMediaFormat_delete(format);
        }
    }
};

std::int32_t getInt32(AMediaFormat *format, const char *key, std::int32_t fallback = 0) {
    std::int32_t value = 0;
    if (format == nullptr || !AMediaFormat_getInt32(format, key, &value)) {
        return fallback;
    }
    return value;
}

// Seeks to the sync frame closest to us, or the first frame when that fails. Returns whether
// there's a sample to read.
bool seekNear(AMediaExtractor *extractor, std::int64_t us) {
    if (us > 0 && AMediaExtractor_seekTo(extractor, us, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC) == AMEDIA_OK &&
        AMediaExtractor_getSampleTime(extractor) >= 0) {
        return true;
    }
    AMediaExtractor_seekTo(extractor, 0, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
    return AMediaExtractor_getSampleTime(extractor) >= 0;
}

std::optional<Failure> openFile(const std::string &path, File &file) {
    file.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat st{};
    if (file.fd < 0 || ::fstat(file.fd, &st) != 0) {
        return Failure{"open", -errno};
    }
    file.extractor = AMediaExtractor_new();
    if (file.extractor == nullptr) {
        return Failure{"extractor", AMEDIA_ERROR_UNKNOWN};
    }
    // The whole file: offset 0, its length.
    if (const media_status_t status = AMediaExtractor_setDataSourceFd(file.extractor, file.fd, 0, st.st_size);
        status != AMEDIA_OK) {
        return Failure{"setDataSourceFd", status};
    }
    const std::size_t tracks = AMediaExtractor_getTrackCount(file.extractor);
    for (std::size_t i = 0; i < tracks && file.format == nullptr; ++i) {
        AMediaFormat *candidate = AMediaExtractor_getTrackFormat(file.extractor, i);
        const char *type = nullptr;
        if (candidate != nullptr && AMediaFormat_getString(candidate, AMEDIAFORMAT_KEY_MIME, &type) &&
            type != nullptr && std::string_view(type).starts_with("video/") &&
            AMediaExtractor_selectTrack(file.extractor, i) == AMEDIA_OK) {
            file.mime = type;
            file.format = candidate;
        } else if (candidate != nullptr) {
            AMediaFormat_delete(candidate);
        }
    }
    if (file.format == nullptr) {
        return Failure{"videoTrack", static_cast<int>(tracks)};
    }
    file.width = getInt32(file.format, AMEDIAFORMAT_KEY_WIDTH);
    file.height = getInt32(file.format, AMEDIAFORMAT_KEY_HEIGHT);
    file.rotation = normalizeRotation(getInt32(file.format, AMEDIAFORMAT_KEY_ROTATION));
    AMediaFormat_getInt64(file.format, AMEDIAFORMAT_KEY_DURATION, &file.durationUs);
    if (file.width <= 0 || file.height <= 0) {
        return Failure{"size", 0};
    }
    file.color = yuvColorFor(getInt32(file.format, AMEDIAFORMAT_KEY_COLOR_STANDARD),
                             getInt32(file.format, AMEDIAFORMAT_KEY_COLOR_RANGE), file.width, file.height);
    return std::nullopt;
}

// The decoder's own idea of the color wins over the container's.
void updateColor(AMediaFormat *format, YuvColor &color, int width, int height) {
    if (format == nullptr) {
        return;
    }
    const std::int32_t standard = getInt32(format, AMEDIAFORMAT_KEY_COLOR_STANDARD);
    const std::int32_t range = getInt32(format, AMEDIAFORMAT_KEY_COLOR_RANGE);
    if (standard != 0 || range != 0) {
        const YuvColor reported = yuvColorFor(standard, range, width, height);
        color.matrix = standard != 0 ? reported.matrix : color.matrix;
        color.fullRange = range != 0 ? reported.fullRange : color.fullRange;
    }
}

std::optional<Failure> startCodec(const File &file, Session &session, ANativeWindow *window) {
    session.format = AMediaFormat_new();
    AMediaFormat_copy(session.format, file.format);
    // The rotation is applied when converting; CPU planes ignore the codec's.
    AMediaFormat_setInt32(session.format, AMEDIAFORMAT_KEY_ROTATION, 0);
    if (window == nullptr) {
        AMediaFormat_setInt32(session.format, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFormatYuv420Flexible);
    }
    session.codec = AMediaCodec_createDecoderByType(file.mime.c_str());
    if (session.codec == nullptr) {
        return Failure{"createDecoder", AMEDIA_ERROR_UNSUPPORTED};
    }
    if (const media_status_t status = AMediaCodec_configure(session.codec, session.format, window, nullptr, 0);
        status != AMEDIA_OK) {
        return Failure{"configure", status};
    }
    if (const media_status_t status = AMediaCodec_start(session.codec); status != AMEDIA_OK) {
        return Failure{"start", status};
    }
    session.started = true;
    return std::nullopt;
}

// The surface's newest image as a frame, or why not. NO_BUFFER_AVAILABLE: nothing yet.
std::optional<RgbaImage> convertImage(const AImage *image, YuvColor color, int rotation, int sizePx,
                                      Failure &failure) {
    std::int32_t format = 0;
    std::int32_t planes = 0;
    if (const media_status_t status = AImage_getFormat(image, &format); status != AMEDIA_OK) {
        failure = {"imageFormat", status};
        return std::nullopt;
    }
    if (format != AIMAGE_FORMAT_YUV_420_888 || AImage_getNumberOfPlanes(image, &planes) != AMEDIA_OK || planes < 3) {
        failure = {"imageFormat", format};
        return std::nullopt;
    }
    // A 10-bit decoder may fill the reader with P010, whose CPU planes AImage describes as 8-bit;
    // the buffer way reads it properly.
    AHardwareBuffer *hardware = nullptr;
    if (AImage_getHardwareBuffer(image, &hardware) == AMEDIA_OK && hardware != nullptr) {
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(hardware, &desc);
        if (desc.format == AHARDWAREBUFFER_FORMAT_YCbCr_P010) {
            failure = {"imageP010", static_cast<int>(desc.format)};
            return std::nullopt;
        }
    }
    YuvFrame frame;
    if (AImage_getWidth(image, &frame.width) != AMEDIA_OK || AImage_getHeight(image, &frame.height) != AMEDIA_OK) {
        failure = {"imageSize", 0};
        return std::nullopt;
    }
    AImageCropRect crop{};
    if (AImage_getCropRect(image, &crop) == AMEDIA_OK && crop.right > crop.left && crop.bottom > crop.top) {
        frame.cropLeft = crop.left;
        frame.cropTop = crop.top;
        frame.cropRight = crop.right;
        frame.cropBottom = crop.bottom;
        // A buffer smaller than the reader (the stream's size differs from the container's) is
        // as big as its crop.
        frame.width = std::min(frame.width, crop.right);
        frame.height = std::min(frame.height, crop.bottom);
    }
    YuvPlane *targets[] = {&frame.y, &frame.u, &frame.v};
    for (int i = 0; i < 3; ++i) {
        std::uint8_t *data = nullptr;
        int length = 0;
        if (const media_status_t status = AImage_getPlaneData(image, i, &data, &length);
            status != AMEDIA_OK || data == nullptr) {
            failure = {"planeData", status};
            return std::nullopt;
        }
        if (AImage_getPlaneRowStride(image, i, &targets[i]->rowStride) != AMEDIA_OK ||
            AImage_getPlanePixelStride(image, i, &targets[i]->pixelStride) != AMEDIA_OK) {
            failure = {"planeStride", i};
            return std::nullopt;
        }
        // The plane must hold its last row's last pixel; AImage maps chroma to w / 2 and h / 2.
        const int w = i == 0 ? frame.width : std::max(1, frame.width / 2);
        const int h = i == 0 ? frame.height : std::max(1, frame.height / 2);
        const std::int64_t needed = static_cast<std::int64_t>(h - 1) * targets[i]->rowStride +
                                    static_cast<std::int64_t>(w - 1) * targets[i]->pixelStride + 1;
        if (targets[i]->rowStride <= 0 || targets[i]->pixelStride <= 0 || length < needed) {
            failure = {"planeLength", length};
            return std::nullopt;
        }
        targets[i]->data = data;
    }
    // The renderer reads chroma up to (w + 1) / 2; keep an odd edge inside what's mapped.
    frame.width &= ~1;
    frame.height &= ~1;
    if (frame.cropRight > frame.width) {
        frame.cropRight = frame.width;
    }
    if (frame.cropBottom > frame.height) {
        frame.cropBottom = frame.height;
    }
    std::optional<RgbaImage> result = renderVideoThumbnail(frame, color, rotation, sizePx);
    if (!result) {
        failure = {"render", 0};
    }
    return result;
}

std::optional<RgbaImage> convertBuffer(AMediaCodec *codec, std::size_t index, const AMediaCodecBufferInfo &info,
                                       YuvColor color, int rotation, int sizePx, Failure &failure) {
    std::size_t capacity = 0;
    std::uint8_t *data = AMediaCodec_getOutputBuffer(codec, index, &capacity);
    if (data == nullptr || info.offset < 0 || static_cast<std::size_t>(info.offset) + info.size > capacity) {
        failure = {"outputBuffer", static_cast<int>(capacity)};
        return std::nullopt;
    }
    // The buffer's own format first: it has the MediaImage2.
    AMediaFormat *format = AMediaCodec_getBufferFormat(codec, index);
    if (format == nullptr) {
        format = AMediaCodec_getOutputFormat(codec);
    }
    if (format == nullptr) {
        failure = {"outputFormat", 0};
        return std::nullopt;
    }
    CodecBufferLayout layout;
    void *imageData = nullptr;
    std::size_t imageDataSize = 0;
    if (AMediaFormat_getBuffer(format, "image-data", &imageData, &imageDataSize) && imageData != nullptr) {
        layout.imageData = {static_cast<const std::uint8_t *>(imageData), imageDataSize};
    }
    layout.colorFormat = getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT);
    layout.width = getInt32(format, AMEDIAFORMAT_KEY_WIDTH);
    layout.height = getInt32(format, AMEDIAFORMAT_KEY_HEIGHT);
    layout.stride = getInt32(format, AMEDIAFORMAT_KEY_STRIDE);
    layout.sliceHeight = getInt32(format, AMEDIAFORMAT_KEY_SLICE_HEIGHT);
    AMediaFormat_getRect(format, AMEDIAFORMAT_KEY_DISPLAY_CROP, &layout.cropLeft, &layout.cropTop, &layout.cropRight,
                         &layout.cropBottom);
    updateColor(format, color, layout.width, layout.height);
    std::optional<RgbaImage> result;
    const std::span<const std::uint8_t> bytes(data + info.offset, static_cast<std::size_t>(info.size));
    if (const std::optional<YuvFrame> frame = codecBufferFrame(bytes, layout)) {
        result = renderVideoThumbnail(*frame, color, rotation, sizePx);
        if (!result) {
            failure = {"render", 0};
        }
    } else {
        failure = {"bufferLayout", layout.colorFormat};
    }
    AMediaFormat_delete(format);
    return result;
}

// Decodes from a sync frame near the seek time until a frame comes out: rendered into the reader
// (surface) or read from the output buffer. When the stream ends with no frame, tries again from
// the start.
std::optional<RgbaImage> decode(File &file, Session &session, Clock::time_point deadline, int sizePx,
                                Failure &failure) {
    const bool surface = session.reader != nullptr;
    YuvColor color = file.color;
    std::int64_t target = thumbnailSeekTimeUs(file.durationUs);
    bool samplesDone = !seekNear(file.extractor, target);
    bool eosQueued = false;
    bool rendered = false;
    while (Clock::now() < deadline) {
        if (rendered) {
            // A rendered frame reaches the reader asynchronously; meanwhile, keep decoding in
            // case the codec dropped it.
            const media_status_t status = AImageReader_acquireNextImage(session.reader, &session.image);
            if (status == AMEDIA_OK && session.image != nullptr) {
                return convertImage(session.image, color, file.rotation, sizePx, failure);
            }
            if (status != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
                failure = {"acquireNextImage", status};
                return std::nullopt;
            }
        }
        if (!eosQueued) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(session.codec, kDequeueTimeoutUs);
            if (index >= 0) {
                const auto at = static_cast<std::size_t>(index);
                std::size_t capacity = 0;
                std::uint8_t *buffer = AMediaCodec_getInputBuffer(session.codec, at, &capacity);
                const ssize_t size = samplesDone || buffer == nullptr
                                             ? -1
                                             : AMediaExtractor_readSampleData(file.extractor, buffer, capacity);
                if (size < 0) {
                    AMediaCodec_queueInputBuffer(session.codec, at, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    eosQueued = true;
                } else {
                    const std::int64_t time = std::max<std::int64_t>(AMediaExtractor_getSampleTime(file.extractor), 0);
                    AMediaCodec_queueInputBuffer(session.codec, at, 0, static_cast<std::size_t>(size),
                                                 static_cast<std::uint64_t>(time), 0);
                    samplesDone = !AMediaExtractor_advance(file.extractor);
                }
            } else if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                failure = {"dequeueInputBuffer", static_cast<int>(index)};
                return std::nullopt;
            }
        }
        AMediaCodecBufferInfo info{};
        const ssize_t output = AMediaCodec_dequeueOutputBuffer(session.codec, &info, kDequeueTimeoutUs);
        if (output == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *format = AMediaCodec_getOutputFormat(session.codec);
            updateColor(format, color, file.width, file.height);
            if (format != nullptr) {
                AMediaFormat_delete(format);
            }
            continue;
        }
        if (output == AMEDIACODEC_INFO_TRY_AGAIN_LATER || output == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            if (rendered) {
                std::this_thread::sleep_for(kImagePoll);
            }
            continue;
        }
        if (output < 0) {
            failure = {"dequeueOutputBuffer", static_cast<int>(output)};
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(output);
        const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        if ((info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0) {
            AMediaCodec_releaseOutputBuffer(session.codec, index, false);
            continue;
        }
        // A frame: one with data, or (to a surface, where some codecs report no size) any
        // buffer but a bare end of stream.
        if (info.size > 0 || (surface && !eos)) {
            if (surface) {
                AMediaCodec_releaseOutputBuffer(session.codec, index, true);
                rendered = true;
                if (!eos) {
                    continue;
                }
            } else {
                std::optional<RgbaImage> image =
                        convertBuffer(session.codec, index, info, color, file.rotation, sizePx, failure);
                AMediaCodec_releaseOutputBuffer(session.codec, index, false);
                return image;
            }
        } else {
            AMediaCodec_releaseOutputBuffer(session.codec, index, false);
        }
        if (eos && !rendered) {
            if (target == 0) {
                failure = {"noFrame", 0};
                return std::nullopt;
            }
            // Nothing decoded from near the target: try the first frame instead.
            target = 0;
            AMediaCodec_flush(session.codec);
            samplesDone = !seekNear(file.extractor, 0);
            eosQueued = false;
        } else if (eos) {
            eosQueued = true;  // Nothing more will come; wait for the rendered frame.
        }
    }
    failure = {rendered ? "timeoutImage" : "timeout", 0};
    return std::nullopt;
}

std::optional<RgbaImage> viaSurface(File &file, Clock::time_point deadline, int sizePx, Failure &failure) {
    Session session;
    // CPU-readable flexible YUV (what AImageReader_new gives too, spelled out). Two images: the
    // one being read and one more the codec may render meanwhile.
    if (const media_status_t status =
                AImageReader_newWithUsage(file.width, file.height, AIMAGE_FORMAT_YUV_420_888,
                                          AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 2, &session.reader);
        status != AMEDIA_OK) {
        failure = {"AImageReader_new", status};
        return std::nullopt;
    }
    ANativeWindow *window = nullptr;  // Owned by the reader.
    if (const media_status_t status = AImageReader_getWindow(session.reader, &window); status != AMEDIA_OK) {
        failure = {"getWindow", status};
        return std::nullopt;
    }
    if (const auto failed = startCodec(file, session, window)) {
        failure = *failed;
        return std::nullopt;
    }
    return decode(file, session, deadline, sizePx, failure);
}

std::optional<RgbaImage> viaBuffer(File &file, Clock::time_point deadline, int sizePx, Failure &failure) {
    Session session;
    if (const auto failed = startCodec(file, session, nullptr)) {
        failure = *failed;
        return std::nullopt;
    }
    return decode(file, session, deadline, sizePx, failure);
}

}  // namespace

std::optional<RgbaImage> grabVideoThumbnail(const std::string &path, int sizePx, int timeoutMs) {
    if (sizePx <= 0) {
        return std::nullopt;
    }
    const Turn turn;
    const Clock::time_point start = Clock::now();
    const Clock::time_point deadline = start + std::chrono::milliseconds(timeoutMs);
    File file;
    if (const auto failed = openFile(path, file)) {
        FILEPAN_LOGW("Video thumbnail: %s failed (%d): %s", failed->step, failed->status, path.c_str());
        return std::nullopt;
    }
    Failure surface;
    const auto surfaceDeadline =
            start + std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(timeoutMs) * kSurfaceShare);
    if (std::optional<RgbaImage> image = viaSurface(file, surfaceDeadline, sizePx, surface)) {
        return image;
    }
    Failure buffer;
    std::optional<RgbaImage> image;
    if (Clock::now() < deadline) {
        image = viaBuffer(file, deadline, sizePx, buffer);
    } else {
        buffer = {"noTimeLeft", 0};
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    if (!image) {
        FILEPAN_LOGW("Video thumbnail: surface %s (%d), buffer %s (%d), %s %dx%d, %lld ms: %s", surface.step,
                     surface.status, buffer.step, buffer.status, file.mime.c_str(), file.width, file.height,
                     static_cast<long long>(ms), path.c_str());
    } else {
        FILEPAN_LOGW("Video thumbnail: surface %s (%d), buffer way worked, %s: %s", surface.step, surface.status,
                     file.mime.c_str(), path.c_str());
    }
    return image;
}

}  // namespace filepan::media

#endif
