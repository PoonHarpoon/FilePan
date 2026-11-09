// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "filepan/ui/animation.hpp"
#include "ui_harness.hpp"

using namespace filepan;
using namespace filepan::ui;
using filepan::test::UiHarness;

namespace {

void testIdleFramesProduceNothing() {
    UiHarness h;
    int frames = 0;
    h.root->setFrameSink([&](const UiRoot::Frame &frame) {
        ++frames;
        EXPECT(frame.displayList != nullptr);
    });
    Signal<std::string> title("Files");
    auto screen = Box::create({}, {.background = gfx::Paint{gfx::Color{0, 0, 0, 1}}});
    auto label = Text::create();
    label->bind([raw = label.get(), title] { raw->setText(title()); });
    screen->appendChild(label);
    h.root->setContent(screen);

    EXPECT(h.loop.frameRequested());
    h.loop.runFrame(h.now);
    EXPECT(frames == 1);
    EXPECT(h.root->lastFrameStats().laidOut && h.root->lastFrameStats().recorded);

    // Idle: no frame is requested, and a frame anyway does no work.
    EXPECT(!h.loop.frameRequested() && !h.root->needsFrame());
    EXPECT(!h.root->frame(h.now).has_value());
    EXPECT(!h.root->lastFrameStats().laidOut);
    h.advance(100);
    EXPECT(frames == 1);

    // A signal write requests one frame; its effect runs before layout and only the label
    // repaints.
    const auto screenPaints = screen->paintCount();
    title.set("Downloads");
    EXPECT(label->text() == "Files");  // Not until the frame.
    EXPECT(h.loop.frameRequested());
    h.loop.runFrame(h.now);
    EXPECT(frames == 2 && label->text() == "Downloads");
    EXPECT(label->paintCount() == 2 && screen->paintCount() == screenPaints);
    EXPECT(!h.loop.frameRequested());

    // Writing the same value is not a change.
    title.set("Downloads");
    EXPECT(!h.loop.frameRequested());
}

void testAnimationDrivesFrames() {
    UiHarness h;
    auto box = Box::create({.width = 100, .height = 100});
    auto fade = std::make_shared<AnimatedValue<float>>(1.0f);
    box->bind([raw = box.get(), fade] { raw->setOpacity((*fade)()); });
    auto screen = Box::create();
    screen->appendChild(box);
    h.root->setContent(screen);
    h.loop.runFrame(h.now);

    bool ended = false;
    fade->animateTo(0, Tween{100, easing::kLinear}, [&] { ended = true; });
    EXPECT(h.loop.frameRequested());
    std::vector<float> values;
    for (int i = 0; i < 20 && h.loop.frameRequested(); ++i) {
        h.now += 8'000'000;
        h.loop.runFrame(h.now);
        values.push_back(box->opacity());
    }
    EXPECT(ended && box->opacity() == 0);
    EXPECT(values.size() >= 12 && values.size() <= 14);
    for (std::size_t i = 1; i < values.size(); ++i) {
        EXPECT(values[i] <= values[i - 1]);
    }
    EXPECT(box->paintCount() == 1);  // Opacity is composited, never repainted.
    EXPECT(!h.loop.frameRequested() && !h.root->needsFrame());

    // A spring settles too, overshooting with low damping.
    auto slide = std::make_shared<AnimatedValue<float>>(0.0f);
    float peak = 0;
    slide->animateTo(100, Spring{400, 0.4f});
    for (int i = 0; i < 400 && slide->animating(); ++i) {
        h.now += 8'000'000;
        h.loop.runFrame(h.now);
        peak = std::max(peak, slide->peek());
    }
    EXPECT(!slide->animating() && slide->peek() == 100 && peak > 110);

    // The design's springy curve overshoots and lands exactly.
    EXPECT(easing::kSpringy(0.5f) > 1 && easing::kSpringy(1) == 1 && easing::kSpringy(0) == 0);
    EXPECT(test::near(easing::kLinear(0.3f), 0.3f, 1e-4f));

    // Reduced motion snaps, with no frames.
    h.root->env().reducedMotion.set(true);
    h.loop.runFrame(h.now);
    fade->animateTo(1, Tween{300});
    EXPECT((*fade).peek() == 1 && !fade->animating());
    // Crossfades replace movement under reduced motion, so they still run.
    fade->animateTo(0, Tween{.durationMs = 120, .crossfade = true});
    EXPECT(fade->animating());
    fade->snapTo(1);
    h.loop.runFrame(h.now);
    EXPECT(box->opacity() == 1 && !h.root->needsFrame());
}

void testDensityChangeRelayouts() {
    UiHarness h;
    auto screen = Box::create();
    auto row = Box::create({.height = 56}, {.background = gfx::Paint{gfx::Color{1, 1, 1, 1}}});
    screen->appendChild(row);
    h.root->setContent(screen);
    h.frame();
    const auto paints = row->paintCount();
    h.root->env().density.set(3);
    h.root->env().windowSize.set({1200, 2400});
    EXPECT(h.frame());
    EXPECT(row->paintCount() == paints + 1);
    const auto &fill = std::get<gfx::FillCommand>(row->composite()->commands().front());
    EXPECT(fill.shape.rect.height == 168);

    // Paint hooks read the theme, so switching presets repaints them.
    const auto beforeTheme = row->paintCount();
    h.root->env().theme.set(makeTheme(ThemePreset::ultraviolet));
    EXPECT(h.frame());
    EXPECT(row->paintCount() == beforeTheme + 1);
}

class FakeIme final : public TextInputPlatform {
public:
    void show(const TextInputConfig &) override { visible = true; }
    void hide() override { visible = false; }
    void updateEditingValue(const TextEditingValue &value) override { last = value; }

