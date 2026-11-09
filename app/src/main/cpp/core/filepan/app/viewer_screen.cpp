// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/viewer_screen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

#include "filepan/app/format.hpp"
#include "filepan/app/screen_parts.hpp"
#include "filepan/ui/components/recipes.hpp"

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;

constexpr float kTouchSlopDp = 8;
constexpr std::int64_t kTapNs = 400'000'000;
constexpr std::int64_t kDoubleTapNs = 300'000'000;
constexpr float kDoubleTapSlopDp = 32;
constexpr float kDoubleTapZoom = 2.5f;
constexpr float kSwipeFraction = 0.25f;  // Of the width, to turn the page on release.
constexpr float kFlingDp = 600;          // dp/s
constexpr float kEdgeResistance = 0.35f;
constexpr std::int64_t kHideControlsNs = 2'500'000'000;
constexpr float kMaxZoom = 4;
constexpr std::int64_t kSeekThrottleNs = 80'000'000;
constexpr float kPageTurnMs = 220;
// Remote items' blocks kept in memory between them; the rest go to disk.
constexpr std::size_t kRemoteMemoryBytes = 64 << 20;
// How often the current video is checked for waiting on the network, and how long it waits
// before the spinner shows.
constexpr std::int64_t kBufferPollNs = 100'000'000;
constexpr std::int64_t kBufferingNs = 300'000'000;

Style fill() {
    Style s;
    s.position = Position::absolute;
    s.left = 0;
    s.top = 0;
    s.right = 0;
    s.bottom = 0;
    return s;
}

float distance(gfx::Vec2 a, gfx::Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

// m:ss, or h:mm:ss past an hour.
std::string playbackTime(std::int64_t us) {
    const std::int64_t seconds = std::max<std::int64_t>(us, 0) / 1'000'000;
    char buffer[32];
    if (seconds >= 3600) {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld", static_cast<long long>(seconds / 3600),
                      static_cast<long long>(seconds / 60 % 60), static_cast<long long>(seconds % 60));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld", static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
    }
    return buffer;
}

// Media sits on black, whatever the theme; chrome over it is white.
constexpr gfx::Color kBlack{0, 0, 0, 1};
constexpr gfx::Color kWhite{1, 1, 1, 1};

gfx::Color backdrop(const Theme &) { return kBlack; }

// A scrim behind chrome over media: black at alpha fading out, from the top or the bottom.
void scrim(PaintContext &context, bool fromTop, float alpha) {
    const gfx::Rect b = context.bounds();
    context.list.fill(b, recipes::verticalGradient(b, {{0, kBlack.withAlpha(fromTop ? alpha : 0)},
                                                       {1, kBlack.withAlpha(fromTop ? 0 : alpha)}}));
}

enum class ButtonBody : std::uint8_t {
    none,   // Bare white icon: the top bar and the bottom bar.
    dim,    // On a black 35% circle: the side chevrons.
    glass,  // A glass circle: the centred play/pause.
};

struct ChromeButton {
    std::shared_ptr<Box> node;
    std::shared_ptr<Icon> icon;
};

// A round icon button for the chrome over media. The label is its spoken name.
ChromeButton chromeButton(const std::string &icon, const std::string &label, float size, float iconSize,
                          ButtonBody body, std::function<void()> onTap) {
    auto node = Box::create({.justify = Justify::center, .alignItems = Align::center, .width = size,
                             .height = size});
    auto glyph = Icon::create(icon, iconSize, kWhite, body == ButtonBody::glass);
    node->appendChild(glyph);
    auto pressable = makePressable(*node, {.onTap = std::move(onTap)});
    node->updateSemantics([&](Semantics &s) {
        s.role = Role::button;
        s.label = label;
    });
    Box *raw = node.get();
    node->setPaintHook([body, pressable](PaintContext &context) {
        const Theme &theme = currentTheme();
        const gfx::RoundedRect circle{context.bounds(), gfx::CornerRadii::uniform(theme.cornerPx(1e6f, context.density))};
        if (body == ButtonBody::dim) {
            context.list.fill(circle, kBlack.withAlpha(0.35f));
        } else if (body == ButtonBody::glass) {
            recipes::glass(context.list, circle, theme, context.density);
        }
        if (pressable->pressed.peek()) {
            recipes::pressedOverlay(context.list, circle, 0.14f);
        }
    });
    node->bind([raw, pressable] {
        (void)pressable->pressed();
        untrack([&] { raw->markPaintDirty(); });
    });
    return {node, glyph};
}

