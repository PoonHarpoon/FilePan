// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/sftp_screens.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <cstring>

#include "filepan/app/format.hpp"
#include "filepan/ui/components/recipes.hpp"

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;

constexpr const char *kTestPrefix = "test-connection:";

std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::optional<int> parsePort(std::string_view text) {
    int port = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
    if (text.empty() || error != std::errc() || end != text.data() + text.size() || port < 1 || port > 65535) {
        return std::nullopt;
    }
    return port;
}

EndpointErrors validateEndpoint(const EndpointDraft &e) {
    EndpointErrors errors;
    const std::string host = trim(e.host);
    if (host.empty()) {
        errors.host = "Required";
    } else if (std::ranges::any_of(host, [](char c) { return std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '@'; })) {
        errors.host = "A host name or address, without user or path";
    }
    if (!trim(e.port).empty() && !parsePort(trim(e.port))) {
        errors.port = "1 to 65535";
    }
    if (trim(e.username).empty()) {
        errors.username = "Required";
    }
    if (e.privateKey) {
        if (trim(e.keyText()).empty()) {
            errors.key = "Choose a key file or paste a key";
        } else if (const auto &key = checkKey(e.keyText()); !key) {
            errors.key = key.error();
        }
    } else if (e.card && e.cardKey.empty()) {
        errors.key = "Read the key from the card";
    }
    return errors;
}

bool blank(std::string_view text) {
    return std::ranges::all_of(text, [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
}

// "ssh-ed25519" -> "Ed25519", "ecdsa-sha2-nistp256" -> "ECDSA P-256".
std::string keyTypeName(std::string_view type) {
    if (type == "ssh-ed25519") {
        return "Ed25519";
    }
    if (type == "ssh-rsa") {
        return "RSA";
    }
    if (type.starts_with("ecdsa-sha2-nistp")) {
        return "ECDSA P-" + std::string(type.substr(16));
    }
    if (type.starts_with("sk-")) {
        return "Security key";
    }
    return std::string(type);
}

// The key that'll be saved, in a sunken well: a stamp for its type, its fingerprint, and where it
// came from.
NodePtr keyCard(const SshPrivateKey &key, const std::string &source, std::string_view noun = "private key") {
    const Theme &t = currentTheme();
    auto card = Box::create({.gap = 8, .padding = Edges::all(14)});
    card->setPaintHook([](PaintContext &context) {
        const Theme &th = currentTheme();
        recipes::sunken(context.list, {context.bounds(), gfx::CornerRadii::uniform(context.px(th.radii.input))}, th,
                        context.density);
    });
    auto top = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 8});
    top->appendChild(key.encrypted ? Stamp("Encrypted", StampTone::secondary, "lock")
                                   : Stamp(keyTypeName(key.type), StampTone::accent, "key"));
    auto from = Text::create(source, t.type.label, textInk(t, t.text.meta, 11));
    from->setMaxLines(1);
    from->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    from->updateStyle([](Style &s) {
        s.grow = 1;
        s.shrink = 1;
        s.basis = 0;
    });
    top->appendChild(from);
    card->appendChild(top);
    // An encrypted key shows its type and fingerprint only once unlocked.
    const std::string detail =
            key.encrypted ? std::string("Unlocked with its passphrase when connecting") : key.fingerprint;
    auto line = Text::create(detail, key.encrypted ? t.type.bodySmall : monoText(t, 13, 500),
                             key.encrypted ? ink(t, t.text.body) : t.colors.ink);
    line->setMaxLines(key.encrypted ? 2 : 1);
    line->setOverflow(gfx::TextOverflow::ellipsisMiddle);
    card->appendChild(line);
    const std::string spoken = key.encrypted ? "Encrypted private key, " + source
                                             : keyTypeName(key.type) + ' ' + std::string(noun) + ", " + source + ", " + key.fingerprint;
    card->updateSemantics([&](Semantics &s) {
        s.label = spoken;
        s.mergeDescendants = true;
    });
    return card;
}

vfs::SftpEndpoint endpointFrom(const EndpointDraft &e) {
    vfs::SftpEndpoint endpoint;
    endpoint.host = trim(e.host);
    endpoint.port = parsePort(trim(e.port)).value_or(22);
    endpoint.username = trim(e.username);
    if (e.privateKey) {
        endpoint.authentication.type = vfs::SftpAuthentication::Type::kPrivateKey;
        // As parsePrivateKey normalized it; a valid draft has a valid key.
        const auto &key = checkKey(e.keyText());
        endpoint.authentication.privateKey = key ? key->text : e.keyText();
        endpoint.authentication.passphrase = e.passphrase;
    } else if (e.card) {
        endpoint.authentication.type = vfs::SftpAuthentication::Type::kOpenPgpCard;
        endpoint.authentication.cardPublicKey = e.cardKey;
    } else {
        endpoint.authentication.type = vfs::SftpAuthentication::Type::kPassword;
        endpoint.authentication.password = e.password;
    }
    return endpoint;
}

EndpointDraft endpointDraft(const vfs::SftpEndpoint &e) {
    EndpointDraft d;
    d.host = e.host;
    d.port = std::to_string(e.port);
    d.username = e.username;
    d.privateKey = e.authentication.type == vfs::SftpAuthentication::Type::kPrivateKey;
    d.password = e.authentication.password;
    d.key = e.authentication.privateKey;
    d.passphrase = e.authentication.passphrase;
    d.card = e.authentication.type == vfs::SftpAuthentication::Type::kOpenPgpCard;
    d.cardKey = e.authentication.cardPublicKey;
    return d;
}

