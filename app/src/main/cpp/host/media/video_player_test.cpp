// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The A/V sync logic (MediaClock, frame picking) and VideoPlayer driven by a fake backend and a
// hand-run frame clock.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "filepan/media/av_sync.hpp"
#include "filepan/media/video_player.hpp"
#include "test_util.hpp"

namespace {

using filepan::gfx::vulkan::ExternalFrame;
using filepan::media::ByteSource;
using filepan::media::FramePick;
using filepan::media::MediaClock;
using filepan::media::MediaError;
using filepan::media::MediaResult;
using filepan::media::MediaSource;
using filepan::media::VideoBackend;
using filepan::media::VideoInfo;
using filepan::media::VideoPlayer;

constexpr std::int64_t kMs = 1'000'000;  // ns
constexpr std::int64_t kUsPerMs = 1'000;

void testMonotonicClock() {
    MediaClock clock;
    clock.reset(500 * kUsPerMs);
    EXPECT(clock.now(0) == 500 * kUsPerMs);
    EXPECT(clock.now(1000 * kMs) == 500 * kUsPerMs);  // Paused.
    clock.play(1000 * kMs);
    EXPECT(clock.now(1100 * kMs) == 600 * kUsPerMs);
    clock.pause(1200 * kMs);
    EXPECT(clock.now(5000 * kMs) == 700 * kUsPerMs);
    clock.play(6000 * kMs);
    EXPECT(clock.now(6050 * kMs) == 750 * kUsPerMs);
    // Audio reports mean nothing without audio.
    clock.audioPosition(0, 6050 * kMs);
    EXPECT(clock.now(6060 * kMs) == 760 * kUsPerMs);
    clock.reset(0);
    EXPECT(!clock.playing());
    EXPECT(clock.now(7000 * kMs) == 0);
}

void testAudioClock() {
    MediaClock clock(true);
    EXPECT(clock.audioDriven());
    clock.reset(0);
    clock.play(0);
    // Waiting for the output's first report: holds.
    EXPECT(clock.now(100 * kMs) == 0);
    clock.audioPosition(20 * kUsPerMs, 100 * kMs);
    EXPECT(clock.now(150 * kMs) == 70 * kUsPerMs);
    // Audio stalls: the clock runs on a little, then holds.
    EXPECT(clock.now(1000 * kMs) == 20 * kUsPerMs + MediaClock::kMaxExtrapolationUs);
    // A report behind what was shown doesn't take the clock back.
    clock.audioPosition(100 * kUsPerMs, 1000 * kMs);
    EXPECT(clock.now(1000 * kMs) == 220 * kUsPerMs);
    EXPECT(clock.now(1200 * kMs) == 300 * kUsPerMs);
    // Paused, reports are ignored; playing again waits for a fresh one.
    clock.pause(1200 * kMs);
    clock.audioPosition(900 * kUsPerMs, 1300 * kMs);
    EXPECT(clock.now(1300 * kMs) == 300 * kUsPerMs);
    clock.play(2000 * kMs);
    EXPECT(clock.now(2100 * kMs) == 300 * kUsPerMs);
    clock.audioPosition(310 * kUsPerMs, 2100 * kMs);
    EXPECT(clock.now(2110 * kMs) == 320 * kUsPerMs);
    // Audio ends before the video: monotonic from where it was.
    clock.audioEnded(2110 * kMs);
    EXPECT(!clock.audioDriven());
    EXPECT(clock.now(3110 * kMs) == 1320 * kUsPerMs);

    // An output that never reports: after the timeout the clock runs without it.
    MediaClock silent(true);
    silent.reset(1000);
    silent.play(0);
    EXPECT(silent.now(MediaClock::kAudioStartTimeoutNs - 1) == 1000);
    EXPECT(silent.now(MediaClock::kAudioStartTimeoutNs) == 1000);
    EXPECT(!silent.audioDriven());
    EXPECT(silent.now(MediaClock::kAudioStartTimeoutNs + 10 * kMs) == 1000 + 10 * kUsPerMs);
}

void testAudioMark() {
    using filepan::media::AudioMark;
    using filepan::media::audioMediaTime;
    const AudioMark mark{48000, 2'000'000};
    EXPECT(audioMediaTime(mark, 48000, 48000) == 2'000'000);
    EXPECT(audioMediaTime(mark, 72000, 48000) == 2'500'000);
    EXPECT(audioMediaTime(mark, 24000, 48000) == 1'500'000);  // Heard before the mark was written.
    EXPECT(audioMediaTime(mark, 0, 0) == 2'000'000);
}

void testPickFrame() {
    using filepan::media::pickFrame;
    using filepan::media::tooLateToRender;
    const std::vector<std::int64_t> queue{40'000, 80'000, 120'000};
    // Early: everything waits.
    FramePick pick = pickFrame(queue, 20'000, 8'000);
    EXPECT(!pick.show && pick.drop == 0);
    // Due within the tolerance.
    pick = pickFrame(queue, 33'000, 8'000);
    EXPECT(pick.show == 0u && pick.drop == 0);
    // Behind: the newest due frame shows and the ones before it drop.
    pick = pickFrame(queue, 125'000, 8'000);
    EXPECT(pick.show == 2u && pick.drop == 2);
    pick = pickFrame(queue, 85'000, 8'000);
    EXPECT(pick.show == 1u && pick.drop == 1);
    EXPECT(!pickFrame({}, 1'000'000, 8'000).show);
    EXPECT(tooLateToRender(0, 100'000, false));
    EXPECT(!tooLateToRender(0, 100'000, true));
    EXPECT(!tooLateToRender(30'000, 100'000, false));
}

// Frames every 40 ms with sync frames every 500 ms; up to three decoded ahead.
class FakeBackend final : public VideoBackend {
public:
    MediaResult<VideoInfo> open(const MediaSource &source) override {
        std::lock_guard lock(mutex_);
        ++opens;
        openedBytes = source.bytes;
        if (source.path == "/broken.mp4") {
            return std::unexpected(MediaError{"unsupported"});
        }
        return VideoInfo{64, 32, 90, kDurationUs, hasAudio};
    }
    MediaResult<std::int64_t> seek(std::int64_t us) override {
        std::lock_guard lock(mutex_);
        seeks.push_back(us);
        queue_.clear();
        next_ = us / 500'000 * 500'000;
        fill();
        return next_ - static_cast<std::int64_t>(queue_.size()) * kFrameUs;
    }
    void close() override {
        std::lock_guard lock(mutex_);
        ++closes;
        queue_.clear();
    }
    void setPlaying(bool p) override {
        std::lock_guard lock(mutex_);
        playing = p;
    }
    void setClockHint(std::int64_t) override {}
    void setOnFrame(std::function<void()>) override {}
    std::vector<std::int64_t> queuedFrames() override {
        std::lock_guard lock(mutex_);
        return {queue_.begin(), queue_.end()};
    }
    std::optional<ExternalFrame> takeFrame(std::size_t drop) override {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < drop && !queue_.empty(); ++i) {
            queue_.pop_front();
        }
        if (queue_.empty()) {
            return std::nullopt;
        }
        const std::int64_t pts = queue_.front();
        queue_.pop_front();
        taken.push_back(pts);
        fill();
        return ExternalFrame::fromPixels(1, 1, {255, 255, 255, 255});
    }
    bool videoEnded() override {
        std::lock_guard lock(mutex_);
        return queue_.empty() && next_ >= kDurationUs;
    }
    std::optional<std::pair<std::int64_t, std::int64_t>> audioPosition() override {
        std::lock_guard lock(mutex_);
        return playing ? audio : std::nullopt;
    }
    bool audioEnded() override {
        std::lock_guard lock(mutex_);
        return !hasAudio || audioDone;
    }

    static constexpr std::int64_t kDurationUs = 1'000'000;
    static constexpr std::int64_t kFrameUs = 40'000;
    std::mutex mutex_;
    bool hasAudio = false;
    bool audioDone = false;
    bool playing = false;
    std::optional<std::pair<std::int64_t, std::int64_t>> audio;
    int opens = 0;
    std::shared_ptr<ByteSource> openedBytes;
    int closes = 0;
    std::vector<std::int64_t> seeks;
    std::vector<std::int64_t> taken;

private:
    void fill() {
        while (queue_.size() < 3 && next_ < kDurationUs) {
            queue_.push_back(next_);
            next_ += kFrameUs;
        }
    }
    std::deque<std::int64_t> queue_;
    std::int64_t next_ = 0;
};

struct Rig {
    filepan::ui::UiLoop loop;
    filepan::ui::IoPool io{loop, 1};
    filepan::gfx::vulkan::ExternalImageStore images;
    std::shared_ptr<FakeBackend> backend = std::make_shared<FakeBackend>();
    std::int64_t now = 1'000 * kMs;
    int presented = 0;

    Rig() {
        loop.setClock([this] { return now; });
    }

    template <typename F>
    void pumpUntil(F condition) {
        for (int i = 0; i < 2000 && !condition(); ++i) {
            loop.runPending();
            if (!condition()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        EXPECT(condition());
    }

    // Advances fake time by ms, ticking the frame clock, and counts presented frames.
    void advance(std::int64_t ms) {
        now += ms * kMs;
        loop.runPending();
        filepan::ui::frameClock().tick(now);
        for (auto &op : images.takePending()) {
            presented += op.frame.has_value() ? 1 : 0;
        }
    }

    std::vector<std::int64_t> taken() {
        std::lock_guard lock(backend->mutex_);
        return std::exchange(backend->taken, {});
    }
};

void testPlayback() {
    Rig rig;
    VideoPlayer player(rig.loop, rig.io, rig.images, rig.backend);
    int changes = 0;
    player.setOnChange([&] { ++changes; });
    player.setVisible(true);
    player.open(MediaSource::file("/clip.mp4"));
    EXPECT(player.state() == VideoPlayer::State::opening);
    rig.pumpUntil([&] { return player.state() != VideoPlayer::State::opening; });
    EXPECT(player.state() == VideoPlayer::State::paused);
    EXPECT(player.info().rotation == 90);
    EXPECT(player.image() != 0);
    // The first frame shows though paused.
    rig.advance(16);
    EXPECT(rig.taken() == std::vector<std::int64_t>{0});
    EXPECT(rig.presented == 1);
    rig.advance(16);
    EXPECT(rig.taken().empty());
    EXPECT(!filepan::ui::frameClock().active());

    // Without audio the clock is monotonic: 100 ms in, frame 80 shows and 40 was late.
    player.play();
    EXPECT(player.state() == VideoPlayer::State::playing);
    rig.advance(100);
    EXPECT(rig.taken() == std::vector<std::int64_t>{80'000});
    EXPECT(player.droppedFrames() == 1);
    rig.advance(16);  // 116 ms: 120 is due within the tolerance.
    EXPECT(rig.taken() == std::vector<std::int64_t>{120'000});
    rig.advance(16);  // 132 ms: 160 isn't.
    EXPECT(rig.taken().empty());
    EXPECT(player.positionUs() == 132'000);

    player.pause();
    EXPECT(player.state() == VideoPlayer::State::paused);
    rig.advance(500);
    EXPECT(rig.taken().empty());
    EXPECT(player.positionUs() == 132'000);

    // Seeking lands on the sync frame before, and shows it while paused.
    player.seek(730'000);
    EXPECT(player.positionUs() == 730'000);
    rig.pumpUntil([&] { return player.positionUs() == 500'000; });
    rig.advance(16);
    EXPECT(rig.taken() == std::vector<std::int64_t>{500'000});

    // Playing to the end.
    player.play();
    for (int i = 0; i < 60 && player.state() == VideoPlayer::State::playing; ++i) {
        rig.advance(16);
    }
    EXPECT(player.state() == VideoPlayer::State::ended);
    EXPECT(player.positionUs() == FakeBackend::kDurationUs);
    EXPECT(!filepan::ui::frameClock().active());
    // Playing again starts over.
    rig.taken();
    player.play();
    rig.pumpUntil([&] { return player.state() == VideoPlayer::State::playing; });
    EXPECT(rig.backend->seeks.back() == 0);
    rig.advance(16);
    EXPECT(rig.taken() == std::vector<std::int64_t>{0});

    // Hidden: pauses.
    player.setVisible(false);
    EXPECT(player.state() == VideoPlayer::State::paused);
    EXPECT(changes > 0);
}

void testAudioMaster() {
    Rig rig;
    rig.backend->hasAudio = true;
    VideoPlayer player(rig.loop, rig.io, rig.images, rig.backend);
    player.setVisible(true);
    player.open(MediaSource::file("/clip.mp4"));
    rig.pumpUntil([&] { return player.state() == VideoPlayer::State::paused; });
    rig.advance(16);
    EXPECT(rig.taken() == std::vector<std::int64_t>{0});
    player.play();
    // No report from the output yet: video holds.
    rig.advance(100);
    rig.advance(100);
    EXPECT(rig.taken().empty());
    EXPECT(rig.backend->playing);
    // The output says 200 ms is heard now.
    {
        std::lock_guard lock(rig.backend->mutex_);
        rig.backend->audio = {{200'000, rig.now + 16 * kMs}};
    }
    rig.advance(16);
    EXPECT(rig.taken() == std::vector<std::int64_t>{120'000});  // All that's decoded ahead.
    rig.advance(16);
    EXPECT(rig.taken() == std::vector<std::int64_t>{200'000});
    // The output stalls: video stops soon after.
    for (int i = 0; i < 30; ++i) {
        rig.advance(16);
    }
    rig.taken();
    rig.advance(16);
    EXPECT(rig.taken().empty());
    EXPECT(player.positionUs() == 200'000 + MediaClock::kMaxExtrapolationUs);
    // Audio ends: the clock goes on without it.
    {
        std::lock_guard lock(rig.backend->mutex_);
        rig.backend->audioDone = true;
    }
    rig.advance(16);
    rig.advance(48);
    EXPECT(!rig.taken().empty());
}

void testSuspendAndErrors() {
    Rig rig;
    VideoPlayer player(rig.loop, rig.io, rig.images, rig.backend);
    player.setVisible(true);
    player.open(MediaSource::file("/clip.mp4"));
    rig.pumpUntil([&] { return player.state() == VideoPlayer::State::paused; });
    player.play();
    rig.advance(300);
    player.suspend();
    EXPECT(player.state() == VideoPlayer::State::paused);
    const std::int64_t at = player.positionUs();
    rig.pumpUntil([&] { return rig.backend->closes == 1; });
    rig.advance(16);
    EXPECT(!filepan::ui::frameClock().active());
    player.resume();
    rig.pumpUntil([&] { return rig.backend->opens == 2; });
    rig.pumpUntil([&] {
        std::lock_guard lock(rig.backend->mutex_);
        return rig.backend->seeks.size() == 2;
    });
    EXPECT(rig.backend->seeks.back() == at);
    rig.loop.runPending();
    EXPECT(player.state() == VideoPlayer::State::paused);

    VideoPlayer broken(rig.loop, rig.io, rig.images, rig.backend);
    broken.open(MediaSource::file("/broken.mp4"));
    rig.pumpUntil([&] { return broken.state() == VideoPlayer::State::error; });
    EXPECT(broken.error() == "unsupported");

    VideoPlayer none(rig.loop, rig.io, rig.images, nullptr);
    none.open(MediaSource::file("/clip.mp4"));
    EXPECT(none.state() == VideoPlayer::State::error);
}

// A streamed file: records what the player tells it.
class FakeBytes final : public ByteSource {
public:
    MediaResult<std::uint64_t> size() override { return 0; }
    MediaResult<std::size_t> readAt(std::uint64_t, std::span<std::uint8_t>) override { return 0; }
    void cancel() override { cancelled = true; }
    void setActive(bool now) override { active = now; }
    [[nodiscard]] bool waiting() const override { return blocked; }

    std::atomic<bool> cancelled{false};
    std::atomic<bool> active{false};
    std::atomic<bool> blocked{false};
};

void testStream() {
    Rig rig;
    auto bytes = std::make_shared<FakeBytes>();
    auto player = std::make_unique<VideoPlayer>(rig.loop, rig.io, rig.images, rig.backend);
    player->open(MediaSource::stream(bytes));
    EXPECT(!bytes->active);  // Neither visible nor playing.
    rig.pumpUntil([&] { return player->state() == VideoPlayer::State::paused; });
    {
        std::lock_guard lock(rig.backend->mutex_);
        EXPECT(rig.backend->openedBytes == bytes);
    }
    player->setVisible(true);
    EXPECT(bytes->active);
    // Waiting counts only while playing.
    bytes->blocked = true;
    EXPECT(!player->waiting());
    player->play();
    EXPECT(player->waiting());
    bytes->blocked = false;
    EXPECT(!player->waiting());
    player->setVisible(false);  // Pauses too.
    EXPECT(!bytes->active);
    player->play();
    EXPECT(bytes->active);
    // Suspending isn't the end: the source stays usable for resume.
    player->suspend();
    EXPECT(!bytes->active);
    rig.pumpUntil([&] { return rig.backend->closes == 1; });
    EXPECT(!bytes->cancelled);
    player->resume();
    rig.pumpUntil([&] { return rig.backend->opens == 2; });
    player.reset();
    EXPECT(bytes->cancelled);
}

}  // namespace

int main() {
    testMonotonicClock();
    testAudioClock();
    testAudioMark();
    testPickFrame();
    testPlayback();
    testAudioMaster();
    testSuspendAndErrors();
    testStream();
    std::printf("video_player_test passed\n");
    return 0;
}
