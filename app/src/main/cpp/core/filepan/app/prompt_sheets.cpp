// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/prompt_sheets.hpp"

#include <algorithm>
#include <utility>

#include "filepan/app/screen_parts.hpp"
#include "filepan/app/sheet_parts.hpp"
#include "filepan/ui/components/recipes.hpp"

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;

std::string hostPort(const std::string &host, int port) {
    return port == 22 ? host : host + ":" + std::to_string(port);
}

// "ssh-ed25519" -> "ED25519".
std::string keyTypeLabel(const std::string &type) {
    std::string out = type.starts_with("ssh-") ? type.substr(4) : type;
    for (char &c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

// A sunken well around content.
std::shared_ptr<Box> well(float gap = 8) {
    auto box = Box::create({.gap = gap, .padding = Edges::all(16)});
    box->setPaintHook([](PaintContext &context) {
        const Theme &t = currentTheme();
        recipes::sunken(context.list, {context.bounds(), gfx::CornerRadii::uniform(context.px(t.radii.input))}, t,
                        context.density);
    });
    return box;
}

// A fingerprint block: a mono caption with a barcode, then the fingerprint itself.
NodePtr fingerprintBlock(const std::string &caption, const std::string &fingerprint, gfx::Color color,
                         bool barcode = true) {
    const Theme &t = currentTheme();
    auto box = well(10);
    auto top = Box::create({.direction = Direction::row, .justify = Justify::spaceBetween, .alignItems = Align::center});
    top->appendChild(Text::create(caption, t.type.label, textInk(t, t.text.label, 11)));
    if (barcode) {
        top->appendChild(Barcode(fingerprint, 72, 18));
    }
    box->appendChild(top);
    // The fingerprint is case-sensitive base64: no caps.
    std::string shown = fingerprint;
    auto text = Text::create(shown, monoText(t, 14, 500), color);
    text->updateSemantics([&](Semantics &s) { s.label = caption + ": " + fingerprint; });
    box->appendChild(text);
    return box;
}

// Which host of the chain: hops may share a name and differ by port.
std::string hopLabel(const std::string &username, const std::string &host, int port, bool isJumpHost,
                     std::size_t hop) {
    std::string who = username.empty() ? host : username + "@" + host;
    if (port != 22) {
        who += ":" + std::to_string(port);
    }
    if (isJumpHost) {
        who = "jump host " + std::to_string(hop + 1) + " (" + who + ")";
    }
    return who;
}

std::size_t codePoints(const std::string &text) {
    return static_cast<std::size_t>(
            std::ranges::count_if(text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

// The request still waits for an answer.
bool isPending(const PromptSheets::Request &request) {
    return std::visit(
            [](const auto &r) {
                if constexpr (std::is_same_v<std::decay_t<decltype(r)>, PromptSheets::SecurityKey>) {
                    return !r.session->isCancelled()
                           && r.session->state().step != vfs::SecurityKeySession::Step::kDone;
                } else {
                    return r.promise.isPending();
                }
            },
            request);
}

// Answers no. (Does nothing to one already answered.)
void decline(PromptSheets::Request &request) {
    std::visit(
            [](auto &r) {
                using T = std::decay_t<decltype(r)>;
                if constexpr (std::is_same_v<T, PromptSheets::HostKey>) {
                    r.promise.fulfil(vfs::HostKeyDecision::kReject);
                } else if constexpr (std::is_same_v<T, PromptSheets::SecurityKey>) {
                    if (r.session->state().step != vfs::SecurityKeySession::Step::kDone) {
                        r.session->cancel();
                    }
                } else {
                    r.promise.fulfil(std::optional<std::string>());
                }
            },
            request);
}

}  // namespace

std::shared_ptr<PromptSheets> PromptSheets::create(AppContext &context,
                                                   std::function<std::string(const std::string &id)> serverName) {
    return std::make_shared<PromptSheets>(context, std::move(serverName));
}

// --- From the I/O threads ---

void PromptSheets::post(Request request) {
    std::weak_ptr<PromptSheets> weak = weak_from_this();
    // Shared so the posted closure stays copyable.
    auto held = std::make_shared<Request>(std::move(request));
    context_.services.loop.post([weak, held] {
        if (auto self = weak.lock()) {
            self->enqueue(std::move(*held));
        }
        // Otherwise the promise goes with the closure, which declines.
    });
}

void PromptSheets::requestPassword(vfs::PasswordRequest request, DecisionPromise<std::optional<std::string>> promise) {
    post(ArchivePassword{std::move(request), std::move(promise)});
}

void PromptSheets::requestHostKey(vfs::HostKeyPrompt prompt, DecisionPromise<vfs::HostKeyDecision> promise) {
    post(HostKey{std::move(prompt), std::move(promise)});
}

void PromptSheets::requestCredential(vfs::CredentialPrompt prompt, DecisionPromise<std::optional<std::string>> promise) {
    post(Credential{std::move(prompt), std::move(promise)});
}

void PromptSheets::requestSecurityKey(vfs::SecurityKeyPrompt prompt, std::shared_ptr<vfs::SecurityKeySession> session) {
    std::weak_ptr<PromptSheets> weak = weak_from_this();
    std::weak_ptr<vfs::SecurityKeySession> weakSession = session;
    session->setOnChange([weak, weakSession, &loop = context_.services.loop] {
        loop.post([weak, weakSession] {
            auto self = weak.lock();
            auto session = weakSession.lock();
            if (self && session) {
                self->securityKeyChanged(session.get());
            }
        });
    });
    post(SecurityKey{std::move(prompt), std::move(session)});
}

// --- UI thread ---

void PromptSheets::enqueue(Request request) {
    if (shutDown_) {
        return;
    }
    queue_.push_back(std::move(request));
    if (!current_) {
        showNext();
    }
}

void PromptSheets::showNext() {
    while (!current_ && !queue_.empty() && !shutDown_) {
        Request next = std::move(queue_.front());
        queue_.pop_front();
        // A request whose job gave up meanwhile (cancelled) needs no answer.
        if (!isPending(next)) {
            continue;
        }
        current_ = std::move(next);
        show(*current_);
    }
}

void PromptSheets::show(Request &request) {
    field_ = nullptr;
    securityBody_.reset();
    securityState_.reset();
    pinError_.clear();
    pinLetters_ = context_.state.settings.peek().cardPinLetters;
    pinDraft_.clear();
    std::string title;
    NodePtr content;
    if (auto *hostKey = std::get_if<HostKey>(&request)) {
        title = hostKey->prompt.status == vfs::HostKeyPrompt::Status::kChanged ? "Host key changed" : "Trust this host?";
        content = hostKeyContent(*hostKey);
    } else if (auto *credential = std::get_if<Credential>(&request)) {
        const auto &p = credential->prompt;
        const bool passphrase = p.kind == vfs::CredentialPrompt::Kind::kPassphrase;
        title = passphrase ? "Key passphrase" : "Password";
        const std::string who = hopLabel(p.username, p.host, p.port, p.isJumpHost, p.hop);
        content = secretContent(passphrase ? "The private key for " + who + " is encrypted." : "Log in to " + who + ".",
                                serverName_(p.serverId),
                                p.retry, passphrase ? "Passphrase" : "Password",
                                passphrase ? "Unlock" : "Log in");
    } else if (auto *archive = std::get_if<ArchivePassword>(&request)) {
        const auto &r = archive->request;
        title = "Archive password";
        const std::string file(r.archive.name());
        const std::string entry = r.entryPath == "/" ? std::string("The archive's contents are encrypted.")
                                                      : "“" + r.entryPath.substr(r.entryPath.starts_with('/') ? 1 : 0) +
                                                                "” is encrypted.";
        content = secretContent(entry, file, r.retry, "Password", "Open");
    } else if (auto *securityKey = std::get_if<SecurityKey>(&request)) {
        title = securityKey->session->kind() == vfs::SecurityKeySession::Kind::kOpenPgpCard ? "OpenPGP card"
                                                                                             : "Security key";
        content = securityKeyContent(*securityKey);
    }
    const std::uint64_t generation = ++generation_;
    std::weak_ptr<PromptSheets> weak = weak_from_this();
    sheet_ = &context_.overlays.showSheet(std::move(title), parts::scrolling(std::move(content)), true, [weak, generation] {
        auto self = weak.lock();
        if (self && self->generation_ == generation) {
            self->finish();
        }
    });
    if (field_ != nullptr) {
        field_->editor().requestFocus();
    }
}

NodePtr PromptSheets::hostKeyContent(const HostKey &request) {
    const Theme &t = currentTheme();
    const auto &p = request.prompt;
    const bool changed = p.status == vfs::HostKeyPrompt::Status::kChanged;
    const std::string name = serverName_(p.serverId);
    const std::string where = hostPort(p.host, p.port);
    auto content = Box::create({.gap = 14, .padding = {24, 0, 24, 8}});

    auto stamps = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 8});
    if (changed) {
        stamps->appendChild(Stamp("Key changed", StampTone::destructive, "warning"));
    } else {
        stamps->appendChild(Stamp("Unknown host", StampTone::accent, "fingerprint"));
    }
    if (p.isJumpHost) {
        stamps->appendChild(OutlineTag("Jump host " + std::to_string(p.hop + 1)));
    }
    content->appendChild(stamps);

    auto host = Text::create(where, monoText(t, 15, 600), t.colors.ink);
    host->setMaxLines(1);
    host->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    content->appendChild(host);

    const gfx::Color danger = legible(t.colors.destructive, t.colors.groundDeep);
    if (changed) {
        auto warning = Box::create({.direction = Direction::row, .gap = 12, .padding = Edges::all(16)});
        warning->setPaintHook([](PaintContext &context) {
            const Theme &th = currentTheme();
            const gfx::RoundedRect shape{context.bounds(), gfx::CornerRadii::uniform(context.px(th.radii.input))};
            context.list.fill(shape, th.colors.destructive.withAlpha(0.16f));
            context.list.stroke(shape, context.px(1.5f), th.colors.destructive.withAlpha(0.85f));
        });
        auto bar = Box::create({.alignSelf = Align::stretch, .width = 4},
                               {.background = gfx::Paint{t.colors.destructive}, .radii = gfx::CornerRadii::uniform(2)});
        warning->appendChild(bar);
        auto text = Box::create({.gap = 6, .grow = 1, .shrink = 1, .basis = 0});
        text->appendChild(Text::create("Possible interception", withWeight(t.type.body, 700), danger));
        auto body = Text::create(name + " presented a different key than the one you trusted. Someone may be "
                                        "intercepting the connection, or the server was reinstalled. Reject "
                                        "unless you know why it changed.",
                                 t.type.bodySmall, ink(t, t.text.body));
        text->appendChild(body);
        warning->appendChild(text);
        content->appendChild(warning);
        content->appendChild(fingerprintBlock("Trusted · " + keyTypeLabel(p.keyType), p.knownFingerprint,
                                              ink(t, t.text.label), false));
        content->appendChild(fingerprintBlock("Presented now · " + keyTypeLabel(p.keyType), p.fingerprint, danger));
    } else {
        content->appendChild(Text::create("First connection to " + name +
                                                  ". Check that this fingerprint matches the server's before you "
                                                  "trust it.",
                                          t.type.bodySmall, ink(t, t.text.body)));
        content->appendChild(fingerprintBlock(keyTypeLabel(p.keyType) + " · SHA256", p.fingerprint, t.colors.ink));
    }

    std::weak_ptr<PromptSheets> weak = weak_from_this();
    const auto answer = [weak](vfs::HostKeyDecision decision) {
        return [weak, decision] {
            if (auto self = weak.lock()) {
                self->answerHostKey(decision);
            }
        };
    };
    const auto grow = [](NodePtr node) {
        node->updateStyle([](Style &s) {
            s.grow = 1;
            s.shrink = 1;
            s.basis = 0;
        });
        return node;
    };
    auto actions = Box::create({.gap = 10, .margin = {0, 6, 0, 0}});
    if (changed) {
        // Reject is the default: the one primary button, first in focus order.
        actions->appendChild(Button::create({.label = "Reject",
                                             .variant = ButtonVariant::primary,
                                             .icon = "block",
                                             .onTap = answer(vfs::HostKeyDecision::kReject),
                                             .accessibilityLabel = "Reject the new key"}));
        auto row = Box::create({.direction = Direction::row, .gap = 10});
        row->appendChild(grow(Button::create({.label = "Accept once",
                                              .variant = ButtonVariant::glass,
                                              .onTap = answer(vfs::HostKeyDecision::kAcceptOnce),
                                              .haptic = Haptic::reject,
                                              .accessibilityLabel = "Accept the new key once"})));
        row->appendChild(grow(Button::create({.label = "Replace key",
                                              .variant = ButtonVariant::glass,
                                              .onTap = answer(vfs::HostKeyDecision::kAcceptAndSave),
                                              .haptic = Haptic::reject,
                                              .accessibilityLabel = "Replace the trusted key with the new one"})));
        actions->appendChild(row);
    } else {
        actions->appendChild(Button::create({.label = "Trust and save",
                                             .variant = ButtonVariant::primary,
                                             .icon = "verified_user",
                                             .onTap = answer(vfs::HostKeyDecision::kAcceptAndSave),
                                             .accessibilityLabel = "Trust this key and remember it"}));
        auto row = Box::create({.direction = Direction::row, .gap = 10});
        row->appendChild(grow(Button::create({.label = "Reject",
                                              .variant = ButtonVariant::glass,
                                              .onTap = answer(vfs::HostKeyDecision::kReject)})));
        row->appendChild(grow(Button::create({.label = "Accept once",
                                              .variant = ButtonVariant::glass,
                                              .onTap = answer(vfs::HostKeyDecision::kAcceptOnce),
                                              .accessibilityLabel = "Trust this key for this connection only"})));
        actions->appendChild(row);
    }
    content->appendChild(actions);
    return content;
}

NodePtr PromptSheets::secretContent(const std::string &message, const std::string &detail, bool retry,
                                    const std::string &fieldLabel, const std::string &submitLabel) {
    const Theme &t = currentTheme();
    auto content = Box::create({.gap = 14, .padding = {24, 0, 24, 8}});
    if (!detail.empty()) {
        auto tag = Box::create({.direction = Direction::row});
        tag->appendChild(BarcodeTag(detail));
        content->appendChild(tag);
    }
    content->appendChild(Text::create(message, t.type.bodySmall, ink(t, t.text.body)));
    std::weak_ptr<PromptSheets> weak = weak_from_this();
    auto submit = [weak] {
        auto self = weak.lock();
        if (self && self->field_ != nullptr) {
            self->answerSecret(self->field_->editor().editingValue().text);
        }
    };
    auto field = TextField::create({.label = fieldLabel,
                                    .icon = "key",
                                    .helper = std::string(retry ? "" : "Not saved: asked again next time"),
                                    .error = std::string(retry ? "That didn't work. Try again." : ""),
                                    .config = {.keyboard = KeyboardType::password,
                                               .action = ImeAction::done},
                                    .onSubmit = submit});
    field_ = field.get();
    content->appendChild(field);
    auto row = Box::create({.direction = Direction::row, .gap = 10, .margin = {0, 4, 0, 0}});
    auto cancel = Button::create({.label = "Cancel", .variant = ButtonVariant::glass, .onTap = [weak] {
                                      if (auto self = weak.lock()) {
                                          self->cancel();
                                      }
                                  }});
    auto ok = Button::create({.label = submitLabel, .variant = ButtonVariant::primary, .icon = "lock_open", .onTap = submit});
    for (Node *b : {static_cast<Node *>(cancel.get()), static_cast<Node *>(ok.get())}) {
        b->updateStyle([](Style &s) {
            s.grow = 1;
            s.shrink = 1;
            s.basis = 0;
        });
    }
    row->appendChild(cancel);
    row->appendChild(ok);
    content->appendChild(row);
    return content;
}

NodePtr PromptSheets::securityKeyContent(const SecurityKey &request) {
    const Theme &t = currentTheme();
    const auto &p = request.prompt;
    auto content = Box::create({.gap = 14, .padding = {24, 0, 24, 8}});
    auto stamps = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 8});
    securityCard_ = request.session->kind() == vfs::SecurityKeySession::Kind::kOpenPgpCard;
    if (securityCard_) {
        stamps->appendChild(Stamp(p.userVerification ? "Card and PIN" : "Card", StampTone::accent, "contactless"));
    } else {
        stamps->appendChild(Stamp(p.userVerification ? "Key and PIN" : "Key", StampTone::accent, "key"));
    }
    if (p.isJumpHost) {
        stamps->appendChild(OutlineTag("Jump host " + std::to_string(p.hop + 1)));
    }
    content->appendChild(stamps);
    auto host = Text::create(hopLabel(p.username, p.host, p.port, false, p.hop), monoText(t, 15, 600),
                             t.colors.ink);
    host->setMaxLines(1);
    host->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    content->appendChild(host);
    securityBody_ = Box::create({.gap = 14});
    content->appendChild(securityBody_);
    fillSecurityKeyBody(request.session->state());
    std::weak_ptr<PromptSheets> weak = weak_from_this();
    content->appendChild(Button::create({.label = "Cancel",
                                         .variant = ButtonVariant::glass,
                                         .onTap = [weak] {
                                             if (auto self = weak.lock()) {
                                                 self->cancel();
                                             }
                                         },
                                         .accessibilityLabel = securityCard_ && !p.userVerification
                                                                       ? "Cancel reading the card"
                                                                       : "Cancel signing in"}));
    return content;
}