std::string errorText(const Error &error) {
    std::string text = error.message;
    const char *reason = std::strerror(error.code);
    if (text.empty()) {
        return reason;
    }
    return text;
}

const vfs::SftpServerConfig *findServer(const AppContext &context, const std::string &id) {
    for (const auto &server : context.state.settings.peek().servers) {
        if (server.id == id) {
            return &server;
        }
    }
    return nullptr;
}

std::string displayName(const vfs::SftpServerConfig &server) {
    return server.name.empty() ? server.server.host : server.name;
}

void growRow(Node &node) {
    node.updateStyle([](Style &s) {
        s.grow = 1;
        s.shrink = 1;
        s.basis = 0;
    });
}

}  // namespace

// --- Model ---

const std::string &EndpointDraft::keyText() const { return blank(pastedKey) ? key : pastedKey; }

const std::expected<SshPrivateKey, std::string> &checkKey(const std::string &text) {
    // The saved or picked key and the pasted one, at least.
    struct Remembered {
        bool used = false;
        std::string input;
        std::expected<SshPrivateKey, std::string> result;
    };
    static Remembered remembered[4];
    static std::size_t next = 0;
    for (const Remembered &slot : remembered) {
        if (slot.used && slot.input == text) {
            return slot.result;
        }
    }
    Remembered &slot = remembered[next];
    next = (next + 1) % std::size(remembered);
    slot = {.used = true, .input = text, .result = parsePrivateKey(text)};
    return slot.result;
}

bool DraftErrors::empty() const {
    return name.empty() && server.empty() && std::ranges::all_of(jumps, [](const EndpointErrors &e) { return e.empty(); });
}

ServerDraft draftFrom(const vfs::SftpServerConfig &config) {
    ServerDraft draft;
    draft.name = config.name;
    draft.server = endpointDraft(config.server);
    for (const auto &jump : config.jumpHosts) {
        draft.jumps.push_back(endpointDraft(jump));
    }
    return draft;
}

DraftErrors validate(const ServerDraft &draft) {
    DraftErrors errors;
    if (trim(draft.name).empty()) {
        errors.name = "Required";
    }
    errors.server = validateEndpoint(draft.server);
    for (const auto &jump : draft.jumps) {
        errors.jumps.push_back(validateEndpoint(jump));
    }
    return errors;
}

vfs::SftpServerConfig configFrom(const ServerDraft &draft, const std::string &id, const std::string &knownHosts) {
    vfs::SftpServerConfig config;
    config.id = id;
    config.name = trim(draft.name);
    config.server = endpointFrom(draft.server);
    for (const auto &jump : draft.jumps) {
        config.jumpHosts.push_back(endpointFrom(jump));
    }
    config.knownHostsPath = knownHosts;
    return config;
}

std::string newServerId(const std::string &name, const std::vector<vfs::SftpServerConfig> &existing) {
    std::string slug;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            slug += static_cast<char>(std::tolower(u));
        } else if (!slug.empty() && slug.back() != '-') {
            slug += '-';
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    if (slug.empty()) {
        slug = "server";
    }
    const auto taken = [&](const std::string &id) {
        return std::ranges::any_of(existing, [&](const auto &s) { return s.id == id; });
    };
    std::string id = slug;
    for (int n = 2; taken(id); ++n) {
        id = slug + "-" + std::to_string(n);
    }
    return id;
}

std::string endpointLine(const vfs::SftpEndpoint &endpoint) {
    std::string line = endpoint.username.empty() ? endpoint.host : endpoint.username + "@" + endpoint.host;
    if (endpoint.port != 22) {
        line += ":" + std::to_string(endpoint.port);
    }
    return line;
}

std::string knownHostsPath(const AppContext &context) {
    return context.services.filesDir.empty() ? std::string() : context.services.filesDir + "/known_hosts";
}

void openServer(AppContext &context, const std::string &id) {
    vfs::SftpBackend *sftp = &context.services.sftp;
    AppContext *ctx = &context;
    const auto *server = findServer(context, id);
    const std::string name = server ? displayName(*server) : id;
    context.notify("Connecting to " + name + "…");
    context.services.io.submit([sftp, id] { return sftp->homeDirectory(id); },
                               [ctx, id, name](Result<std::string> home) {
                                   if (!home) {
                                       if (!home.error().isCancelled()) {
                                           ctx->overlays.showSnackbar(
                                                   {.text = "Couldn't connect to " + name + ": " + errorText(home.error()),
                                                    .durationMs = 6000});
                                       }
                                       return;
                                   }
                                   ctx->overlays.snackbars().dismissCurrent();
                                   ctx->state.navigate(vfs::Location::sftp(id, *home));
                               });
}

void ServerSync::apply(const std::vector<vfs::SftpServerConfig> &servers) {
    for (const auto &old : pushed_) {
        if (std::ranges::none_of(servers, [&](const auto &s) { return s.id == old.id; })) {
            sftp_.removeServer(old.id);
        }
    }
    for (const auto &server : servers) {
        const auto before = std::ranges::find_if(pushed_, [&](const auto &s) { return s.id == server.id; });
        if (before == pushed_.end() || !sameServer(*before, server)) {
            sftp_.setServer(server);
        }
    }
    pushed_ = servers;
}

// --- The list ---