// The video seek track: 4 dp, filled in the accent up to a 14 dp accent knob. Tap or drag.
class SeekBar final : public Node {
public:
    SeekBar(std::function<float()> value, std::function<void(float)> onChange, std::function<std::string(float)> format)
        : value_(std::move(value)), onChange_(std::move(onChange)), format_(std::move(format)) {
        setDebugName("SeekBar");
        setStyle({.grow = 1, .shrink = 1, .basis = 0, .height = 28});
        auto tap = std::make_shared<TapRecognizer>();
        auto tapX = std::make_shared<float>(0);
        tap->onTapDown = [tapX](gfx::Vec2 local) { *tapX = local.x; };
        tap->onTap = [this, tapX] { setFromX(*tapX); };
        addGestureRecognizer(tap);
        auto drag = std::make_shared<DragRecognizer>(DragAxis::horizontal);
        drag->onStart = [this](gfx::Vec2 local) {
            finger_.start(local.x);
            setFromX(finger_.x());
        };
        drag->onUpdate = [this](gfx::Vec2 delta) { setFromX(finger_.update(delta.x)); };
        addGestureRecognizer(drag);
        bind([this] {
            const float value = value_();
            untrack([&] {
                updateSemantics([&](Semantics &s) {
                    s.role = Role::slider;
                    s.label = "Seek";
                    s.value = format_(value);
                    s.actions = Action::scrollForward | Action::scrollBackward;
                    s.onAction = [this](Action action, const ActionArgs &) {
                        if (action != Action::scrollForward && action != Action::scrollBackward) {
                            return false;
                        }
                        const float step = action == Action::scrollForward ? 0.05f : -0.05f;
                        onChange_(std::clamp(untrack([&] { return value_(); }) + step, 0.0f, 1.0f));
                        return true;
                    };
                });
                markPaintDirty();
            });
        });
    }

protected:
    void paint(PaintContext &context) override {
        const float value = std::clamp(untrack([&] { return value_(); }), 0.0f, 1.0f);
        const float knob = context.px(14);
        const float inset = knob / 2;
        const float height = context.px(4);
        const float y = context.size.height / 2;
        const float width = std::max(0.0f, context.size.width - 2 * inset);
        const Theme &theme = currentTheme();
        const gfx::Color accent = theme.colors.accent;
        const gfx::RoundedRect track{{inset, y - height / 2, width, height}, gfx::CornerRadii::uniform(height / 2)};
        context.list.fill(track, kWhite.withAlpha(0.25f));
        context.list.fill(gfx::RoundedRect{{inset, y - height / 2, width * value, height}, track.radii}, accent);
        const float x = inset + width * value;
        context.list.fill(gfx::RoundedRect{{x - knob / 2, y - knob / 2, knob, knob},
                                           gfx::CornerRadii::uniform(theme.cornerPx(knob, context.density))},
                          accent);
    }

private:
    void setFromX(float xPx) {
        const float density = environment().density.peek();
        const float inset = 7 * density;
        const float usable = std::max(1.0f, frame().width * density - 2 * inset);
        onChange_(std::clamp((xPx - inset) / usable, 0.0f, 1.0f));
    }

    std::function<float()> value_;
    std::function<void(float)> onChange_;
    std::function<std::string(float)> format_;
    FingerTracker finger_;
};

// Buffering: eight white dots in a ring over the play button's glass, the brightest going round
// once a second.
class Spinner final : public Node, private FrameClock::Ticker {
public:
    Spinner() {
        setDebugName("Spinner");
        setStyle(fill());
        setHitTestBehavior(HitTestBehavior::none);
    }
    ~Spinner() override { setSpinning(false); }
    Spinner(const Spinner &) = delete;
    Spinner &operator=(const Spinner &) = delete;

