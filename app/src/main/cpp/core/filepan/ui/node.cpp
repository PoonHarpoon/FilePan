// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/node.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_set>
#include <variant>

#include "filepan/ui/layout.hpp"

namespace filepan::ui {

namespace {

std::atomic<std::uint32_t> nextNodeId{1};

constexpr gfx::Transform kIdentity{};

}  // namespace

std::optional<gfx::Transform> invert(const gfx::Transform &t) {
    const float det = t.a * t.d - t.b * t.c;
    if (det == 0 || !std::isfinite(det)) {
        return std::nullopt;
    }
    const float inv = 1 / det;
    gfx::Transform r;
    r.a = t.d * inv;
    r.b = -t.b * inv;
    r.c = -t.c * inv;
    r.d = t.a * inv;
    r.tx = -(r.a * t.tx + r.c * t.ty);
    r.ty = -(r.b * t.tx + r.d * t.ty);
    return r;
}

gfx::Paint scalePaint(const gfx::Paint &paint, float density) {
    struct Visitor {
        float d;
        gfx::Paint operator()(const gfx::Color &color) const { return color; }
        gfx::Paint operator()(gfx::LinearGradient g) const {
            g.from = g.from * d;
            g.to = g.to * d;
            return g;
        }
        gfx::Paint operator()(gfx::RadialGradient g) const {
            g.center = g.center * d;
            g.radius *= d;
            return g;
        }
        gfx::Paint operator()(gfx::ConicGradient g) const {
            g.center = g.center * d;
            return g;
        }
        gfx::Paint operator()(gfx::Stripes s) const {
            s.width *= d;
            s.period *= d;
            s.phase *= d;
            return s;
        }
    };
    if (density == 1) {
        return paint;
    }
    return std::visit(Visitor{density}, paint);
}

// --- Node ---

Node::Node() : id_(nextNodeId.fetch_add(1, std::memory_order_relaxed)) {}

Node::~Node() {
    // Dispose effects first: cleanups may still look at the node.
    scope_.dispose();
    for (auto &child : children_) {
        child->parent_ = nullptr;
    }
}

NodeHost *Node::host() const {
    const Node *n = this;
    while (n->parent_ != nullptr) {
        n = n->parent_;
    }
    return n->host_;
}

void Node::requestFrame() {
    if (NodeHost *h = host()) {
        h->requestFrame();
    }
}

void Node::attachChild(Node &child) {
    child.parent_ = this;
    // The child may have been laid out elsewhere: make sure its new ancestors relayout and
    // recompose.
    markLayoutDirty();
    invalidateComposite();
    markSemanticsDirty();
}

void Node::detachChild(Node &child) {
    if (NodeHost *h = host()) {
        h->nodeDetached(child);
    }
    child.parent_ = nullptr;
}

void Node::appendChild(NodePtr child) { insertChild(children_.size(), std::move(child)); }

void Node::insertChild(std::size_t index, NodePtr child) {
    if (child->parent_ != nullptr) {
        child->parent_->removeChild(child.get());
    }
    Node &ref = *child;
    children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(std::min(index, children_.size())),
                     std::move(child));
    attachChild(ref);
}

void Node::removeChild(Node *child) {
    auto it = std::ranges::find_if(children_, [child](const NodePtr &c) { return c.get() == child; });
    if (it == children_.end()) {
        return;
    }
    NodePtr keep = std::move(*it);
    children_.erase(it);
    detachChild(*keep);
    markLayoutDirty();
    invalidateComposite();
    markSemanticsDirty();
}

void Node::clearChildren() {
    if (children_.empty()) {
        return;
    }
    auto old = std::move(children_);
    children_.clear();
    for (auto &child : old) {
        detachChild(*child);
    }
    markLayoutDirty();
    invalidateComposite();
    markSemanticsDirty();
}