void ServersScreen::construct(std::shared_ptr<std::optional<std::string>> editing) {
    setDebugName("ServersScreen");
    setStyle({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    editing_ = std::move(editing);
    page_ = scrollPage(context_, 12);
    appendChild(page_.screen);
    ServersScreen *self = this;
    build([&] {
        bind([self] {
            (void)self->context_.state.settings().servers;
            (void)theme();
            untrack([&] { self->rebuild(); });
        });
    });
}

void ServersScreen::rebuild() {
    Node &column = *page_.column;
    column.clearChildren();
    cards_.clear();
    const Theme &t = currentTheme();
    const auto &servers = context_.state.settings.peek().servers;

    // The page's header: a big title and a mono count.
    auto header = Box::create({.gap = 6, .padding = {4, 8, 4, 4}});
    auto title = Text::create("SFTP", t.type.display, t.colors.ink);
    title->updateSemantics([](Semantics &s) { s.role = Role::heading; });
    header->appendChild(title);
    header->appendChild(Text::create(servers.empty() ? std::string("No servers saved")
                                                     : std::to_string(servers.size()) + (servers.size() == 1 ? " server" : " servers") +
                                                               " · SSH file transfer",
                                     t.type.label, textInk(t, t.text.label, 11)));
    column.appendChild(header);

    column.appendChild(pageSection("Saved"));
    ServersScreen *self = this;
    for (const auto &server : servers) {
        auto card = Glass::create({.gap = 12, .padding = {18, 16, 10, 16}},
                                  {.radii = gfx::CornerRadii::uniform(t.radii.panel)});
        auto top = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 14});
        auto tile = Box::create({.justify = Justify::center, .alignItems = Align::center, .width = 44, .height = 44});
        tile->setPaintHook([](PaintContext &context) {
            const Theme &th = currentTheme();
            const gfx::RoundedRect shape{context.bounds(), gfx::CornerRadii::uniform(context.px(th.corner(th.radii.iconTile + 2)))};
            context.list.fill(shape, th.colors.tertiary.withAlpha(0.18f));
            context.list.stroke(shape, context.px(1), th.colors.tertiary.withAlpha(0.45f));
        });
        tile->appendChild(Icon::create("dns", 24, legible(t.colors.tertiary, t.colors.ground)));
        top->appendChild(tile);
        auto texts = Box::create({.gap = 4, .grow = 1, .shrink = 1, .basis = 0});
        auto name = Text::create(displayName(server), withWeight(t.type.body, 600), t.colors.ink);
        name->setMaxLines(1);
        texts->appendChild(name);
        auto line = Text::create(endpointLine(server.server), monoText(t, 12), ink(t, t.text.label));
        line->setMaxLines(1);
        line->setOverflow(gfx::TextOverflow::ellipsisMiddle);
        texts->appendChild(line);
        top->appendChild(texts);
        const std::string id = server.id;
        card->appendChild(top);

        auto tags = Box::create({.direction = Direction::row, .alignItems = Align::center, .wrap = true, .gap = 8, .lineGap = 8});
        tags->appendChild(BarcodeTag("SFTP-" + shortCode(server.id).substr(2)));
        const auto login = server.server.authentication.type;
        tags->appendChild(OutlineTag(login == vfs::SftpAuthentication::Type::kPrivateKey     ? std::string("Key")
                                     : login == vfs::SftpAuthentication::Type::kOpenPgpCard ? std::string("Card")
                                                                                            : std::string("Password")));
        if (!server.jumpHosts.empty()) {
            tags->appendChild(OutlineTag("Via " + std::to_string(server.jumpHosts.size()) +
                                         (server.jumpHosts.size() == 1 ? " hop" : " hops")));
        }
        card->appendChild(tags);

        AppContext *ctx = &context_;
        auto pressable = makePressable(*card, {.onTap = [ctx, id] { openServer(*ctx, id); }});
        Glass *raw = card.get();
        card->setOverlayHook([pressable](PaintContext &context) {
            if (pressable->pressed.peek()) {
                const Theme &th = currentTheme();
                context.list.fill(gfx::RoundedRect{context.bounds(), gfx::CornerRadii::uniform(context.px(th.radii.panel))},
                                  th.colors.ink.withAlpha(0.06f));
            }
        });
        card->bind([raw, pressable] {
            (void)pressable->pressed();
            untrack([&] { raw->markPaintDirty(); });
        });
        const std::string spoken = displayName(server) + ", " + endpointLine(server.server);
        card->updateSemantics([&](Semantics &s) {
            s.role = Role::button;
            s.label = spoken;
            s.customActions = {{1, "Edit"}, {2, "Delete"}};
            s.onAction = [self, id](Action action, const ActionArgs &args) {
                if (action == Action::custom) {
                    if (args.customId == 1) {
                        self->edit(id);
                    } else {
                        self->remove(id);
                    }
                    return true;
                }
                return false;
            };
        });
        // The menu drops from the button itself.
        auto holder = std::make_shared<Node *>(nullptr);
        auto button = IconButton::create({.icon = "more_vert",
                                          .label = "Options for " + displayName(server),
                                          .style = IconButtonStyle::plain,
                                          .size = 40,
                                          .onTap = [self, id, ctx, holder] {
                                              if (*holder == nullptr) {
                                                  return;
                                              }
                                              ctx->overlays.showMenu(
                                                      {{"edit", "Edit", [self, id] { self->edit(id); }},
                                                       {"delete", "Delete", [self, id] { self->remove(id); }, true}},
                                                      **holder);
                                          }});
        *holder = button.get();
        top->appendChild(button);
        cards_.push_back(card.get());
        column.appendChild(card);
    }
    if (servers.empty()) {
        auto empty = GlassPanel({.alignItems = Align::start, .gap = 10, .padding = Edges::all(20)});
        empty->appendChild(Stamp("Empty", StampTone::ink));
        empty->appendChild(Text::create("Save a server to browse it like local storage. Host keys are checked on "
                                        "first connection; passwords are sealed with a key only this device holds.",
                                        t.type.bodySmall, ink(t, t.text.body)));
        column.appendChild(empty);
    }
    auto add = Button::create({.label = "Add server", .variant = ButtonVariant::primary, .icon = "add", .onTap = [self] {
                                   self->add();
                               }});
    add->updateStyle([](Style &s) { s.margin = {0, 8, 0, 0}; });
    add_ = add.get();
    column.appendChild(add);
}

