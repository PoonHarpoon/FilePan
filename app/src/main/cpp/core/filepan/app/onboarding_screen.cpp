// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/onboarding_screen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "filepan/app/screen_parts.hpp"
#include "filepan/app/settings_screen.hpp"
#include "filepan/ui/components/components.hpp"

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;

struct PageText {
    const char *title;
    const char *body;
};

constexpr PageText kText[OnboardingScreen::kPages] = {
        {"FilePan", "A fast file manager for this phone and your servers."},
        {"All your files",
         "To browse and manage your storage, FilePan needs access to all files. Nothing leaves the "
         "phone unless you send it."},
        {"Jobs in the background",
         "Copies, moves and extractions carry on when you leave the app. A notification shows how "
         "far they've got."},
        {"Your look", "A theme and an accent, applied at once. Settings has them too."},
        {"Remote files",
         "Browse SFTP servers like folders here. On mobile data, FilePan can hold back what it "
         "would fetch on its own."},
};

Style fill() {
    Style s;
    s.position = Position::absolute;
    s.left = 0;
    s.top = 0;
    s.right = 0;
    s.bottom = 0;
    return s;
}

NodePtr button(std::string label, ButtonVariant variant, std::function<void()> onTap, std::string icon = {}) {
    return Button::create({.label = std::move(label), .variant = variant, .icon = std::move(icon), .onTap = std::move(onTap)});
}

// What a page's action left: a stamp in place of its button.
NodePtr done(const char *text) {
    auto row = Box::create({.direction = Direction::row});
    row->appendChild(Stamp(std::string(text), StampTone::accent, "check"));
    return row;
}

}  // namespace

std::shared_ptr<OnboardingScreen> OnboardingScreen::create(AppContext &context, std::function<void(Exit)> onExit) {
    auto node = std::make_shared<OnboardingScreen>(context);
    node->construct(std::move(onExit));
    return node;
}

void OnboardingScreen::construct(std::function<void(Exit)> onExit) {
    onExit_ = std::move(onExit);
    setDebugName("Onboarding");
    setStyle(fill());
    setHitTestBehavior(HitTestBehavior::opaque);
    setPaintHook([](PaintContext &context) { context.list.fill(context.bounds(), currentTheme().colors.ground); });
    enter_ = std::make_shared<AnimatedValue<float>>(1.0f);

    Node::build([&] {
        OnboardingScreen *self = this;
        auto column = Box::create([] {
            Style s = fill();
            s.padding = {24, 16, 24, 16};
            return s;
        }());

        auto top = Box::create({.direction = Direction::row, .justify = Justify::end, .minHeight = 48});
        auto skip = button("Skip", ButtonVariant::text, [self] { self->finish(Exit::done); });
        top->appendChild(skip);
        column->appendChild(top);

        pageSlot_ = Box::create({.justify = Justify::center, .grow = 1, .shrink = 1, .basis = 0});
        column->appendChild(pageSlot_);

        auto bottom = Box::create({.direction = Direction::row, .justify = Justify::spaceBetween,
                                   .alignItems = Align::center, .gap = 16, .minHeight = 56});
        auto dots = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 6});
        dots->updateSemantics([](Semantics &s) { s.hidden = true; });
        for (std::size_t i = 0; i < kPages; ++i) {
            auto dot = Box::create({.width = 6, .height = 6});
            dot->bind([raw = dot.get(), self, i] {
                const bool current = self->page_() == i;
                const Theme &t = theme();
                untrack([&] {
                    raw->updateStyle([&](Style &s) { s.width = current ? 20.0f : 6.0f; });
                    raw->setDecoration({.background = gfx::Paint{current ? legible(t.colors.accent, t.colors.ground)
                                                                         : ink(t, 0.28f)},
                                        .radii = gfx::CornerRadii::uniform(3)});
                });
            });
            dots->appendChild(dot);
        }
        bottom->appendChild(dots);
        nextSlot_ = Box::create({.direction = Direction::row, .minWidth = 140});
        bottom->appendChild(nextSlot_);
        column->appendChild(bottom);
        appendChild(column);

        // Clear of the status and navigation bars.
        bind([raw = column.get()] {
            const float density = environment().density();
            const Insets safe = environment().safeArea();
            untrack([&] {
                raw->updateStyle([&](Style &s) {
                    s.padding = {24 + safe.left / density, 8 + safe.top / density, 24 + safe.right / density,
                                 16 + safe.bottom / density};
                });
            });
        });
        // Skip goes on the last page, where Done does the same.
        bind([raw = skip.get(), self] {
            const bool last = self->page_() + 1 == kPages;
            untrack([&] { raw->updateStyle([&](Style &s) { s.display = !last; }); });
        });
        // The page itself, and Next, which is quiet while the page's action is undone.
        bind([self] {
            (void)theme();
            (void)self->page_();
            (void)self->context_.services.allFilesAccess();
            (void)self->context_.services.notificationsAllowed();
            (void)self->askedNotifications_();
            untrack([&] {
                self->nextSlot_->clearChildren();
                self->nextSlot_->appendChild(self->buildNext());
                self->pageSlot_->clearChildren();
                self->pageSlot_->appendChild(self->buildPage(self->page_.peek()));
            });
        });
        // Pages slide in from the side they come from; under reduced motion they fade instead.
        bind([self] {
            const float t = (*self->enter_)();
            untrack([&] {
                if (environment().reducedMotion.peek()) {
                    self->pageSlot_->setOpacity(1 - std::abs(t));
                    self->pageSlot_->setTransform({});
                } else {
                    self->pageSlot_->setOpacity(1 - std::min(1.0f, std::abs(t)));
                    self->pageSlot_->setTransform(gfx::Transform::translation({t * 48 * environment().density.peek(), 0}));
                }
            });
        });
    });
    enter_->snapTo(0);
}

