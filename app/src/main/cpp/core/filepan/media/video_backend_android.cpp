// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The Android VideoBackend: the media NDK and AAudio.
//
// Video: an AMediaExtractor feeds an AMediaCodec that renders into an AImageReader
// (AIMAGE_FORMAT_PRIVATE, GPU sampled). The video thread renders an output buffer only while
// fewer than kMaxQueued frames wait for the UI, and polls the reader for the images it rendered.
// Each frame's AHardwareBuffer goes to the renderer, which deletes the AImage once the GPU is
// done with it; the reader outlives its images through a shared reference in each release.
//
// Audio: a second extractor and codec decode into a lock-free ring buffer that an AAudio
// stream's data callback plays from. The callback marks which media time it wrote at which
// stream frame; with AAudioStream_getTimestamp that gives the media time heard now, the
// player's master clock.
//
// A local file is read through its fd. A streamed one gives each extractor an AMediaDataSource
// of its own over the ByteSource, read from that extractor's thread; the player cancels the
// source before tearing down, so no read blocked on the network holds up joining the threads.

#ifdef __ANDROID__

#include <aaudio/AAudio.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <fcntl.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaDataSource.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>

#include "filepan/media/video_player.hpp"

namespace filepan::media {

namespace {

#define FILEPAN_MEDIA_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "FilePanMedia", __VA_ARGS__)

// Images the reader may have out at once: up to kMaxQueued waiting for the UI, the one on
// screen and up to two the renderer still holds for frames in flight, and one spare.
constexpr std::int32_t kMaxImages = 6;
constexpr std::size_t kMaxQueued = 2;
constexpr std::int64_t kDequeueTimeoutUs = 5'000;
constexpr auto kIdleWait = std::chrono::milliseconds(5);
// Audio buffered ahead of the output.
constexpr int kRingMilliseconds = 500;
constexpr std::int32_t kPcmEncodingFloat = 4;  // AudioFormat.ENCODING_PCM_FLOAT.

// Owns the image reader. Each frame handed to the renderer holds one, so the reader (and the
// buffers its images live in) outlives every image it gave out.
struct Reader {
    AImageReader *reader = nullptr;
    ~Reader() {
        if (reader != nullptr) {
            AImageReader_delete(reader);
        }
    }
};

struct Track {
    AMediaExtractor *extractor = nullptr;
    AMediaDataSource *data = nullptr;  // A streamed source's, which the extractor reads through.
    AMediaCodec *codec = nullptr;
    bool inputDone = false;

    void release() {
        if (codec != nullptr) {
            AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
            codec = nullptr;
        }
        if (extractor != nullptr) {
            AMediaExtractor_delete(extractor);
            extractor = nullptr;
        }
        // Only once the extractor is gone: it may read until then.
        if (data != nullptr) {
            AMediaDataSource_delete(data);
            data = nullptr;
        }
        inputDone = false;
    }
};

// Decoded audio between the decoder thread (the only writer) and the AAudio callback (the
// only reader). Indices count frames since the last reset and only grow.
class AudioRing {
public:
    // Not while either side runs.
    void reset(int channels, std::size_t capacityFrames) {
        channels_ = channels;
        capacity_ = std::max<std::size_t>(capacityFrames, 1);
        data_.assign(capacity_ * static_cast<std::size_t>(channels_), 0.0f);
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_relaxed);
    }
    void clear() { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }

    [[nodiscard]] std::int64_t readIndex() const { return read_.load(std::memory_order_acquire); }
    [[nodiscard]] std::int64_t writeIndex() const { return write_.load(std::memory_order_acquire); }
    [[nodiscard]] int channels() const { return channels_; }

    std::size_t write(const float *frames, std::size_t count) {
        const std::int64_t write = write_.load(std::memory_order_relaxed);
        const std::int64_t read = read_.load(std::memory_order_acquire);
        const std::size_t room = capacity_ - static_cast<std::size_t>(write - read);
        count = std::min(count, room);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t slot = static_cast<std::size_t>(write + static_cast<std::int64_t>(i)) % capacity_;
            std::copy_n(frames + i * static_cast<std::size_t>(channels_), channels_,
                        data_.data() + slot * static_cast<std::size_t>(channels_));
        }
        write_.store(write + static_cast<std::int64_t>(count), std::memory_order_release);
        return count;
    }