void ServersScreen::add() {
    *editing_ = std::nullopt;
    context_.state.openScreen(kServerEditorScreen);
}

void ServersScreen::edit(const std::string &id) {
    *editing_ = id;
    context_.state.openScreen(kServerEditorScreen);
}

void ServersScreen::remove(const std::string &id) {
    const auto &servers = context_.state.settings.peek().servers;
    const auto at = std::ranges::find_if(servers, [&](const auto &s) { return s.id == id; });
    if (at == servers.end()) {
        return;
    }
    const vfs::SftpServerConfig removed = *at;
    const auto index = static_cast<std::size_t>(at - servers.begin());
    context_.state.updateSettings([&](Settings &s) { std::erase_if(s.servers, [&](const auto &x) { return x.id == id; }); });
    AppState *state = &context_.state;
    context_.overlays.showSnackbar({.text = "Deleted “" + displayName(removed) + "”",
                                    .action = "Undo",
                                    .onAction = [state, removed, index] {
                                        state->updateSettings([&](Settings &s) {
                                            s.servers.insert(s.servers.begin() +
                                                                     static_cast<long>(std::min(index, s.servers.size())),
                                                             removed);
                                        });
                                    }});
}

// --- The editor ---

void ServerEditor::construct(std::optional<std::string> editing) {
    setDebugName("ServerEditor");
    setStyle({.position = Position::absolute, .left = 0, .top = 0, .right = 0, .bottom = 0});
    if (editing) {
        if (const auto *server = findServer(context_, *editing)) {
            editing_ = editing;
            *draft_ = draftFrom(*server);
        }
    }
    page_ = scrollPage(context_, 10);
    appendChild(page_.screen);
    ServerEditor *self = this;
    build([&] {
        bind([self] {
            (void)theme();
            untrack([&] { self->rebuild(); });
        });
    });
}

TextField *ServerEditor::field(const std::string &label) const {
    for (const auto &[name, field] : fields_) {
        if (name == label) {
            return field;
        }
    }
    return nullptr;
}

NodePtr ServerEditor::endpointFields(EndpointDraft &endpoint, std::optional<std::size_t> jump) {
    ServerEditor *self = this;
    const std::string prefix = jump ? "Jump host " + std::to_string(*jump + 1) + " " : std::string();
    auto box = Box::create({.gap = 10});
    const auto error = [self, jump](std::string EndpointErrors::*member) {
        return [self, jump, member] {
            const DraftErrors &e = self->errors_();
            if (!jump) {
                return e.server.*member;
            }
            return *jump < e.jumps.size() ? e.jumps[*jump].*member : std::string();
        };
    };
    const auto addField = [&](Node &parent, const std::string &label, const std::string &spoken, std::string &value,
                              TextFieldProps props) {
        props.label = label;
        props.text = value;
        props.onChange = [self, target = &value](const std::string &text) {
            *target = text;
            if (self->showErrors_) {
                self->errors_.set(validate(*self->draft_));
            }
        };
        auto field = TextField::create(std::move(props));
        fields_.emplace_back(spoken, field.get());
        parent.appendChild(field);
        return field;
    };

    auto hostRow = Box::create({.direction = Direction::row, .alignItems = Align::start, .gap = 10});
    auto host = addField(*hostRow, "Host", prefix + "Host", endpoint.host,
                         {.icon = "dns", .error = error(&EndpointErrors::host),
                          .config = {.keyboard = KeyboardType::uri, .action = ImeAction::next}});
    growRow(*host);
    auto port = addField(*hostRow, "Port", prefix + "Port", endpoint.port,
                         {.helper = std::string("Blank: 22"), .error = error(&EndpointErrors::port),
                          .config = {.keyboard = KeyboardType::number, .action = ImeAction::next}});
    port->updateStyle([](Style &s) { s.width = 104; });
    box->appendChild(hostRow);
    addField(*box, "Username", prefix + "Username", endpoint.username,
             {.icon = "person", .error = error(&EndpointErrors::username),
              .config = {.action = ImeAction::next}});

    const std::size_t login = endpoint.card ? 2 : endpoint.privateKey ? 1 : 0;
    auto segmented = SegmentedControl::create(
            {.options = {"Password", "Private key", "Card"},
             .selected = login,
             .onSelect = [self, jump](std::size_t i) { self->setLogin(jump, static_cast<Login>(i)); },
             .label = prefix + "Login with"});
    segmented->updateStyle([](Style &s) { s.margin = {0, 8, 0, 8}; });
    box->appendChild(segmented);
    const TextInputConfig secret{.keyboard = KeyboardType::password, .action = ImeAction::done};
    if (endpoint.privateKey) {
        box->appendChild(keyFields(endpoint, jump, prefix));
        addField(*box, "Passphrase", prefix + "Passphrase", endpoint.passphrase,
                 {.icon = "password", .helper = std::string("Empty: asked for if needed"),
                  .config = secret});
    } else if (endpoint.card) {
        box->appendChild(cardFields(endpoint, jump, prefix));
    } else {
        addField(*box, "Password", prefix + "Password", endpoint.password,
                 {.icon = "password", .helper = std::string("Empty: asked when connecting"),
                  .config = secret});
    }
    return box;
}