NodePtr OnboardingScreen::buildNext() {
    const std::size_t page = page_.peek();
    const bool last = page + 1 == kPages;
    bool actionDone = true;
    if (page == 1) {
        actionDone = context_.services.allFilesAccess.peek();
    } else if (page == 2) {
        actionDone = context_.services.notificationsAllowed.peek();
    }
    OnboardingScreen *self = this;
    auto next = Button::create({.label = last ? "Done" : "Next",
                                .variant = actionDone ? ButtonVariant::primary : ButtonVariant::glass,
                                .icon = last ? "check" : "arrow_forward",
                                .onTap = [self] { self->next(); }});
    next->updateStyle([](Style &s) { s.grow = 1; });
    return next;
}

NodePtr OnboardingScreen::buildPage(std::size_t page) {
    const Theme &t = currentTheme();
    OnboardingScreen *self = this;
    auto content = Box::create({.gap = 16});
    if (page == 0 && t.style.marks) {
        auto hero = Box::create({.alignItems = Align::center, .padding = {0, 0, 0, 24}});
        hero->appendChild(Orb::create({.size = 160, .animated = true}));
        content->appendChild(hero);
    }
    char counter[16];
    std::snprintf(counter, sizeof counter, "%02zu / %02zu", page + 1, kPages);
    content->appendChild(Text::create(counter, t.type.label, textInk(t, t.text.label, 11)));
    auto title = Text::create(kText[page].title, t.type.display, t.colors.ink);
    title->updateSemantics([](Semantics &s) { s.role = Role::heading; });
    content->appendChild(title);
    content->appendChild(Text::create(kText[page].body, t.type.body, ink(t, t.text.body)));

    auto action = Box::create({.gap = 12, .padding = {0, 12, 0, 0}});
    switch (page) {
    case 1:
        if (context_.services.allFilesAccess.peek()) {
            action->appendChild(done("Access granted"));
        } else {
            action->appendChild(button("Grant access", ButtonVariant::primary,
                                       [self] { self->context_.services.platform.requestAllFilesAccess(); },
                                       "folder_open"));
        }
        break;
    case 2:
        if (context_.services.notificationsAllowed.peek()) {
            action->appendChild(done("Notifications on"));
        } else if (askedNotifications_.peek()) {
            // Refused: the dialog may not show again, so the system settings it is.
            action->appendChild(button("Open settings", ButtonVariant::primary,
                                       [self] { self->context_.services.platform.openNotificationSettings(); },
                                       "settings"));
        } else {
            action->appendChild(button("Allow notifications", ButtonVariant::primary,
                                       [self] {
                                           self->askedNotifications_.set(true);
                                           self->context_.services.platform.requestNotificationPermission();
                                       },
                                       "notifications"));
        }
        break;
    case 3: {
        auto panel = GlassPanel({.gap = 6, .padding = {18, 14, 18, 14}});
        panel->appendChild(Text::create("Theme", t.type.label, textInk(t, t.text.label, 11)));
        auto presets = themePresetControl(context_.state);
        presets->updateStyle([](Style &s) { s.margin = {0, 4, 0, 10}; });
        panel->appendChild(presets);
        panel->appendChild(Text::create("Accent", t.type.label, textInk(t, t.text.label, 11)));
        auto swatches = accentSwatches(context_);
        swatches->updateStyle([](Style &s) { s.padding = {0, 6, 0, 0}; });
        panel->appendChild(swatches);
        action->appendChild(panel);
        break;
    }
    case 4: {
        auto panel = GlassPanel({.padding = {18, 6, 18, 6}});
        panel->appendChild(dataSaverLine(context_));
        action->appendChild(panel);
        auto add = button("Add an SFTP server", ButtonVariant::glass, [self] { self->finish(Exit::addServer); }, "dns");
        action->appendChild(add);
        break;
    }
    default: break;
    }
    if (!action->children().empty()) {
        content->appendChild(action);
    }
    return content;
}

void OnboardingScreen::showPage(std::size_t page, int direction) {
    page_.set(page);
    // In from the right going on, from the left going back.
    enter_->snapTo(static_cast<float>(direction));
    if (environment().reducedMotion.peek()) {
        enter_->snapTo(1);
        enter_->animateTo(0, Tween{.durationMs = 120, .crossfade = true});
    } else {
        enter_->animateTo(0, Spring{currentTheme().motion.springStiffness, 1});
    }
    context_.services.platform.announce(kText[page].title);
}

void OnboardingScreen::goTo(std::size_t page) {
    if (page < kPages && page != page_.peek()) {
        showPage(page, page > page_.peek() ? 1 : -1);
    }
}

void OnboardingScreen::next() {
    if (page_.peek() + 1 >= kPages) {
        finish(Exit::done);
        return;
    }
    goTo(page_.peek() + 1);
}

bool OnboardingScreen::back() {
    if (page_.peek() == 0) {
        return false;
    }
    goTo(page_.peek() - 1);
    return true;
}

void OnboardingScreen::finish(Exit exit) {
    if (finished_) {
        return;
    }
    finished_ = true;
    if (onExit_) {
        onExit_(exit);
    }
}

}  // namespace filepan::app