void Node::setChildren(std::vector<NodePtr> children) {
    std::unordered_set<Node *> kept;
    kept.reserve(children.size());
    for (auto &child : children) {
        kept.insert(child.get());
    }
    for (auto &child : children_) {
        if (!kept.contains(child.get())) {
            detachChild(*child);
        }
    }
    for (auto &child : children) {
        if (child->parent_ != this) {
            if (child->parent_ != nullptr) {
                child->parent_->removeChild(child.get());
            }
            child->parent_ = this;
        }
    }
    children_ = std::move(children);
    markLayoutDirty();
    invalidateComposite();
    markSemanticsDirty();
}

void Node::bind(std::function<void()> fn) {
    scope_.run([&] { effect(std::move(fn)); });
}

void Node::setStyle(const Style &style) {
    if (style == style_) {
        return;
    }
    const bool displayChanged = style.display != style_.display;
    style_ = style;
    markLayoutDirty();
    if (displayChanged && parent_ != nullptr) {
        parent_->invalidateComposite();
        parent_->markSemanticsDirty();
    }
}

void Node::setMeasure(MeasureFn measure) {
    measure_ = std::move(measure);
    markLayoutDirty();
}

gfx::Size Node::measureContent(const LayoutContext &context, float maxWidth, float maxHeight) {
    return measure_ ? measure_(context, maxWidth, maxHeight) : gfx::Size{};
}

gfx::Size Node::measureCustom(LayoutEngine &, std::optional<float> width, std::optional<float> height,
                              float availableWidth, float availableHeight) {
    return {width.value_or(availableWidth < kInfinity ? availableWidth : 0),
            height.value_or(availableHeight < kInfinity ? availableHeight : 0)};
}

void Node::layoutCustom(LayoutEngine &, gfx::Size) {}

void Node::setOpacity(float opacity) {
    if (opacity == opacity_) {
        return;
    }
    opacity_ = opacity;
    if (parent_ != nullptr) {
        parent_->invalidateComposite();
    }
}

void Node::setTransform(const gfx::Transform &transform) {
    if (transform == transform_) {
        return;
    }
    transform_ = transform;
    if (parent_ != nullptr) {
        parent_->invalidateComposite();
    }
}

void Node::setTransformOrigin(gfx::Vec2 fraction) {
    if (fraction == transformOrigin_) {
        return;
    }
    transformOrigin_ = fraction;
    if (parent_ != nullptr && !(transform_ == kIdentity)) {
        parent_->invalidateComposite();
    }
}

void Node::setClip(bool clip, gfx::CornerRadii radii) {
    if (clip == clip_ && radii == clipRadii_) {
        return;
    }
    clip_ = clip;
    clipRadii_ = radii;
    invalidateComposite();
}

void Node::setPaintHook(std::function<void(PaintContext &)> hook) {
    paintHook_ = std::move(hook);
    markPaintDirty();
}

void Node::setOverlayHook(std::function<void(PaintContext &)> hook) {
    overlayHook_ = std::move(hook);
    markPaintDirty();
}

void Node::paint(PaintContext &context) {
    if (paintHook_) {
        paintHook_(context);
    }
}

void Node::paintOverlay(PaintContext &context) {
    if (overlayHook_) {
        overlayHook_(context);
    }
}

void Node::addGestureRecognizer(std::shared_ptr<GestureRecognizer> recognizer) {
    recognizers_.push_back(std::move(recognizer));
}

void Node::removeGestureRecognizer(const GestureRecognizer *recognizer) {
    std::erase_if(recognizers_, [recognizer](const auto &r) { return r.get() == recognizer; });
}

bool Node::isInteractive() const {
    return !recognizers_.empty() || static_cast<bool>(pointerHandler_);
}

void Node::requestFocus() {
    if (NodeHost *h = host()) {
        h->requestFocus(this);
    }
}

void Node::clearFocus() {
    if (NodeHost *h = host(); h != nullptr && h->focusedNode() == this) {
        h->requestFocus(nullptr);
    }
}