NodePtr ServerEditor::keyFields(EndpointDraft &endpoint, std::optional<std::size_t> jump, const std::string &prefix) {
    ServerEditor *self = this;
    EndpointDraft *e = &endpoint;
    auto box = Box::create({.gap = 10});

    // The key that'll be saved, or why the file picked last wasn't taken.
    auto summary = Box::create({.gap = 10});
    summary->bind([self, e, raw = summary.get()] {
        (void)self->keyRevision_();
        (void)theme();
        untrack([&] {
            raw->clearChildren();
            const Theme &th = currentTheme();
            if (!e->pickError.empty()) {
                raw->appendChild(Text::create(e->pickError, th.type.bodySmall,
                                              legible(mix(th.colors.destructive, th.colors.ink, 0.2f), th.colors.ground)));
            }
            const std::string &text = e->keyText();
            if (blank(text)) {
                return;
            }
            // An invalid pasted key: the paste field says why.
            if (const auto &key = checkKey(text)) {
                const std::string source = !blank(e->pastedKey) ? std::string("Pasted")
                                           : e->keySource.empty() ? std::string("Saved")
                                                                  : e->keySource;
                raw->appendChild(keyCard(*key, source));
            }
        });
    });
    box->appendChild(summary);

    auto choose = Button::create({.label = endpoint.key.empty() ? "Choose key file" : "Choose another key file",
                                  .variant = ButtonVariant::glass,
                                  .icon = "file_open",
                                  .enabled = [documents = context_.services.documents] { return documents != nullptr; },
                                  .onTap = [self, jump] { self->pickKey(jump); },
                                  .accessibilityLabel = prefix + "Choose key file"});
    choose->updateStyle([](Style &s) { s.alignSelf = Align::start; });
    box->appendChild(choose);

    auto paste = TextField::create(
            {.label = "Or paste it",
             .text = endpoint.pastedKey,
             .icon = "content_paste",
             .helper = std::string("OpenSSH or PEM private key text"),
             .error =
                     [self, e, jump] {
                         (void)self->keyRevision_();
                         if (!blank(e->pastedKey)) {
                             const auto &key = checkKey(e->pastedKey);
                             return key ? std::string() : key.error();
                         }
                         const DraftErrors &errors = self->errors_();
                         if (!jump) {
                             return errors.server.key;
                         }
                         return *jump < errors.jumps.size() ? errors.jumps[*jump].key : std::string();
                     },
             // A visible password, so the key shows as pasted.
             .config = {.keyboard = KeyboardType::visiblePassword},
             .onChange =
                     [self, e](const std::string &text) {
                         e->pastedKey = text;
                         e->pickError.clear();
                         self->keyRevision_.set(self->keyRevision_.peek() + 1);
                         self->revalidate();
                     },
             .lines = 5});
    fields_.emplace_back(prefix + "Private key", paste.get());
    box->appendChild(paste);
    return box;
}

NodePtr ServerEditor::cardFields(EndpointDraft &endpoint, std::optional<std::size_t> jump, const std::string &prefix) {
    ServerEditor *self = this;
    EndpointDraft *e = &endpoint;
    AppContext *context = &context_;
    // The key read from the card and what to do with it, or how to read one.
    auto box = Box::create({.gap = 10});
    box->bind([self, context, e, jump, prefix, raw = box.get()] {
        (void)self->keyRevision_();
        (void)self->errors_();
        (void)theme();
        untrack([&] {
            raw->clearChildren();
            const Theme &th = currentTheme();
            const auto key = e->cardKey.empty() ? std::expected<SshPublicKey, std::string>(std::unexpected(""))
                                                : parsePublicKey(e->cardKey);
            if (key) {
                const SshPrivateKey shown{.text = {}, .type = key->type, .fingerprint = key->fingerprint, .encrypted = false};
                raw->appendChild(keyCard(shown, key->comment.empty() ? std::string("OpenPGP card") : key->comment,
                                         "card key"));
            } else {
                raw->appendChild(Text::create("Uses the authentication key on a YubiKey or another OpenPGP card, "
                                              "as gpg-agent does. Plug it in or hold it to the phone when asked.",
                                              th.type.bodySmall, ink(th, th.text.body)));
            }
            std::string problem = e->cardError;
            if (problem.empty()) {
                const DraftErrors &errors = self->errors_.peek();
                problem = !jump ? errors.server.key : *jump < errors.jumps.size() ? errors.jumps[*jump].key : "";
            }
            if (!problem.empty()) {
                raw->appendChild(Text::create(problem, th.type.bodySmall,
                                              legible(mix(th.colors.destructive, th.colors.ink, 0.2f), th.colors.ground)));
            }
            auto buttons = Box::create({.direction = Direction::row, .wrap = true, .gap = 8, .lineGap = 8});
            buttons->appendChild(Button::create({.label = e->readingCard ? "Reading…"
                                                          : key           ? "Read again"
                                                                          : "Read key from card",
                                                 .variant = ButtonVariant::glass,
                                                 .icon = "contactless",
                                                 .enabled = [e] { return !e->readingCard; },
                                                 .onTap = [self, jump] { self->readCard(jump); },
                                                 .accessibilityLabel = prefix + "Read key from card"}));
            if (key) {
                buttons->appendChild(Button::create({.label = "Copy public key",
                                                     .variant = ButtonVariant::glass,
                                                     .icon = "content_copy",
                                                     .onTap =
                                                             [context, e] {
                                                                 context->services.platform.setClipboardText(e->cardKey);
                                                                 context->notify("Public key copied, for the server's "
                                                                                 "authorized_keys");
                                                             },
                                                     .accessibilityLabel = prefix + "Copy public key"}));
            }
            raw->appendChild(buttons);
        });
    });
    return box;
}

