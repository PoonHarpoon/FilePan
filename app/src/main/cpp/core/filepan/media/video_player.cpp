// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/media/video_player.hpp"

#include <algorithm>

namespace filepan::media {

namespace {

struct Opened {
    MediaResult<VideoInfo> info;
    MediaResult<std::int64_t> start = 0;
};

}  // namespace

#ifndef __ANDROID__
std::shared_ptr<VideoBackend> createVideoBackend() { return nullptr; }
#endif

VideoPlayer::VideoPlayer(ui::UiLoop &loop, ui::IoPool &io, gfx::vulkan::ExternalImageStore &images,
                         std::shared_ptr<VideoBackend> backend)
    : loop_(loop), io_(io), images_(images), backend_(std::move(backend)), image_(images.create()) {
    if (backend_) {
        backend_->setOnFrame([&loop, alive = std::weak_ptr<bool>(alive_), this] {
            loop.post([alive, this] {
                if (alive.lock()) {
                    updateTicking();
                }
            });
        });
    }
}

VideoPlayer::~VideoPlayer() {
    *alive_ = false;
    alive_.reset();
    if (ticking_) {
        ui::frameClock().stop(this);
    }
    images_.destroy(image_);
    if (source_.bytes) {
        // Unblocks a read a decoder thread is waiting on, so the close below can't hang.
        source_.bytes->cancel();
    }
    if (backend_) {
        backend_->setOnFrame(nullptr);
        // Closed on the pool: whatever operation is running finishes first (the backend
        // serializes them), and nothing here waits for it.
        io_.execute([backend = std::move(backend_)] { backend->close(); });
    }
}

void VideoPlayer::open(const MediaSource &source) {
    if (requested_) {
        return;
    }
    requested_ = true;
    source_ = source;
    if (!backend_) {
        fail("Video can't play on this device");
        return;
    }
    wantOpen_ = true;
    updateActive();
    drive();
}

void VideoPlayer::drive() {
    if (busy_ || !backend_) {
        return;
    }
    std::weak_ptr<bool> alive = alive_;
    if (wantOpen_ && !opened_) {
        busy_ = true;
        io_.submit(
                [backend = backend_, source = source_, at = position_]() -> Opened {
                    auto info = backend->open(source);
                    if (!info) {
                        return {std::move(info), 0};
                    }
                    return {std::move(info), backend->seek(at)};
                },
                [this, alive](Opened opened) {
                    if (!alive.lock()) {
                        return;
                    }
                    busy_ = false;
                    opened_ = true;  // Even half-open, close cleans up.
                    if (!opened.info || !opened.start) {
                        fail(!opened.info ? opened.info.error().message : opened.start.error().message);
                        return;
                    }
                    if (wantOpen_) {
                        info_ = *opened.info;
                        clock_ = MediaClock(info_.hasAudio);
                        clock_.reset(*opened.start);
                        position_ = *opened.start;
                        owesFrame_ = true;
                        state_ = State::paused;
                        if (wantPlaying_) {
                            play();
                        }
                        changed();
                    }
                    updateTicking();
                    drive();
                });
    } else if (!wantOpen_ && opened_) {
        busy_ = true;
        io_.submit([backend = backend_] { backend->close(); },
                   [this, alive] {
                       if (!alive.lock()) {
                           return;
                       }
                       busy_ = false;
                       opened_ = false;
                       drive();
                   });
    } else if (wantOpen_ && opened_ && pendingSeek_) {
        busy_ = true;
        seeking_ = true;
        const std::int64_t target = *pendingSeek_;
        pendingSeek_.reset();
        updateTicking();
        io_.submit([backend = backend_, target] { return backend->seek(target); },
                   [this, alive](MediaResult<std::int64_t> start) {
                       if (!alive.lock()) {
                           return;
                       }
                       busy_ = false;
                       seeking_ = false;
                       if (!start) {
                           fail(start.error().message);
                           return;
                       }
                       clock_.reset(*start);
                       if (!pendingSeek_) {
                           position_ = *start;
                       }
                       owesFrame_ = true;
                       if (wantPlaying_) {
                           clock_.play(loop_.now());
                           backend_->setPlaying(true);
                           state_ = State::playing;
                       } else if (state_ == State::ended || state_ == State::playing) {
                           state_ = State::paused;
                       }
                       updateTicking();
                       changed();
                       drive();
                   });
    }
}

void VideoPlayer::play() {
    if (state_ == State::error) {
        return;
    }
    wantPlaying_ = true;
    updateActive();
    if (state_ == State::opening) {
        return;  // Starts once open.
    }
    if (state_ == State::ended) {
        seek(0);  // Plays from the start once there.
        return;
    }
    if (state_ != State::playing && opened_ && !seeking_) {
        clock_.play(loop_.now());
        backend_->setPlaying(true);
    }
    state_ = State::playing;
    updateTicking();
    changed();
}

void VideoPlayer::pause() {
    wantPlaying_ = false;
    updateActive();
    if (state_ != State::playing) {
        return;
    }
    if (opened_) {
        const std::int64_t now = loop_.now();
        clock_.pause(now);
        backend_->setPlaying(false);
        if (!seeking_ && !pendingSeek_) {
            position_ = clock_.now(now);
        }
    }
    state_ = State::paused;
    updateTicking();
    changed();
}

void VideoPlayer::togglePlaying() {
    if (state_ == State::playing) {
        pause();
    } else {
        play();
    }
}

void VideoPlayer::setMuted(bool muted) {
    muted_ = muted;
    if (backend_) {
        backend_->setMuted(muted);
    }
    changed();
}

void VideoPlayer::seek(std::int64_t us) {
    if (state_ == State::error || !backend_) {
        return;
    }
    us = std::max<std::int64_t>(us, 0);
    if (info_.durationUs > 0) {
        us = std::min(us, info_.durationUs);
    }
    position_ = us;
    if (state_ == State::ended) {
        state_ = State::paused;
    }
    if (opened_ || busy_) {
        pendingSeek_ = us;
        drive();
    }
    changed();
}

void VideoPlayer::setVisible(bool visible) {
    visible_ = visible;
    if (!visible) {
        pause();
    }
    updateActive();
    updateTicking();
}

void VideoPlayer::suspend() {
    if (!wantOpen_) {
        return;
    }
    pause();
    if (pendingSeek_) {
        position_ = *pendingSeek_;
        pendingSeek_.reset();
    }
    wantOpen_ = false;
    updateActive();
    updateTicking();
    drive();
}

void VideoPlayer::resume() {
    if (wantOpen_ || state_ == State::error || !requested_ || !backend_) {
        return;
    }
    wantOpen_ = true;
    updateActive();
    drive();
}

bool VideoPlayer::tick(std::int64_t nowNs) {
    if (opened_ && wantOpen_ && !seeking_ && !busy_) {
        if (auto audio = backend_->audioPosition()) {
            clock_.audioPosition(audio->first, audio->second);
        }
        if (clock_.audioDriven() && backend_->audioEnded()) {
            clock_.audioEnded(nowNs);
        }
        const std::int64_t clock = clock_.now(nowNs);
        backend_->setClockHint(clock);
        const std::vector<std::int64_t> queued = backend_->queuedFrames();
        FramePick pick = pickFrame(queued, clock, kPickToleranceUs);
        if (owesFrame_ && !pick.show && !queued.empty()) {
            pick = {0, 0};  // After a seek the first frame shows, even a little early.
        }
        bool presented = false;
        if (pick.show) {
            if (auto frame = backend_->takeFrame(pick.drop)) {
                dropped_ += pick.drop;
                ++shown_;
                images_.present(image_, std::move(*frame));
                hasFrame_ = true;
                owesFrame_ = false;
                presented = true;
            }
        }
        if (state_ == State::playing) {
            position_ = info_.durationUs > 0 ? std::min(clock, info_.durationUs) : clock;
            if (backend_->videoEnded() && (!info_.hasAudio || backend_->audioEnded())) {
                clock_.pause(nowNs);
                backend_->setPlaying(false);
                wantPlaying_ = false;
                state_ = State::ended;
                if (info_.durationUs > 0) {
                    position_ = info_.durationUs;
                }
            }
        }
        if (presented || state_ == State::playing || state_ == State::ended) {
            changed();
        }
    }
    ticking_ = visible_ && opened_ && wantOpen_ && !seeking_ && !busy_ &&
               (state_ == State::playing || owesFrame_);
    return ticking_;
}

void VideoPlayer::updateTicking() {
    const bool want = visible_ && opened_ && wantOpen_ && !seeking_ && !busy_ &&
                      (state_ == State::playing || owesFrame_);
    if (want && !ticking_) {
        ticking_ = true;
        ui::frameClock().start(this);
    } else if (!want && ticking_) {
        ticking_ = false;
        ui::frameClock().stop(this);
    }
}

bool VideoPlayer::waiting() const {
    return source_.bytes && state_ == State::playing && source_.bytes->waiting();
}

void VideoPlayer::updateActive() {
    const bool active = wantOpen_ && (visible_ || wantPlaying_);
    if (source_.bytes && active != active_) {
        source_.bytes->setActive(active);
    }
    active_ = active;
}

void VideoPlayer::fail(std::string message) {
    state_ = State::error;
    error_ = std::move(message);
    wantPlaying_ = false;
    wantOpen_ = false;  // Closes whatever opened.
    updateActive();
    updateTicking();
    drive();
    changed();
}

}  // namespace filepan::media