void Node::revealInAncestors() {
    if (frame_.width <= 0 || frame_.height <= 0) {
        return;
    }
    // In each parent's children space: frames are relative to the parent, before its scroll.
    gfx::Rect rect = frame_;
    for (Node *parent = parent_; parent != nullptr; parent = parent->parent_) {
        rect = parent->revealRect(rect);
        rect.x += parent->frame_.x;
        rect.y += parent->frame_.y;
    }
}

void Node::setSemantics(Semantics semantics) {
    semantics_ = std::move(semantics);
    markSemanticsDirty();
}

bool Node::performDefaultAction(Action action, const ActionArgs &) {
    switch (action) {
    case Action::focus:
        requestFocus();
        return true;
    case Action::clearFocus:
        clearFocus();
        return true;
    default:
        return false;
    }
}

void Node::markLayoutDirty() {
    layoutValid_ = false;
    measureCacheCount_ = 0;
    measureCacheNext_ = 0;
    // Ancestors depend on this node's size. Stop at the first ancestor that's already dirty:
    // its own ancestors are too, and a frame was requested when it became dirty.
    for (Node *n = parent_; n != nullptr; n = n->parent_) {
        if (!n->layoutValid_ && n->measureCacheCount_ == 0) {
            return;
        }
        n->layoutValid_ = false;
        n->measureCacheCount_ = 0;
        n->measureCacheNext_ = 0;
    }
    requestFrame();
}

void Node::invalidateComposite() {
    bool changed = false;
    for (Node *n = this; n != nullptr && !n->compositeDirty_; n = n->parent_) {
        n->compositeDirty_ = true;
        changed = true;
    }
    if (changed) {
        requestFrame();
    }
}

void Node::markPaintDirty() {
    if (paintDirty_ && compositeDirty_) {
        return;
    }
    paintDirty_ = true;
    compositeDirty_ = true;
    if (parent_ != nullptr) {
        parent_->invalidateComposite();
    }
    requestFrame();
}

void Node::markSemanticsDirty() {
    if (NodeHost *h = host()) {
        h->semanticsChanged();
    }
}

void Node::markSubtreeDirty() {
    layoutValid_ = false;
    measureCacheCount_ = 0;
    measureCacheNext_ = 0;
    paintDirty_ = true;
    compositeDirty_ = true;
    for (auto &child : children_) {
        child->markSubtreeDirty();
    }
    if (parent_ != nullptr) {
        parent_->markLayoutDirty();
        parent_->invalidateComposite();
    } else {
        requestFrame();
    }
}

gfx::Size Node::sizePx(float density) const {
    return {std::round(frame_.width * density), std::round(frame_.height * density)};
}

gfx::Transform Node::transformPx(float density) const {
    const gfx::Vec2 position{std::round(frame_.x * density), std::round(frame_.y * density)};
    auto result = gfx::Transform::translation(position);
    if (transform_ == kIdentity) {
        return result;
    }
    const gfx::Size size = sizePx(density);
    const gfx::Vec2 origin{transformOrigin_.x * size.width, transformOrigin_.y * size.height};
    gfx::Transform scaled = transform_;
    scaled.tx *= density;
    scaled.ty *= density;
    return result * gfx::Transform::translation(origin) * scaled *
           gfx::Transform::translation(origin * -1);
}

gfx::Transform Node::childrenTransformPx(float) const { return {}; }