void ServerEditor::readCard(std::optional<std::size_t> jump) {
    EndpointDraft *e = endpoint(jump);
    if (e == nullptr || e->readingCard) {
        return;
    }
    e->readingCard = true;
    e->cardError.clear();
    keyRevision_.set(keyRevision_.peek() + 1);
    // As logging in through this hop would ask: hops count the jump hosts first.
    const vfs::SecurityKeyPrompt prompt{.serverId = editing_.value_or(std::string()),
                                        .hop = jump ? *jump : draft_->jumps.size(),
                                        .isJumpHost = jump.has_value(),
                                        .host = trim(e->host),
                                        .port = parsePort(trim(e->port)).value_or(22),
                                        .username = trim(e->username),
                                        .userVerification = false};
    vfs::SftpBackend *sftp = &context_.services.sftp;
    std::weak_ptr<Node> weak = weak_from_this();
    ServerEditor *self = this;
    context_.services.io.submit([sftp, prompt] { return sftp->readCardKey(prompt); },
                                [weak, self, jump](Result<vfs::CardKey> key) {
                                    if (!weak.expired()) {
                                        self->cardRead(jump, key);
                                    }
                                });
}

void ServerEditor::cardRead(std::optional<std::size_t> jump, const Result<vfs::CardKey> &key) {
    EndpointDraft *e = endpoint(jump);
    if (e == nullptr) {
        return;
    }
    e->readingCard = false;
    if (key) {
        e->cardKey = key->publicKey;
        e->cardError.clear();
    } else {
        // Cancelled from the sheet: nothing to say.
        e->cardError = key.error().code == ECANCELED ? std::string() : errorText(key.error());
    }
    keyRevision_.set(keyRevision_.peek() + 1);
    revalidate();
}

EndpointDraft *ServerEditor::endpoint(std::optional<std::size_t> jump) {
    if (!jump) {
        return &draft_->server;
    }
    return *jump < draft_->jumps.size() ? &draft_->jumps[*jump] : nullptr;
}

void ServerEditor::revalidate() {
    if (showErrors_) {
        errors_.set(validate(*draft_));
    }
}

void ServerEditor::pickKey(std::optional<std::size_t> jump) {
    runtime::DocumentPicker *documents = context_.services.documents;
    std::weak_ptr<Node> weak = weak_from_this();
    ServerEditor *self = this;
    const bool opened =
            documents != nullptr &&
            documents->pick({.mimeTypes = {"*/*"}, .maxBytes = kMaxPrivateKeyBytes},
                            [weak, self, jump](runtime::PickedDocument document) {
                                if (!weak.expired()) {
                                    self->keyPicked(jump, document);
                                }
                            });
    if (!opened) {
        context_.notify("There's no file picker here; paste the key instead");
    }
}

void ServerEditor::keyPicked(std::optional<std::size_t> jump, const runtime::PickedDocument &document) {
    using Status = runtime::PickedDocument::Status;
    EndpointDraft *e = endpoint(jump);
    if (e == nullptr || !e->privateKey || document.status == Status::cancelled) {
        return;
    }
    const std::string name = document.name.empty() ? std::string("The file") : "“" + document.name + "”";
    if (document.status == Status::failed) {
        e->pickError = "Couldn't read " + (document.name.empty() ? std::string("the file") : name) + ".";
    } else if (document.status == Status::tooLarge) {
        e->pickError = name + " is too large to be a private key.";
    } else if (const auto &key = checkKey(document.bytes); !key) {
        e->pickError = name + ": " + key.error();
    } else {
        e->key = key->text;
        e->keySource = document.name.empty() ? std::string("Key file") : document.name;
        e->pastedKey.clear();
        e->pickError.clear();
        revalidate();
        // For the paste field emptied and the button's label.
        rebuild();
        return;
    }
    keyRevision_.set(keyRevision_.peek() + 1);
    revalidate();
}

