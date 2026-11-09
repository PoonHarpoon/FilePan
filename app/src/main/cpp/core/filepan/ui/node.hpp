// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "filepan/gfx/display_list.hpp"
#include "filepan/gfx/geometry.hpp"
#include "filepan/gfx/paint.hpp"
#include "filepan/gfx/text.hpp"
#include "filepan/ui/input.hpp"
#include "filepan/ui/reactive.hpp"
#include "filepan/ui/semantics.hpp"
#include "filepan/ui/style.hpp"

// The retained node tree.
//
// A component builds its subtree once and binds signal-driven properties with Node::bind, an
// effect owned by the node's scope: when a signal changes, only that property is written, and
// the node marks itself dirty. There is no diffing.
//
// Each node has:
// - a Style (layout input, dp) and a computed frame (dp, relative to the parent's box);
// - a paint hook recording into the node's own cached display list, re-recorded only when the
//   node is paint-dirty (markPaintDirty, or its size changed);
// - a composite display list that draws the node's content and composes its children's
//   composites with DisplayListCommand. Moving, fading or transforming a child only rebuilds
//   its parent's composite; clean subtrees are reused as is;
// - gesture recognizers and a raw pointer handler, a key handler and focusability;
// - semantics.
//
// Lifetime: nodes are shared_ptrs; a parent owns its children. Effects bound to a node live in
// its scope and die with it, so bind lambdas should capture the node as a raw pointer (or
// `this`), never a shared_ptr to itself, which would leak.
namespace filepan::ui {

class GestureRecognizer;
class Node;
class TextInputClient;

using NodePtr = std::shared_ptr<Node>;

// What a node needs to measure: text and unit conversion.
struct LayoutContext {
    gfx::TextSystem *text = nullptr;
    float density = 1;    // px per dp.
    float fontScale = 1;  // sp per dp.
};

// What a paint hook records into, in the node's local physical pixels: (0, 0) is its top left.
struct PaintContext {
    gfx::DisplayList &list;
    gfx::Size size;  // px.
    float density = 1;
    float fontScale = 1;
    gfx::TextSystem *text = nullptr;

    [[nodiscard]] float px(float dp) const { return dp * density; }
    [[nodiscard]] gfx::Rect bounds() const { return {0, 0, size.width, size.height}; }
    [[nodiscard]] gfx::CornerRadii px(const gfx::CornerRadii &dp) const {
        return {dp.topLeft * density, dp.topRight * density, dp.bottomRight * density,
                dp.bottomLeft * density};
    }
};

// Implemented by UiRoot; the top of an attached tree points at it.
class NodeHost {
public:
    virtual void requestFrame() = 0;
    // A subtree left the tree: drop focus and pointer routes into it.
    virtual void nodeDetached(Node &node) = 0;
    // nullptr clears focus.
    virtual void requestFocus(Node *node) = 0;
    [[nodiscard]] virtual Node *focusedNode() const = 0;
    // Semantics changed somewhere; the next frame rebuilds the snapshot.
    virtual void semanticsChanged() {}

protected:
    ~NodeHost() = default;
};

enum class HitTestBehavior : std::uint8_t {
    // Hit if a child is, or if this node handles input itself.
    deferToChild,
    // Always hit inside its bounds, blocking what's behind (scrims, cards).
    opaque,
    // Neither this node nor its children take input.
    none,
};

// The inverse of an affine transform, or nothing if it's singular.
std::optional<gfx::Transform> invert(const gfx::Transform &transform);

// A dp-space paint scaled to px (gradient positions, stripe widths).
gfx::Paint scalePaint(const gfx::Paint &paint, float density);

class Node : public std::enable_shared_from_this<Node> {
public:
    Node();
    virtual ~Node();
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

    // Stable for the node's lifetime and never reused; also its semantics id.
    [[nodiscard]] std::uint32_t id() const { return id_; }
    [[nodiscard]] std::string_view debugName() const { return debugName_; }
    void setDebugName(std::string name) { debugName_ = std::move(name); }

    // --- Tree ---
    [[nodiscard]] Node *parent() const { return parent_; }
    [[nodiscard]] const std::vector<NodePtr> &children() const { return children_; }
    void appendChild(NodePtr child);
    void insertChild(std::size_t index, NodePtr child);
    void removeChild(Node *child);
    void clearChildren();
    // Replaces the children, keeping nodes that stay (the reconciler's primitive).
    void setChildren(std::vector<NodePtr> children);
    [[nodiscard]] NodeHost *host() const;
    void setHost(NodeHost *host) { host_ = host; }