    void setSpinning(bool spinning) {
        if (spinning == spinning_) {
            return;
        }
        spinning_ = spinning;
        if (spinning) {
            frameClock().start(this);
        } else {
            frameClock().stop(this);
        }
        markPaintDirty();
    }

protected:
    void paint(PaintContext &context) override {
        if (!spinning_) {
            return;
        }
        constexpr int kDots = 8;
        const float radius = context.px(14);
        const float dot = context.px(5);
        const gfx::Vec2 centre{context.size.width / 2, context.size.height / 2};
        const int lead = static_cast<int>(frameClock().now() / (1'000'000'000 / kDots) % kDots);
        for (int i = 0; i < kDots; ++i) {
            const float angle = static_cast<float>(i) * 2 * std::numbers::pi_v<float> / kDots - std::numbers::pi_v<float> / 2;
            const int behind = (lead - i + kDots) % kDots;
            const float alpha = 1.0f - 0.8f * static_cast<float>(behind) / kDots;
            const gfx::Vec2 at{centre.x + radius * std::cos(angle), centre.y + radius * std::sin(angle)};
            context.list.fill(gfx::RoundedRect{{at.x - dot / 2, at.y - dot / 2, dot, dot}, gfx::CornerRadii::uniform(dot)},
                              kWhite.withAlpha(alpha));
        }
    }

private:
    bool tick(std::int64_t) override {
        markPaintDirty();
        return spinning_;
    }

    bool spinning_ = false;
};

}  // namespace

std::optional<MediaItem::Kind> viewerKind(const vfs::Entry &entry) {
    if (entry.targetType != vfs::EntryType::kFile) {
        return std::nullopt;
    }
    const vfs::MimeType mime = entry.mimeType();
    // Vector images, plain or gzipped (image/svg+xml-compressed), need a renderer the platform's
    // decoder doesn't have.
    if (mime.kind == vfs::FileKind::kImage && !mime.type.starts_with("image/svg+xml")) {
        return MediaItem::Kind::image;
    }
    if (mime.kind == vfs::FileKind::kVideo) {
        return MediaItem::Kind::video;
    }
    return std::nullopt;
}

// --- MediaPage ---

MediaPage::MediaPage(AppContext &context, MediaItem item, const ViewerOptions &options,
                     const media::DecodeLimits &limits, std::shared_ptr<media::ByteSource> bytes)
    : context_(context), item_(std::move(item)), options_(options), bytes_(std::move(bytes)) {
    setDebugName("MediaPage");
    setStyle(fill());
    updateSemantics([&](Semantics &s) {
        s.role = Role::image;
        s.label = item_.name;
    });
    if (item_.kind == MediaItem::Kind::image) {
        auto &services = context_.services;
        image_ = std::make_unique<media::ImagePlayer>(services.loop, services.io, services.images,
                                                      options_.imageOpener);
        image_->setOnChange([this] {
            if (image_->state() == media::ImagePlayer::State::error) {
                showError("Can't show this image. " + image_->error());
            }
            changed();
        });
        image_->open(source(), limits);
    }
}

MediaPage::~MediaPage() = default;

bool MediaPage::failed() const {
    return error_ != nullptr;
}

media::MediaSource MediaPage::source() const {
    return bytes_ ? media::MediaSource::stream(bytes_) : media::MediaSource::file(item_.path);
}

void MediaPage::setActive(bool active) {
    active_ = active;
    updateSemantics([&](Semantics &s) { s.hidden = !active; });
    if (image_) {
        image_->setVisible(active);
        return;
    }
    if (active && !video_ && !error_) {
        auto backend = options_.videoBackend ? options_.videoBackend() : nullptr;
        if (!backend || (backend->needsHardwareBuffers() && !context_.services.externalImages.supported())) {
            showError("Video can't play here on this device.");
            return;
        }
        auto &services = context_.services;
        video_ = std::make_unique<media::VideoPlayer>(services.loop, services.io, services.externalImages,
                                                      std::move(backend));
        video_->setOnChange([this] {
            if (video_->state() == media::VideoPlayer::State::error) {
                showError("Can't play this video. " + video_->error());
            }
            changed();
        });
        video_->setVisible(true);
        video_->open(source());
        video_->play();  // Video plays when its page comes up.
    } else if (video_) {
        video_->setVisible(active);
    }
}

gfx::Size MediaPage::mediaSize() const {
    if (image_ && image_->image() != 0) {
        return {static_cast<float>(image_->info().decodedWidth), static_cast<float>(image_->info().decodedHeight)};
    }
    if (video_ && video_->info().width > 0) {
        const auto &info = video_->info();
        const bool sideways = info.rotation == 90 || info.rotation == 270;
        return {static_cast<float>(sideways ? info.height : info.width),
                static_cast<float>(sideways ? info.width : info.height)};
    }
    return {};
}

gfx::Rect MediaPage::fitRect() const {
    const gfx::Size page = sizePx(environment().density.peek());
    const gfx::Size media = mediaSize();
    if (media.width <= 0 || media.height <= 0 || page.width <= 0 || page.height <= 0) {
        return {};
    }
    const float scale = std::min(page.width / media.width, page.height / media.height);
    const float w = media.width * scale;
    const float h = media.height * scale;
    return {(page.width - w) / 2, (page.height - h) / 2, w, h};
}

float MediaPage::maxScale() const {
    const gfx::Rect fit = fitRect();
    if (!image_ || fit.width <= 0) {
        return 1;
    }
    // Pinch goes to 4x, but never past 1 image px per screen px.
    return std::clamp(mediaSize().width / fit.width, 1.0f, kMaxZoom);
}

void MediaPage::zoomAbout(float scale, gfx::Vec2 focal) {
    if (!image_ || image_->image() == 0) {
        return;
    }
    const gfx::Size page = sizePx(environment().density.peek());
    const gfx::Vec2 centre{page.width / 2, page.height / 2};
    const float next = std::clamp(scale, 1.0f, maxScale());
    // The image point under focal stays there.
    const gfx::Vec2 point{(focal.x - centre.x - pan_.x) / scale_, (focal.y - centre.y - pan_.y) / scale_};
    pan_ = {focal.x - centre.x - point.x * next, focal.y - centre.y - point.y * next};
    scale_ = next;
    clampPan();
    changed();
}

void MediaPage::panBy(gfx::Vec2 deltaPx) {
    pan_ = {pan_.x + deltaPx.x, pan_.y + deltaPx.y};
    clampPan();
    changed();
}

void MediaPage::toggleZoom(gfx::Vec2 point) {
    if (zoomed()) {
        resetZoom();
    } else {
        zoomAbout(std::min(maxScale(), kDoubleTapZoom), point);
    }
}

void MediaPage::resetZoom() {
    if (scale_ == 1 && pan_.x == 0 && pan_.y == 0) {
        return;
    }
    scale_ = 1;
    pan_ = {};
    changed();
}

void MediaPage::clampPan() {
    const gfx::Size page = sizePx(environment().density.peek());
    const gfx::Rect fit = fitRect();
    const float spareX = std::max(0.0f, (fit.width * scale_ - page.width) / 2);
    const float spareY = std::max(0.0f, (fit.height * scale_ - page.height) / 2);
    pan_ = {std::clamp(pan_.x, -spareX, spareX), std::clamp(pan_.y, -spareY, spareY)};
}

void MediaPage::onSizeChanged() {
    clampPan();
    markPaintDirty();
}

void MediaPage::paint(PaintContext &context) {
    const gfx::Rect fit = fitRect();
    if (fit.empty()) {
        return;
    }
    const gfx::Vec2 centre{context.size.width / 2 + pan_.x, context.size.height / 2 + pan_.y};
    if (image_ && image_->image() != 0) {
        const float w = fit.width * scale_;
        const float h = fit.height * scale_;
        context.list.image(image_->image(), {centre.x - w / 2, centre.y - h / 2, w, h});
        return;
    }
    if (video_ && video_->hasFrame()) {
        // The picture is drawn as coded and turned to its rotation around the centre.
        const int rotation = video_->info().rotation;
        const bool sideways = rotation == 90 || rotation == 270;
        const float w = sideways ? fit.height : fit.width;
        const float h = sideways ? fit.width : fit.height;
        if (rotation != 0) {
            const float radians = static_cast<float>(rotation) * std::numbers::pi_v<float> / 180;
            const float c = std::cos(radians);
            const float s = std::sin(radians);
            context.list.pushTransform(gfx::Transform::translation(centre) * gfx::Transform{c, s, -s, c, 0, 0} *
                                       gfx::Transform::translation({-centre.x, -centre.y}));
        }
        context.list.externalImage(video_->image(), {centre.x - w / 2, centre.y - h / 2, w, h});
        if (rotation != 0) {
            context.list.pop();
        }
    }
}

void MediaPage::showError(const std::string &message) {
    if (error_) {
        return;
    }
    const Theme &t = currentTheme();
    auto panel = Box::create({.justify = Justify::center, .alignItems = Align::center, .gap = 14,
                              .padding = Edges::all(32), .position = Position::absolute, .left = 0, .top = 0,
                              .right = 0, .bottom = 0});
    panel->appendChild(Icon::create(item_.kind == MediaItem::Kind::image ? "broken_image" : "videocam_off", 40,
                                    ink(t, t.text.meta)));
    auto text = Text::create(message, t.type.body, ink(t, t.text.body));
    text->setAlign(gfx::TextAlign::center);
    text->setMaxLines(4);
    panel->appendChild(text);
    openWith_ = Button::create({.label = "Open with",
                                .variant = ButtonVariant::glass,
                                .icon = "open_in_new",
                                .onTap = [this] {
                                    // Through the actions, which copy a remote file out first.
                                    context_.actions.openWith({item_.directory, {item_.entry}});
                                }});
    panel->appendChild(openWith_);
    error_ = panel;
    appendChild(panel);
    updateSemantics([&](Semantics &s) { s.label = item_.name + ". " + message; });
}

void MediaPage::changed() {
    markPaintDirty();
    if (onChange_) {
        onChange_();
    }
}

// --- ViewerScreen ---

std::shared_ptr<ViewerScreen> ViewerScreen::create(AppContext &context, std::vector<MediaItem> items,
                                                   std::size_t index, ViewerOptions options,
                                                   std::function<void()> onClose) {
    auto viewer = std::make_shared<ViewerScreen>(context, std::move(items), index, std::move(options),
                                                 std::move(onClose));
    viewer->construct();
    return viewer;
}

ViewerScreen::ViewerScreen(AppContext &context, std::vector<MediaItem> items, std::size_t index,
                           ViewerOptions options, std::function<void()> onClose)
    : context_(context),
      loop_(context.services.loop),
      items_(std::move(items)),
      index_(std::min(index, items_.empty() ? std::size_t{0} : items_.size() - 1)),
      options_(std::move(options)),
      onClose_(std::move(onClose)),
      drag_(std::make_shared<AnimatedValue<float>>(0.0f)),
      chromeAlpha_(std::make_shared<AnimatedValue<float>>(1.0f)) {
    const gfx::Size window = environment().windowSize.peek();
    limits_ = media::decodeLimitsFor(static_cast<int>(window.width), static_cast<int>(window.height));
    remote_ = std::ranges::any_of(items_, [](const MediaItem &item) { return !item.location.isLocal(); });
    if (remote_) {
        const Settings &settings = context_.state.settings.peek();
        std::vector<RemoteMediaItem> remote;
        for (const MediaItem &item : items_) {
            remote.push_back({item.location, item.kind == MediaItem::Kind::video, item.entry.size});
        }
        const std::string &cacheDir = context_.services.cacheDir;
        cache_ = std::make_unique<RemoteMediaCache>(
                context_.services.backends, std::move(remote),
                RemoteMediaCacheOptions{
                        .around = settings.viewerPrefetch,
                        .videoHeadBytes = static_cast<std::uint64_t>(settings.viewerPrefetchVideoMb) << 20,
                        .concurrency = settings.remoteConnections,
                        .memoryBytes = kRemoteMemoryBytes,
                        .spillDirectory = cacheDir.empty() ? std::string() : cacheDir + kRemoteSpillDir,
                });
    }
}

ViewerScreen::~ViewerScreen() {
    for (auto timer : {hideTimer_, tapTimer_, seekTimer_, bufferTimer_}) {
        if (timer) {
            loop_.cancel(*timer);
        }
    }
}

void ViewerScreen::construct() {
    setDebugName("Viewer");
    setStyle(fill());
    setHitTestBehavior(HitTestBehavior::opaque);
    setPaintHook([](PaintContext &context) { context.list.fill(context.bounds(), backdrop(currentTheme())); });

    Node::build([&] {
        const Theme &t = currentTheme();
        ViewerScreen *self = this;

        stage_ = Box::create(fill());
        stage_->setDebugName("ViewerStage");
        stage_->setClip(true);
        stage_->setHitTestBehavior(HitTestBehavior::opaque);
        stage_->setPointerHandler([self](const PointerEvent &event) { self->pointer(event); });
        stage_->updateSemantics([self](Semantics &s) {
            s.role = Role::scroll;
            s.label = "Media viewer";
            s.onAction = [self](Action action, const ActionArgs &) {
                if (action == Action::scrollForward && self->index_ + 1 < self->items_.size()) {
                    self->goTo(self->index_ + 1, false);
                    return true;
                }
                if (action == Action::scrollBackward && self->index_ > 0) {
                    self->goTo(self->index_ - 1, false);
                    return true;
                }
                return false;
            };
        });
        appendChild(stage_);

        // Top: back, the name over its position and size, share, more. White on a black scrim.
        top_ = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 4,
                            .position = Position::absolute, .left = 0, .top = 0, .right = 0});
        top_->setDebugName("ViewerTop");
        top_->setPaintHook([](PaintContext &context) { scrim(context, true, 0.6f); });
        back_ = chromeButton("arrow_back", "Back", 48, 24, ButtonBody::none, [self] { self->close(); }).node;
        top_->appendChild(back_);
        auto names = Box::create({.gap = 2, .grow = 1, .shrink = 1, .basis = 0});
        title_ = Text::create(current().name, withWeight(withSize(t.type.body, 15), 550), kWhite);
        title_->setMaxLines(1);
        title_->setOverflow(gfx::TextOverflow::ellipsisMiddle);
        title_->updateSemantics([](Semantics &s) { s.role = Role::heading; });
        names->appendChild(title_);
        subtitle_ = Text::create("", monoText(t, 11, 500), kWhite.withAlpha(0.65f));
        subtitle_->setMaxLines(1);
        names->appendChild(subtitle_);
        top_->appendChild(names);
        share_ = chromeButton("share", "Share", 48, 24, ButtonBody::none,
                              [self] { self->context_.actions.share(self->targets()); })
                         .node;
        top_->appendChild(share_);
        more_ = chromeButton("more_vert", "More options", 48, 24, ButtonBody::none, [self] { self->showMore(); }).node;
        top_->appendChild(more_);
        appendChild(top_);

        // The sides: previous and next, besides swiping; hidden at the ends.
        previous_ = chromeButton("chevron_left", "Previous", 44, 24, ButtonBody::dim,
                                 [self] { self->goTo(self->index_ - 1); })
                            .node;
        previous_->updateStyle([](Style &s) {
            s.position = Position::absolute;
            s.left = 8;  // Centred vertically by the size binding below.
        });
        appendChild(previous_);
        next_ = chromeButton("chevron_right", "Next", 44, 24, ButtonBody::dim, [self] { self->goTo(self->index_ + 1); })
                        .node;
        next_->updateStyle([](Style &s) {
            s.position = Position::absolute;
            s.right = 8;
        });
        appendChild(next_);

        // Animations: a small tag, bottom left.
        gifTag_ = Box::create({.padding = {7, 4, 7, 4}, .position = Position::absolute, .left = 18, .bottom = 40},
                              {.background = gfx::Paint{kWhite.withAlpha(0.14f)},
                               .radii = gfx::CornerRadii::uniform(t.radii.tag)});
        gifTag_->appendChild(Text::create("GIF", withWeight(monoText(t, 10.5f), 700), kWhite));
        gifTag_->updateSemantics([](Semantics &s) { s.hidden = true; });  // The page says it's animated.
        appendChild(gifTag_);

        // Video: a centred play/pause, and a bottom bar with the time, the seek track, the
        // duration and mute.
        auto play = chromeButton("pause", "Pause", 72, 40, ButtonBody::glass, [self] {
            if (auto *video = self->currentVideo()) {
                video->togglePlaying();
                self->scheduleHide();
            }
        });
        play_ = play.node;
        playIcon_ = play.icon;
        play_->updateStyle([](Style &s) { s.position = Position::absolute; });
        spinner_ = std::make_shared<Spinner>();
        play_->appendChild(spinner_);
        appendChild(play_);

        controls_ = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12,
                                 .position = Position::absolute, .left = 0, .right = 0, .bottom = 0});
        controls_->setDebugName("ViewerControls");
        controls_->setPaintHook([](PaintContext &context) { scrim(context, false, 0.65f); });
        const gfx::TextStyle mono = monoText(t, 12, 500);
        time_ = Text::create("0:00", mono, kWhite);
        time_->updateSemantics([](Semantics &s) { s.hidden = true; });  // The seek bar reads it.
        controls_->appendChild(time_);
        seek_ = std::make_shared<SeekBar>(
                [self] { return self->progress_(); },
                [self](float value) {
                    self->progress_.set(value);
                    self->pendingSeek_ = value;
                    self->scheduleHide();
                    if (self->seekTimer_) {
                        return;
                    }
                    // Seeks go out at a steady pace while dragging; the last one lands.
                    std::weak_ptr<Node> weak = self->weak_from_this();
                    self->seekTimer_ = self->context_.services.loop.postDelayed(
                            [weak, self] {
                                if (!weak.lock()) {
                                    return;
                                }
                                self->seekTimer_.reset();
                                auto *video = self->currentVideo();
                                if (video != nullptr && self->pendingSeek_) {
                                    video->seek(static_cast<std::int64_t>(static_cast<double>(*self->pendingSeek_) *
                                                                          static_cast<double>(video->info().durationUs)));
                                }
                                self->pendingSeek_.reset();
                            },
                            kSeekThrottleNs);
                },
                [self](float value) {
                    auto *video = self->currentVideo();
                    const std::int64_t duration = video != nullptr ? video->info().durationUs : 0;
                    return playbackTime(static_cast<std::int64_t>(value * static_cast<float>(duration))) + " of " +
                           playbackTime(duration);
                });
        controls_->appendChild(seek_);
        duration_ = Text::create("0:00", mono, kWhite);
        duration_->updateSemantics([](Semantics &s) { s.hidden = true; });
        controls_->appendChild(duration_);
        auto mute = chromeButton("volume_up", "Mute", 36, 22, ButtonBody::none, [self] {
            if (auto *video = self->currentVideo()) {
                video->setMuted(!video->muted());
                self->scheduleHide();
            }
        });
        mute_ = mute.node;
        muteIcon_ = mute.icon;
        controls_->appendChild(mute_);
        appendChild(controls_);

        // --- Bindings ---
        stage_->bind([self] {
            (void)self->drag_->get();
            untrack([&] { self->layoutPages(); });
        });
        // Chrome fades; hidden, it takes no input and leaves semantics.
        top_->bind([self] {
            const float alpha = self->chromeAlpha_->get();
            const bool visible = self->chromeVisible_();
            untrack([&] {
                for (Node *node : {self->top_.get(), self->controls_.get(), self->previous_.get(), self->next_.get(),
                                   self->gifTag_.get(), self->play_.get()}) {
                    node->setOpacity(alpha);
                    node->setHitTestBehavior(visible ? HitTestBehavior::deferToChild : HitTestBehavior::none);
                    node->updateSemantics([&](Semantics &s) { s.hidden = !visible; });
                }
            });
        });
        play_->bind([self] {
            const bool playing = self->playing_();
            const bool buffering = self->buffering_();
            untrack([&] {
                self->playIcon_->setIcon(playing ? "pause" : "play_arrow");
                self->playIcon_->updateStyle([&](Style &s) { s.display = !buffering; });
                static_cast<Spinner &>(*self->spinner_).setSpinning(buffering);
                // Still a pause button while it buffers.
                self->play_->updateSemantics([&](Semantics &s) {
                    s.label = playing ? "Pause" : "Play";
                    s.value = buffering ? "Buffering" : "";
                });
            });
        });
        time_->bind([self] {
            const std::string text = self->timeText_();
            untrack([&] { self->time_->setText(text); });
        });
        duration_->bind([self] {
            const std::string text = self->durationText_();
            untrack([&] { self->duration_->setText(text); });
        });
        // Clear of the status and navigation bars; the side buttons and play centred.
        bind([self] {
            const float density = environment().density();
            const Insets safe = environment().safeArea();
            const gfx::Size window = environment().windowSize();
            untrack([&] {
                const float left = safe.left / density;
                const float right = safe.right / density;
                self->top_->updateStyle([&](Style &s) { s.padding = {6 + left, 8 + safe.top / density, 6 + right, 18}; });
                self->controls_->updateStyle(
                        [&](Style &s) { s.padding = {18 + left, 30, 18 + right, 12 + safe.bottom / density}; });
                const float middle = window.height / density / 2;
                self->previous_->updateStyle([&](Style &s) {
                    s.left = 8 + left;
                    s.top = middle - 22;
                });
                self->next_->updateStyle([&](Style &s) {
                    s.right = 8 + right;
                    s.top = middle - 22;
                });
                self->play_->updateStyle([&](Style &s) {
                    s.left = window.width / density / 2 - 36;
                    s.top = middle - 36;
                });
            });
        });
        // Mobile data mode on a metered network: nothing past the item shown is fetched ahead.
        bind([self, first = std::make_shared<bool>(true)] {
            const bool saving = self->context_.dataSaving();
            untrack([&] {
                if (self->cache_) {
                    self->cache_->setPaused(saving);
                }
                // Pages aren't made inside this effect, which would own their parts.
                if (!*first) {
                    self->context_.services.loop.post([weak = self->weak_from_this(), self] {
                        if (!weak.expired()) {
                            self->loadPages();
                        }
                    });
                }
                *first = false;
            });
        });
        // Video lets go of its decoders while the app is in the background.
        bind([self] {
            const bool resumed = self->context_.services.resumed();
            untrack([&] {
                for (auto &[i, page] : self->pages_) {
                    if (auto *video = page->video()) {
                        if (resumed) {
                            video->resume();
                        } else {
                            video->suspend();
                        }
                    }
                }
            });
        });
    });
    loadPages();
    updateChrome();
    if (remote_) {
        pollBuffering();
    }
}

