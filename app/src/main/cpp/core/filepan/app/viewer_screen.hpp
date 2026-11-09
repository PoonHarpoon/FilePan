// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "filepan/app/app.hpp"
#include "filepan/app/remote_media.hpp"
#include "filepan/media/image_player.hpp"
#include "filepan/media/video_player.hpp"
#include "filepan/ui/components/components.hpp"
#include "filepan/ui/gesture.hpp"

// The full-screen media viewer: the images and videos of a folder, in its sort order.
//
// - Swipe left and right between items; the current one and its neighbours are loaded.
// - Images: pinch and double tap zoom, up to the decoded resolution (1 image px per screen
//   px), and pan while zoomed. Animations play while their page is current.
// - Video plays when its page comes up: a centred play/pause button, and a bottom bar with the
//   time, an accent seek track, the duration and mute. Decoders and the audio stream are
//   released while the app is in the background.
// - A tap shows or hides the chrome, which hides by itself after 2.5 s while video plays.
// - The top bar: back, the file name over `3 of 7 · 4.1 MB`, share, and more (Open with, Rename,
//   Delete, Properties). Chevrons at the sides step through the items; animations get a `GIF`
//   tag. Anything that can't be shown says why and offers "Open with".
// - Files on SFTP servers stream through a RemoteMediaCache (remote_media.hpp), which prefetches
//   the items around the current one as the settings say; while video waits on the network, a
//   spinner takes the play button's icon. A server that refuses the extra connections is told
//   of once.
namespace filepan::app {

// What the viewer opens: images and videos, by MIME type. nullopt for anything else.
std::optional<MediaItem::Kind> viewerKind(const vfs::Entry &entry);

class MediaPage;

class ViewerScreen final : public ui::Node {
public:
    static std::shared_ptr<ViewerScreen> create(AppContext &context, std::vector<MediaItem> items,
                                                std::size_t index, ViewerOptions options,
                                                std::function<void()> onClose);
    ViewerScreen(AppContext &context, std::vector<MediaItem> items, std::size_t index, ViewerOptions options,
                 std::function<void()> onClose);
    ~ViewerScreen() override;

    // Closes the viewer.
    void close();
    [[nodiscard]] std::size_t index() const { return index_; }
    [[nodiscard]] const std::vector<MediaItem> &items() const { return items_; }
    [[nodiscard]] const MediaItem &current() const { return items_[index_]; }
    // The loaded page for items()[index], if any.
    [[nodiscard]] MediaPage *page(std::size_t index) const;
    // Moves to another item, animated unless it's not a neighbour.
    void goTo(std::size_t index, bool animate = true);
    void setChromeVisible(bool visible);
    [[nodiscard]] bool chromeVisible() const { return chromeVisible_.peek(); }
    // Video is playing but waiting on the network: a spinner shows in the play button.
    [[nodiscard]] bool buffering() const { return buffering_.peek(); }
    // A server took fewer connections than prefetching wanted: says so, once per server.
    void poolLimited(const vfs::PoolLimit &limit, const std::string &serverName);
    [[nodiscard]] RemoteMediaCache *remoteCache() const { return cache_.get(); }

    [[nodiscard]] ui::Node &backButton() const { return *back_; }
    [[nodiscard]] ui::Node &title() const { return *title_; }
    [[nodiscard]] ui::Node &stage() const { return *stage_; }
    [[nodiscard]] ui::Node &controls() const { return *controls_; }
    // The centred play/pause button.
    [[nodiscard]] ui::Node &playButton() const { return *play_; }
    [[nodiscard]] ui::Node &seekBar() const { return *seek_; }
    [[nodiscard]] ui::Node &muteButton() const { return *mute_; }
    [[nodiscard]] ui::Node &subtitle() const { return *subtitle_; }
    [[nodiscard]] ui::Node &moreButton() const { return *more_; }
    [[nodiscard]] ui::Node &previousButton() const { return *previous_; }
    [[nodiscard]] ui::Node &nextButton() const { return *next_; }
    [[nodiscard]] ui::Node &gifTag() const { return *gifTag_; }

protected:
    void onSizeChanged() override;

private:
    enum class Mode : std::uint8_t { none, pending, swipe, pan, pinch };

    void construct();
    void loadPages();
    void layoutPages();
    void settle(std::size_t index);
    void pointer(const ui::PointerEvent &event);
    void tapped(gfx::Vec2 local);
    void endSwipe();
    void updateChrome();
    void scheduleHide();
    void mediaChanged(MediaPage &page);
    void showMore();
    // A remote item's bytes, for its page; null for a local one.
    [[nodiscard]] std::shared_ptr<media::ByteSource> remoteBytes(std::size_t index);
    // Checks whether the current video waits on the network, while there are remote items.
    void pollBuffering();
    [[nodiscard]] FileTargets targets() const;
    [[nodiscard]] MediaPage *currentPage() const { return page(index_); }
    [[nodiscard]] media::VideoPlayer *currentVideo() const;

    AppContext &context_;
    // For the destructor: the screen may outlive the App (and its context) as the root's content.
    ui::UiLoop &loop_;
    std::vector<MediaItem> items_;
    std::size_t index_;
    ViewerOptions options_;
    std::function<void()> onClose_;
    media::DecodeLimits limits_;

