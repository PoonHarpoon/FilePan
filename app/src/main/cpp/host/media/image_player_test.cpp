// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// FrameScheduler timing, decode limits, and ImagePlayer driven by a fake ImageSource and a
// hand-run frame clock.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "filepan/media/frame_scheduler.hpp"
#include "filepan/media/image_player.hpp"
#include "filepan/media/image_source.hpp"
#include "test_util.hpp"

namespace {

using filepan::media::ByteSource;
using filepan::media::DecodeLimits;
using filepan::media::FrameScheduler;
using filepan::media::ImageFrame;
using filepan::media::ImageInfo;
using filepan::media::ImagePlayer;
using filepan::media::ImageSource;
using filepan::media::MediaError;
using filepan::media::MediaResult;
using filepan::media::MediaSource;

constexpr std::int64_t kMs = 1'000'000;

void testScheduler() {
    FrameScheduler s;
    EXPECT(!s.wantsTicks());
    s.start(100 * kMs, 0);
    EXPECT(s.deadline() == 100 * kMs);
    EXPECT(!s.wantsTicks());  // Nothing decoded yet.
    s.nextReady(40 * kMs);
    EXPECT(s.wantsTicks());
    EXPECT(!s.tick(99 * kMs));
    // A tick a little late keeps to the schedule.
    EXPECT(s.tick(108 * kMs));
    EXPECT(s.deadline() == 140 * kMs);
    EXPECT(s.shown() == 2);
    EXPECT(!s.tick(200 * kMs));  // Nothing waiting.

    // Decoded too late: shows at once, and the schedule restarts from then.
    s.nextReady(40 * kMs);
    EXPECT(s.tick(400 * kMs));
    EXPECT(s.deadline() == 440 * kMs);

    // GIF quirk: very short frames last 100 ms.
    s.nextReady(0);
    EXPECT(s.tick(440 * kMs));
    EXPECT(s.deadline() == 540 * kMs);
    EXPECT(FrameScheduler::effectiveDuration(10 * kMs) == 100 * kMs);
    EXPECT(FrameScheduler::effectiveDuration(20 * kMs) == 20 * kMs);

    // Pausing keeps what was left of the frame.
    s.nextReady(50 * kMs);
    s.pause(500 * kMs);
    EXPECT(s.paused() && !s.wantsTicks());
    EXPECT(!s.tick(600 * kMs));
    s.resume(1000 * kMs);
    EXPECT(s.deadline() == 1040 * kMs);
    EXPECT(!s.tick(1030 * kMs));
    EXPECT(s.tick(1040 * kMs));

    s.finish();
    EXPECT(s.finished() && !s.wantsTicks());
    s.nextReady(10 * kMs);  // Late results after the end start nothing... until restarted.
    EXPECT(!s.wantsTicks());
}

void testLimits() {
    using filepan::media::decodeLimitsFor;
    using filepan::media::fitWithin;
    EXPECT(decodeLimitsFor(1080, 2400).maxDimension == 4096);
    EXPECT(decodeLimitsFor(1080, 1920).maxDimension == 3840);
    EXPECT(decodeLimitsFor(1080, 2400, 8192).maxDimension == 4800);
    EXPECT(decodeLimitsFor(720, 1280).maxAnimatedDimension == 2048);
    EXPECT(decodeLimitsFor(400, 600).maxAnimatedDimension == 1200);
    EXPECT(fitWithin(8000, 6000, 4000) == std::pair(4000, 3000));
    EXPECT(fitWithin(6000, 8000, 4000) == std::pair(3000, 4000));
    EXPECT(fitWithin(300, 200, 4000) == std::pair(300, 200));  // Never upscaled.
    EXPECT(fitWithin(100000, 1, 1000) == std::pair(1000, 1));  // Never below 1 px.
}

// An animation of frameCount 2x1 frames whose first texel's red is the frame index.
class FakeSource final : public ImageSource {
public:
    FakeSource(int frameCount, int repeatCount, std::int64_t durationNs)
        : frameCount_(frameCount), duration_(durationNs) {
        info_ = {2, 1, 2, 1, frameCount > 1, repeatCount};
    }
    [[nodiscard]] const ImageInfo &info() const override { return info_; }
    MediaResult<std::optional<ImageFrame>> next() override {
        ++*decodes;
        if (failAt >= 0 && index_ == failAt) {
            return std::unexpected(MediaError{"broken frame"});
        }
        if (index_ >= frameCount_) {
            return std::nullopt;
        }
        ImageFrame frame{2, 1, std::vector<std::uint8_t>(8, 255), info_.animated ? duration_ : 0};
        frame.rgba[0] = static_cast<std::uint8_t>(index_++);
        return frame;
    }
    MediaResult<void> rewind() override {
        index_ = 0;
        return {};
    }