void ViewerScreen::pollBuffering() {
    auto &loop = context_.services.loop;
    media::VideoPlayer *video = currentVideo();
    if (video != nullptr && video->waiting()) {
        if (!waitingSince_) {
            waitingSince_ = loop.now();
        }
        buffering_.set(loop.now() - *waitingSince_ >= kBufferingNs);
    } else {
        waitingSince_.reset();
        buffering_.set(false);
    }
    std::weak_ptr<Node> weak = weak_from_this();
    bufferTimer_ = loop.postDelayed(
            [weak, this] {
                if (weak.lock()) {
                    pollBuffering();
                }
            },
            kBufferPollNs);
}

std::shared_ptr<media::ByteSource> ViewerScreen::remoteBytes(std::size_t index) {
    return cache_ ? cache_->open(index) : nullptr;
}

void ViewerScreen::poolLimited(const vfs::PoolLimit &limit, const std::string &serverName) {
    // Once per server while it's open, and only for the server it shows.
    const bool here = std::ranges::any_of(items_, [&](const MediaItem &item) {
        return item.location.kind() == vfs::BackendKind::kSftp && item.location.serverId() == limit.serverId;
    });
    if (!here || !limited_.insert(limit.serverId).second) {
        return;
    }
    context_.notify(serverName + " refused another connection, probably a connection limit. Prefetching over " +
                    std::to_string(limit.working) + (limit.working == 1 ? " connection." : " connections."));
}