    // --- Reactivity ---
    [[nodiscard]] Scope &scope() { return scope_; }
    // Runs fn with this node's scope as owner, so effects created in it die with the node.
    template <typename F>
    decltype(auto) build(F &&fn) {
        return scope_.run(std::forward<F>(fn));
    }
    // An effect owned by this node: re-runs fn when what it reads changes.
    void bind(std::function<void()> fn);

    // --- Layout ---
    [[nodiscard]] const Style &style() const { return style_; }
    void setStyle(const Style &style);
    template <typename F>
    void updateStyle(F &&fn) {
        Style style = style_;
        std::forward<F>(fn)(style);
        setStyle(style);
    }
    // dp, relative to the parent's box (before the parent's scroll offset).
    [[nodiscard]] gfx::Rect frame() const { return frame_; }
    // Leaf measurement in dp: given the available content width/height (possibly infinite),
    // returns the content size. Nodes with a measure function don't lay out children.
    using MeasureFn = std::function<gfx::Size(const LayoutContext &, float maxWidth, float maxHeight)>;
    void setMeasure(MeasureFn measure);
    [[nodiscard]] virtual bool hasMeasure() const { return static_cast<bool>(measure_); }
    virtual gfx::Size measureContent(const LayoutContext &context, float maxWidth, float maxHeight);
    // Nodes such as scroll containers and virtual lists position their children themselves.
    [[nodiscard]] virtual bool hasCustomLayout() const { return false; }
    virtual gfx::Size measureCustom(class LayoutEngine &engine, std::optional<float> width,
                                    std::optional<float> height, float availableWidth,
                                    float availableHeight);
    virtual void layoutCustom(class LayoutEngine &engine, gfx::Size size);

    // --- Visual properties (composited by the parent; changing them doesn't re-record) ---
    void setOpacity(float opacity);
    [[nodiscard]] float opacity() const { return opacity_; }
    // Applied around transformOrigin (fractions of the size); translations in dp.
    void setTransform(const gfx::Transform &transform);
    [[nodiscard]] const gfx::Transform &transform() const { return transform_; }
    void setTransformOrigin(gfx::Vec2 fraction);
    // Clips children (and hit testing) to the node's box with these radii (dp).
    void setClip(bool clip, gfx::CornerRadii radii = {});
    [[nodiscard]] bool clips() const { return clip_; }

    // Records the node's own content (behind its children). Subclasses override paint instead.
    void setPaintHook(std::function<void(PaintContext &)> hook);
    // Records content drawn over the children (focus rings, scroll indicators).
    void setOverlayHook(std::function<void(PaintContext &)> hook);

    // --- Input ---
    void addGestureRecognizer(std::shared_ptr<GestureRecognizer> recognizer);
    void removeGestureRecognizer(const GestureRecognizer *recognizer);
    [[nodiscard]] const std::vector<std::shared_ptr<GestureRecognizer>> &gestureRecognizers() const {
        return recognizers_;
    }
    // Sees every event of pointers that went down on this node or a descendant, deepest first.
    using PointerHandler = std::function<void(const PointerEvent &)>;
    void setPointerHandler(PointerHandler handler) { pointerHandler_ = std::move(handler); }
    [[nodiscard]] const PointerHandler &pointerHandler() const { return pointerHandler_; }
    void setHitTestBehavior(HitTestBehavior behavior) { hitTestBehavior_ = behavior; }
    [[nodiscard]] HitTestBehavior hitTestBehavior() const { return hitTestBehavior_; }
    // Interactive nodes smaller than this (dp) get their hit area expanded to it: 48dp keeps
    // small icons tappable. 0 disables.
    void setMinTouchTarget(float dp) { minTouchTarget_ = dp; }
    [[nodiscard]] float minTouchTarget() const { return minTouchTarget_; }
    [[nodiscard]] bool isInteractive() const;

    // Key events go to the focused node first, then bubble to its ancestors until handled.
    using KeyHandler = std::function<bool(const KeyEvent &)>;
    void setKeyHandler(KeyHandler handler) { keyHandler_ = std::move(handler); }
    [[nodiscard]] const KeyHandler &keyHandler() const { return keyHandler_; }