    bool visible = false;
    TextEditingValue last;
};

class Field final : public TextInputClient {
public:
    [[nodiscard]] const TextEditingValue &editingValue() const override { return value; }
    void updateEditingValue(const TextEditingValue &next) override { value = next; }

    TextEditingValue value;
};

void testFocusAndTextInput() {
    UiHarness h;
    FakeIme ime;
    h.root->textInput().setPlatform(&ime);
    Field field;
    auto screen = Box::create();
    auto name = Box::create({.height = 52});
    name->setFocusable(true);
    name->setTextInputClient(&field);
    auto ok = Box::create({.height = 48});
    ok->setFocusable(true);
    bool focusSeen = false;
    ok->bind([raw = ok.get(), &focusSeen] { focusSeen = raw->trackFocused(); });
    screen->appendChild(name);
    screen->appendChild(ok);
    h.root->setContent(screen);
    h.frame();

    name->requestFocus();
    EXPECT(name->focused() && ime.visible && h.root->textInput().client() == &field);
    // The keyboard was dismissed (or never came up); focusing the field again brings it back.
    ime.visible = false;
    name->requestFocus();
    EXPECT(name->focused() && ime.visible);

    // The IME composes, then commits; UTF-8 offsets.
    field.setComposingText("caf", 1);
    EXPECT(field.value.text == "caf" && field.value.composing == (TextRange{0, 3}));
    field.commitText("café", 1);
    EXPECT(field.value.text == "café" && field.value.selection == (TextRange{5, 5}));
    EXPECT(!field.value.composing.valid());
    field.deleteSurroundingText(2, 0);  // One two-byte character.
    EXPECT(field.value.text == "caf" && field.value.selection == (TextRange{3, 3}));
    field.setSelection(0, 3);
    field.commitText("tea");
    EXPECT(field.value.text == "tea");

    // Tab moves focus; the keyboard goes away with the text field.
    EXPECT(h.root->dispatchKey({.keyCode = keys::kTab}));
    h.frame();  // Effects reading focus run with the frame.
    EXPECT(ok->focused() && !name->focused() && !ime.visible && focusSeen);
    // Keys reach the focused node first, then its ancestors.
    int enters = 0;
    screen->setKeyHandler([&](const KeyEvent &event) {
        enters += event.keyCode == keys::kEnter ? 1 : 0;
        return event.keyCode == keys::kEnter;
    });
    EXPECT(h.root->dispatchKey({.keyCode = keys::kEnter}) && enters == 1);

    // Removing the focused node drops focus.
    screen->removeChild(ok.get());
    h.frame();
    EXPECT(h.root->focusedNode() == nullptr && !focusSeen);
}

void testBackHandlers() {
    UiHarness h;
    std::vector<int> order;
    h.root->addBackHandler([&] {
        order.push_back(1);
        return true;
    });
    {
        Scope sheet;
        sheet.run([&] {
            h.root->addBackHandler([&] {
                order.push_back(2);
                return true;
            });
        });
        EXPECT(h.root->dispatchBack());
        EXPECT(order == std::vector<int>{2});
    }
    // The sheet's scope is gone, and its handler with it.
    EXPECT(h.root->dispatchBack());
    EXPECT(order == (std::vector<int>{2, 1}));
}

void testLoopAndIoPool() {
    UiLoop loop;
    const auto uiThread = std::this_thread::get_id();
    std::vector<int> order;
    loop.postDelayed([&] { order.push_back(3); }, 0);
    loop.post([&] { order.push_back(1); });
    std::thread([&] { loop.post([&] { order.push_back(2); }); }).join();
    loop.runPending();
    EXPECT(order == (std::vector<int>{1, 2, 3}));
    const auto later = loop.postDelayed([&] { order.push_back(4); }, 1'000'000'000);
    EXPECT(loop.runPending().has_value());
    loop.cancel(later);
    EXPECT(!loop.runPending().has_value());

    IoPool pool(loop, 2);
    std::atomic<bool> workedOffThread{false};
    int result = 0;
    bool delivered = false;
    pool.submit(
            [&] {
                workedOffThread = std::this_thread::get_id() != uiThread;
                return 42;
            },
            [&](int value) {
                EXPECT(std::this_thread::get_id() == uiThread);
                result = value;
                delivered = true;
            });
    auto cancelled = pool.submit([] { return 1; }, [&](int) { result = -1; });
    cancelled->cancel();
    for (int i = 0; i < 1000 && !delivered; ++i) {
        loop.runPending();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    loop.runPending();
    EXPECT(delivered && result == 42 && workedOffThread);

    // The host loop: a worker posts quit.
    bool ran = false;
    pool.submit([] {}, [&] {
        ran = true;
        loop.quit();
    });
    loop.run();
    EXPECT(ran);
}


void testContentSwapDropsPointers() {
    UiHarness h;
    auto screen = Box::create({.width = 400, .height = 400});
    auto tap = std::make_shared<TapRecognizer>();
    screen->addGestureRecognizer(tap);
    h.root->setContent(screen);
    h.loop.runFrame(h.now);
    h.root->dispatchPointer({.pointer = 0, .action = PointerAction::down, .position = {100, 100}, .timeNs = h.now});
    // Replacing the content with a finger still down lets the old content go, and cancels its
    // gestures.
    std::weak_ptr<Node> weak = screen;
    screen.reset();
    h.root->setContent(nullptr);
    EXPECT(weak.expired());
    // The finger's later events go nowhere.
    h.root->dispatchPointer({.pointer = 0, .action = PointerAction::up, .position = {100, 100}, .timeNs = h.now});
}

}  // namespace

int main() {
    testIdleFramesProduceNothing();
    testAnimationDrivesFrames();
    testDensityChangeRelayouts();
    testFocusAndTextInput();
    testBackHandlers();
    testLoopAndIoPool();
    testContentSwapDropsPointers();
    return 0;
}