    std::shared_ptr<ui::Node> stage_;
    std::map<std::size_t, std::shared_ptr<MediaPage>> pages_;
    std::shared_ptr<ui::Node> top_;
    std::shared_ptr<ui::Node> back_;
    std::shared_ptr<ui::Text> title_;
    std::shared_ptr<ui::Text> subtitle_;
    std::shared_ptr<ui::Node> share_;
    std::shared_ptr<ui::Node> more_;
    std::shared_ptr<ui::Node> previous_;
    std::shared_ptr<ui::Node> next_;
    std::shared_ptr<ui::Node> gifTag_;
    std::shared_ptr<ui::Node> play_;
    std::shared_ptr<ui::components::Icon> playIcon_;
    std::shared_ptr<ui::Node> controls_;
    std::shared_ptr<ui::Node> seek_;
    std::shared_ptr<ui::Text> time_;
    std::shared_ptr<ui::Text> duration_;
    std::shared_ptr<ui::Node> mute_;
    std::shared_ptr<ui::components::Icon> muteIcon_;
    std::shared_ptr<ui::Node> spinner_;

    // Swipe offset, dp.
    std::shared_ptr<ui::AnimatedValue<float>> drag_;
    std::shared_ptr<ui::AnimatedValue<float>> chromeAlpha_;
    ui::Signal<bool> chromeVisible_{true};
    ui::Signal<bool> playing_{false};
    ui::Signal<float> progress_{0.0f};
    ui::Signal<std::string> timeText_;
    ui::Signal<std::string> durationText_;
    std::optional<ui::UiLoop::TimerId> hideTimer_;
    std::optional<ui::UiLoop::TimerId> tapTimer_;
    std::optional<ui::UiLoop::TimerId> seekTimer_;
    std::optional<float> pendingSeek_;
    // Remote items: their sources, and the buffering check.
    bool remote_ = false;
    std::unique_ptr<RemoteMediaCache> cache_;
    // Servers whose connection limit was told of.
    std::set<std::string> limited_;
    ui::Signal<bool> buffering_{false};
    std::optional<std::int64_t> waitingSince_;
    std::optional<ui::UiLoop::TimerId> bufferTimer_;

    // Gestures, in the stage's local px.
    std::map<std::int32_t, gfx::Vec2> pointers_;
    Mode mode_ = Mode::none;
    bool turning_ = false;
    gfx::Vec2 downAt_;
    std::int64_t downTimeNs_ = 0;
    gfx::Vec2 lastPan_;
    float pinchDistance_ = 0;
    float pinchScale_ = 1;
    gfx::Vec2 pinchFocal_;
    std::optional<std::pair<std::int64_t, gfx::Vec2>> lastTap_;
    ui::VelocityTracker velocity_;
};

// One item: decodes and draws an image (zoomable) or plays a video, or says why it can't.
class MediaPage final : public ui::Node {
public:
    // bytes: where a remote item streams from; null for a local one.
    MediaPage(AppContext &context, MediaItem item, const ViewerOptions &options, const media::DecodeLimits &limits,
              std::shared_ptr<media::ByteSource> bytes = nullptr);
    ~MediaPage() override;

    [[nodiscard]] const MediaItem &item() const { return item_; }
    // Current pages play; others pause (and videos don't open until first current).
    void setActive(bool active);
    void setOnChange(std::function<void()> onChange) { onChange_ = std::move(onChange); }

    [[nodiscard]] media::ImagePlayer *image() const { return image_.get(); }
    [[nodiscard]] media::VideoPlayer *video() const { return video_.get(); }
    [[nodiscard]] bool failed() const;
    [[nodiscard]] ui::Node *errorPanel() const { return error_.get(); }
    [[nodiscard]] ui::Node *openWithButton() const { return openWith_.get(); }

    // Zoom, for images: 1 fits the page; at most the decoded resolution.
    [[nodiscard]] float scale() const { return scale_; }
    [[nodiscard]] float maxScale() const;
    [[nodiscard]] bool zoomed() const { return scale_ > 1.001f; }
    // Zooms to scale keeping the image point under focal (local px) there.
    void zoomAbout(float scale, gfx::Vec2 focal);
    void panBy(gfx::Vec2 deltaPx);
    // Double tap: zooms in on point, or back out.
    void toggleZoom(gfx::Vec2 point);
    void resetZoom();

protected:
    void paint(ui::PaintContext &context) override;
    void onSizeChanged() override;

private:
    // Where the media sits at scale 1 (contained, centred), local px.
    [[nodiscard]] gfx::Rect fitRect() const;
    [[nodiscard]] gfx::Size mediaSize() const;
    void clampPan();
    void showError(const std::string &message);
    void changed();
    [[nodiscard]] media::MediaSource source() const;

    AppContext &context_;
    MediaItem item_;
    ViewerOptions options_;
    std::shared_ptr<media::ByteSource> bytes_;
    std::unique_ptr<media::ImagePlayer> image_;
    std::unique_ptr<media::VideoPlayer> video_;
    std::shared_ptr<ui::Node> error_;
    std::shared_ptr<ui::Node> openWith_;
    std::function<void()> onChange_;
    bool active_ = false;
    float scale_ = 1;
    gfx::Vec2 pan_;  // Content centre from the page centre, px.
};

}  // namespace filepan::app