    // --- Focus ---
    void setFocusable(bool focusable) { focusable_ = focusable; }
    [[nodiscard]] bool focusable() const { return focusable_; }
    [[nodiscard]] bool focused() const { return focused_.peek(); }
    // Tracked: read it in a bind to restyle on focus.
    [[nodiscard]] bool trackFocused() const { return focused_(); }
    void requestFocus();
    void clearFocus();
    // A node that edits text sets its client; focusing the node then shows the IME.
    void setTextInputClient(TextInputClient *client) { textInputClient_ = client; }
    [[nodiscard]] TextInputClient *textInputClient() const { return textInputClient_; }
    // Scrolls every scrolling ancestor so this node is in view. Keyboard focus moves call it, so
    // Tab reaches items scrolled out of sight; a node not laid out yet is left alone.
    void revealInAncestors();
    // Given a rect (dp) in this node's children space, scrolls to show it if this node scrolls,
    // and returns it in this node's own space. A plain node returns it unchanged.
    virtual gfx::Rect revealRect(gfx::Rect rect) { return rect; }

    // --- Semantics ---
    [[nodiscard]] const Semantics &semantics() const { return semantics_; }
    void setSemantics(Semantics semantics);
    template <typename F>
    void updateSemantics(F &&fn) {
        std::forward<F>(fn)(semantics_);
        markSemanticsDirty();
    }
    // Text nodes contribute their text as a label when they have none.
    [[nodiscard]] virtual std::string_view semanticsText() const { return {}; }
    // Lets containers handle scroll actions without the component wiring them.
    virtual bool performDefaultAction(Action action, const ActionArgs &args);

    // --- Dirtiness ---
    void markLayoutDirty();
    void markPaintDirty();
    void markSemanticsDirty();
    // Marks this subtree's layout and paint dirty (density or theme changes).
    void markSubtreeDirty();
    [[nodiscard]] bool layoutDirty() const { return !layoutValid_; }
    [[nodiscard]] bool needsRecord() const { return paintDirty_ || compositeDirty_; }

    // --- Recording (UiRoot calls these; components normally don't) ---
    // Re-records what's dirty in this subtree and returns its composite, or null if it draws
    // nothing.
    std::shared_ptr<const gfx::DisplayList> record(const LayoutContext &context);
    [[nodiscard]] const std::shared_ptr<const gfx::DisplayList> &composite() const { return composite_; }
    // How many times this node's paint hook has run; for tests and profiling.
    [[nodiscard]] std::uint32_t paintCount() const { return paintCount_; }

    // The transform from this node's local px to its parent's children space.
    [[nodiscard]] gfx::Transform transformPx(float density) const;
    // Extra transform applied to the children (scroll offset); identity by default.
    [[nodiscard]] virtual gfx::Transform childrenTransformPx(float density) const;
    [[nodiscard]] gfx::Size sizePx(float density) const;
    [[nodiscard]] gfx::CornerRadii clipRadii() const { return clipRadii_; }

    // Hit testing in this node's parent space (px). Fills path deepest first.
    static bool hitTest(Node &root, gfx::Vec2 point, float density, std::vector<NodePtr> &path);

protected:
    virtual void paint(PaintContext &context);
    virtual void paintOverlay(PaintContext &context);
    // Called when the frame's size changed during layout.
    virtual void onSizeChanged() {}

    // The parent's composite must be rebuilt (a child moved, faded or re-recorded).
    void invalidateComposite();

private:
    friend class LayoutEngine;

    void attachChild(Node &child);
    void detachChild(Node &child);
    void requestFrame();

    std::uint32_t id_;
    std::string debugName_;
    Node *parent_ = nullptr;
    NodeHost *host_ = nullptr;
    std::vector<NodePtr> children_;
    Scope scope_;

    Style style_;
    gfx::Rect frame_;
    MeasureFn measure_;

    // Layout cache, keyed on the constraints; cleared by markLayoutDirty.
    struct LayoutCacheEntry {
        std::optional<float> width;
        std::optional<float> height;
        float availableWidth = 0;
        float availableHeight = 0;
        gfx::Size result;
    };
    static constexpr std::size_t kMeasureCacheSize = 4;
    LayoutCacheEntry measureCache_[kMeasureCacheSize];
    std::uint8_t measureCacheCount_ = 0;
    // Where the next entry goes. Reset with the count: lookups read entries [0, count).
    std::uint8_t measureCacheNext_ = 0;
    LayoutCacheEntry layoutCache_;
    bool layoutValid_ = false;

    float opacity_ = 1;
    gfx::Transform transform_;
    gfx::Vec2 transformOrigin_{0.5f, 0.5f};
    bool clip_ = false;
    gfx::CornerRadii clipRadii_;