    int failAt = -1;
    std::shared_ptr<int> decodes = std::make_shared<int>(0);

private:
    ImageInfo info_;
    int frameCount_;
    std::int64_t duration_;
    int index_ = 0;
};

struct Rig {
    filepan::ui::UiLoop loop;
    filepan::ui::IoPool io{loop, 1};
    filepan::gfx::vulkan::ImageStore store;
    std::int64_t now = 0;

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

    // Runs frames every 10 ms of fake time up to `until`, letting decodes land in between.
    void runFrames(ImagePlayer &player, std::int64_t until) {
        while (now < until) {
            now += 10 * kMs;
            pumpUntil([&] { return !player.decoding(); });
            filepan::ui::frameClock().tick(now);
        }
    }

    // The red value of each upload and update the store has queued, in order.
    std::vector<int> takeFrames() {
        std::vector<int> frames;
        for (const auto &upload : store.takePending().uploads) {
            frames.push_back(upload.pixels[0]);
        }
        return frames;
    }
};

ImagePlayer::State waitOpened(Rig &rig, ImagePlayer &player) {
    rig.pumpUntil([&] { return player.state() != ImagePlayer::State::loading; });
    return player.state();
}

void testStill() {
    Rig rig;
    int changes = 0;
    std::shared_ptr<int> decodes;
    ImagePlayer player(rig.loop, rig.io, rig.store, [&](const MediaSource &media, const DecodeLimits &limits)
                               -> MediaResult<std::unique_ptr<ImageSource>> {
        EXPECT(media.path == "/still.png" && !media.bytes);
        EXPECT(limits.maxDimension == 1234);
        auto source = std::make_unique<FakeSource>(1, 0, 0);
        decodes = source->decodes;
        return source;
    });
    player.setOnChange([&] { ++changes; });
    player.open(MediaSource::file("/still.png"), {1234, 512});
    EXPECT(waitOpened(rig, player) == ImagePlayer::State::ready);
    EXPECT(player.image() != 0);
    EXPECT(changes == 1);
    EXPECT(!player.info().animated);
    player.setVisible(true);
    rig.runFrames(player, 500 * kMs);
    EXPECT(*decodes == 1);  // Decoded once; nothing ticks.
    EXPECT(rig.takeFrames() == std::vector<int>{0});
    EXPECT(!filepan::ui::frameClock().active());
}

void testAnimation() {
    Rig rig;
    std::shared_ptr<int> decodes;
    // Three frames of 100 ms, played twice (one repeat).
    ImagePlayer player(rig.loop, rig.io, rig.store, [&](const MediaSource &, const DecodeLimits &)
                               -> MediaResult<std::unique_ptr<ImageSource>> {
        auto source = std::make_unique<FakeSource>(3, 1, 100 * kMs);
        decodes = source->decodes;
        return source;
    });
    player.open(MediaSource::file("/a.gif"), {});
    EXPECT(waitOpened(rig, player) == ImagePlayer::State::ready);
    EXPECT(player.info().animated);
    rig.pumpUntil([&] { return !player.decoding(); });
    // Hidden: one frame decoded ahead, and nothing more.
    rig.runFrames(player, 300 * kMs);
    EXPECT(rig.takeFrames() == std::vector<int>{0});
    EXPECT(*decodes == 2);

    rig.now = 1000 * kMs;
    player.setVisible(true);
    rig.runFrames(player, 1050 * kMs);
    EXPECT(rig.takeFrames().empty());  // The first frame has its 100 ms.
    rig.runFrames(player, 1100 * kMs);
    EXPECT(rig.takeFrames() == std::vector<int>{1});
    rig.runFrames(player, 1250 * kMs);
    EXPECT(rig.takeFrames() == std::vector<int>{2});

    // Hidden mid-way: pauses.
    player.setVisible(false);
    rig.runFrames(player, 2000 * kMs);
    EXPECT(rig.takeFrames().empty());
    EXPECT(!filepan::ui::frameClock().active());
    player.setVisible(true);
    // The rest: the second play, then it stops on the last frame. (Updates the renderer hasn't
    // taken coalesce, so take them frame by frame.)
    std::vector<int> shown;
    while (rig.now < 3000 * kMs) {
        rig.runFrames(player, rig.now + 10 * kMs);
        for (int frame : rig.takeFrames()) {
            shown.push_back(frame);
        }
    }
    EXPECT(shown == (std::vector<int>{0, 1, 2}));
    EXPECT(player.scheduler().finished());
    EXPECT(player.scheduler().shown() == 6);
    EXPECT(!filepan::ui::frameClock().active());
}

void testErrors() {
    Rig rig;
    ImagePlayer failing(rig.loop, rig.io, rig.store, [](const MediaSource &, const DecodeLimits &)
                                -> MediaResult<std::unique_ptr<ImageSource>> {
        return std::unexpected(MediaError{"not an image"});
    });
    failing.open(MediaSource::file("/x"), {});
    EXPECT(waitOpened(rig, failing) == ImagePlayer::State::error);
    EXPECT(failing.error() == "not an image");
    EXPECT(failing.image() == 0);

    // A frame that fails mid-animation leaves the last good one up.
    ImagePlayer broken(rig.loop, rig.io, rig.store, [](const MediaSource &, const DecodeLimits &)
                               -> MediaResult<std::unique_ptr<ImageSource>> {
        auto source = std::make_unique<FakeSource>(4, -1, 50 * kMs);
        source->failAt = 2;
        return source;
    });
    broken.open(MediaSource::file("/b.webp"), {});
    EXPECT(waitOpened(rig, broken) == ImagePlayer::State::ready);
    EXPECT(rig.takeFrames() == std::vector<int>{0});
    broken.setVisible(true);
    rig.runFrames(broken, 500 * kMs);
    EXPECT(rig.takeFrames() == std::vector<int>{1});
    EXPECT(broken.state() == ImagePlayer::State::ready);
    EXPECT(broken.scheduler().finished());

    // Destroyed while decoding: the result is dropped, and the image freed.
    auto gone = std::make_unique<ImagePlayer>(rig.loop, rig.io, rig.store,
                                              [](const MediaSource &, const DecodeLimits &)
                                                      -> MediaResult<std::unique_ptr<ImageSource>> {
                                                  std::this_thread::sleep_for(std::chrono::milliseconds(20));
                                                  return std::make_unique<FakeSource>(1, 0, 0);
                                              });
    gone->open(MediaSource::file("/slow"), {});
    gone.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rig.loop.runPending();
    EXPECT(rig.store.takePending().uploads.empty());
}

// A streamed file: records what the player tells it.
class FakeBytes final : public ByteSource {
public:
    MediaResult<std::uint64_t> size() override { return 0; }
    MediaResult<std::size_t> readAt(std::uint64_t, std::span<std::uint8_t>) override { return 0; }
    void cancel() override { cancelled = true; }
    void setActive(bool now) override { active = now; }

    std::atomic<bool> cancelled{false};
    std::atomic<bool> active{false};
};

void testStream() {
    Rig rig;
    auto bytes = std::make_shared<FakeBytes>();
    std::atomic<bool> got{false};
    auto player = std::make_unique<ImagePlayer>(
            rig.loop, rig.io, rig.store,
            [&](const MediaSource &source, const DecodeLimits &) -> MediaResult<std::unique_ptr<ImageSource>> {
                got = source.bytes == bytes && source.path.empty();
                return std::make_unique<FakeSource>(1, 0, 0);
            });
    player->open(MediaSource::stream(bytes), {});
    EXPECT(waitOpened(rig, *player) == ImagePlayer::State::ready);
    EXPECT(got);
    EXPECT(!bytes->active);
    player->setVisible(true);
    EXPECT(bytes->active);
    player->setVisible(false);
    EXPECT(!bytes->active);
    EXPECT(!bytes->cancelled);
    player.reset();
    EXPECT(bytes->cancelled);
}

}  // namespace

int main() {
    testScheduler();
    testLimits();
    testStill();
    testAnimation();
    testErrors();
    testStream();
    std::printf("image_player_test passed\n");
    return 0;
}
