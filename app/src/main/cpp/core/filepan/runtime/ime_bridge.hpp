// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <optional>

#include "filepan/runtime/platform.hpp"
#include "filepan/ui/text_input.hpp"

namespace filepan::runtime {

// Forwards the focused text field to the platform's IME, skipping updates the IME already has.
class ImeBridge final : public ui::TextInputPlatform {
public:
    ImeBridge(Platform &platform, ui::TextInputService &service)
        : platform_(platform), service_(service) {}

    void show(const ui::TextInputConfig &config) override {
        auto *client = service_.client();
        if (client == nullptr) {
            return;
        }
        // A new session per field: the IME may still deliver an edit to the field it was on.
        if (client != sessionClient_) {
            sessionClient_ = client;
            ++session_;
        }
        last_ = TextInputState{client->editingValue(), config, session_};
        shown_ = true;
        platform_.showIme(client->caretRect(), *last_);
    }

    void hide() override {
        shown_ = false;
        sessionClient_ = nullptr;
        last_.reset();
        platform_.hideIme();
    }

    void updateEditingValue(const ui::TextEditingValue &value) override {
        auto *client = service_.client();
        if (!shown_ || client == nullptr) {
            return;
        }
        TextInputState state{value, client->inputConfig(), session_};
        if (last_ && *last_ == state) {
            return;
        }
        last_ = state;
        platform_.updateIme(state);
    }

    // The IME's own edit: apply it and remember it so it isn't echoed back. One from an earlier
    // session was meant for a field that has since lost focus, and would overwrite this one.
    void fromIme(const ui::TextEditingValue &value, std::uint64_t session) {
        auto *client = service_.client();
        if (client == nullptr || session != session_) {
            return;
        }
        last_ = TextInputState{value, client->inputConfig(), session_};
        client->updateEditingValue(value);
    }

    void action(ui::ImeAction action, std::uint64_t session) {
        if (session != session_) {
            return;
        }
        if (auto *client = service_.client()) {
            client->performAction(action);
        }
    }

private:
    Platform &platform_;
    ui::TextInputService &service_;
    std::optional<TextInputState> last_;
    bool shown_ = false;
    const ui::TextInputClient *sessionClient_ = nullptr;
    std::uint64_t session_ = 0;
};

}  // namespace filepan::runtime