void PromptSheets::fillSecurityKeyBody(const vfs::SecurityKeySession::State &state) {
    using Step = vfs::SecurityKeySession::Step;
    using Problem = vfs::SecurityKeySession::Problem;
    const Theme &t = currentTheme();
    securityState_ = state;
    securityBody_->clearChildren();
    field_ = nullptr;
    const auto say = [&](const std::string &text) {
        securityBody_->appendChild(Text::create(text, t.type.bodySmall, ink(t, t.text.body)));
    };
    // In a row, so it keeps its own width.
    const auto stamp = [&](std::string text, StampTone tone, std::string icon) {
        auto row = Box::create({.direction = Direction::row});
        row->appendChild(Stamp(std::move(text), tone, std::move(icon)));
        securityBody_->appendChild(row);
    };
    // The FIDO key's words, or the OpenPGP card's.
    const auto pick = [card = securityCard_](const char *key, const char *openPgpCard) -> std::string {
        return card ? openPgpCard : key;
    };
    switch (state.step) {
    case Step::kWaitingForKey:
    case Step::kDone:
        switch (state.problem) {
        case Problem::kNone:
            say(pick("Plug in your security key, or hold it to the back of the phone.",
                     "Plug in your YubiKey or other OpenPGP card, or hold it to the back of the phone."));
            break;
        case Problem::kConnectionLost:
            stamp(pick("Key lost", "Card lost"), StampTone::destructive, "warning");
            say(pick("The key went before it finished. Plug it in again, or hold it still against the phone.",
                     "The card went before it finished. Plug it in again, or hold it still against the phone."));
            break;
        case Problem::kWrongKey:
            stamp(pick("Wrong key", "Wrong card"), StampTone::destructive, "warning");
            say(pick("That security key doesn't have this SSH key. Present the one it was made on.",
                     "That card doesn't have the key saved for this server. Present the one it was read from."));
            break;
        case Problem::kNotASecurityKey:
            stamp(pick("Not a security key", "Not an OpenPGP card"), StampTone::destructive, "warning");
            say(pick("That isn't a security key. Plug in or hold up a FIDO security key.",
                     "That has no OpenPGP card on it. Plug in or hold up a YubiKey or other OpenPGP card."));
            break;
        }
        break;
    case Step::kWorking:
        say(pick("Talking to the security key. Keep it where it is.", "Talking to the card. Keep it where it is."));
        break;
    case Step::kTouch:
        stamp("Touch the key", StampTone::accent, "fingerprint");
        say(pick("Touch the sensor or button on your security key.",
                 "Touch the sensor or button on your YubiKey: its touch policy asks for one."));
        break;
    case Step::kPin: {
        say(pick("Enter the security key's PIN.", "Enter the card's PIN (its user PIN, not the admin PIN)."));
        std::string helper = pick("Not saved: asked again next time", "Remembered for 10 minutes after its last use");
        if (state.pinRetries > 0) {
            helper = std::to_string(state.pinRetries) + (state.pinRetries == 1 ? " try" : " tries") +
                     pick(" left before the key locks", " left before the card locks");
        }
        std::string error = pinError_;
        if (error.empty() && state.pinWrong) {
            error = state.pinRetries > 0 ? "Wrong PIN. " + helper + "." : "Wrong PIN. Try again.";
        }
        std::weak_ptr<PromptSheets> weak = weak_from_this();
        auto submit = [weak] {
            auto self = weak.lock();
            if (self && self->field_ != nullptr) {
                self->answerSecret(self->field_->editor().editingValue().text);
            }
        };
        // The number pad or letters, as the settings say; a PIN may have either, so it can switch.
        auto field = TextField::create(
                {.label = "PIN",
                 .text = std::exchange(pinDraft_, {}),
                 .icon = "key",
                 .helper = std::move(helper),
                 .error = std::move(error),
                 .config = {.keyboard = pinLetters_ ? KeyboardType::password : KeyboardType::numberPassword,
                            .action = ImeAction::done},
                 .onSubmit = submit});
        field_ = field.get();
        securityBody_->appendChild(field);
        auto row = Box::create({.direction = Direction::row, .gap = 10});
        auto unlock = Button::create(
                {.label = "Unlock", .variant = ButtonVariant::primary, .icon = "lock_open", .onTap = submit});
        unlock->updateStyle([](Style &s) {
            s.grow = 1;
            s.shrink = 1;
            s.basis = 0;
        });
        row->appendChild(unlock);
        row->appendChild(Button::create({.label = pinLetters_ ? "Digits" : "Letters",
                                         .variant = ButtonVariant::glass,
                                         .icon = pinLetters_ ? "dialpad" : "keyboard",
                                         .onTap =
                                                 [weak] {
                                                     auto self = weak.lock();
                                                     if (!self || self->field_ == nullptr || !self->securityState_) {
                                                         return;
                                                     }
                                                     self->pinDraft_ = self->field_->editor().editingValue().text;
                                                     self->pinLetters_ = !self->pinLetters_;
                                                     self->fillSecurityKeyBody(*self->securityState_);
                                                     if (self->field_ != nullptr) {
                                                         self->field_->editor().requestFocus();
                                                     }
                                                 },
                                         .accessibilityLabel = pinLetters_ ? "Type the PIN with the number pad"
                                                                           : "Type the PIN with letters"}));
        securityBody_->appendChild(row);
        break;
    }
    }
}