void ServerEditor::rebuild() {
    Node &column = *page_.column;
    column.clearChildren();
    fields_.clear();
    const Theme &t = currentTheme();
    ServerEditor *self = this;

    auto header = Box::create({.gap = 8, .padding = {4, 8, 4, 4}});
    auto title = Text::create(isNew() ? "New server" : "Edit server", t.type.display, t.colors.ink);
    title->updateSemantics([](Semantics &s) { s.role = Role::heading; });
    header->appendChild(title);
    if (!isNew()) {
        auto tagRow = Box::create({.direction = Direction::row});
        tagRow->appendChild(BarcodeTag("SFTP-" + shortCode(*editing_).substr(2)));
        header->appendChild(tagRow);
    }
    column.appendChild(header);

    // Server
    column.appendChild(pageSection("Server"));
    auto server = GlassPanel({.gap = 10, .padding = {18, 16, 18, 18}});
    {
        auto name = TextField::create({.label = "Name",
                                       .text = draft_->name,
                                       .icon = "label",
                                       .error = [self] { return self->errors_().name; },
                                       .config = {.action = ImeAction::next},
                                       .onChange = [self](const std::string &text) {
                                           self->draft_->name = text;
                                           if (self->showErrors_) {
                                               self->errors_.set(validate(*self->draft_));
                                           }
                                       }});
        fields_.emplace_back("Name", name.get());
        server->appendChild(name);
        server->appendChild(endpointFields(draft_->server, std::nullopt));
    }
    column.appendChild(server);

    // Jump hosts
    column.appendChild(pageSection("Jump hosts"));
    auto chain = Box::create({.gap = 10});
    if (draft_->jumps.empty()) {
        auto hint = Text::create("Connect directly. Add a hop to go through a bastion first, like OpenSSH's "
                                 "ProxyJump.",
                                 t.type.bodySmall, ink(t, t.text.label));
        hint->updateStyle([](Style &s) { s.margin = {4, 0, 4, 2}; });
        chain->appendChild(hint);
    }
    for (std::size_t i = 0; i < draft_->jumps.size(); ++i) {
        auto hop = GlassPanel({.gap = 8, .padding = {18, 10, 10, 18}});
        auto head = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 10});
        head->appendChild(Stamp("Hop " + std::to_string(i + 1), StampTone::secondary));
        auto arrow = Text::create(i + 1 < draft_->jumps.size() ? "→ hop " + std::to_string(i + 2) : std::string("→ server"),
                                  t.type.label, textInk(t, t.text.meta, 11));
        growRow(*arrow);
        head->appendChild(arrow);
        head->appendChild(IconButton::create({.icon = "close",
                                              .label = "Remove jump host " + std::to_string(i + 1),
                                              .style = IconButtonStyle::plain,
                                              .size = 40,
                                              .onTap = [self, i] { self->removeJump(i); }}));
        hop->appendChild(head);
        auto fields = endpointFields(draft_->jumps[i], i);
        fields->updateStyle([](Style &s) { s.padding.right = 8; });
        hop->appendChild(fields);
        chain->appendChild(hop);
    }
    auto addHop = Button::create({.label = "Add hop", .variant = ButtonVariant::outline, .icon = "add_link",
                                  .onTap = [self] { self->addJump(); }, .accessibilityLabel = "Add a jump host"});
    addHop->updateStyle([](Style &s) { s.alignSelf = Align::start; });
    chain->appendChild(addHop);
    column.appendChild(chain);

    // Check
    column.appendChild(pageSection("Check"));
    auto check = GlassPanel({.gap = 12, .padding = Edges::all(18)});
    auto testRow = Box::create({.direction = Direction::row, .alignItems = Align::center, .gap = 12});
    auto test = Button::create({.label = "Test connection",
                                .variant = ButtonVariant::glass,
                                .icon = "lan",
                                .enabled = [self] { return self->testState() != TestState::running; },
                                .onTap = [self] { self->testConnection(); }});
    test_ = test.get();
    testRow->appendChild(test);
    auto stampSlot = Box::create({.direction = Direction::row});
    stampSlot->bind([slot = stampSlot.get(), self] {
        const TestState state = self->testState();
        untrack([&] {
            slot->clearChildren();
            switch (state) {
            case TestState::idle:
                break;
            case TestState::running:
                slot->appendChild(Stamp("Testing", StampTone::ink, "sync"));
                break;
            case TestState::passed:
                slot->appendChild(Stamp("Connected", StampTone::accent, "check"));
                break;
            case TestState::failed:
                slot->appendChild(Stamp("Failed", StampTone::destructive, "error"));
                break;
            }
        });
    });
    testRow->appendChild(stampSlot);
    check->appendChild(testRow);
    auto message = Text::create("Connects with these settings, asking for host keys and passwords as needed, then "
                                "disconnects.",
                                monoText(t, 12), ink(t, t.text.label));
    message->bind([raw = message.get(), self] {
        const std::string text = self->testMessage();
        const TestState state = self->testState();
        untrack([&] {
            const Theme &th = currentTheme();
            if (!text.empty()) {
                raw->setText(text);
            }
            raw->setColor(state == TestState::failed ? legible(th.colors.destructive, th.colors.ground)
                                                     : ink(th, th.text.label));
        });
    });
    check->appendChild(message);
    column.appendChild(check);

    // Save, and delete for a saved server.
    auto actions = Box::create({.gap = 10, .margin = {0, 10, 0, 0}});
    auto save = Button::create({.label = isNew() ? "Add server" : "Save", .variant = ButtonVariant::primary,
                                .icon = "check", .onTap = [self] { self->save(); }});
    save_ = save.get();
    actions->appendChild(save);
    if (!isNew()) {
        actions->appendChild(Button::create({.label = "Delete server",
                                             .variant = ButtonVariant::danger,
                                             .icon = "delete",
                                             .onTap = [self] { self->deleteServer(); }}));
    }
    column.appendChild(actions);
}

void ServerEditor::setPrivateKey(std::optional<std::size_t> jump, bool privateKey) {
    setLogin(jump, privateKey ? Login::privateKey : Login::password);
}