std::shared_ptr<const gfx::DisplayList> Node::record(const LayoutContext &context) {
    if (!paintDirty_ && !compositeDirty_) {
        return composite_;
    }
    const float density = context.density;
    if (paintDirty_) {
        const gfx::Size size = sizePx(density);
        {
            gfx::DisplayList list;
            PaintContext paintContext{list, size, density, context.fontScale, context.text};
            paint(paintContext);
            content_ = list.empty() ? nullptr : std::make_shared<const gfx::DisplayList>(std::move(list));
        }
        {
            gfx::DisplayList list;
            PaintContext paintContext{list, size, density, context.fontScale, context.text};
            paintOverlay(paintContext);
            overlay_ = list.empty() ? nullptr : std::make_shared<const gfx::DisplayList>(std::move(list));
        }
        paintDirty_ = false;
        ++paintCount_;
    }

    gfx::DisplayList list;
    if (content_) {
        list.draw(content_);
    }
    bool anyChild = false;
    bool pushedTransform = false;
    for (auto &child : children_) {
        if (!child->style_.display) {
            continue;
        }
        auto childList = child->record(context);
        if (!childList || child->opacity_ <= 0) {
            continue;
        }
        if (!anyChild) {
            anyChild = true;
            if (clip_) {
                const gfx::Size size = sizePx(density);
                list.pushClip({{0, 0, size.width, size.height},
                               {clipRadii_.topLeft * density, clipRadii_.topRight * density,
                                clipRadii_.bottomRight * density, clipRadii_.bottomLeft * density}});
            }
            const gfx::Transform childrenTransform = childrenTransformPx(density);
            if (!(childrenTransform == kIdentity)) {
                list.pushTransform(childrenTransform);
                pushedTransform = true;
            }
        }
        const gfx::Transform transform = child->transformPx(density);
        const bool transformed = !(transform == kIdentity);
        const bool faded = child->opacity_ < 1;
        if (transformed) {
            list.pushTransform(transform);
        }
        if (faded) {
            list.pushOpacity(child->opacity_);
        }
        list.draw(std::move(childList));
        if (faded) {
            list.pop();
        }
        if (transformed) {
            list.pop();
        }
    }
    if (anyChild) {
        if (pushedTransform) {
            list.pop();
        }
        if (clip_) {
            list.pop();
        }
    }
    if (!anyChild && !overlay_) {
        // A leaf: its composite is just its content, no extra indirection.
        composite_ = content_;
    } else {
        if (overlay_) {
            list.draw(overlay_);
        }
        composite_ = std::make_shared<const gfx::DisplayList>(std::move(list));
    }
    compositeDirty_ = false;
    return composite_;
}

// --- Hit testing ---

namespace {

class HitTester {
public:
    explicit HitTester(float density) : density_(density) {}

    bool visit(Node &node, gfx::Vec2 point) {
        if (!node.style().display || node.hitTestBehavior() == HitTestBehavior::none) {
            return false;
        }
        const auto inverse = invert(node.transformPx(density_));
        if (!inverse) {
            return false;
        }
        const gfx::Vec2 local = inverse->apply(point);
        const gfx::Size size = node.sizePx(density_);
        const gfx::Rect bounds{0, 0, size.width, size.height};
        const bool inside = bounds.contains(local);
        if (node.clips() && !inside) {
            return false;
        }

        stack_.push_back(&node);
        const std::uint32_t candidateBefore = candidateVersion_;
        if (const auto childInverse = invert(node.childrenTransformPx(density_))) {
            const gfx::Vec2 childPoint = childInverse->apply(local);
            const auto &children = node.children();
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (visit(**it, childPoint)) {
                    stack_.pop_back();
                    return true;
                }
            }
        }
        const bool selfHit = node.hitTestBehavior() == HitTestBehavior::opaque || node.isInteractive();
        if (inside && selfHit) {
            // A small target inside this one, missed only by its touch slop, wins over a
            // container that merely contains the point (a list behind an icon button).
            if (candidateVersion_ == candidateBefore) {
                result_ = stack_;
            } else {
                result_ = candidate_;
            }
            stack_.pop_back();
            return true;
        }
        if (!inside && node.isInteractive() && node.minTouchTarget() > 0) {
            const float target = node.minTouchTarget() * density_;
            const float growX = std::max(0.0f, (target - size.width) / 2);
            const float growY = std::max(0.0f, (target - size.height) / 2);
            const gfx::Rect expanded{-growX, -growY, size.width + 2 * growX, size.height + 2 * growY};
            if ((growX > 0 || growY > 0) && expanded.contains(local)) {
                const float dx = std::max({0.0f, -local.x, local.x - size.width});
                const float dy = std::max({0.0f, -local.y, local.y - size.height});
                const float distance = dx * dx + dy * dy;
                if (distance < candidateDistance_) {
                    candidateDistance_ = distance;
                    candidate_ = stack_;
                    ++candidateVersion_;
                }
            }
        }
        stack_.pop_back();
        return false;
    }