    std::size_t read(float *out, std::size_t count) {
        const std::int64_t read = read_.load(std::memory_order_relaxed);
        const std::int64_t write = write_.load(std::memory_order_acquire);
        count = std::min(count, static_cast<std::size_t>(write - read));
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t slot = static_cast<std::size_t>(read + static_cast<std::int64_t>(i)) % capacity_;
            std::copy_n(data_.data() + slot * static_cast<std::size_t>(channels_), channels_,
                        out + i * static_cast<std::size_t>(channels_));
        }
        read_.store(read + static_cast<std::int64_t>(count), std::memory_order_release);
        return count;
    }

private:
    std::vector<float> data_;
    std::size_t capacity_ = 1;
    int channels_ = 2;
    std::atomic<std::int64_t> read_{0};
    std::atomic<std::int64_t> write_{0};
};

struct QueuedFrame {
    std::int64_t ptsUs = 0;
    AImage *image = nullptr;
    AHardwareBuffer *buffer = nullptr;
    gfx::Rect crop;
};

class AndroidVideoBackend final : public VideoBackend {
public:
    ~AndroidVideoBackend() override { close(); }

    MediaResult<VideoInfo> open(const MediaSource &source) override {
        std::lock_guard op(opMutex_);
        closeLocked();
        if (source.bytes) {
            bytes_ = source.bytes;
        } else {
            fd_ = ::open(source.path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd_ < 0) {
                return std::unexpected(MediaError{std::string("Couldn't open the file: ") + std::strerror(errno)});
            }
            struct stat st{};
            if (::fstat(fd_, &st) != 0) {
                return std::unexpected(MediaError{"Couldn't read the file"});
            }
            fileSize_ = st.st_size;
        }

        VideoInfo info;
        if (auto video = openVideo(info); !video) {
            closeLocked();
            return std::unexpected(video.error());
        }
        // Audio is optional: a stream that won't decode leaves the video playing silently.
        info.hasAudio = openAudio(info);
        hasAudio_ = info.hasAudio;
        return info;
    }