void ViewerScreen::close() {
    if (auto onClose = std::move(onClose_)) {
        onClose_ = nullptr;
        onClose();
    }
}

MediaPage *ViewerScreen::page(std::size_t index) const {
    const auto it = pages_.find(index);
    return it == pages_.end() ? nullptr : it->second.get();
}

media::VideoPlayer *ViewerScreen::currentVideo() const {
    MediaPage *page = currentPage();
    return page != nullptr ? page->video() : nullptr;
}

void ViewerScreen::loadPages() {
    if (items_.empty()) {
        return;
    }
    if (cache_) {
        // Remote items: the window moves, and what's ahead is prefetched.
        cache_->setIndex(index_);
    }
    const std::size_t first = index_ > 0 ? index_ - 1 : 0;
    const std::size_t last = std::min(items_.size() - 1, index_ + 1);
    for (auto it = pages_.begin(); it != pages_.end();) {
        if (it->first < first || it->first > last) {
            stage_->removeChild(it->second.get());
            it = pages_.erase(it);
        } else {
            ++it;
        }
    }
    // A page opens its file, so in mobile data mode a remote neighbour isn't made until it's shown.
    const bool saving = context_.dataSaving.peek();
    for (std::size_t i = first; i <= last; ++i) {
        if (saving && i != index_ && !items_[i].location.isLocal()) {
            continue;
        }
        if (!pages_.contains(i)) {
            auto page = std::make_shared<MediaPage>(context_, items_[i], options_, limits_, remoteBytes(i));
            MediaPage *raw = page.get();
            page->setOnChange([this, raw] { mediaChanged(*raw); });
            stage_->appendChild(page);
            pages_[i] = std::move(page);
        }
    }
    for (auto &[i, page] : pages_) {
        if (i != index_) {
            page->setActive(false);
        }
    }
    // The resume binding may have suspended things while away; a new current page starts now.
    if (MediaPage *current = currentPage()) {
        current->setActive(true);
        if (auto *video = current->video(); video != nullptr && !context_.services.resumed.peek()) {
            video->suspend();
        }
    }
    layoutPages();
}

