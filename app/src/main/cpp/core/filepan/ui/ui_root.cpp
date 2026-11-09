// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ui/ui_root.hpp"

#include <algorithm>

#include "filepan/ui/layout.hpp"

namespace filepan::ui {

namespace {

thread_local Environment *currentEnvironment = nullptr;

const std::shared_ptr<const gfx::DisplayList> &emptyList() {
    static const auto list = std::make_shared<const gfx::DisplayList>();
    return list;
}

}  // namespace

Environment &environment() {
    if (currentEnvironment != nullptr) {
        return *currentEnvironment;
    }
    thread_local Environment fallback;
    return fallback;
}

UiRoot::UiRoot(UiLoop &loop, gfx::TextSystem *text)
    : loop_(loop), text_(text), previousEnvironment_(currentEnvironment) {
    currentEnvironment = &environment_;
    detail::runtime().scheduler = [this] { requestFrame(); };
    frameClock().requestFrame = [this] { requestFrame(); };
    loop_.setFrameCallback([this](std::int64_t frameTimeNs) {
        if (auto produced = frame(frameTimeNs); produced && frameSink_) {
            frameSink_(*produced);
        }
    });
    scope_.run([this] {
        // Everything in px depends on these, and paint hooks read the theme: relayout and
        // repaint the whole tree.
        effect([this] {
            (void)environment_.density();
            (void)environment_.fontScale();
            (void)environment_.theme();
            untrack([this] {
                if (content_) {
                    content_->markSubtreeDirty();
                }
            });
        });
        effect([this] { arena_.density = environment_.density(); });
        effect([this] { frameClock().reducedMotion = environment_.reducedMotion(); });
        effect([this] {
            (void)environment_.windowSize();
            untrack([this] { requestFrame(); });
        });
    });
}

UiRoot::~UiRoot() {
    if (deadlineTimer_) {
        loop_.cancel(*deadlineTimer_);
    }
    loop_.setFrameCallback(nullptr);
    detail::runtime().scheduler = nullptr;
    frameClock().requestFrame = nullptr;
    if (content_) {
        content_->setHost(nullptr);
    }
    currentEnvironment = previousEnvironment_;
}

void UiRoot::setContent(NodePtr content) {
    if (content_) {
        focus_.requestFocus(nullptr);
        content_->setHost(nullptr);
    }
    // A pointer still down holds its hit path, which would keep the old content alive past
    // whatever it refers to.
    for (const auto &[pointer, route] : routes_) {
        arena_.cancel(pointer);
    }
    routes_.clear();
    content_ = std::move(content);
    laidOutSize_ = {-1, -1};
    if (content_) {
        content_->setHost(this);
        content_->markLayoutDirty();
        content_->markPaintDirty();
    }
    semanticsDirty_ = true;
    requestFrame();
}

LayoutContext UiRoot::layoutContext() const {
    return {text_, environment_.density.peek(), environment_.fontScale.peek()};
}

void UiRoot::requestFrame() {
    if (inFrame_ || frameRequested_) {
        return;
    }
    frameRequested_ = true;
    loop_.requestFrame();
}

bool UiRoot::needsFrame() const {
    if (frameClock().active() || detail::runtime().hasPendingEffects()) {
        return true;
    }
    if (accessibilityEnabled_ && semanticsDirty_) {
        return true;
    }
    if (!content_) {
        return false;
    }
    const float density = environment_.density.peek();
    const gfx::Size window = environment_.windowSize.peek();
    const gfx::Size size{window.width / density, window.height / density};
    return size != laidOutSize_ || content_->layoutDirty() || content_->needsRecord();
}

std::optional<UiRoot::Frame> UiRoot::frame(std::int64_t frameTimeNs) {
    frameRequested_ = false;
    stats_ = {};
    if (!needsFrame()) {
        return std::nullopt;
    }
    inFrame_ = true;
    arena_.checkDeadlines(frameTimeNs);
    frameClock().tick(frameTimeNs);
    flushEffects();

    const LayoutContext context = layoutContext();
    bool recorded = false;
    if (content_) {
        const gfx::Size window = environment_.windowSize.peek();
        const gfx::Size size{window.width / context.density, window.height / context.density};
        // Layout can bind list items, whose effects may dirty layout again; settle in a few passes.
        for (int pass = 0; pass < 3; ++pass) {
            if (content_->layoutDirty() || size != laidOutSize_) {
                LayoutEngine engine(context);
                engine.layoutRoot(*content_, size);
                laidOutSize_ = size;
                stats_.laidOut = true;
                stats_.layoutComputes += engine.computeCount();
            }
            if (!detail::runtime().hasPendingEffects()) {
                break;
            }
            flushEffects();
        }
        if (content_->needsRecord()) {
            content_->record(context);
            recorded = true;
        }
    }
    // Edits made on this side (a key, a tap that moves the caret, a paste) reach the IME; the
    // platform skips a value it already has.
    textInput_.notifyValueChanged();
    bool semanticsChanged = false;
    if (accessibilityEnabled_ && content_ && (semanticsDirty_ || stats_.laidOut || recorded)) {
        semanticsDirty_ = false;
        if (semantics_.update(*content_, context.density)) {
            semanticsPending_ = true;
            semanticsChanged = true;
        }
    }
    inFrame_ = false;
    stats_.recorded = recorded;
    if (frameClock().active() || detail::runtime().hasPendingEffects() ||
        (content_ && (content_->layoutDirty() || content_->needsRecord()))) {
        requestFrame();
    }
    if (!recorded && !semanticsChanged) {
        return std::nullopt;
    }
    Frame result;
    result.displayList = content_ && content_->composite() ? content_->composite() : emptyList();
    result.semanticsChanged = semanticsChanged;
    return result;
}

gfx::Vec2 UiRoot::toLocal(Node &node, gfx::Vec2 window) const {
    const float density = environment_.density.peek();
    std::vector<Node *> chain;
    for (Node *n = &node; n != nullptr; n = n->parent()) {
        chain.push_back(n);
    }
    gfx::Vec2 point = window;
    for (std::size_t i = chain.size(); i-- > 0;) {
        Node *n = chain[i];
        if (const auto inverse = invert(n->transformPx(density))) {
            point = inverse->apply(point);
        }
        if (i == 0) {
            break;
        }
        if (const auto inverse = invert(n->childrenTransformPx(density))) {
            point = inverse->apply(point);
        }
    }
    return point;
}

void UiRoot::dispatchPointer(PointerEvent event) {
    if (!content_) {
        return;
    }
    if (event.action == PointerAction::down) {
        Route route;
        Node::hitTest(*content_, event.position, environment_.density.peek(), route.path);
        for (const auto &node : route.path) {
            if (const auto &handler = node->pointerHandler()) {
                PointerEvent local = event;
                local.local = toLocal(*node, event.position);
                handler(local);
            }
        }
        for (const auto &node : route.path) {
            for (const auto &recognizer : node->gestureRecognizers()) {
                route.owners[recognizer.get()] = node;
                PointerEvent local = event;
                local.local = toLocal(*node, event.position);
                recognizer->addPointer(local, arena_);
            }
        }
        routes_[event.pointer] = std::move(route);
        arena_.close(event.pointer);
        scheduleArenaDeadline();
        return;
    }

    auto it = routes_.find(event.pointer);
    if (it == routes_.end()) {
        return;
    }
    const Route route = it->second;
    for (const auto &node : route.path) {
        if (const auto &handler = node->pointerHandler()) {
            PointerEvent local = event;
            local.local = toLocal(*node, event.position);
            handler(local);
        }
    }
    arena_.dispatch(event, [this, &route](GestureRecognizer &recognizer, PointerEvent &local) {
        if (auto found = route.owners.find(&recognizer); found != route.owners.end()) {
            if (auto node = found->second.lock()) {
                local.local = toLocal(*node, local.position);
            }
        }
    });
    if (event.action == PointerAction::up) {
        arena_.sweep(event.pointer);
        routes_.erase(event.pointer);
    } else if (event.action == PointerAction::cancel) {
        arena_.cancel(event.pointer);
        routes_.erase(event.pointer);
    }
}

void UiRoot::scheduleArenaDeadline() {
    if (deadlineTimer_) {
        loop_.cancel(*deadlineTimer_);
        deadlineTimer_.reset();
    }
    const auto next = arena_.nextDeadline();
    if (!next) {
        return;
    }
    deadlineTimer_ = loop_.postDelayed(
            [this] {
                deadlineTimer_.reset();
                arena_.checkDeadlines(loop_.now());
                scheduleArenaDeadline();
            },
            std::max<std::int64_t>(0, *next - loop_.now()));
}

bool UiRoot::dispatchKey(const KeyEvent &event) {
    for (Node *n = focus_.focused(); n != nullptr; n = n->parent()) {
        if (const auto &handler = n->keyHandler(); handler && handler(event)) {
            return true;
        }
    }
    if (event.action == KeyAction::down && event.keyCode == keys::kTab && content_) {
        return focus_.moveFocus(*content_, (event.modifiers & modifiers::kShift) == 0);
    }
    return false;
}

bool UiRoot::dispatchBack() {
    for (std::size_t i = backHandlers_.size(); i-- > 0;) {
        auto handler = backHandlers_[i].handler;
        if (handler && handler()) {
            return true;
        }
    }
    return false;
}

UiRoot::BackHandlerId UiRoot::addBackHandler(std::function<bool()> handler) {
    const BackHandlerId id = nextBackHandler_++;
    backHandlers_.push_back({id, std::move(handler)});
    if (detail::runtime().owner != nullptr) {
        onCleanup([this, id] { removeBackHandler(id); });
    }
    return id;
}

void UiRoot::removeBackHandler(BackHandlerId id) {
    std::erase_if(backHandlers_, [id](const BackHandler &h) { return h.id == id; });
}

void UiRoot::setAccessibilityEnabled(bool enabled) {
    accessibilityEnabled_ = enabled;
    semanticsDirty_ = true;
    if (enabled) {
        requestFrame();
    }
}

bool UiRoot::takeSemanticsUpdate(ByteWriter &out, bool full) {
    if (!semanticsPending_ && !full) {
        return false;
    }
    semantics_.write(out, full);
    semanticsPending_ = false;
    return true;
}

bool UiRoot::performAccessibilityAction(std::uint32_t id, Action action, const ActionArgs &args) {
    auto node = semantics_.nodeFor(id);
    if (!node) {
        return false;
    }
    if (const auto &onAction = node->semantics().onAction; onAction && onAction(action, args)) {
        return true;
    }
    for (const auto &recognizer : node->gestureRecognizers()) {
        if (recognizer->activate(static_cast<std::uint32_t>(action))) {
            return true;
        }
    }
    return node->performDefaultAction(action, args);
}

void UiRoot::nodeDetached(Node &node) { focus_.nodeDetached(node); }

void UiRoot::requestFocus(Node *node) { focus_.requestFocus(node); }

}  // namespace filepan::ui