    MediaResult<std::int64_t> seek(std::int64_t us) override {
        std::lock_guard op(opMutex_);
        if (video_.codec == nullptr) {
            return std::unexpected(MediaError{"The video isn't open"});
        }
        stopThreads();
        if (hasAudio_) {
            pauseStream(true);
            AMediaCodec_flush(audio_.codec);
        }
        AMediaCodec_flush(video_.codec);
        {
            std::lock_guard lock(mutex_);
            for (QueuedFrame &frame : queue_) {
                AImage_delete(frame.image);
            }
            queue_.clear();
            // Images rendered before the flush that the reader still has are stale.
            discard_ += rendered_;
            rendered_ = 0;
            acquireLocked();
            videoOutputDone_ = false;
            firstAfterSeek_ = true;
        }
        video_.inputDone = false;
        AMediaExtractor_seekTo(video_.extractor, us, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
        std::int64_t start = AMediaExtractor_getSampleTime(video_.extractor);
        if (start < 0) {
            start = us;  // Past the last sample: the video ends at once.
        }
        if (hasAudio_) {
            audio_.inputDone = false;
            AMediaExtractor_seekTo(audio_.extractor, start, AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
            ring_.clear();
            pcm_.clear();
            pcmOffset_ = 0;
            audioOutputDone_ = false;
            audioDrained_.store(false, std::memory_order_release);
            baseSet_ = false;
            markSeq_.store(0, std::memory_order_release);
        }
        startThreads();
        if (hasAudio_ && playing_.load(std::memory_order_acquire)) {
            pauseStream(false);
        }
        return start;
    }

    void close() override {
        std::lock_guard op(opMutex_);
        closeLocked();
    }

    void setPlaying(bool playing) override {
        playing_.store(playing, std::memory_order_release);
        if (hasAudio_) {
            pauseStream(!playing);
        }
    }

    void setMuted(bool muted) override { muted_.store(muted, std::memory_order_release); }

    void setClockHint(std::int64_t us) override { clockHint_.store(us, std::memory_order_relaxed); }

    void setOnFrame(std::function<void()> onFrame) override {
        std::lock_guard lock(callbackMutex_);
        onFrame_ = std::move(onFrame);
    }

    std::vector<std::int64_t> queuedFrames() override {
        std::lock_guard lock(mutex_);
        std::vector<std::int64_t> pts;
        pts.reserve(queue_.size());
        for (const QueuedFrame &frame : queue_) {
            pts.push_back(frame.ptsUs);
        }
        return pts;
    }

    std::optional<gfx::vulkan::ExternalFrame> takeFrame(std::size_t drop) override {
        std::unique_lock lock(mutex_);
        for (std::size_t i = 0; i < drop && !queue_.empty(); ++i) {
            AImage_delete(queue_.front().image);
            queue_.pop_front();
        }
        if (queue_.empty()) {
            return std::nullopt;
        }
        QueuedFrame frame = queue_.front();
        queue_.pop_front();
        std::shared_ptr<Reader> reader = reader_;
        lock.unlock();
        space_.notify_all();
        return gfx::vulkan::ExternalFrame(frame.buffer, frame.crop,
                                          [image = frame.image, reader = std::move(reader)] {
                                              AImage_delete(image);
                                          });
    }

    bool videoEnded() override {
        std::lock_guard lock(mutex_);
        return videoOutputDone_ && rendered_ == 0 && queue_.empty();
    }

    std::optional<std::pair<std::int64_t, std::int64_t>> audioPosition() override {
        if (!hasAudio_ || !playing_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        AudioMark mark;
        if (!readMark(mark)) {
            return std::nullopt;
        }
        std::lock_guard lock(streamMutex_);
        if (stream_ == nullptr) {
            return std::nullopt;
        }
        std::int64_t framePosition = 0;
        std::int64_t timeNs = 0;
        if (AAudioStream_getTimestamp(stream_, CLOCK_MONOTONIC, &framePosition, &timeNs) != AAUDIO_OK) {
            return std::nullopt;
        }
        return std::pair{audioMediaTime(mark, framePosition, sampleRate_), timeNs};
    }

    bool audioEnded() override {
        return !hasAudio_ || audioFailed_.load(std::memory_order_acquire) ||
               (audioOutputDone_.load(std::memory_order_acquire) && audioDrained_.load(std::memory_order_acquire));
    }

private:
    // Points the track's new extractor at the file: the fd, or for a streamed source a data
    // source of the track's own (the extractors read from different threads).
    media_status_t setDataSource(Track &track) {
        if (!bytes_) {
            return AMediaExtractor_setDataSourceFd(track.extractor, fd_, 0, fileSize_);
        }
        track.data = AMediaDataSource_new();
        if (track.data == nullptr) {
            return AMEDIA_ERROR_UNKNOWN;
        }
        // bytes_ outlives the data source: both go in close, the data source first.
        AMediaDataSource_setUserdata(track.data, bytes_.get());
        AMediaDataSource_setReadAt(track.data, &AndroidVideoBackend::readAt);
        AMediaDataSource_setGetSize(track.data, &AndroidVideoBackend::getSize);
        AMediaDataSource_setClose(track.data, &AndroidVideoBackend::closeSource);
        return AMediaExtractor_setDataSourceCustom(track.extractor, track.data);
    }

    // The data source callbacks, on the extractor's threads. -1 is EOF or an error (a cancelled
    // read among them); the extractor treats both as the end.
    static ssize_t readAt(void *user, off64_t offset, void *buffer, size_t size) {
        if (size == 0) {
            return 0;
        }
        if (offset < 0) {
            return -1;
        }
        size = std::min<size_t>(size, std::numeric_limits<ssize_t>::max());
        auto read = static_cast<ByteSource *>(user)->readAt(
                static_cast<std::uint64_t>(offset), std::span(static_cast<std::uint8_t *>(buffer), size));
        if (!read || *read == 0) {
            return -1;
        }
        return static_cast<ssize_t>(*read);
    }

    static ssize_t getSize(void *user) {
        auto size = static_cast<ByteSource *>(user)->size();
        if (!size || *size > static_cast<std::uint64_t>(std::numeric_limits<ssize_t>::max())) {
            return -1;
        }
        return static_cast<ssize_t>(*size);
    }

    // Nothing to release: the backend owns the source and cancels nothing here, as close
    // also comes before a reopen (suspend). Teardown cancels it from the player.
    static void closeSource(void *) {}

    MediaResult<void> openVideo(VideoInfo &info) {
        video_.extractor = AMediaExtractor_new();
        if (setDataSource(video_) != AMEDIA_OK) {
            return std::unexpected(MediaError{"Not a video this device can read"});
        }
        AMediaFormat *format = nullptr;
        std::string mime;
        const std::size_t tracks = AMediaExtractor_getTrackCount(video_.extractor);
        for (std::size_t i = 0; i < tracks; ++i) {
            AMediaFormat *candidate = AMediaExtractor_getTrackFormat(video_.extractor, i);
            const char *type = nullptr;
            if (AMediaFormat_getString(candidate, AMEDIAFORMAT_KEY_MIME, &type) && type != nullptr &&
                std::string_view(type).starts_with("video/")) {
                mime = type;
                format = candidate;
                AMediaExtractor_selectTrack(video_.extractor, i);
                break;
            }
            AMediaFormat_delete(candidate);
        }
        if (format == nullptr) {
            return std::unexpected(MediaError{"The file has no video"});
        }
        struct Delete {
            AMediaFormat *format;
            ~Delete() { AMediaFormat_delete(format); }
        } guard{format};
        std::int32_t width = 0;
        std::int32_t height = 0;
        std::int32_t rotation = 0;
        std::int64_t duration = 0;
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_ROTATION, &rotation);
        AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &duration);
        if (width <= 0 || height <= 0) {
            return std::unexpected(MediaError{"The video has no size"});
        }
        info.width = width;
        info.height = height;
        info.rotation = ((rotation % 360) + 360) % 360 / 90 * 90;
        info.durationUs = std::max<std::int64_t>(duration, 0);
        // The viewer rotates the picture itself; the reader would ignore the codec's transform.
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_ROTATION, 0);

        auto reader = std::make_shared<Reader>();
        if (AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
                                      kMaxImages, &reader->reader) != AMEDIA_OK) {
            return std::unexpected(MediaError{"Couldn't set up video output"});
        }
        ANativeWindow *window = nullptr;  // Owned by the reader.
        if (AImageReader_getWindow(reader->reader, &window) != AMEDIA_OK) {
            return std::unexpected(MediaError{"Couldn't set up video output"});
        }
        video_.codec = AMediaCodec_createDecoderByType(mime.c_str());
        if (video_.codec == nullptr) {
            return std::unexpected(MediaError{"This device can't decode " + mime});
        }
        if (AMediaCodec_configure(video_.codec, format, window, nullptr, 0) != AMEDIA_OK ||
            AMediaCodec_start(video_.codec) != AMEDIA_OK) {
            return std::unexpected(MediaError{"This device can't decode this video"});
        }
        std::lock_guard lock(mutex_);
        reader_ = std::move(reader);
        return {};
    }

    bool openAudio(VideoInfo &info) {
        audio_.extractor = AMediaExtractor_new();
        if (setDataSource(audio_) != AMEDIA_OK) {
            audio_.release();
            return false;
        }
        AMediaFormat *format = nullptr;
        std::string mime;
        const std::size_t tracks = AMediaExtractor_getTrackCount(audio_.extractor);
        for (std::size_t i = 0; i < tracks; ++i) {
            AMediaFormat *candidate = AMediaExtractor_getTrackFormat(audio_.extractor, i);
            const char *type = nullptr;
            if (AMediaFormat_getString(candidate, AMEDIAFORMAT_KEY_MIME, &type) && type != nullptr &&
                std::string_view(type).starts_with("audio/")) {
                mime = type;
                format = candidate;
                AMediaExtractor_selectTrack(audio_.extractor, i);
                break;
            }
            AMediaFormat_delete(candidate);
        }
        if (format == nullptr) {
            audio_.release();
            return false;
        }
        std::int64_t duration = 0;
        if (AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &duration)) {
            info.durationUs = std::max(info.durationUs, duration);
        }
        // Float if the decoder will; 16-bit otherwise, converted here.
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PCM_ENCODING, kPcmEncodingFloat);
        audio_.codec = AMediaCodec_createDecoderByType(mime.c_str());
        const bool ok = audio_.codec != nullptr &&
                        AMediaCodec_configure(audio_.codec, format, nullptr, nullptr, 0) == AMEDIA_OK &&
                        AMediaCodec_start(audio_.codec) == AMEDIA_OK;
        AMediaFormat_delete(format);
        if (!ok) {
            FILEPAN_MEDIA_LOGW("no decoder for %s; playing without sound", mime.c_str());
            audio_.release();
            return false;
        }
        audioFailed_.store(false, std::memory_order_release);
        return true;
    }

    void closeLocked() {
        stopThreads();
        closeStream();
        video_.release();
        audio_.release();
        hasAudio_ = false;
        {
            std::lock_guard lock(mutex_);
            for (QueuedFrame &frame : queue_) {
                AImage_delete(frame.image);
            }
            queue_.clear();
            rendered_ = 0;
            discard_ = 0;
            videoOutputDone_ = false;
            // Deleted once the renderer has released its last image.
            reader_.reset();
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        // After the tracks, whose data sources read from it.
        bytes_.reset();
    }

    void startThreads() {
        stop_.store(false, std::memory_order_release);
        videoThread_ = std::thread([this] { videoLoop(); });
        if (hasAudio_) {
            audioThread_ = std::thread([this] { audioLoop(); });
        }
    }

    void stopThreads() {
        stop_.store(true, std::memory_order_release);
        space_.notify_all();
        if (videoThread_.joinable()) {
            videoThread_.join();
        }
        if (audioThread_.joinable()) {
            audioThread_.join();
        }
    }

    void notifyFrame() {
        std::function<void()> onFrame;
        {
            std::lock_guard lock(callbackMutex_);
            onFrame = onFrame_;
        }
        if (onFrame) {
            onFrame();
        }
    }

    // Moves the images the codec rendered from the reader into the queue. Under mutex_.
    bool acquireLocked() {
        bool acquired = false;
        while ((rendered_ > 0 || discard_ > 0) && reader_) {
            AImage *image = nullptr;
            // Not yet arrived, or every image is out (the renderer holds some): later.
            if (AImageReader_acquireNextImage(reader_->reader, &image) != AMEDIA_OK || image == nullptr) {
                break;
            }
            if (discard_ > 0) {
                --discard_;
                AImage_delete(image);
                continue;
            }
            --rendered_;
            std::int64_t timestampNs = 0;
            AHardwareBuffer *buffer = nullptr;
            AImageCropRect crop{};
            AImage_getTimestamp(image, &timestampNs);
            if (AImage_getHardwareBuffer(image, &buffer) != AMEDIA_OK || buffer == nullptr) {
                AImage_delete(image);
                continue;
            }
            gfx::Rect rect;
            if (AImage_getCropRect(image, &crop) == AMEDIA_OK && crop.right > crop.left && crop.bottom > crop.top) {
                rect = {static_cast<float>(crop.left), static_cast<float>(crop.top),
                        static_cast<float>(crop.right - crop.left), static_cast<float>(crop.bottom - crop.top)};
            }
            // MediaCodec stamps rendered buffers with their presentation time, in ns.
            queue_.push_back({timestampNs / 1000, image, buffer, rect});
            acquired = true;
        }
        return acquired;
    }

    // Queues one sample if the codec has room. Returns whether it did anything.
    static bool feed(Track &track) {
        if (track.inputDone) {
            return false;
        }
        const ssize_t index = AMediaCodec_dequeueInputBuffer(track.codec, 0);
        if (index < 0) {
            return false;
        }
        std::size_t capacity = 0;
        std::uint8_t *buffer = AMediaCodec_getInputBuffer(track.codec, static_cast<std::size_t>(index), &capacity);
        const ssize_t size =
                buffer == nullptr ? -1 : AMediaExtractor_readSampleData(track.extractor, buffer, capacity);
        if (size < 0) {
            AMediaCodec_queueInputBuffer(track.codec, static_cast<std::size_t>(index), 0, 0, 0,
                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            track.inputDone = true;
            return true;
        }
        const std::int64_t pts = AMediaExtractor_getSampleTime(track.extractor);
        AMediaCodec_queueInputBuffer(track.codec, static_cast<std::size_t>(index), 0, static_cast<std::size_t>(size),
                                     static_cast<std::uint64_t>(std::max<std::int64_t>(pts, 0)), 0);
        AMediaExtractor_advance(track.extractor);
        return true;
    }

    void videoLoop() {
        while (!stop_.load(std::memory_order_acquire)) {
            bool notify = false;
            bool full = false;
            {
                std::lock_guard lock(mutex_);
                notify = acquireLocked();
                full = queue_.size() + static_cast<std::size_t>(rendered_) >= kMaxQueued || videoOutputDone_;
            }
            if (notify) {
                notifyFrame();
            }
            while (feed(video_)) {
            }
            if (full) {
                std::unique_lock lock(mutex_);
                space_.wait_for(lock, kIdleWait);
                continue;
            }
            AMediaCodecBufferInfo info{};
            const ssize_t index = AMediaCodec_dequeueOutputBuffer(video_.codec, &info, kDequeueTimeoutUs);
            if (index >= 0) {
                const bool end = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
                bool render = info.size > 0 || !end;
                {
                    std::lock_guard lock(mutex_);
                    if (render && playing_.load(std::memory_order_acquire) &&
                        tooLateToRender(info.presentationTimeUs, clockHint_.load(std::memory_order_relaxed),
                                        firstAfterSeek_)) {
                        render = false;
                    }
                    if (render) {
                        ++rendered_;
                        firstAfterSeek_ = false;
                    }
                    if (end) {
                        videoOutputDone_ = true;
                    }
                }
                AMediaCodec_releaseOutputBuffer(video_.codec, static_cast<std::size_t>(index), render);
                if (end) {
                    notifyFrame();
                }
            } else if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER && index != AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED &&
                       index != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                FILEPAN_MEDIA_LOGW("video decoding failed: %zd", index);
                {
                    std::lock_guard lock(mutex_);
                    videoOutputDone_ = true;
                }
                notifyFrame();
            }
        }
    }

    // --- Audio ---

    void audioLoop() {
        while (!stop_.load(std::memory_order_acquire)) {
            if (streamLost_.exchange(false, std::memory_order_acq_rel)) {
                reopenStream();  // A headset unplugged, say: carry on on the new device.
            }
            while (feed(audio_)) {
            }
            // Decoded audio that didn't fit goes first.
            if (pcmOffset_ < pcm_.size()) {
                const std::size_t channels = static_cast<std::size_t>(ring_.channels());
                const std::size_t frames = (pcm_.size() - pcmOffset_) / channels;
                pcmOffset_ += ring_.write(pcm_.data() + pcmOffset_, frames) * channels;
                if (pcmOffset_ < pcm_.size()) {
                    std::this_thread::sleep_for(kIdleWait);
                    continue;
                }
            }
            if (audioOutputDone_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(kIdleWait * 4);
                continue;
            }
            AMediaCodecBufferInfo info{};
            const ssize_t index = AMediaCodec_dequeueOutputBuffer(audio_.codec, &info, kDequeueTimeoutUs);
            if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                outputFormatChanged();
                continue;
            }
            if (index < 0) {
                if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER && index != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                    FILEPAN_MEDIA_LOGW("audio decoding failed: %zd", index);
                    audioFailed_.store(true, std::memory_order_release);
                    audioOutputDone_.store(true, std::memory_order_release);
                }
                continue;
            }
            if (info.size > 0 && sampleRate_ > 0) {
                std::size_t capacity = 0;
                const std::uint8_t *data =
                        AMediaCodec_getOutputBuffer(audio_.codec, static_cast<std::size_t>(index), &capacity);
                if (data != nullptr && static_cast<std::size_t>(info.offset + info.size) <= capacity) {
                    convert(data + info.offset, static_cast<std::size_t>(info.size));
                    if (!baseSet_) {
                        baseIndex_.store(ring_.writeIndex(), std::memory_order_release);
                        basePts_.store(info.presentationTimeUs, std::memory_order_release);
                        baseSet_ = true;
                    }
                }
            }
            AMediaCodec_releaseOutputBuffer(audio_.codec, static_cast<std::size_t>(index), false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                audioOutputDone_.store(true, std::memory_order_release);
            }
        }
    }

    // Decoded PCM into pcm_ as float, at most stereo.
    void convert(const std::uint8_t *data, std::size_t bytes) {
        const std::size_t sourceChannels = static_cast<std::size_t>(std::max(sourceChannels_, 1));
        const std::size_t channels = static_cast<std::size_t>(ring_.channels());
        const std::size_t sampleBytes = floatPcm_ ? 4 : 2;
        const std::size_t frames = bytes / (sampleBytes * sourceChannels);
        pcm_.resize(frames * channels);
        pcmOffset_ = 0;
        const auto sample = [&](std::size_t frame, std::size_t channel) {
            const std::uint8_t *at = data + (frame * sourceChannels + channel) * sampleBytes;
            if (floatPcm_) {
                float value;
                std::memcpy(&value, at, 4);
                return value;
            }
            std::int16_t value;
            std::memcpy(&value, at, 2);
            return static_cast<float>(value) / 32768.0f;
        };
        for (std::size_t f = 0; f < frames; ++f) {
            if (sourceChannels == channels) {
                for (std::size_t c = 0; c < channels; ++c) {
                    pcm_[f * channels + c] = sample(f, c);
                }
            } else {
                // More than two channels: even ones left, odd ones right.
                float left = 0;
                float right = 0;
                for (std::size_t c = 0; c < sourceChannels; ++c) {
                    (c % 2 == 0 ? left : right) += sample(f, c);
                }
                const float half = static_cast<float>((sourceChannels + 1) / 2);
                pcm_[f * channels] = left / half;
                pcm_[f * channels + 1] = right / std::max(1.0f, static_cast<float>(sourceChannels / 2));
            }
        }
    }

    void outputFormatChanged() {
        AMediaFormat *format = AMediaCodec_getOutputFormat(audio_.codec);
        if (format == nullptr) {
            return;
        }
        std::int32_t rate = 0;
        std::int32_t channels = 0;
        std::int32_t encoding = 2;  // ENCODING_PCM_16BIT unless it says.
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_PCM_ENCODING, &encoding);
        AMediaFormat_delete(format);
        if (rate <= 0 || channels <= 0 || (encoding != 2 && encoding != kPcmEncodingFloat)) {
            FILEPAN_MEDIA_LOGW("unsupported audio output: %d Hz, %d channels, encoding %d", rate, channels, encoding);
            audioFailed_.store(true, std::memory_order_release);
            audioOutputDone_.store(true, std::memory_order_release);
            return;
        }
        floatPcm_ = encoding == kPcmEncodingFloat;
        sourceChannels_ = channels;
        {
            std::lock_guard lock(streamMutex_);
            if (rate == sampleRate_ && std::min(channels, 2) == ring_.channels() && stream_ != nullptr) {
                return;
            }
        }
        closeStream();
        {
            std::lock_guard lock(streamMutex_);
            sampleRate_ = rate;
        }
        ring_.reset(std::min(channels, 2), static_cast<std::size_t>(rate) * kRingMilliseconds / 1000);
        baseSet_ = false;
        markSeq_.store(0, std::memory_order_release);
        reopenStream();
    }

    void reopenStream() {
        closeStream();
        AAudioStreamBuilder *builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
            audioFailed_.store(true, std::memory_order_release);
            return;
        }
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, ring_.channels());
        AAudioStreamBuilder_setSampleRate(builder, sampleRate_);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_POWER_SAVING);
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
        AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MOVIE);
        AAudioStreamBuilder_setDataCallback(builder, &AndroidVideoBackend::dataCallback, this);
        AAudioStreamBuilder_setErrorCallback(builder, &AndroidVideoBackend::errorCallback, this);
        AAudioStream *stream = nullptr;
        const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (opened != AAUDIO_OK || stream == nullptr) {
            FILEPAN_MEDIA_LOGW("couldn't open the audio output: %s", AAudio_convertResultToText(opened));
            audioFailed_.store(true, std::memory_order_release);
            return;
        }
        std::lock_guard lock(streamMutex_);
        stream_ = stream;
        if (playing_.load(std::memory_order_acquire)) {
            AAudioStream_requestStart(stream_);
        }
    }

    void closeStream() {
        std::lock_guard lock(streamMutex_);
        if (stream_ != nullptr) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
    }

    // Pausing waits until the callback has stopped, then drops what the device buffered, so a
    // seek doesn't play stale audio.
    void pauseStream(bool pause) {
        std::lock_guard lock(streamMutex_);
        if (stream_ == nullptr) {
            return;
        }
        if (!pause) {
            AAudioStream_requestStart(stream_);
            return;
        }
        if (AAudioStream_requestPause(stream_) == AAUDIO_OK) {
            aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
            AAudioStream_waitForStateChange(stream_, AAUDIO_STREAM_STATE_PAUSING, &next, 100'000'000);
            AAudioStream_requestFlush(stream_);
        }
    }

    static aaudio_data_callback_result_t dataCallback(AAudioStream *stream, void *user, void *audioData,
                                                       std::int32_t numFrames) {
        auto *self = static_cast<AndroidVideoBackend *>(user);
        auto *out = static_cast<float *>(audioData);
        const std::size_t channels = static_cast<std::size_t>(self->ring_.channels());
        const std::int64_t written = AAudioStream_getFramesWritten(stream);
        const std::int64_t readIndex = self->ring_.readIndex();
        const std::size_t got = self->ring_.read(out, static_cast<std::size_t>(numFrames));
        std::fill(out + got * channels, out + static_cast<std::size_t>(numFrames) * channels, 0.0f);
        if (self->muted_.load(std::memory_order_acquire)) {
            std::fill(out, out + got * channels, 0.0f);  // Consumed all the same: the clock runs on.
        }
        if (got > 0 && self->sampleRate_ > 0) {
            const std::int64_t mediaUs =
                    self->basePts_.load(std::memory_order_acquire) +
                    (readIndex - self->baseIndex_.load(std::memory_order_acquire)) * 1'000'000 / self->sampleRate_;
            self->writeMark({written, mediaUs});
        } else if (self->audioOutputDone_.load(std::memory_order_acquire)) {
            self->audioDrained_.store(true, std::memory_order_release);
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void errorCallback(AAudioStream *, void *user, aaudio_result_t error) {
        // Not allowed to close the stream here; the audio thread reopens it.
        auto *self = static_cast<AndroidVideoBackend *>(user);
        if (error == AAUDIO_ERROR_DISCONNECTED) {
            self->streamLost_.store(true, std::memory_order_release);
        } else {
            self->audioFailed_.store(true, std::memory_order_release);
        }
    }

    // A seqlock: the callback writes, the UI thread reads, neither blocks.
    void writeMark(const AudioMark &mark) {
        const std::uint32_t seq = markSeq_.load(std::memory_order_relaxed);
        markSeq_.store(seq | 1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        markFrame_.store(mark.streamFrame, std::memory_order_relaxed);
        markUs_.store(mark.mediaUs, std::memory_order_relaxed);
        markSeq_.store((seq | 1) + 1, std::memory_order_release);
    }

    bool readMark(AudioMark &mark) const {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const std::uint32_t before = markSeq_.load(std::memory_order_acquire);
            if (before == 0) {
                return false;  // None since the last seek.
            }
            if (before & 1) {
                continue;
            }
            mark.streamFrame = markFrame_.load(std::memory_order_relaxed);
            mark.mediaUs = markUs_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (markSeq_.load(std::memory_order_relaxed) == before) {
                return true;
            }
        }
        return false;
    }

    // open, seek and close, one at a time.
    std::mutex opMutex_;
    int fd_ = -1;
    std::shared_ptr<ByteSource> bytes_;  // Or a streamed source.
    off64_t fileSize_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> muted_{false};
    std::atomic<std::int64_t> clockHint_{0};

    std::mutex callbackMutex_;
    std::function<void()> onFrame_;

    // Video.
    Track video_;
    std::thread videoThread_;
    std::mutex mutex_;  // The frame queue and the reader.
    std::condition_variable space_;
    std::shared_ptr<Reader> reader_;
    std::deque<QueuedFrame> queue_;
    int rendered_ = 0;  // Rendered into the reader, not acquired yet.
    int discard_ = 0;   // Stale images still in the reader after a seek.
    bool videoOutputDone_ = false;
    bool firstAfterSeek_ = true;

    // Audio.
    bool hasAudio_ = false;
    Track audio_;
    std::thread audioThread_;
    std::mutex streamMutex_;
    AAudioStream *stream_ = nullptr;
    AudioRing ring_;
    std::vector<float> pcm_;
    std::size_t pcmOffset_ = 0;
    bool floatPcm_ = false;
    int sourceChannels_ = 2;
    int sampleRate_ = 0;
    bool baseSet_ = false;
    std::atomic<std::int64_t> baseIndex_{0};
    std::atomic<std::int64_t> basePts_{0};
    std::atomic<std::uint32_t> markSeq_{0};
    std::atomic<std::int64_t> markFrame_{0};
    std::atomic<std::int64_t> markUs_{0};
    std::atomic<bool> audioOutputDone_{false};
    std::atomic<bool> audioDrained_{false};
    std::atomic<bool> audioFailed_{false};
    std::atomic<bool> streamLost_{false};
};

}  // namespace

std::shared_ptr<VideoBackend> createVideoBackend() { return std::make_shared<AndroidVideoBackend>(); }

}  // namespace filepan::media

#endif  // __ANDROID__