void ViewerScreen::layoutPages() {
    const float width = stage_->frame().width;
    const float offset = drag_->peek();
    for (auto &[i, page] : pages_) {
        const float slot = (static_cast<float>(i) - static_cast<float>(index_)) * width;
        page->setTransform(gfx::Transform::translation({slot + offset, 0}));
    }
}

void ViewerScreen::onSizeChanged() { layoutPages(); }

void ViewerScreen::goTo(std::size_t index, bool animate) {
    if (index >= items_.size() || index == index_) {
        return;
    }
    const float width = stage_->frame().width;
    const bool neighbour = index + 1 == index_ || index == index_ + 1;
    if (animate && neighbour && width > 0) {
        const float target = index > index_ ? -width : width;
        std::weak_ptr<Node> weak = weak_from_this();
        turning_ = true;
        drag_->animateTo(target, Tween{.durationMs = kPageTurnMs}, [weak, this, index] {
            if (weak.lock()) {
                settle(index);
            }
        });
        return;
    }
    settle(index);
}

void ViewerScreen::settle(std::size_t index) {
    turning_ = false;
    if (MediaPage *old = currentPage()) {
        old->resetZoom();
    }
    index_ = index;
    drag_->snapTo(0);
    loadPages();
    updateChrome();
}

void ViewerScreen::updateChrome() {
    title_->setText(current().name);
    stage_->updateSemantics([&](Semantics &s) {
        s.value = std::to_string(index_ + 1) + " of " + std::to_string(items_.size());
        s.actions = 0;
        if (index_ + 1 < items_.size()) {
            s.actions = s.actions | Action::scrollForward;
        }
        if (index_ > 0) {
            s.actions = s.actions | Action::scrollBackward;
        }
    });
    subtitle_->setText(std::to_string(index_ + 1) + " of " + std::to_string(items_.size()) + " · " +
                       formatSize(current().entry.size));
    previous_->updateStyle([&](Style &s) { s.display = index_ > 0; });
    next_->updateStyle([&](Style &s) { s.display = index_ + 1 < items_.size(); });
    const bool video = current().kind == MediaItem::Kind::video;
    controls_->updateStyle([&](Style &s) { s.display = video; });
    play_->updateStyle([&](Style &s) { s.display = video; });
    gifTag_->updateStyle([&](Style &s) { s.display = false; });
    if (MediaPage *page = currentPage()) {
        mediaChanged(*page);
    }
    if (!video) {
        playing_.set(false);
    }
    scheduleHide();
}