    // Deepest first.
    bool finish(std::vector<NodePtr> &path) {
        const std::vector<Node *> *chosen = nullptr;
        if (!result_.empty()) {
            chosen = &result_;
        } else if (!candidate_.empty()) {
            chosen = &candidate_;
        }
        path.clear();
        if (chosen == nullptr) {
            return false;
        }
        for (auto it = chosen->rbegin(); it != chosen->rend(); ++it) {
            path.push_back((*it)->shared_from_this());
        }
        return true;
    }

private:
    float density_;
    std::vector<Node *> stack_;
    std::vector<Node *> result_;
    std::vector<Node *> candidate_;
    float candidateDistance_ = kInfinity;
    std::uint32_t candidateVersion_ = 0;
};

}  // namespace

bool Node::hitTest(Node &root, gfx::Vec2 point, float density, std::vector<NodePtr> &path) {
    HitTester tester(density);
    tester.visit(root, point);
    return tester.finish(path);
}

// --- Box ---

std::shared_ptr<Box> Box::create(Style style, BoxDecoration decoration) {
    auto box = std::make_shared<Box>();
    box->setStyle(style);
    box->decoration_ = std::move(decoration);
    return box;
}

void Box::setDecoration(BoxDecoration decoration) {
    if (decoration == decoration_) {
        return;
    }
    decoration_ = std::move(decoration);
    markPaintDirty();
}

void Box::paint(PaintContext &context) {
    const auto &d = decoration_;
    const gfx::RoundedRect shape{context.bounds(), context.px(d.radii)};
    const auto scaledShadow = [&](gfx::Shadow shadow) {
        shadow.offset = shadow.offset * context.density;
        shadow.blur *= context.density;
        shadow.spread *= context.density;
        return shadow;
    };
    for (const auto &shadow : d.shadows) {
        if (!shadow.inset) {
            context.list.shadow(shape, scaledShadow(shadow));
        }
    }
    if (d.backdropBlur > 0) {
        context.list.backdropBlur(shape, context.px(d.backdropBlur), d.backdropSaturation, d.blurSource);
    }
    if (d.background) {
        context.list.fill(shape, scalePaint(*d.background, context.density));
    }
    for (const auto &shadow : d.shadows) {
        if (shadow.inset) {
            context.list.shadow(shape, scaledShadow(shadow));
        }
    }
    if (d.borderWidth > 0) {
        context.list.stroke(shape, context.px(d.borderWidth), scalePaint(d.border, context.density));
    }
    Node::paint(context);
}

// --- Text ---

std::shared_ptr<Text> Text::create(std::string text, gfx::TextStyle style, gfx::Color color) {
    auto node = std::make_shared<Text>();
    node->text_ = std::move(text);
    node->textStyle_ = style;
    node->color_ = color;
    return node;
}

void Text::invalidateShapes() {
    shapes_[0] = {};
    shapes_[1] = {};
    markLayoutDirty();
    markPaintDirty();
}

void Text::setText(std::string text) {
    if (text == text_) {
        return;
    }
    text_ = std::move(text);
    invalidateShapes();
    markSemanticsDirty();
}

void Text::setTextStyle(const gfx::TextStyle &style) {
    if (style == textStyle_) {
        return;
    }
    textStyle_ = style;
    invalidateShapes();
}