    std::function<void(PaintContext &)> paintHook_;
    std::function<void(PaintContext &)> overlayHook_;
    std::shared_ptr<const gfx::DisplayList> content_;
    std::shared_ptr<const gfx::DisplayList> overlay_;
    std::shared_ptr<const gfx::DisplayList> composite_;
    bool paintDirty_ = true;
    bool compositeDirty_ = true;
    std::uint32_t paintCount_ = 0;

    std::vector<std::shared_ptr<GestureRecognizer>> recognizers_;
    PointerHandler pointerHandler_;
    HitTestBehavior hitTestBehavior_ = HitTestBehavior::deferToChild;
    float minTouchTarget_ = 48;
    KeyHandler keyHandler_;
    bool focusable_ = false;
    Signal<bool> focused_{false};
    TextInputClient *textInputClient_ = nullptr;
    friend class FocusManager;

    Semantics semantics_;
};

// A box with CSS-like decoration. Values are dp; paints are in the node's local dp space.
struct BoxDecoration {
    std::optional<gfx::Paint> background;
    gfx::CornerRadii radii;
    float borderWidth = 0;
    gfx::Paint border = gfx::Color{};
    // Outer shadows draw before the background, inset ones after it.
    std::vector<gfx::Shadow> shadows;
    // Glass: blurs what's behind before the background is drawn. 0 disables.
    float backdropBlur = 0;
    float backdropSaturation = 1;
    gfx::BlurSource blurSource = gfx::BlurSource::live;

    bool operator==(const BoxDecoration &) const = default;
};

class Box : public Node {
public:
    static std::shared_ptr<Box> create(Style style = {}, BoxDecoration decoration = {});

    void setDecoration(BoxDecoration decoration);
    [[nodiscard]] const BoxDecoration &decoration() const { return decoration_; }
    template <typename F>
    void updateDecoration(F &&fn) {
        BoxDecoration decoration = decoration_;
        std::forward<F>(fn)(decoration);
        setDecoration(std::move(decoration));
    }

protected:
    void paint(PaintContext &context) override;

private:
    BoxDecoration decoration_;
};

// A run of text, shaped through gfx::TextSystem. The style's size is in sp; it re-shapes when
// the text, style or width constraint changes and keeps the last two shapes for reuse.
class Text : public Node {
public:
    static std::shared_ptr<Text> create(std::string text = {}, gfx::TextStyle style = {},
                                        gfx::Color color = {1, 1, 1, 1});

    void setText(std::string text);
    [[nodiscard]] const std::string &text() const { return text_; }
    void setTextStyle(const gfx::TextStyle &style);
    [[nodiscard]] const gfx::TextStyle &textStyle() const { return textStyle_; }
    void setColor(gfx::Color color);
    void setMaxLines(int maxLines);
    void setOverflow(gfx::TextOverflow overflow);
    void setAlign(gfx::TextAlign align);

    // The text shaped for the current frame width, or null before the first record.
    [[nodiscard]] const std::shared_ptr<const gfx::ShapedText> &shaped() const { return painted_; }
    [[nodiscard]] std::uint32_t shapeCount() const { return shapeCount_; }

    [[nodiscard]] bool hasMeasure() const override { return true; }
    gfx::Size measureContent(const LayoutContext &context, float maxWidth, float maxHeight) override;
    [[nodiscard]] std::string_view semanticsText() const override { return text_; }

protected:
    void paint(PaintContext &context) override;

private:
    struct Shape {
        float maxWidthPx = -1;
        float density = 0;
        float fontScale = 0;
        std::shared_ptr<const gfx::ShapedText> text;
    };
    const Shape &shape(gfx::TextSystem &system, float maxWidthPx, float density, float fontScale);
    void invalidateShapes();

    std::string text_;
    gfx::TextStyle textStyle_;
    gfx::Color color_;
    gfx::TextLayoutOptions options_;
    Shape shapes_[2];
    std::uint8_t nextShape_ = 0;
    std::shared_ptr<const gfx::ShapedText> painted_;
    std::uint32_t shapeCount_ = 0;
};

// An image the renderer owns (a thumbnail), stretched to the node's frame.
class Image : public Node {
public:
    static std::shared_ptr<Image> create(gfx::ImageId image = 0);

    void setImage(gfx::ImageId image);
    [[nodiscard]] gfx::ImageId image() const { return image_; }
    void setRadii(gfx::CornerRadii radii);
    void setImageOpacity(float opacity);

protected:
    void paint(PaintContext &context) override;

private:
    gfx::ImageId image_ = 0;
    gfx::CornerRadii radii_;
    float imageOpacity_ = 1;
};

}  // namespace filepan::ui