FileTargets ViewerScreen::targets() const { return {current().directory, {current().entry}}; }

void ViewerScreen::showMore() {
    // Rename and delete change the folder under the viewer: it closes first and the browsing
    // list shows the result.
    AppContext &context = context_;
    const FileTargets here = targets();
    ViewerScreen *self = this;
    std::vector<MenuItem> items{
            {"open_in_new", "Open with", [&context, here] { context.actions.openWith(here); }},
            {"edit", "Rename",
             [&context, here, self] {
                 self->close();
                 context.actions.rename(here);
             }},
            {"delete", "Delete",
             [&context, here, self] {
                 self->close();
                 context.actions.remove(here);
             },
             true},
            {"info", "Properties", [&context, here] { context.actions.properties(here); }},
    };
    context_.overlays.showMenu(std::move(items), *more_);
}

void ViewerScreen::mediaChanged(MediaPage &page) {
    if (&page != currentPage()) {
        return;
    }
    if (media::ImagePlayer *image = page.image()) {
        const bool animated = image->state() == media::ImagePlayer::State::ready && image->info().animated;
        gifTag_->updateStyle([&](Style &s) { s.display = animated; });
    }
    media::VideoPlayer *video = page.video();
    if (video == nullptr) {
        return;
    }
    const bool playing = video->state() == media::VideoPlayer::State::playing;
    const bool wasPlaying = playing_.peek();
    playing_.set(playing);
    const std::int64_t duration = video->info().durationUs;
    if (!seekTimer_) {
        progress_.set(duration > 0 ? std::clamp(static_cast<float>(video->positionUs()) /
                                                        static_cast<float>(duration),
                                                0.0f, 1.0f)
                                   : 0.0f);
    }
    timeText_.set(playbackTime(video->positionUs()));
    durationText_.set(playbackTime(duration));
    muteIcon_->setIcon(video->muted() ? "volume_off" : "volume_up");
    mute_->updateSemantics([&](Semantics &s) { s.label = video->muted() ? "Unmute" : "Mute"; });
    if (video->state() == media::VideoPlayer::State::ended || video->state() == media::VideoPlayer::State::error) {
        setChromeVisible(true);
    } else if (playing && !wasPlaying) {
        scheduleHide();
    }
}

void ViewerScreen::setChromeVisible(bool visible) {
    if (visible == chromeVisible_.peek()) {
        if (visible) {
            scheduleHide();
        }
        return;
    }
    chromeVisible_.set(visible);
    chromeAlpha_->animateTo(visible ? 1.0f : 0.0f, Tween{.durationMs = 150, .crossfade = true});
    if (visible) {
        scheduleHide();
    }
}

void ViewerScreen::scheduleHide() {
    auto &loop = context_.services.loop;
    if (hideTimer_) {
        loop.cancel(*hideTimer_);
        hideTimer_.reset();
    }
    // Only playing video hides its controls, and never for a screen reader.
    media::VideoPlayer *video = currentVideo();
    if (video == nullptr || video->state() != media::VideoPlayer::State::playing ||
        context_.services.root.accessibilityEnabled() || !chromeVisible_.peek()) {
        return;
    }
    std::weak_ptr<Node> weak = weak_from_this();
    hideTimer_ = loop.postDelayed(
            [weak, this] {
                if (!weak.lock()) {
                    return;
                }
                hideTimer_.reset();
                media::VideoPlayer *playing = currentVideo();
                if (playing != nullptr && playing->state() == media::VideoPlayer::State::playing) {
                    setChromeVisible(false);
                }
            },
            kHideControlsNs);
}

// --- Gestures ---

