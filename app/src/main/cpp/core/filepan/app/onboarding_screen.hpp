// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "filepan/app/app.hpp"
#include "filepan/ui/animation.hpp"
#include "filepan/ui/node.hpp"

// The intro (glass-signal-mobile.md, "Other screens"), shown over everything on a fresh install
// and again from Settings. Five pages, one main action each:
//
//   1 Welcome          The orb, the name, what FilePan is.
//   2 Files            All files access: asks, and says once it's granted.
//   3 Notifications    For the jobs notification: asks, then offers the system settings if the
//                      dialog was refused.
//   4 Look             The theme preset and the accent, applied at once.
//   5 Remote files     Mobile data mode, and adding an SFTP server.
//
// Next stays a secondary button while the page's own action is undone, so the page has one loud
// thing. Skip (or Done) finishes; back steps back a page, and on the first leaves it to the
// system.
namespace filepan::app {

class OnboardingScreen final : public ui::Node {
public:
    enum class Exit : std::uint8_t { done, addServer };
    static constexpr std::size_t kPages = 5;

    static std::shared_ptr<OnboardingScreen> create(AppContext &context, std::function<void(Exit)> onExit);
    explicit OnboardingScreen(AppContext &context) : context_(context) {}

    [[nodiscard]] std::size_t page() const { return page_.peek(); }
    void next();
    void goTo(std::size_t page);
    // Back a page; false on the first.
    bool back();
    void finish(Exit exit);

private:
    void construct(std::function<void(Exit)> onExit);
    void showPage(std::size_t page, int direction);
    ui::NodePtr buildPage(std::size_t page);
    ui::NodePtr buildNext();

    AppContext &context_;
    std::function<void(Exit)> onExit_;
    ui::Signal<std::size_t> page_{0};
    // A permission asked for on this run: still refused when back, the button opens settings.
    ui::Signal<bool> askedNotifications_{false};
    std::shared_ptr<ui::Node> pageSlot_;
    std::shared_ptr<ui::Node> nextSlot_;
    std::shared_ptr<ui::AnimatedValue<float>> enter_;
    bool finished_ = false;
};

}  // namespace filepan::app
