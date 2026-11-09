// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Scrolls a 100k-item file list through UiRoot and reports per-frame UI-thread time (effects,
// layout, recording), then again with rows changing under the fling as thumbnails and folder
// counts arrive (the change calls are timed too: they run on the UI thread). Text goes through the fake TextSystem, so real shaping cost isn't
// included; rebinding a row re-shapes only its changed strings. Not part of ctest:
//
//   build/host/ui/virtual_list_benchmark

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "filepan/ui/virtual_list.hpp"
#include "ui_harness.hpp"

using namespace filepan;
using namespace filepan::ui;

namespace {

constexpr std::size_t kItems = 100'000;

struct FileRow {
    Signal<std::string> name;
    Signal<std::string> meta;
    Signal<bool> selected{false};
};

// A typical file row: icon, two lines of text, a trailing size.
NodePtr buildRow(std::unordered_map<Node *, std::shared_ptr<FileRow>> &rows) {
    auto state = std::make_shared<FileRow>();
    auto row = Box::create({.direction = Direction::row, .alignItems = Align::center,
                            .padding = Edges::symmetric(16, 0)});
    row->bind([raw = row.get(), selected = state->selected] {
        raw->setDecoration({.background = gfx::Paint{gfx::Color{1, 0.3f, 0.77f, selected() ? 0.18f : 0.0f}},
                            .radii = gfx::CornerRadii::uniform(12)});
    });
    auto icon = Box::create({.margin = {.right = 16}, .width = 40, .height = 40},
                            {.background = gfx::Paint{gfx::Color{1, 1, 1, 0.1f}}, .radii = gfx::CornerRadii::uniform(10)});
    auto lines = Box::create({.grow = 1, .shrink = 1});
    auto name = Text::create({}, {.size = 15});
    name->setMaxLines(1);
    name->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    name->bind([raw = name.get(), text = state->name] { raw->setText(text()); });
    auto meta = Text::create({}, {.size = 13}, {1, 1, 1, 0.62f});
    meta->setMaxLines(1);
    meta->bind([raw = meta.get(), text = state->meta] { raw->setText(text()); });
    lines->appendChild(name);
    lines->appendChild(meta);
    row->appendChild(icon);
    row->appendChild(lines);
    rows[row.get()] = state;
    return row;
}

struct Result {
    double average = 0;
    double p99 = 0;
    double max = 0;
};

Result run(bool measured, bool changes) {
    test::UiHarness h;
    std::unordered_map<Node *, std::shared_ptr<FileRow>> rows;
    auto list = VirtualList::create();
    if (measured) {
        list->setEstimatedItemExtent(60);
    } else {
        list->setFixedItemExtent(60);
    }
    list->setCreate([&](std::uint32_t) { return buildRow(rows); });
    list->setBind([&](std::size_t index, Node &node) {
        auto &row = *rows[&node];
        row.name.set("IMG_" + std::to_string(20260000 + index) + "_holiday_photo.jpg");
        row.meta.set(std::to_string(index % 97 + 1) + " MB  ·  Oct " + std::to_string(index % 28 + 1));
        row.selected.set(index % 10 == 0);
    });
    list->setItemCount(kItems);
    h.root->setContent(list);
    h.frame();

    // A long fling's worth of frames: 40dp per frame is ~4800dp/s at 120 Hz, then jumps.
    std::vector<double> times;
    const auto timeFrame = [&] {
        h.now += 8'333'333;
        const auto start = std::chrono::steady_clock::now();
        if (changes) {
            // A few thumbnails decoded, and a folder count: BrowseScreen rebinds every row.
            const auto [first, last] = list->boundRange();
            for (std::size_t i = first; i < last; i += 4) {
                list->notifyChanged(i);
            }
            list->notifyChanged(0, list->itemCount());
        }
        h.root->frame(h.now);
        times.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    };
    for (int frame = 0; frame < 3000; ++frame) {
        list->scrollTo(list->offset() + 40);
        timeFrame();
    }
    for (int jump = 0; jump < 200; ++jump) {
        list->scrollToItem(static_cast<std::size_t>(jump) * 7919 % kItems);
        timeFrame();
    }
    std::ranges::sort(times);
    Result result;
    for (double t : times) {
        result.average += t;
    }
    result.average /= static_cast<double>(times.size());
    result.p99 = times[times.size() * 99 / 100];
    result.max = times.back();
    std::printf("%s rows%s: %zu items, %zu frames, binds %llu, nodes created %llu\n",
                measured ? "measured" : "fixed", changes ? ", changing" : "", kItems, times.size(),
                static_cast<unsigned long long>(list->stats().binds),
                static_cast<unsigned long long>(list->stats().creates));
    return result;
}

}  // namespace

int main() {
    for (const auto &[measured, changes] : {std::pair{false, false}, {true, false}, {false, true}, {true, true}}) {
        const Result r = run(measured, changes);
        std::printf("  frame: avg %.1f us, p99 %.1f us, max %.1f us (budget at 120 Hz: 8333 us)\n",
                    r.average, r.p99, r.max);
    }
    return 0;
}
