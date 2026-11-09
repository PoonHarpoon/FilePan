// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <memory>

#include "filepan/ui/ui_root.hpp"
#include "ui_test_util.hpp"

namespace filepan::test {

// A UiRoot on a fake clock: 400x800 dp at density 2, events in dp.
class UiHarness {
public:
    static constexpr float kDensity = 2;

    UiHarness() {
        loop.setClock([this] { return now; });
        root = std::make_unique<ui::UiRoot>(loop, &text);
        root->env().density.set(kDensity);
        root->env().windowSize.set({400 * kDensity, 800 * kDensity});
    }

    // Runs a frame now; returns whether it produced anything.
    bool frame() { return root->frame(now).has_value(); }
    // Moves the clock, running due timers (long-press deadlines) and a frame per 8 ms.
    void advance(std::int64_t ms) {
        for (std::int64_t step = 0; step < ms; step += 8) {
            now += 8'000'000;
            loop.runPending();
            frame();
        }
    }
    void pointer(ui::PointerAction action, float x, float y, std::int32_t id = 0) {
        root->dispatchPointer({.pointer = id, .action = action, .position = {x * kDensity, y * kDensity},
                               .timeNs = now});
    }
    void down(float x, float y) { pointer(ui::PointerAction::down, x, y); }
    void move(float x, float y) { pointer(ui::PointerAction::move, x, y); }
    void up(float x, float y) { pointer(ui::PointerAction::up, x, y); }

    std::int64_t now = 1'000'000'000;
    ui::UiLoop loop;
    FakeTextSystem text;
    std::unique_ptr<ui::UiRoot> root;
};

}  // namespace filepan::test