void ServerEditor::setLogin(std::optional<std::size_t> jump, Login login) {
    EndpointDraft &e = jump ? draft_->jumps.at(*jump) : draft_->server;
    if (e.privateKey == (login == Login::privateKey) && e.card == (login == Login::card)) {
        return;
    }
    e.privateKey = login == Login::privateKey;
    e.card = login == Login::card;
    if (showErrors_) {
        errors_.set(validate(*draft_));
    }
    rebuild();
}

void ServerEditor::addJump() {
    EndpointDraft hop;
    draft_->jumps.push_back(std::move(hop));
    if (showErrors_) {
        errors_.set(validate(*draft_));
    }
    rebuild();
}

void ServerEditor::removeJump(std::size_t index) {
    if (index >= draft_->jumps.size()) {
        return;
    }
    draft_->jumps.erase(draft_->jumps.begin() + static_cast<long>(index));
    if (showErrors_) {
        errors_.set(validate(*draft_));
    }
    rebuild();
}

bool ServerEditor::save() {
    showErrors_ = true;
    DraftErrors errors = validate(*draft_);
    const bool ok = errors.empty();
    errors_.set(std::move(errors));
    if (!ok) {
        context_.notify("Check the highlighted fields");
        return false;
    }
    const auto &servers = context_.state.settings.peek().servers;
    const std::string id = editing_ ? *editing_ : newServerId(draft_->name, servers);
    vfs::SftpServerConfig config = configFrom(*draft_, id, knownHostsPath(context_));
    if (const auto *old = findServer(context_, id)) {
        config.timeoutSeconds = old->timeoutSeconds;
    }
    const bool added = !editing_;
    context_.state.updateSettings([&](Settings &s) {
        const auto at = std::ranges::find_if(s.servers, [&](const auto &x) { return x.id == id; });
        if (at != s.servers.end()) {
            *at = config;
        } else {
            s.servers.push_back(config);
        }
    });
    context_.notify(added ? "Added “" + config.name + "”" : "Saved");
    context_.state.pop();
    return true;
}

void ServerEditor::deleteServer() {
    if (!editing_) {
        return;
    }
    const std::string id = *editing_;
    const auto *server = findServer(context_, id);
    if (server == nullptr) {
        context_.state.pop();
        return;
    }
    const vfs::SftpServerConfig removed = *server;
    AppState *state = &context_.state;
    state->updateSettings([&](Settings &s) { std::erase_if(s.servers, [&](const auto &x) { return x.id == id; }); });
    context_.overlays.showSnackbar({.text = "Deleted “" + displayName(removed) + "”",
                                    .action = "Undo",
                                    .onAction = [state, removed] {
                                        state->updateSettings([&](Settings &s) { s.servers.push_back(removed); });
                                    }});
    state->pop();
}

void ServerEditor::testConnection() {
    showErrors_ = true;
    DraftErrors errors = validate(*draft_);
    // The name isn't needed to connect.
    errors.name.clear();
    const bool ok = errors.empty();
    errors_.set(validate(*draft_));
    if (!ok) {
        testState.set(TestState::failed);
        testMessage.set("Fill in the highlighted fields first.");
        return;
    }
    const std::uint64_t generation = ++testGeneration_;
    const std::string id = kTestPrefix + std::to_string(generation) + ":" + trim(draft_->name);
    vfs::SftpServerConfig config = configFrom(*draft_, id, knownHostsPath(context_));
    vfs::SftpBackend *sftp = &context_.services.sftp;
    sftp->setServer(config);
    testState.set(TestState::running);
    testMessage.set("Connecting to " + endpointLine(config.server) + "…");
    std::weak_ptr<Node> weak = weak_from_this();
    ServerEditor *self = this;
    context_.services.io.submit(
            [sftp, id] {
                auto home = sftp->homeDirectory(id);
                sftp->removeServer(id);
                return home;
            },
            [weak, self, generation](Result<std::string> home) {
                if (weak.expired() || self->testGeneration_ != generation) {
                    return;
                }
                if (home) {
                    self->testState.set(TestState::passed);
                    self->testMessage.set("Logged in. Home: " + *home);
                } else {
                    self->testState.set(TestState::failed);
                    self->testMessage.set(errorText(home.error()));
                }
            });
}

// --- Registration ---

std::vector<ScreenSpec> sftpScreenSpecs() {
    // Which server the editor opens: set by the list just before it opens the editor.
    auto editing = std::make_shared<std::optional<std::string>>();
    return {
            {.id = kServersScreen,
             .title = "SFTP servers",
             .icon = "dns",
             .build =
                     [editing](AppContext &context) {
                         auto node = std::make_shared<ServersScreen>(context);
                         node->construct(editing);
                         return NodePtr(node);
                     },
             .inMenu = false},
            {.id = kServerEditorScreen,
             .title = "Server",
             .icon = "dns",
             .build =
                     [editing](AppContext &context) {
                         auto node = std::make_shared<ServerEditor>(context);
                         node->construct(*editing);
                         return NodePtr(node);
                     },
             .inMenu = false},
            {.id = kNewServerScreen,
             .title = "Server",
             .icon = "dns",
             .build =
                     [editing](AppContext &context) {
                         *editing = std::nullopt;
                         auto node = std::make_shared<ServerEditor>(context);
                         node->construct(std::nullopt);
                         return NodePtr(node);
                     },
             .inMenu = false},
    };
}

}  // namespace filepan::app