void Text::setColor(gfx::Color color) {
    if (color == color_) {
        return;
    }
    color_ = color;
    markPaintDirty();
}

void Text::setMaxLines(int maxLines) {
    if (maxLines == options_.maxLines) {
        return;
    }
    options_.maxLines = maxLines;
    invalidateShapes();
}

void Text::setOverflow(gfx::TextOverflow overflow) {
    if (overflow == options_.overflow) {
        return;
    }
    options_.overflow = overflow;
    invalidateShapes();
}

void Text::setAlign(gfx::TextAlign align) {
    if (align == options_.align) {
        return;
    }
    options_.align = align;
    invalidateShapes();
}

const Text::Shape &Text::shape(gfx::TextSystem &system, float maxWidthPx, float density, float fontScale) {
    for (const auto &shape : shapes_) {
        if (shape.text && shape.maxWidthPx == maxWidthPx && shape.density == density &&
            shape.fontScale == fontScale) {
            return shape;
        }
    }
    gfx::TextStyle style = textStyle_;
    style.size *= fontScale * density;
    gfx::TextLayoutOptions options = options_;
    options.maxWidth = maxWidthPx;
    Shape &slot = shapes_[nextShape_];
    nextShape_ ^= 1;
    slot.maxWidthPx = maxWidthPx;
    slot.density = density;
    slot.fontScale = fontScale;
    slot.text = std::make_shared<const gfx::ShapedText>(system.shape(text_, style, options));
    ++shapeCount_;
    return slot;
}

gfx::Size Text::measureContent(const LayoutContext &context, float maxWidth, float) {
    if (context.text == nullptr) {
        return {};
    }
    const float maxWidthPx = maxWidth < kInfinity ? maxWidth * context.density : kInfinity;
    const auto &shaped = shape(*context.text, maxWidthPx, context.density, context.fontScale);
    return {std::ceil(shaped.text->size.width) / context.density,
            std::ceil(shaped.text->size.height) / context.density};
}

void Text::paint(PaintContext &context) {
    if (context.text == nullptr) {
        return;
    }
    const Edges &padding = style().padding;
    const float width = std::max(0.0f, context.size.width - std::round(padding.horizontal() * context.density));
    const Shape *chosen = nullptr;
    for (const auto &shape : shapes_) {
        if (!shape.text || shape.density != context.density || shape.fontScale != context.fontScale) {
            continue;
        }
        if (shape.maxWidthPx == width) {
            chosen = &shape;
            break;
        }
        // Shaped with at least as much room and it fits untruncated: same glyphs.
        if (options_.align == gfx::TextAlign::start && shape.maxWidthPx >= width &&
            !shape.text->truncated && shape.text->size.width <= width) {
            chosen = &shape;
        }
    }
    if (chosen == nullptr) {
        chosen = &shape(*context.text, width, context.density, context.fontScale);
    }
    painted_ = chosen->text;
    context.list.text(painted_, {std::round(padding.left * context.density), std::round(padding.top * context.density)},
                      color_);
    Node::paint(context);
}

// --- Image ---

std::shared_ptr<Image> Image::create(gfx::ImageId image) {
    auto node = std::make_shared<Image>();
    node->image_ = image;
    return node;
}

void Image::setImage(gfx::ImageId image) {
    if (image == image_) {
        return;
    }
    image_ = image;
    markPaintDirty();
}

void Image::setRadii(gfx::CornerRadii radii) {
    if (radii == radii_) {
        return;
    }
    radii_ = radii;
    markPaintDirty();
}

void Image::setImageOpacity(float opacity) {
    if (opacity == imageOpacity_) {
        return;
    }
    imageOpacity_ = opacity;
    markPaintDirty();
}

void Image::paint(PaintContext &context) {
    if (image_ != 0) {
        context.list.image(image_, context.bounds(), context.px(radii_), imageOpacity_);
    }
    Node::paint(context);
}

}  // namespace filepan::ui
