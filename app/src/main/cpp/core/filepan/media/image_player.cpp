// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/media/image_player.hpp"

#include <utility>

namespace filepan::media {

namespace {

struct Opened {
    MediaResult<std::shared_ptr<ImageSource>> source;
    MediaResult<std::optional<ImageFrame>> first;
};

}  // namespace

ImagePlayer::ImagePlayer(ui::UiLoop &loop, ui::IoPool &io, gfx::vulkan::ImageStore &images, ImageOpener opener)
    : loop_(loop), io_(io), images_(images), opener_(std::move(opener)) {}

ImagePlayer::~ImagePlayer() {
    if (ticking_) {
        ui::frameClock().stop(this);
    }
    if (job_) {
        job_->cancel();
    }
    if (bytes_) {
        bytes_->cancel();  // Unblocks a read the job is waiting on.
    }
    images_.remove(image_);
}

void ImagePlayer::open(const MediaSource &source, const DecodeLimits &limits) {
    if (busy_ || source_ || image_ != 0 || !opener_) {
        return;
    }
    bytes_ = source.bytes;
    if (bytes_) {
        bytes_->setActive(visible_);
    }
    busy_ = true;
    job_ = io_.submit(
            [opener = opener_, media = source, limits]() -> Opened {
                auto opened = opener(media, limits);
                if (!opened) {
                    return {std::unexpected(opened.error()), std::nullopt};
                }
                std::shared_ptr<ImageSource> source = std::move(*opened);
                auto first = source->next();
                return {std::move(source), std::move(first)};
            },
            [this](Opened opened) {
                busy_ = false;
                if (!opened.source) {
                    fail(opened.source.error().message);
                    return;
                }
                if (!opened.first) {
                    fail(opened.first.error().message);
                    return;
                }
                if (!*opened.first) {
                    fail("The image is empty");
                    return;
                }
                ImageFrame &frame = **opened.first;
                info_ = (*opened.source)->info();
                image_ = images_.add(frame.width, frame.height, std::move(frame.rgba));
                if (image_ == 0) {
                    fail("The image is empty");
                    return;
                }
                state_ = State::ready;
                if (info_.animated) {
                    source_ = std::move(*opened.source);
                    loopsLeft_ = info_.repeatCount;
                    const std::int64_t now = loop_.now();
                    scheduler_.start(frame.durationNs, now);
                    if (!visible_) {
                        scheduler_.pause(now);
                    }
                    decodeNext();
                }
                // A still's decoder is dropped with the job's reference.
                changed();
            });
}

void ImagePlayer::setVisible(bool visible) {
    if (visible == visible_) {
        return;
    }
    visible_ = visible;
    if (bytes_) {
        bytes_->setActive(visible);
    }
    if (scheduler_.started()) {
        const std::int64_t now = loop_.now();
        if (visible) {
            scheduler_.resume(now);
        } else {
            scheduler_.pause(now);
        }
    }
    updateTicking();
}

void ImagePlayer::decodeNext() {
    if (busy_ || !source_ || scheduler_.finished() || next_) {
        return;
    }
    busy_ = true;
    const bool mayLoop = loopsLeft_ != 0;
    job_ = io_.submit(
            [source = source_, mayLoop]() -> Decoded {
                Decoded decoded{source->next(), false};
                if (decoded.frame && !*decoded.frame && mayLoop) {
                    if (auto rewound = source->rewind(); !rewound) {
                        decoded.frame = std::unexpected(rewound.error());
                    } else {
                        decoded.looped = true;
                        decoded.frame = source->next();
                    }
                }
                return decoded;
            },
            [this](Decoded decoded) {
                busy_ = false;
                if (!decoded.frame || !*decoded.frame) {
                    // The end, or a broken frame mid-way: the last good frame stays.
                    scheduler_.finish();
                    source_.reset();
                    updateTicking();
                    return;
                }
                if (decoded.looped && loopsLeft_ > 0) {
                    --loopsLeft_;
                }
                scheduler_.nextReady((*decoded.frame)->durationNs);
                next_ = std::move(**decoded.frame);
                updateTicking();
            });
}

bool ImagePlayer::tick(std::int64_t nowNs) {
    if (scheduler_.tick(nowNs) && next_) {
        images_.update(image_, next_->width, next_->height, std::move(next_->rgba));
        next_.reset();
        changed();
        decodeNext();
    }
    // Waiting on a decode needs no frames; its result starts ticking again.
    ticking_ = scheduler_.wantsTicks();
    return ticking_;
}

void ImagePlayer::updateTicking() {
    const bool want = scheduler_.wantsTicks();
    if (want && !ticking_) {
        ticking_ = true;
        ui::frameClock().start(this);
    } else if (!want && ticking_) {
        ticking_ = false;
        ui::frameClock().stop(this);
    }
}

void ImagePlayer::fail(std::string message) {
    state_ = State::error;
    error_ = std::move(message);
    source_.reset();
    changed();
}

}  // namespace filepan::media