void PromptSheets::securityKeyChanged(const vfs::SecurityKeySession *session) {
    auto *request = current_ ? std::get_if<SecurityKey>(&*current_) : nullptr;
    if (request == nullptr || request->session.get() != session || securityBody_ == nullptr) {
        return;
    }
    const auto state = session->state();
    const bool same = securityState_ && securityState_->step == state.step &&
                      securityState_->problem == state.problem && securityState_->pinRetries == state.pinRetries &&
                      securityState_->pinWrong == state.pinWrong;
    if (same) {
        return;
    }
    if (field_ != nullptr) {
        field_->editor().clearFocus();
    }
    if (state.step == vfs::SecurityKeySession::Step::kDone) {
        if (sheet_ != nullptr) {
            sheet_->dismiss();
        }
        return;
    }
    pinError_.clear();
    fillSecurityKeyBody(state);
    if (field_ != nullptr) {
        field_->editor().requestFocus();
    }
}

void PromptSheets::answerHostKey(vfs::HostKeyDecision decision) {
    if (auto *request = current_ ? std::get_if<HostKey>(&*current_) : nullptr) {
        request->promise.fulfil(decision);
        if (sheet_ != nullptr) {
            sheet_->dismiss();
        }
    }
}

void PromptSheets::answerSecret(std::string secret) {
    if (!current_ || std::holds_alternative<HostKey>(*current_)) {
        return;
    }
    if (auto *securityKey = std::get_if<SecurityKey>(&*current_)) {
        // The sheet stays, and follows the signing on.
        if (!securityState_ || securityState_->step != vfs::SecurityKeySession::Step::kPin) {
            return;
        }
        if (field_ != nullptr) {
            field_->editor().clearFocus();
        }
        if (codePoints(secret) < (securityCard_ ? 6u : 4u)) {
            pinError_ = securityCard_ ? "An OpenPGP card's PIN has at least 6 characters." : "A PIN has at least 4 characters.";
            fillSecurityKeyBody(*securityState_);
            if (field_ != nullptr) {
                field_->editor().requestFocus();
            }
            return;
        }
        securityKey->session->submitPin(std::move(secret));
        return;
    }
    std::visit(
            [&](auto &r) {
                using T = std::decay_t<decltype(r)>;
                if constexpr (!std::is_same_v<T, HostKey> && !std::is_same_v<T, SecurityKey>) {
                    r.promise.fulfil(std::optional<std::string>(std::move(secret)));
                }
            },
            *current_);
    if (field_ != nullptr) {
        field_->editor().clearFocus();
    }
    if (sheet_ != nullptr) {
        sheet_->dismiss();
    }
}

void PromptSheets::cancel() {
    if (sheet_ != nullptr) {
        if (field_ != nullptr) {
            field_->editor().clearFocus();
        }
        sheet_->dismiss();
    }
}

void PromptSheets::finish() {
    if (current_) {
        // Closed without an answer: decline.
        decline(*current_);
    }
    current_.reset();
    sheet_ = nullptr;
    field_ = nullptr;
    securityBody_.reset();
    securityState_.reset();
    // The next one opens after this sheet has gone.
    std::weak_ptr<PromptSheets> weak = weak_from_this();
    context_.services.loop.post([weak] {
        if (auto self = weak.lock()) {
            self->showNext();
        }
    });
}

void PromptSheets::shutdown() {
    shutDown_ = true;
    for (auto &request : queue_) {
        decline(request);
    }
    queue_.clear();
    if (current_) {
        decline(*current_);
        current_.reset();
    }
}

}  // namespace filepan::app
