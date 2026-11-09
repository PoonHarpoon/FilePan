// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The IME bridge's sessions: an edit the IME sends after focus moved to another field is dropped,
// instead of overwriting the newly focused field with the old one's text.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "filepan/runtime/ime_bridge.hpp"
#include "test_util.hpp"

namespace {

using namespace filepan;

class Platform final : public runtime::Platform {
public:
    std::optional<runtime::TextInputState> shown;
    std::vector<runtime::TextInputState> updates;

    bool hasAllFilesAccess() override { return true; }
    void requestAllFilesAccess() override {}
    std::vector<runtime::StorageVolume> storageVolumes() override { return {}; }
    void openFile(const std::string &, const std::string &) override {}
    void shareFiles(const std::vector<std::string> &, const std::vector<std::string> &) override {}
    void setClipboardText(const std::string &) override {}
    void showIme(gfx::Rect, const runtime::TextInputState &state) override { shown = state; }
    void hideIme() override { shown.reset(); }
    void updateIme(const runtime::TextInputState &state) override { updates.push_back(state); }
    void updateSemantics(std::span<const std::uint8_t>) override {}
    void announce(const std::string &) override {}
    void jobsActive(bool) override {}
    void updateJobNotification(const runtime::JobSummary &) override {}
    void performHaptic(runtime::HapticKind) override {}
    void finishActivity() override {}
};

class Field final : public ui::TextInputClient {
public:
    explicit Field(std::string text) { value_.text = std::move(text); }

    [[nodiscard]] const ui::TextEditingValue &editingValue() const override { return value_; }
    void updateEditingValue(const ui::TextEditingValue &value) override { value_ = value; }
    void performAction(ui::ImeAction action) override { actions.push_back(action); }

    std::vector<ui::ImeAction> actions;

private:
    ui::TextEditingValue value_;
};

ui::TextEditingValue typed(std::string text) {
    const auto end = static_cast<std::int32_t>(text.size());
    return {.text = std::move(text), .selection = {end, end}};
}

// Typing in one field and moving to the next, with the IME's last edit for the first field
// arriving after the move (as Gboard's finishComposingText after "Next" does).
void testStaleEditAfterFocusMoves() {
    Platform platform;
    ui::TextInputService service;
    runtime::ImeBridge bridge(platform, service);
    service.setPlatform(&bridge);

    Field name("");
    Field host("");
    service.attach(&name);
    service.requestShow();
    EXPECT(platform.shown);
    const std::uint64_t nameSession = platform.shown->session;

    bridge.fromIme(typed("Home server"), nameSession);
    EXPECT(name.editingValue().text == "Home server");

    service.attach(&host);
    EXPECT(platform.shown);
    const std::uint64_t hostSession = platform.shown->session;
    EXPECT(hostSession != nameSession);

    // The late edit and action for the name field: neither may reach the host field.
    bridge.fromIme(typed("Home server"), nameSession);
    bridge.action(ui::ImeAction::next, nameSession);
    EXPECT(host.editingValue().text.empty());
    EXPECT(host.actions.empty());
    EXPECT(name.editingValue().text == "Home server");

    bridge.fromIme(typed("nas.example"), hostSession);
    bridge.action(ui::ImeAction::done, hostSession);
    EXPECT(host.editingValue().text == "nas.example");
    EXPECT(host.actions.size() == 1 && host.actions[0] == ui::ImeAction::done);
    EXPECT(name.editingValue().text == "Home server");
}

// Showing the keyboard again for the same field keeps its session, so edits already on their way
// from the IME still land.
void testSameFieldKeepsItsSession() {
    Platform platform;
    ui::TextInputService service;
    runtime::ImeBridge bridge(platform, service);
    service.setPlatform(&bridge);

    Field name("");
    service.attach(&name);
    service.requestShow();
    const std::uint64_t session = platform.shown->session;
    service.requestShow();
    EXPECT(platform.shown->session == session);
    bridge.fromIme(typed("a"), session);
    EXPECT(name.editingValue().text == "a");

    // App-side changes carry the session too, so the platform can match them to its field.
    name.updateEditingValue(typed("ab"));
    service.notifyValueChanged();
    EXPECT(!platform.updates.empty() && platform.updates.back().session == session);
}

// After the keyboard is hidden, the next field shown is a new session even if it's the same
// field: anything the IME sends meanwhile belongs to the old one.
void testHideEndsTheSession() {
    Platform platform;
    ui::TextInputService service;
    runtime::ImeBridge bridge(platform, service);
    service.setPlatform(&bridge);

    Field name("");
    service.attach(&name);
    service.requestShow();
    const std::uint64_t first = platform.shown->session;
    service.requestHide();
    service.requestShow();
    EXPECT(platform.shown && platform.shown->session != first);
    bridge.fromIme(typed("late"), first);
    EXPECT(name.editingValue().text.empty());
}

}  // namespace

int main() {
    testStaleEditAfterFocusMoves();
    testSameFieldKeepsItsSession();
    testHideEndsTheSession();
    std::puts("ime_bridge_test: ok");
    return 0;
}