void ViewerScreen::pointer(const PointerEvent &event) {
    const float density = environment().density.peek();
    const gfx::Vec2 local = event.local;
    switch (event.action) {
    case PointerAction::down: {
        if (turning_) {
            return;  // A page is turning; let it land.
        }
        if (drag_->animating()) {
            drag_->snapTo(drag_->peek());  // Caught mid spring-back.
        }
        pointers_[event.pointer] = local;
        if (pointers_.size() == 1) {
            mode_ = Mode::pending;
            downAt_ = local;
            downTimeNs_ = event.timeNs;
            lastPan_ = local;
            velocity_.reset();
            velocity_.add(event.timeNs, local);
        } else if (pointers_.size() == 2) {
            if (mode_ == Mode::swipe) {
                endSwipe();
            }
            MediaPage *page = currentPage();
            mode_ = Mode::none;
            if (page != nullptr && page->image() != nullptr && page->image()->image() != 0) {
                const gfx::Vec2 a = pointers_.begin()->second;
                const gfx::Vec2 b = std::next(pointers_.begin())->second;
                pinchDistance_ = std::max(1.0f, distance(a, b));
                pinchScale_ = page->scale();
                pinchFocal_ = {(a.x + b.x) / 2, (a.y + b.y) / 2};
                mode_ = Mode::pinch;
            }
        }
        return;
    }
    case PointerAction::move: {
        auto it = pointers_.find(event.pointer);
        if (it == pointers_.end()) {
            return;
        }
        it->second = local;
        MediaPage *page = currentPage();
        switch (mode_) {
        case Mode::pending: {
            velocity_.add(event.timeNs, local);
            const gfx::Vec2 d{local.x - downAt_.x, local.y - downAt_.y};
            if (std::hypot(d.x, d.y) <= kTouchSlopDp * density) {
                return;
            }
            if (page != nullptr && page->zoomed()) {
                mode_ = Mode::pan;
                lastPan_ = local;
            } else if (std::abs(d.x) > std::abs(d.y)) {
                mode_ = Mode::swipe;
            } else {
                mode_ = Mode::none;
            }
            return;
        }
        case Mode::swipe: {
            velocity_.add(event.timeNs, local);
            float dx = (local.x - downAt_.x) / density;
            const bool edge = (dx > 0 && index_ == 0) || (dx < 0 && index_ + 1 >= items_.size());
            if (edge) {
                dx *= kEdgeResistance;
            }
            drag_->snapTo(dx);
            return;
        }
        case Mode::pan:
            if (page != nullptr) {
                page->panBy({local.x - lastPan_.x, local.y - lastPan_.y});
            }
            lastPan_ = local;
            return;
        case Mode::pinch: {
            if (page == nullptr || pointers_.size() < 2) {
                return;
            }
            const gfx::Vec2 a = pointers_.begin()->second;
            const gfx::Vec2 b = std::next(pointers_.begin())->second;
            const gfx::Vec2 focal{(a.x + b.x) / 2, (a.y + b.y) / 2};
            page->zoomAbout(pinchScale_ * distance(a, b) / pinchDistance_, focal);
            page->panBy({focal.x - pinchFocal_.x, focal.y - pinchFocal_.y});
            pinchFocal_ = focal;
            return;
        }
        case Mode::none:
            return;
        }
        return;
    }
    case PointerAction::up:
    case PointerAction::cancel: {
        if (pointers_.erase(event.pointer) == 0) {
            return;
        }
        if (mode_ == Mode::pinch) {
            // The other finger stays down but does nothing more.
            mode_ = Mode::none;
            return;
        }
        if (mode_ == Mode::swipe) {
            endSwipe();
        } else if (mode_ == Mode::pending && event.action == PointerAction::up &&
                   event.timeNs - downTimeNs_ < kTapNs) {
            tapped(local);
        }
        if (pointers_.empty()) {
            mode_ = Mode::none;
        }
        return;
    }
    }
}

void ViewerScreen::tapped(gfx::Vec2 local) {
    MediaPage *page = currentPage();
    if (page != nullptr && page->failed()) {
        return;  // The chrome stays up with an error (and its button's taps land here too).
    }
    auto &loop = context_.services.loop;
    const bool image = page != nullptr && page->image() != nullptr && page->image()->image() != 0;
    if (!image) {
        setChromeVisible(!chromeVisible_.peek());
        return;
    }
    // Images wait a moment for a second tap, which zooms instead.
    const std::int64_t now = loop.now();
    const float density = environment().density.peek();
    if (lastTap_ && now - lastTap_->first < kDoubleTapNs &&
        distance(lastTap_->second, local) < kDoubleTapSlopDp * density) {
        if (tapTimer_) {
            loop.cancel(*tapTimer_);
            tapTimer_.reset();
        }
        lastTap_.reset();
        page->toggleZoom(local);
        return;
    }
    lastTap_ = {now, local};
    if (tapTimer_) {
        loop.cancel(*tapTimer_);
    }
    std::weak_ptr<Node> weak = weak_from_this();
    tapTimer_ = loop.postDelayed(
            [weak, this] {
                if (!weak.lock()) {
                    return;
                }
                tapTimer_.reset();
                lastTap_.reset();
                setChromeVisible(!chromeVisible_.peek());
            },
            kDoubleTapNs);
}

void ViewerScreen::endSwipe() {
    const float width = stage_->frame().width;
    const float dx = drag_->peek();
    const float velocity = velocity_.velocity().x / environment().density.peek();
    const bool next = (dx < -width * kSwipeFraction || velocity < -kFlingDp) && index_ + 1 < items_.size();
    const bool previous = (dx > width * kSwipeFraction || velocity > kFlingDp) && index_ > 0;
    mode_ = Mode::none;
    if (next && dx < 0) {
        goTo(index_ + 1);
    } else if (previous && dx > 0) {
        goTo(index_ - 1);
    } else {
        drag_->animateTo(0.0f, Spring{});
    }
}

}  // namespace filepan::app
