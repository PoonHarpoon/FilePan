// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The SFTP server screens, the prompt sheets, the settings screen, secret storage and the
// licenses, on the app with a fake Platform. With sshd available (FILEPAN_SSHD), the SFTP flows
// run against a throwaway OpenSSH server on localhost: a connection test, a host key accepted and
// saved, then a reconnect without a prompt, a changed key (rejected by default) and a key
// passphrase asked again after a wrong one; and one and two jump hosts set up in the editor, each
// hop its own sshd with its own keys, with the failures naming the hop and the step.

#include <archive.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "app_harness.hpp"
#include "filepan/app/about_screen.hpp"
#include "filepan/app/backup_sheets.hpp"
#include "filepan/app/licenses.hpp"
#include "filepan/app/onboarding_screen.hpp"
#include "filepan/app/prompt_sheets.hpp"
#include "filepan/app/secrets.hpp"
#include "filepan/base64.hpp"
#include "filepan/app/settings_screen.hpp"
#include "filepan/app/sftp_screens.hpp"
#include "filepan/create_archive.hpp"
#ifdef FILEPAN_SSHD
#include "sshd_harness.hpp"
#endif

using namespace filepan;
using namespace filepan::test;
namespace fs = std::filesystem;

namespace {

struct Dirs {
    TempDir dir;
    fs::path home;
    fs::path files;

    Dirs() {
        home = dir.path() / "storage";
        files = dir.path() / "files";
        fs::create_directories(files);
        makeFolder(home / "Download", 2026, 10, 3);
    }
};

std::string readFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

ui::Node *findLabel(ui::Node &root, std::string_view label) {
    if (root.style().display) {
        if (root.semantics().label == label || root.semanticsText() == label) {
            return &root;
        }
        for (const auto &child : root.children()) {
            if (ui::Node *found = findLabel(*child, label)) {
                return found;
            }
        }
    }
    return nullptr;
}

// The first node under root with this role and semantics label.
ui::Node *findControl(ui::Node &root, std::string_view label, ui::Role role) {
    if (!root.style().display) {
        return nullptr;
    }
    if (root.semantics().role == role && root.semantics().label == label) {
        return &root;
    }
    for (const auto &child : root.children()) {
        if (ui::Node *found = findControl(*child, label, role)) {
            return found;
        }
    }
    return nullptr;
}

// The registered screen showing, as T, checked by its debug name.
template <typename T>
T *screen(AppHarness &h, std::string_view name) {
    ui::Node *node = h.app->screenNode();
    return node != nullptr && node->debugName() == name ? static_cast<T *>(node) : nullptr;
}

void type(ui::components::TextField *field, const std::string &text) {
    EXPECT(field != nullptr);
    const auto end = static_cast<std::int32_t>(text.size());
    field->editor().updateEditingValue({text, {end, end}, {}});
}

app::ServerEditor *openEditor(AppHarness &h) {
    h.state().openScreen(app::kServersScreen);
    h.pump(100);
    auto *list = screen<app::ServersScreen>(h, "ServersScreen");
    EXPECT(list != nullptr && list->addButton() != nullptr);
    h.tap(*list->addButton());
    h.pump(100);
    auto *editor = screen<app::ServerEditor>(h, "ServerEditor");
    EXPECT(editor != nullptr && editor->isNew());
    return editor;
}

const vfs::SftpServerConfig *saved(AppHarness &h, const std::string &id) {
    for (const auto &server : h.state().settings.peek().servers) {
        if (server.id == id) {
            return &server;
        }
    }
    return nullptr;
}

// Adding, editing and deleting servers, persisted across a restart, with the password sealed.
void testServerCrud() {
    Dirs d;
    {
        AppHarness h(d.home.string(), d.files.string());
        auto *editor = openEditor(h);
        // A new server starts with every field empty: nothing guessed or filled in.
        for (const char *label : {"Name", "Host", "Port", "Username", "Password"}) {
            EXPECT(editor->field(label) != nullptr && editor->field(label)->editor().editingValue().text.empty());
        }
        // Nothing filled in: the save is refused and the errors show.
        EXPECT(!editor->save());
        EXPECT(!editor->errors().peek().empty() && editor->errors().peek().server.host == "Required");
        EXPECT(screen<app::ServerEditor>(h, "ServerEditor") == editor);

        type(editor->field("Name"), "Home NAS");
        type(editor->field("Host"), "nas.local");
        type(editor->field("Port"), "99999");
        type(editor->field("Username"), "me");
        type(editor->field("Password"), "hunter2");
        EXPECT(!editor->save());
        EXPECT(editor->errors().peek().server.port == "1 to 65535");
        type(editor->field("Port"), "2222");
        // A jump host, which needs its own host and user.
        editor->addJump();
        h.pump(50);
        type(editor->field("Jump host 1 Host"), "bastion.example.org");
        // Nothing is filled in for it: not the server's user, nor a port.
        EXPECT(editor->draft().jumps[0].username.empty() && editor->draft().jumps[0].port.empty());
        EXPECT(!editor->save() && editor->errors().peek().jumps[0].username == "Required");
        type(editor->field("Jump host 1 Username"), "me");
        EXPECT(editor->save());
        h.pump(100);
        EXPECT(screen<app::ServersScreen>(h, "ServersScreen") != nullptr);

        const auto *server = saved(h, "home-nas");
        EXPECT(server != nullptr && server->name == "Home NAS" && server->server.host == "nas.local");
        EXPECT(server->server.port == 2222 && server->server.username == "me");
        EXPECT(server->server.authentication.password == "hunter2");
        EXPECT(server->jumpHosts.size() == 1 && server->jumpHosts[0].host == "bastion.example.org");
        EXPECT(server->jumpHosts[0].username == "me" && server->jumpHosts[0].port == 22);
        EXPECT(server->knownHostsPath == (d.files / "known_hosts").string());

        // The list shows it, and so does the storage home.
        auto *list = screen<app::ServersScreen>(h, "ServersScreen");
        EXPECT(list->cards().size() == 1);
        EXPECT(findLabel(*list->cards()[0], "Home NAS, me@nas.local:2222") != nullptr);

        // Edit: a key login instead.
        list->edit("home-nas");
        h.pump(100);
        editor = screen<app::ServerEditor>(h, "ServerEditor");
        EXPECT(editor != nullptr && !editor->isNew() && editor->draft().server.host == "nas.local");
        editor->setPrivateKey(std::nullopt, true);
        h.pump(30);
        EXPECT(editor->field("Password") == nullptr && editor->field("Private key") != nullptr);
        EXPECT(editor->field("Private key")->editor().multiline());
        EXPECT(!editor->save() && editor->errors().peek().server.key == "Choose a key file or paste a key");
        // A public key pasted by mistake is refused as it's pasted, saying why.
        type(editor->field("Private key"), "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPLaWF6s6nZOOUpOUGJyViqrU38Gsjh50aX787/W18xU me");
        h.pump(30);
        EXPECT(editor->errors().peek().server.key.starts_with("A public key."));
        EXPECT(findLabel(*h.app->screenNode(), "Pasted") == nullptr);
        EXPECT(!editor->save());
        // The private one, pasted with CRLF line ends: taken, and shown by its fingerprint.
        std::string crlf;
        for (const char c : std::string(kTestPrivateKey)) {
            crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
        }
        type(editor->field("Private key"), crlf);
        h.pump(30);
        EXPECT(editor->errors().peek().server.key.empty());
        EXPECT(findLabel(*h.app->screenNode(),
                         std::string("Ed25519 private key, Pasted, ") + kTestPrivateKeyFingerprint) != nullptr);
        type(editor->field("Passphrase"), "open sesame");
        editor->removeJump(0);
        EXPECT(editor->save());
        h.pump(100);
        server = saved(h, "home-nas");
        EXPECT(server->server.authentication.type == vfs::SftpAuthentication::Type::kPrivateKey);
        // Saved as parsed: LF line ends.
        EXPECT(server->server.authentication.privateKey == kTestPrivateKey);
        EXPECT(server->server.authentication.passphrase == "open sesame" && server->jumpHosts.empty());

        // Editing it again shows the saved key without its text; a key file replaces it.
        list = screen<app::ServersScreen>(h, "ServersScreen");
        list->edit("home-nas");
        h.pump(100);
        editor = screen<app::ServerEditor>(h, "ServerEditor");
        EXPECT(editor->field("Private key")->editor().editingValue().text.empty());
        EXPECT(findLabel(*h.app->screenNode(),
                         std::string("Ed25519 private key, Saved, ") + kTestPrivateKeyFingerprint) != nullptr);
        const auto pick = [&](runtime::PickedDocument document) {
            const std::size_t before = h.platform.picks.size();
            editor->pickKey(std::nullopt);
            EXPECT(h.platform.picks.size() == before + 1);
            EXPECT(h.platform.picks.back().second.maxBytes == kMaxPrivateKeyBytes);
            h.documents.deliver(h.platform.picks.back().first, std::move(document));
            h.pump(30);
        };
        using Status = runtime::PickedDocument::Status;
        // Backing out changes nothing; a file that isn't a key, or is too large, says so.
        pick({.status = Status::cancelled});
        EXPECT(editor->draft().server.key == kTestPrivateKey && editor->draft().server.pickError.empty());
        pick({.status = Status::picked, .name = "id_ed25519.pub",
              .bytes = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPLaWF6s6nZOOUpOUGJyViqrU38Gsjh50aX787/W18xU me\n"});
        EXPECT(editor->draft().server.pickError.starts_with("“id_ed25519.pub”: A public key."));
        EXPECT(findLabel(*h.app->screenNode(), editor->draft().server.pickError) != nullptr);
        EXPECT(editor->draft().server.key == kTestPrivateKey);
        pick({.status = Status::tooLarge, .name = "photo.jpg"});
        EXPECT(editor->draft().server.pickError == "“photo.jpg” is too large to be a private key.");
        pick({.status = Status::failed});
        EXPECT(editor->draft().server.pickError == "Couldn't read the file.");
        // A key file (with a byte order mark, as some editors write) is taken.
        const std::string other = std::string("\xEF\xBB\xBF") + kTestPrivateKey;
        pick({.status = Status::picked, .name = "work_key", .bytes = other});
        EXPECT(editor->draft().server.pickError.empty() && editor->draft().server.key == kTestPrivateKey);
        EXPECT(editor->draft().server.keySource == "work_key");
        EXPECT(findLabel(*h.app->screenNode(),
                         std::string("Ed25519 private key, work_key, ") + kTestPrivateKeyFingerprint) != nullptr);
        EXPECT(editor->save());
        h.pump(100);

        // A second one with the same name gets its own id.
        editor = openEditor(h);
        type(editor->field("Name"), "Home NAS");
        type(editor->field("Host"), "10.0.0.2");
        type(editor->field("Username"), "root");
        EXPECT(editor->save());
        h.pump(100);
        EXPECT(saved(h, "home-nas-2") != nullptr);
        h.state().navigateHome();
        h.pump(100);
        EXPECT(h.app->home().serverRows().size() == 2);

        // Delete from the list, undo, delete again.
        h.state().openScreen(app::kServersScreen);
        h.pump(100);
        list = screen<app::ServersScreen>(h, "ServersScreen");
        list->remove("home-nas-2");
        h.pump(50);
        EXPECT(saved(h, "home-nas-2") == nullptr && list->cards().size() == 1);
        // The undo is behind the earlier toasts.
        auto &snackbars = h.app->context().overlays.snackbars();
        for (int i = 0; i < 10 && !(snackbars.current() && snackbars.current()->text.starts_with("Deleted")); ++i) {
            snackbars.dismissCurrent();
            h.pump(200);
        }
        EXPECT(snackbars.current() && snackbars.current()->onAction);
        snackbars.current()->onAction();
        h.pump(50);
        EXPECT(saved(h, "home-nas-2") != nullptr);
        list->remove("home-nas-2");
        h.pump(50);
        h.app->flushSettings();

        // On disk: no secret as typed.
        const std::string file = readFile(d.files / "settings.txt");
        EXPECT(file.find("open sesame") == std::string::npos && file.find("open%20sesame") == std::string::npos);
        EXPECT(file.find("hunter2") == std::string::npos);
        EXPECT(file.find("secret home-nas 0 passphrase ") != std::string::npos);
        // The private key too: sealed, never as given.
        EXPECT(file.find("secret home-nas 0 key ") != std::string::npos);
        EXPECT(file.find("OPENSSH") == std::string::npos && file.find("b3BlbnNzaC1rZXkt") == std::string::npos);
        EXPECT(file.find("server-name home-nas Home%20NAS") != std::string::npos);
    }
    // After a restart the server is back, passphrase and all.
    AppHarness h(d.home.string(), d.files.string());
    EXPECT(h.state().settings.peek().servers.size() == 1);
    const auto *server = saved(h, "home-nas");
    EXPECT(server != nullptr && server->server.authentication.passphrase == "open sesame");
    EXPECT(server->server.authentication.privateKey == kTestPrivateKey);
    EXPECT(h.platform.sealed == 0);  // Nothing was sealed again: the blob was reused.
}

// Old files kept passwords as typed: they're rewritten at once, sealed. Without a key store,
// secrets are left out of the file entirely.
void testSecretsMigrationAndNoKeyStore() {
    Dirs d;
    std::ofstream(d.files / "settings.txt") << "filepan-settings 1\n"
                                               "server nas nas.local 22 me password plain%20pw - - - 15 "
                                               "jump hop 22 me password hop-pw - - \n";
    {
        AppHarness h(d.home.string(), d.files.string());
        const auto *server = saved(h, "nas");
        EXPECT(server != nullptr && server->server.authentication.password == "plain pw");
        EXPECT(server->jumpHosts.size() == 1 && server->jumpHosts[0].authentication.password == "hop-pw");
        const std::string file = readFile(d.files / "settings.txt");
        EXPECT(file.find("plain") == std::string::npos && file.find("hop-pw") == std::string::npos);
        EXPECT(file.find("secret nas 0 password ") != std::string::npos);
        EXPECT(file.find("secret nas 1 password ") != std::string::npos);
    }
    {
        AppHarness h(d.home.string(), d.files.string());
        EXPECT(saved(h, "nas")->server.authentication.password == "plain pw");
    }
    // A platform that can't seal: nothing secret is written, and the rest still is.
    Dirs other;
    {
        AppHarness h(other.home.string(), other.files.string(), {}, false);
        h.platform.secretsWork = false;
        h.start();
        auto *editor = openEditor(h);
        type(editor->field("Name"), "Box");
        type(editor->field("Host"), "box");
        type(editor->field("Username"), "u");
        type(editor->field("Password"), "topsecret");
        EXPECT(editor->save());
        h.pump(50);
        h.app->flushSettings();
        const std::string file = readFile(other.files / "settings.txt");
        EXPECT(file.find("topsecret") == std::string::npos && file.find("secret ") == std::string::npos);
        EXPECT(file.find("server box ") != std::string::npos);
    }
    // base64 round trips.
    for (const std::string s : {"", "a", "ab", "abc", "abcd", "\xff\x00\x10"}) {
        const std::vector<std::uint8_t> bytes(s.begin(), s.end());
        EXPECT(base64Decode(base64Encode(bytes)) == bytes);
    }
    EXPECT(base64Encode(std::vector<std::uint8_t>{'h', 'i', '!', '?'}) == "aGkhPw==");
    EXPECT(!base64Decode("abc") && !base64Decode("ab=c") && !base64Decode("a$cd"));
}

// A server saved before private keys were kept themselves: its key fields held paths to key
// files. It loads as a key login without a key, the editor asks for one, and connecting says
// which hop has none.
void testOldKeyPaths() {
    Dirs d;
    std::ofstream(d.files / "settings.txt") << "filepan-settings 1\n"
                                               "server old nas.local 22 me key - /sdcard/id_nas - - 15 "
                                               "jump bastion 2222 ops key - /sdcard/id_bastion - \n";
    AppHarness h(d.home.string(), d.files.string());
    const auto *server = saved(h, "old");
    EXPECT(server != nullptr && server->jumpHosts.size() == 1);
    EXPECT(server->jumpHosts[0].authentication.type == vfs::SftpAuthentication::Type::kPrivateKey);
    EXPECT(server->jumpHosts[0].authentication.privateKey.empty());
    h.state().openScreen(app::kServersScreen);
    h.pump(100);
    screen<app::ServersScreen>(h, "ServersScreen")->edit("old");
    h.pump(100);
    auto *editor = screen<app::ServerEditor>(h, "ServerEditor");
    EXPECT(editor != nullptr && editor->draft().jumps.size() == 1 && editor->draft().jumps[0].privateKey);
    EXPECT(editor->field("Jump host 1 Private key") != nullptr && editor->field("Private key") != nullptr);
    EXPECT(!editor->save());
    EXPECT(editor->errors().peek().jumps.size() == 1);
    EXPECT(editor->errors().peek().jumps[0].key == "Choose a key file or paste a key");
    EXPECT(editor->errors().peek().server.key == "Choose a key file or paste a key");
    // Connecting anyway: the first hop without a key is named.
    auto home = h.sftp->homeDirectory("old");
    EXPECT(!home && home.error().code == ENOKEY);
    EXPECT(home.error().message.starts_with("Jump host 1 (ops@bastion:2222): no private key saved"));
}

// The prompt sheets, fed straight through the relay as a backend would from its I/O thread.
void testPromptSheets() {
    Dirs d;
    AppHarness h(d.home.string(), d.files.string());
    app::PromptSheets &sheets = h.app->prompts();

    // An unknown host key, accepted and saved from the sheet.
    vfs::HostKeyPrompt prompt{.serverId = "nas", .host = "nas.local", .port = 2222, .keyType = "ssh-ed25519",
                              .fingerprint = "SHA256:abcDEF123"};
    auto [promise, future] = makeDecision<vfs::HostKeyDecision>();
    std::thread([&, p = promise] { h.prompts->requestHostKey(prompt, p); }).join();
    EXPECT(h.waitUntil([&] { return sheets.showing(); }));
    h.pump(400);
    EXPECT(sheets.sheet() != nullptr && findLabel(sheets.sheet()->panel(), "nas.local:2222") != nullptr);
    // The fingerprint keeps its case.
    EXPECT(findLabel(sheets.sheet()->panel(), "SHA256:abcDEF123") != nullptr);
    sheets.answerHostKey(vfs::HostKeyDecision::kAcceptAndSave);
    CancellationToken never;
    EXPECT(h.waitUntil([&] { return !promise.isPending(); }));
    EXPECT(future.wait(never) == vfs::HostKeyDecision::kAcceptAndSave);
    EXPECT(h.waitUntil([&] { return !sheets.showing() && h.app->context().overlays.openCount.peek() == 0; }));

    // Two at once: shown one at a time. A changed key's sheet makes reject the one primary
    // button, and closing it any other way (back) rejects.
    prompt.status = vfs::HostKeyPrompt::Status::kChanged;
    prompt.knownFingerprint = "SHA256:old";
    auto [changed, changedFuture] = makeDecision<vfs::HostKeyDecision>();
    auto [login, loginFuture] = makeDecision<std::optional<std::string>>();
    h.prompts->requestHostKey(prompt, changed);
    h.prompts->requestCredential({.serverId = "nas", .host = "nas.local", .username = "me"}, login);
    EXPECT(h.waitUntil([&] { return sheets.showing(); }));
    h.pump(400);
    EXPECT(sheets.waiting() == 1);
    int primaries = 0;
    std::string primary;
    std::function<void(ui::Node &)> walk = [&](ui::Node &node) {
        if (node.debugName() == "Button" &&
            static_cast<ui::components::Button &>(node).variant() == ui::components::ButtonVariant::primary) {
            ++primaries;
            primary = node.semantics().label;
        }
        for (const auto &child : node.children()) {
            walk(*child);
        }
    };
    walk(sheets.sheet()->panel());
    EXPECT(primaries == 1 && primary == "Reject the new key");
    EXPECT(findLabel(sheets.sheet()->panel(), "Possible interception") != nullptr);
    EXPECT(h.app->back());
    EXPECT(h.waitUntil([&] { return !changed.isPending(); }));
    EXPECT(changedFuture.wait(never) == vfs::HostKeyDecision::kReject);

    // Then the password sheet; cancelling declines.
    EXPECT(h.waitUntil([&] { return sheets.showing() && sheets.current() != nullptr &&
                                    std::holds_alternative<app::PromptSheets::Credential>(*sheets.current()); }));
    h.pump(300);
    sheets.cancel();
    EXPECT(h.waitUntil([&] { return !login.isPending(); }));
    EXPECT(loginFuture.wait(never) == std::optional<std::optional<std::string>>(std::optional<std::string>()));

    // A request whose job gave up before its turn is skipped.
    {
        auto [gone, goneFuture] = makeDecision<std::optional<std::string>>();
        auto [next, nextFuture] = makeDecision<std::optional<std::string>>();
        h.prompts->requestCredential({.serverId = "x", .host = "a"}, gone);
        {
            auto dropped = std::move(goneFuture);
        }
        h.prompts->requestCredential({.serverId = "y", .host = "b", .username = "u", .retry = true}, next);
        EXPECT(h.waitUntil([&] { return sheets.showing(); }));
        h.pump(300);
        const auto *shown = std::get_if<app::PromptSheets::Credential>(sheets.current());
        EXPECT(shown != nullptr && shown->prompt.host == "b" && shown->prompt.retry);
        sheets.answerSecret("pw");
        EXPECT(h.waitUntil([&] { return !next.isPending(); }));
        EXPECT(nextFuture.wait(never) == std::optional<std::optional<std::string>>(std::string("pw")));
    }
}

// The security key sheet follows a signing session as the signing thread moves it on: present
// the key, the PIN (too short, then given), and closing when it's done; dismissing it cancels.
void testSecurityKeySheet() {
    Dirs d;
    AppHarness h(d.home.string(), d.files.string());
    app::PromptSheets &sheets = h.app->prompts();
    const auto shows = [&](std::string_view label) {
        return sheets.sheet() != nullptr && findLabel(sheets.sheet()->panel(), label) != nullptr;
    };

    auto session = std::make_shared<vfs::SecurityKeySession>();
    std::thread([&] {
        h.prompts->requestSecurityKey(
                {.serverId = "nas", .host = "nas.local", .username = "me", .userVerification = true}, session);
    }).join();
    EXPECT(h.waitUntil([&] { return sheets.showing(); }));
    h.pump(400);
    EXPECT(shows("me@nas.local"));
    EXPECT(shows("Plug in your security key, or hold it to the back of the phone."));
    session->offerProblem(vfs::SecurityKeySession::Problem::kNotASecurityKey);
    EXPECT(h.waitUntil([&] { return shows("That isn't a security key. Plug in or hold up a FIDO security key."); }));

    std::optional<std::string> pin;
    std::thread signer([&] { pin = session->awaitPin(5, false); });
    EXPECT(h.waitUntil([&] { return shows("Enter the security key's PIN."); }));
    h.pump(100);
    sheets.answerSecret("12");
    h.pump(100);
    EXPECT(sheets.showing() && shows("Enter the security key's PIN."));
    sheets.answerSecret("1234");
    signer.join();
    EXPECT(pin == "1234");
    // Asked before a key came: back to waiting for one.
    EXPECT(h.waitUntil([&] { return shows("That isn't a security key. Plug in or hold up a FIDO security key."); }));

    // Done on the signing side: the sheet closes by itself, without cancelling.
    session->finish();
    EXPECT(h.waitUntil([&] { return !sheets.showing() && h.app->context().overlays.openCount.peek() == 0; }));
    EXPECT(!session->isCancelled());

    // Closed with back: cancelled.
    auto second = std::make_shared<vfs::SecurityKeySession>();
    h.prompts->requestSecurityKey({.serverId = "nas", .isJumpHost = true, .host = "nas.local"}, second);
    EXPECT(h.waitUntil([&] { return sheets.showing(); }));
    h.pump(400);
    EXPECT(shows("JUMP HOST 1") || shows("Jump host 1"));
    EXPECT(h.app->back());
    EXPECT(h.waitUntil([&] { return second->isCancelled() && !sheets.showing(); }));
}

// Types into the text field labelled label under root.
void typeField(ui::Node &root, std::string_view label, const std::string &text) {
    auto *editor = static_cast<ui::components::EditableText *>(findControl(root, label, ui::Role::textField));
    EXPECT(editor != nullptr);
    const auto end = static_cast<std::int32_t>(text.size());
    editor->updateEditingValue({text, {end, end}, {}});
}

// The same sheet for an OpenPGP card: its own words, a PIN of six characters or more, and the
// tries left before the card locks.
void testOpenPgpCardSheet() {
    Dirs d;
    AppHarness h(d.home.string(), d.files.string());
    app::PromptSheets &sheets = h.app->prompts();
    const auto shows = [&](std::string_view label) {
        return sheets.sheet() != nullptr && findLabel(sheets.sheet()->panel(), label) != nullptr;
    };
    auto session = std::make_shared<vfs::SecurityKeySession>(vfs::SecurityKeySession::Kind::kOpenPgpCard);
    std::thread([&] {
        h.prompts->requestSecurityKey(
                {.serverId = "nas", .host = "nas.local", .username = "me", .userVerification = true}, session);
    }).join();
    EXPECT(h.waitUntil([&] { return sheets.showing(); }));
    h.pump(400);
    EXPECT(shows("Plug in your YubiKey or other OpenPGP card, or hold it to the back of the phone."));
    session->offerProblem(vfs::SecurityKeySession::Problem::kWrongKey);
    EXPECT(h.waitUntil([&] {
        return shows("That card doesn't have the key saved for this server. Present the one it was read from.");
    }));

    std::optional<std::string> pin;
    std::thread signer([&] { pin = session->awaitPin(2, true); });
    EXPECT(h.waitUntil([&] { return shows("Enter the card's PIN (its user PIN, not the admin PIN)."); }));
    h.pump(100);
    EXPECT(shows("Wrong PIN. 2 tries left before the card locks."));
    // The number pad by default; Letters switches the keyboard and keeps what was typed.
    const auto pinKeyboard = [&] {
        return h.root->textInput().client() != nullptr ? h.root->textInput().client()->inputConfig().keyboard
                                                       : ui::KeyboardType::text;
    };
    EXPECT(pinKeyboard() == ui::KeyboardType::numberPassword);
    typeField(sheets.sheet()->panel(), "PIN", "12ab");
    h.tap(*findControl(sheets.sheet()->panel(), "Type the PIN with letters", ui::Role::button));
    h.pump(100);
    EXPECT(pinKeyboard() == ui::KeyboardType::password);
    EXPECT(findControl(sheets.sheet()->panel(), "Type the PIN with the number pad", ui::Role::button) != nullptr);
    EXPECT(h.root->textInput().client()->editingValue().text == "12ab");
    sheets.answerSecret("1234");
    h.pump(100);
    EXPECT(sheets.showing() && shows("An OpenPGP card's PIN has at least 6 characters."));
    sheets.answerSecret("123456");
    signer.join();
    EXPECT(pin == "123456");
    session->finish();
    EXPECT(h.waitUntil([&] { return !sheets.showing(); }));

    // Reading the key: no PIN, and cancelling says what's cancelled.
    auto reading = std::make_shared<vfs::SecurityKeySession>(vfs::SecurityKeySession::Kind::kOpenPgpCard);
    h.prompts->requestSecurityKey({.serverId = "nas", .host = "nas.local", .username = "me"}, reading);
    EXPECT(h.waitUntil([&] { return sheets.showing(); }));
    h.pump(400);
    EXPECT(shows("Cancel reading the card"));
    EXPECT(h.app->back());
    EXPECT(h.waitUntil([&] { return reading->isCancelled() && !sheets.showing(); }));
}

// A server that logs in with an OpenPGP card: the key read off the card shows by its
// fingerprint, can be copied for authorized_keys, and is saved in plain, as it isn't secret.
void testCardLogin() {
    Dirs d;
    const std::string publicKey =
            "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPLaWF6s6nZOOUpOUGJyViqrU38Gsjh50aX787/W18xU cardno:000612345678";
    const auto parsed = parsePublicKey(publicKey);
    EXPECT(parsed.has_value());
    {
        AppHarness h(d.home.string(), d.files.string());
        auto *editor = openEditor(h);
        type(editor->field("Name"), "Card box");
        type(editor->field("Host"), "box.local");
        type(editor->field("Username"), "me");
        editor->setLogin(std::nullopt, app::ServerEditor::Login::card);
        h.pump(30);
        EXPECT(editor->field("Password") == nullptr && editor->field("Private key") == nullptr);
        EXPECT(findLabel(*h.app->screenNode(), "Read key from card") != nullptr);
        EXPECT(findLabel(*h.app->screenNode(), "Copy public key") == nullptr);
        EXPECT(!editor->save() && editor->errors().peek().server.key == "Read the key from the card");
        h.pump(30);
        EXPECT(findLabel(*h.app->screenNode(), "Read the key from the card") != nullptr);

        // Reading fails, then is cancelled (which says nothing), then works.
        editor->cardRead(std::nullopt, std::unexpected(Error{EIO, "OpenPGP card: the card was lost"}));
        h.pump(30);
        const std::string failure = editor->draft().server.cardError;
        EXPECT(failure.starts_with("OpenPGP card: the card was lost"));
        EXPECT(findLabel(*h.app->screenNode(), failure) != nullptr);
        editor->cardRead(std::nullopt, std::unexpected(Error{ECANCELED, "reading the card was cancelled"}));
        EXPECT(editor->draft().server.cardError.empty());
        editor->cardRead(std::nullopt, vfs::CardKey{.publicKey = publicKey,
                                                    .type = parsed->type,
                                                    .fingerprint = parsed->fingerprint});
        h.pump(30);
        EXPECT(editor->errors().peek().server.key.empty());
        EXPECT(findLabel(*h.app->screenNode(),
                         "Ed25519 card key, cardno:000612345678, " + parsed->fingerprint) != nullptr);
        EXPECT(findLabel(*h.app->screenNode(), "Copy public key") != nullptr);
        EXPECT(editor->save());
        h.pump(100);
        const auto *server = saved(h, "card-box");
        EXPECT(server != nullptr && server->server.authentication.type == vfs::SftpAuthentication::Type::kOpenPgpCard);
        EXPECT(server->server.authentication.cardPublicKey == publicKey);
        auto *list = screen<app::ServersScreen>(h, "ServersScreen");
        EXPECT(list != nullptr && list->cards().size() == 1);

        // Switching away and back keeps the key read.
        list->edit("card-box");
        h.pump(100);
        editor = screen<app::ServerEditor>(h, "ServerEditor");
        EXPECT(editor->draft().server.card && editor->draft().server.cardKey == publicKey);
        editor->setLogin(std::nullopt, app::ServerEditor::Login::password);
        editor->setLogin(std::nullopt, app::ServerEditor::Login::card);
        h.pump(30);
        EXPECT(findLabel(*h.app->screenNode(), "Copy public key") != nullptr);
        h.app->flushSettings();
        const std::string file = readFile(d.files / "settings.txt");
        EXPECT(file.find(" card - ssh-ed25519%20AAAAC3Nza") != std::string::npos);
    }
    // After a restart it's back.
    AppHarness h(d.home.string(), d.files.string());
    const auto *server = saved(h, "card-box");
    EXPECT(server != nullptr && server->server.authentication.type == vfs::SftpAuthentication::Type::kOpenPgpCard);
    EXPECT(server->server.authentication.cardPublicKey == publicKey);
}

// An encrypted zip read through the app's archive backend: the password sheet, a wrong answer,
// then the retry.
void testArchivePasswordRetry() {
    Dirs d;
    makeFile(d.dir.path() / "src" / "secret.txt", 5, 2026, 1, 1);
    struct Quiet final : CreateArchiveCallbacks {
        bool onProgress(std::uint64_t, std::uint64_t, std::string_view) override { return true; }
        bool onSourceError(std::string_view, int) override { return false; }
    } quiet;
    const fs::path zip = d.home / "locked.zip";
    EXPECT(createArchive(zip.string(), {ARCHIVE_FORMAT_ZIP, ARCHIVE_FILTER_NONE, "s3cret", 1},
                         {{(d.dir.path() / "src" / "secret.txt").string(), "secret.txt"}}, quiet)
                   .error == 0);
    AppHarness h(d.home.string(), d.files.string());
    app::PromptSheets &sheets = h.app->prompts();
    std::atomic<bool> done = false;
    std::string content;
    std::thread reader([&] {
        auto stream = h.archives->openRead(vfs::Location::archive(vfs::Location::local(zip.string()), "/secret.txt"));
        if (stream) {
            std::vector<std::uint8_t> buffer(64);
            if (auto n = (*stream)->read(buffer)) {
                content.assign(buffer.begin(), buffer.begin() + static_cast<long>(*n));
            }
        }
        done = true;
    });
    const auto archivePrompt = [&]() -> const app::PromptSheets::ArchivePassword * {
        return sheets.current() ? std::get_if<app::PromptSheets::ArchivePassword>(sheets.current()) : nullptr;
    };
    EXPECT(h.waitUntil([&] { return archivePrompt() != nullptr; }));
    h.pump(300);
    EXPECT(!archivePrompt()->request.retry && archivePrompt()->request.entryPath == "/secret.txt");
    EXPECT(findLabel(sheets.sheet()->panel(), "locked.zip") != nullptr);
    sheets.answerSecret("nope");
    EXPECT(h.waitUntil([&] { return archivePrompt() != nullptr && archivePrompt()->request.retry; }));
    h.pump(300);
    sheets.answerSecret("s3cret");
    EXPECT(h.waitUntil([&] { return done.load(); }));
    reader.join();
    EXPECT(content.size() == 5 && content[0] == 'x');
}

// Settings change live and persist.
void testSettingsLive() {
    Dirs d;
    {
        AppHarness h(d.home.string(), d.files.string());
        h.state().openScreen(app::kSettingsScreen);
        h.pump(100);
        ui::Node *screenNode = h.app->screenNode();
        EXPECT(screenNode != nullptr);
        // Thumbnails off.
        ui::Node *thumbs = findControl(*screenNode, "Thumbnails", ui::Role::toggleSwitch);
        EXPECT(thumbs != nullptr);
        // It's below the fold: scroll the page (the screen's first child) to bring it to the
        // middle of the screen, as for each control below.
        auto *page = static_cast<ui::ScrollNode *>(screenNode->children()[0].get());
        const auto reveal = [&](ui::Node &node) {
            page->scrollTo(std::max(0.0f, page->offset() + h.center(node).y -
                                                  AppHarness::kHeight / AppHarness::kDensity / 2));
            h.pump(50);
        };
        reveal(*thumbs);
        h.tap(*thumbs);
        h.pump(50);
        EXPECT(!h.state().settings.peek().thumbnails && !h.app->context().thumbnails.enabled());
        // The thumbnail cache's size and decay, through their menus.
        ui::Node *size = findControl(*screenNode, "Thumbnail cache size", ui::Role::button);
        EXPECT(size != nullptr && size->semantics().value == "1024 MiB");
        reveal(*size);
        h.tap(*size);
        h.pump(300);
        h.tap(*findLabel(*h.app->root(), "256 MiB"));
        h.pump(300);
        EXPECT(h.state().settings.peek().thumbnailCacheSize == 256 && size->semantics().value == "256 MiB");
        ui::Node *decay = findControl(*screenNode, "Remove thumbnails not viewed for", ui::Role::button);
        EXPECT(decay != nullptr && decay->semantics().value == "3 days");
        reveal(*decay);
        h.tap(*decay);
        h.pump(300);
        h.tap(*findLabel(*h.app->root(), "Never"));
        h.pump(300);
        EXPECT(h.state().settings.peek().thumbnailDecayDays == 0);
        ui::Node *clear = findControl(*screenNode, "Clear thumbnail cache", ui::Role::button);
        EXPECT(clear != nullptr);
        reveal(*clear);
        h.tap(*clear);
        h.pump(50);
        // Remote thumbnails: off, and the largest file.
        ui::Node *remote = findControl(*screenNode, "Thumbnails for remote files", ui::Role::toggleSwitch);
        EXPECT(remote != nullptr && h.app->context().thumbnails.remoteEnabled());
        reveal(*remote);
        h.tap(*remote);
        h.pump(50);
        EXPECT(!h.state().settings.peek().remoteThumbnails && !h.app->context().thumbnails.remoteEnabled());
        ui::Node *largest = findControl(*screenNode, "Largest remote file to thumbnail", ui::Role::button);
        EXPECT(largest != nullptr && largest->semantics().value == "10 MiB");
        reveal(*largest);
        h.tap(*largest);
        h.pump(300);
        h.tap(*findLabel(*h.app->root(), "5 MiB"));
        h.pump(300);
        EXPECT(h.state().settings.peek().remoteThumbnailMaxSize == 5);
        // The viewer's prefetching, and the connections it may use, which the SFTP backend takes.
        const auto choose = [&](const char *control, const char *value, const char *option) {
            ui::Node *button = findControl(*screenNode, control, ui::Role::button);
            EXPECT(button != nullptr && button->semantics().value == value);
            reveal(*button);
            h.tap(*button);
            h.pump(300);
            h.tap(*findLabel(*h.app->root(), option));
            h.pump(300);
        };
        EXPECT(h.sftp->parallelConnections() == 3);
        choose("Viewer prefetch", "2 files", "Off");
        choose("Video prefetch", "8 MiB", "32 MiB");
        choose("Parallel connections", "3", "4");
        EXPECT(h.state().settings.peek().viewerPrefetch == 0 && h.state().settings.peek().viewerPrefetchVideoMb == 32);
        EXPECT(h.state().settings.peek().remoteConnections == 4 && h.sftp->parallelConnections() == 4);
        static_cast<ui::ScrollNode *>(screenNode->children()[0].get())->scrollTo(0);
        h.pump(50);
        // The preset, through the segmented control: from the default Acid to Ultraviolet.
        ui::Node *uv = findLabel(*h.app->screenNode(), "Ultraviolet");
        EXPECT(uv != nullptr && h.state().settings.peek().preset == ui::ThemePreset::acid);
        h.tap(*uv);
        h.pump(50);
        EXPECT(h.state().settings.peek().preset == ui::ThemePreset::ultraviolet);
        EXPECT(h.root->env().theme.peek().preset == ui::ThemePreset::ultraviolet);
        EXPECT(h.platform.lightSystemBars == false);
        // Clean follows the system's dark mode, and the system bars follow it; the others stay dark.
        h.root->env().night.set(false);
        h.pump(50);
        EXPECT(!h.root->env().theme.peek().colors.light);
        h.tap(*findLabel(*h.app->screenNode(), "Clean"));
        h.pump(50);
        EXPECT(h.state().settings.peek().preset == ui::ThemePreset::clean);
        EXPECT(h.root->env().theme.peek().colors.light && h.platform.lightSystemBars == true);
        h.root->env().night.set(true);
        h.pump(50);
        EXPECT(!h.root->env().theme.peek().colors.light && h.platform.lightSystemBars == false);
        h.tap(*findLabel(*h.app->screenNode(), "Ultraviolet"));
        h.pump(50);
        // A swatch, then a custom accent through the picker's hex field and sliders.
        ui::Node *teal = findLabel(*h.app->screenNode(), "Teal accent");
        h.tap(*teal);
        h.pump(50);
        EXPECT(h.state().settings.peek().accent && app::colorToHex(*h.state().settings.peek().accent) == "#2DD4BF");
        app::showAccentPicker(h.app->context());
        h.pump(400);
        auto *sheet = h.app->context().overlays.topSheet();
        EXPECT(sheet != nullptr);
        auto *hexField = static_cast<ui::components::TextField *>(findLabel(sheet->panel(), "Hex"));
        EXPECT(hexField != nullptr);
        // The label sits on the editor; walk up to the field.
        ui::Node *field = hexField;
        while (field != nullptr && field->debugName() != "TextField") {
            field = field->parent();
        }
        EXPECT(field != nullptr);
        type(static_cast<ui::components::TextField *>(field), "#3366ff");
        h.pump(50);
        EXPECT(app::colorToHex(*h.state().settings.peek().accent) == "#3366FF");
        EXPECT(h.root->env().theme.peek().colors.accent == *h.state().settings.peek().accent);
        // The contrast check shows both ratios.
        EXPECT(findLabel(sheet->panel(), "Too dim on the ground") == nullptr);
        // Reset goes back to the preset.
        ui::Node *reset = findLabel(sheet->panel(), "Reset to the preset's accent");
        h.tap(*reset);
        h.pump(50);
        EXPECT(!h.state().settings.peek().accent);
        h.app->back();
        h.pump(400);
        // Default view and sort.
        static_cast<ui::ScrollNode *>(screenNode->children()[0].get())->scrollTo(0);
        h.pump(50);
        h.tap(*findLabel(*h.app->screenNode(), "Grid"));
        h.tap(*findLabel(*h.app->screenNode(), "Size"));
        h.pump(50);
        EXPECT(h.state().settings.peek().view == app::ViewMode::grid);
        EXPECT(h.state().settings.peek().sort.key == vfs::SortKey::kSize);
    }
    AppHarness h(d.home.string(), d.files.string());
    const app::Settings &s = h.state().settings.peek();
    EXPECT(!s.thumbnails && s.preset == ui::ThemePreset::ultraviolet && !s.accent);
    EXPECT(s.thumbnailCacheSize == 256 && s.thumbnailDecayDays == 0);
    EXPECT(!s.remoteThumbnails && s.remoteThumbnailMaxSize == 5);
    EXPECT(s.viewerPrefetch == 0 && s.viewerPrefetchVideoMb == 32 && s.remoteConnections == 4);
    EXPECT(h.sftp->parallelConnections() == 4);
    EXPECT(s.view == app::ViewMode::grid && s.sort.key == vfs::SortKey::kSize);

    // HSL conversion and the contrast check.
    const gfx::Color c = gfx::Color::fromRgb(0x3366FF);
    const app::Hsl hsl = app::toHsl(c);
    EXPECT(std::abs(hsl.h - 225) < 0.5f && std::abs(hsl.s - 1) < 0.01f && std::abs(hsl.l - 0.6f) < 0.01f);
    EXPECT(app::colorToHex(app::fromHsl(hsl)) == "#3366FF");
    EXPECT(app::accentContrast(ui::ThemePreset::ultraviolet, gfx::Color::fromRgb(0xFF4FC4)).groundPasses());
    EXPECT(!app::accentContrast(ui::ThemePreset::ultraviolet, gfx::Color::fromRgb(0x1A1030)).groundPasses());
}

// Backing up with a password and restoring: the secrets come back and are sealed again by the
// key store, the open tabs stay, and the known hosts are merged.
void testBackupRestore() {
    Dirs d;
    AppHarness h(d.home.string(), d.files.string());
    const std::string knownHosts = (d.files / "known_hosts").string();
    h.state().updateSettings([&](app::Settings &s) {
        vfs::SftpServerConfig server;
        server.id = "nas";
        server.name = "NAS";
        server.server = {.host = "nas.local", .port = 22, .username = "me", .authentication = {.password = "hunter2"}};
        server.knownHostsPath = knownHosts;
        s.servers = {server};
        s.favorites = {{"Download", vfs::Location::local((d.home / "Download").string()), app::Favorite::Kind::folder}};
        s.preset = ui::ThemePreset::ultraviolet;
    });
    std::ofstream(knownHosts) << "nas.local ssh-ed25519 AAAAC3Nz\n";
    h.pump(50);
    app::AppContext &context = h.app->context();

    // Back up, with a password typed twice; a mismatch doesn't save.
    app::showBackupSheet(context);
    h.pump(300);
    ui::Node *sheet = context.overlays.topSheet();
    EXPECT(sheet != nullptr);
    typeField(*sheet, "Password", "pw");
    typeField(*sheet, "Repeat password", "px");
    h.pump(50);
    ui::Node *save = findControl(*sheet, "Save backup", ui::Role::button);
    EXPECT(save != nullptr);
    h.tap(*save);
    h.pump(50);
    EXPECT(context.overlays.topSheet() == sheet && h.platform.saves.empty());
    typeField(*sheet, "Repeat password", "pw");
    h.pump(50);
    h.tap(*save);
    EXPECT(h.waitUntil([&] { return h.platform.saves.size() == 1; }, 20000));
    const std::string backup = h.platform.saves[0].second.bytes;
    EXPECT(h.platform.saves[0].second.name.ends_with(".filepan"));
    EXPECT(backup.find("hunter2") == std::string::npos && backup.find("\nsecret nas 0 password ") != std::string::npos);
    h.saver.deliver(h.platform.saves[0].first, {.status = runtime::SavedDocument::Status::saved});
    h.pump(50);
    EXPECT(context.overlays.snackbars().current() && context.overlays.snackbars().current()->text == "Settings backed up");
    context.overlays.snackbars().dismissCurrent();

    // Then everything changes: the server goes, the theme, a second tab.
    h.state().updateSettings([](app::Settings &s) {
        s.servers.clear();
        s.favorites.clear();
        s.preset = ui::ThemePreset::acid;
    });
    h.state().addTab(vfs::Location::local((d.home / "Download").string()));
    h.pump(50);
    const std::size_t tabs = h.state().tabs.peek().size();
    std::ofstream(knownHosts, std::ios::trunc) << "other ssh-rsa AAAAB3\n";

    // Restore: pick the file, a wrong password, then the right one, then confirm.
    app::startRestore(context);
    EXPECT(h.platform.picks.size() == 1);
    h.documents.deliver(h.platform.picks[0].first, {.status = runtime::PickedDocument::Status::picked,
                                                     .name = "backup.filepan", .bytes = backup});
    h.pump(300);
    sheet = context.overlays.topSheet();
    EXPECT(sheet != nullptr && findControl(*sheet, "Backup password", ui::Role::textField) != nullptr);
    typeField(*sheet, "Backup password", "wrong");
    h.tap(*findControl(*sheet, "Unlock", ui::Role::button));
    EXPECT(h.waitUntil([&] {
        auto *field = findControl(*sheet, "Backup password", ui::Role::textField);
        return field != nullptr && field->semantics().hint == "That didn't work. Try again.";
    }, 20000));
    typeField(*sheet, "Backup password", "pw");
    h.tap(*findControl(*sheet, "Unlock", ui::Role::button));
    EXPECT(h.waitUntil([&] {
        ui::Node *top = context.overlays.topSheet();
        return top != nullptr && top != sheet && findControl(*top, "Replace", ui::Role::button) != nullptr;
    }, 20000));
    h.pump(300);
    ui::Node *confirm = context.overlays.topSheet();
    EXPECT(findLabel(*confirm, "1 server · 1 favorite · 0 hidden items") != nullptr);
    h.tap(*findControl(*confirm, "Replace", ui::Role::button));
    h.pump(100);
    const app::Settings &restored = h.state().settings.peek();
    EXPECT(restored.servers.size() == 1 && restored.servers[0].server.authentication.password == "hunter2");
    EXPECT(restored.servers[0].knownHostsPath == knownHosts);
    EXPECT(restored.favorites.size() == 1 && restored.preset == ui::ThemePreset::ultraviolet);
    EXPECT(h.state().tabs.peek().size() == tabs);
    EXPECT(h.waitUntil([&] {
        const auto &current = context.overlays.snackbars().current();
        return current && current->text == "Settings restored";
    }));
    // The known hosts were added to, and the settings file seals the password again.
    EXPECT(h.waitUntil([&] { return readFile(knownHosts).find("nas.local ssh-ed25519 AAAAC3Nz") != std::string::npos; }));
    EXPECT(readFile(knownHosts).find("other ssh-rsa AAAAB3") != std::string::npos);
    h.app->flushSettings();
    const std::string file = readFile(d.files / "settings.txt");
    EXPECT(file.find("hunter2") == std::string::npos && file.find("secret nas 0 password ") != std::string::npos);
}

// Restoring a backup made without a password keeps the passwords saved here for the same server.
void testRestoreWithoutSecrets() {
    Dirs d;
    AppHarness h(d.home.string(), d.files.string());
    vfs::SftpServerConfig server;
    server.id = "nas";
    server.server = {.host = "nas.local", .port = 22, .username = "me", .authentication = {.password = "hunter2"}};
    app::Settings source;
    source.servers = {server};
    auto backup = app::writeBackup(source, {});
    EXPECT(backup && backup->find("hunter2") == std::string::npos);
    h.state().updateSettings([&](app::Settings &s) { s.servers = {server}; });
    app::AppContext &context = h.app->context();
    app::startRestore(context);
    h.documents.deliver(h.platform.picks.back().first, {.status = runtime::PickedDocument::Status::picked, .bytes = *backup});
    h.pump(300);
    ui::Node *confirm = context.overlays.topSheet();
    EXPECT(confirm != nullptr && findControl(*confirm, "Replace", ui::Role::button) != nullptr);
    h.tap(*findControl(*confirm, "Replace", ui::Role::button));
    h.pump(100);
    EXPECT(h.state().settings.peek().servers[0].server.authentication.password == "hunter2");
    // Not a backup at all: a toast says so.
    app::startRestore(context);
    h.documents.deliver(h.platform.picks.back().first,
                        {.status = runtime::PickedDocument::Status::picked, .bytes = "filepan-settings 1\n"});
    auto &snackbars = context.overlays.snackbars();
    EXPECT(h.waitUntil([&] {
        if (snackbars.current() && snackbars.current()->text != "This isn't a FilePan backup") {
            snackbars.dismissCurrent();
        }
        return snackbars.current().has_value();
    }));
}

// The intro: shown on a fresh install, not to someone who's been here; its pages' actions; Done,
// Skip and back; going on to a new server; shown again from Settings.
void testOnboarding() {
    Dirs d;
    {
        // A settings file from before the intro: not shown.
        std::ofstream(d.files / "settings.txt") << "filepan-settings 1\ntheme acid\n";
        AppHarness h(d.home.string(), d.files.string(), {.onboarding = true});
        EXPECT(h.app->onboarding() == nullptr);
    }
    fs::remove(d.files / "settings.txt");
    AppHarness h(d.home.string(), d.files.string(), {.onboarding = true}, false);
    h.platform.access = false;
    h.platform.notificationsGranted = false;
    h.start();
    h.pump(100);
    app::OnboardingScreen *intro = h.app->onboarding();
    EXPECT(intro != nullptr && intro->page() == 0);
    EXPECT(findLabel(*intro, "FilePan") != nullptr);
    const auto tapButton = [&](std::string_view label) {
        ui::Node *button = findControl(*h.app->onboarding(), label, ui::Role::button);
        EXPECT(button != nullptr);
        h.tap(*button);
        h.pump(400);
    };
    // Back on the first page leaves it to the system.
    EXPECT(!intro->back());
    tapButton("Next");
    EXPECT(intro->page() == 1);

    // All files access: asked; granted on the way back from settings.
    tapButton("Grant access");
    EXPECT(h.platform.accessRequests == 1);
    h.services->allFilesAccess.set(true);
    h.pump(50);
    EXPECT(findLabel(*intro, "Access granted") != nullptr && findControl(*intro, "Grant access", ui::Role::button) == nullptr);
    tapButton("Next");

    // Notifications: asked; refused, the system settings; then on.
    tapButton("Allow notifications");
    EXPECT(h.platform.notificationRequests == 1);
    tapButton("Open settings");
    EXPECT(h.platform.notificationSettings == 1);
    h.services->notificationsAllowed.set(true);
    h.pump(50);
    EXPECT(findLabel(*intro, "Notifications on") != nullptr);
    tapButton("Next");

    // The look, applied at once.
    EXPECT(intro->page() == 3);
    ui::Node *uv = findLabel(*intro, "Ultraviolet");
    EXPECT(uv != nullptr);
    h.tap(*uv);
    h.pump(100);
    EXPECT(h.state().settings.peek().preset == ui::ThemePreset::ultraviolet);
    // System back steps back a page.
    EXPECT(h.app->back() && intro->page() == 2);
    tapButton("Next");
    tapButton("Next");

    // Remote files: mobile data mode, then Done.
    EXPECT(intro->page() == 4);
    ui::Node *saver = findControl(*intro, "Mobile data mode", ui::Role::toggleSwitch);
    EXPECT(saver != nullptr);
    h.tap(*saver);
    h.pump(50);
    EXPECT(h.state().settings.peek().dataSaver);
    EXPECT(findControl(*intro, "Skip", ui::Role::button) == nullptr);
    tapButton("Done");
    EXPECT(h.app->onboarding() == nullptr && h.state().settings.peek().onboarded == app::kOnboardingVersion);
    h.app->flushSettings();
    EXPECT(h.waitUntil([&] { return readFile(d.files / "settings.txt").find("\nonboarded 1\n") != std::string::npos; }));

    // Again from Settings: Skip finishes it.
    h.state().openScreen(app::kSettingsScreen);
    h.pump(100);
    ui::Node *again = findControl(*h.app->screenNode(), "Show the intro again", ui::Role::button);
    EXPECT(again != nullptr);
    h.app->context().showOnboarding();
    h.pump(100);
    EXPECT(h.app->onboarding() != nullptr && h.app->onboarding()->page() == 0);
    tapButton("Skip");
    EXPECT(h.app->onboarding() == nullptr);

    // Adding a server from the last page opens a new server's editor, with the list behind it.
    h.app->context().showOnboarding();
    h.pump(100);
    h.app->onboarding()->goTo(4);
    h.pump(400);
    tapButton("Add an SFTP server");
    EXPECT(h.app->onboarding() == nullptr);
    auto *editor = screen<app::ServerEditor>(h, "ServerEditor");
    EXPECT(editor != nullptr && editor->isNew());
    EXPECT(h.app->back());
    h.pump(100);
    EXPECT(screen<app::ServersScreen>(h, "ServersScreen") != nullptr);
}

// The licenses file: parsed, complete, every text present, and shown.
void testLicenses() {
    const fs::path dir = fs::path(FILEPAN_ASSETS_DIR) / "licenses";
    auto parsed = app::parseLicenses(readFile(dir / "licenses.xml"));
    EXPECT(parsed.has_value());
    EXPECT(parsed->notices.size() >= 10 && parsed->notices.front().name == "FilePan");
    const auto missing = parsed->missingTexts();
    for (const auto &m : missing) {
        std::fprintf(stderr, "no license text for %s\n", m.c_str());
    }
    EXPECT(missing.empty());
    for (const auto &text : parsed->texts) {
        EXPECT(fs::file_size(dir / text.file) > 200);
    }
    // Every text file is listed.
    for (const auto &entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.ends_with(".txt")) {
            EXPECT(std::ranges::any_of(parsed->texts, [&](const auto &t) { return t.file == name; }));
        }
    }

    // The parser: entities, comments, whitespace, and what it rejects.
    auto small = app::parseLicenses("<?xml version=\"1.0\"?>\n<!-- c -->\n<notices>\n"
                                    "<text license=\"A &amp; B\" file=\"a.txt\" />\n"
                                    "<notice><name>X</name><copyright>  Copyright\n   &#169; me  </copyright>"
                                    "<license>A &amp; B</license></notice>\n"
                                    "<notice><name>Y</name><license>Nope</license></notice></notices>");
    EXPECT(small && small->notices.size() == 2 && small->notices[0].copyright == "Copyright \xC2\xA9 me");
    EXPECT(small->fileFor("A & B") == "a.txt");
    EXPECT(small->missingTexts() == std::vector<std::string>{"Y: Nope"});
    EXPECT(!app::parseLicenses("<notices><notice><name>x</name></notices>"));
    EXPECT(!app::parseLicenses("<other/>"));
    EXPECT(!app::parseLicenses("<notices><text license=\"a\" file=\"../x\"/></notices>"));
    EXPECT(!app::parseLicenses("<notices><notice><name>&bogus;</name></notice></notices>"));

    // The screens read them through the assets.
    Dirs d;
    AppHarness h(d.home.string(), d.files.string());
    h.state().openScreen(app::kLicensesScreen);
    h.pump(50);
    auto *licenses = screen<app::LicensesScreen>(h, "LicensesScreen");
    EXPECT(licenses != nullptr);
    EXPECT(h.waitUntil([&] { return licenses->licenses().peek().done; }));
    h.pump(50);
    EXPECT(licenses->licenses().peek().error.empty());
    EXPECT(licenses->rows().size() == parsed->notices.size());
    // A row opens its license: FilePan's by a tap, then libssh's (below the fold) directly.
    h.tap(*licenses->rows()[0]);
    h.pump(50);
    EXPECT(screen<app::LicenseScreen>(h, "LicenseScreen") != nullptr);
    h.app->back();
    h.pump(50);
    licenses = screen<app::LicensesScreen>(h, "LicensesScreen");
    EXPECT(licenses != nullptr);
    EXPECT(h.waitUntil([&] { return licenses->licenses().peek().done; }));
    for (const auto &notice : parsed->notices) {
        if (notice.name == "libssh") {
            licenses->open(notice);
        }
    }
    h.pump(50);
    auto *license = screen<app::LicenseScreen>(h, "LicenseScreen");
    EXPECT(license != nullptr);
    EXPECT(h.waitUntil([&] { return license->text().peek().done; }));
    EXPECT(license->text().peek().value.find("GNU LESSER GENERAL PUBLIC LICENSE") != std::string::npos);
    // About links to the source.
    h.state().openScreen(app::kAboutScreen);
    h.pump(100);
    h.tap(*findLabel(*h.app->screenNode(), "Source code, github.com/PoonHarpoon/FilePan"));
    EXPECT(h.platform.urls == std::vector<std::string>{app::kSourceUrl});
}

#ifdef FILEPAN_SSHD
// The SFTP flows against a real sshd.
void testSftpAgainstSshd() {
    Dirs d;
    TempDir sshdDir;
    Sshd sshd(sshdDir.path());
    AppHarness h(d.home.string(), d.files.string());
    app::PromptSheets &sheets = h.app->prompts();
    const std::string user = getenv("USER") ? getenv("USER") : "root";
    const auto hostKeyPrompt = [&]() -> const app::PromptSheets::HostKey * {
        return sheets.current() ? std::get_if<app::PromptSheets::HostKey>(sheets.current()) : nullptr;
    };

    // The connection test: the unknown host key, accepted and saved.
    auto *editor = openEditor(h);
    type(editor->field("Name"), "Local");
    type(editor->field("Host"), "127.0.0.1");
    type(editor->field("Port"), std::to_string(sshd.port()));
    type(editor->field("Username"), user);
    editor->setPrivateKey(std::nullopt, true);
    h.pump(30);
    // The key pasted.
    type(editor->field("Private key"), readFile(sshdDir.path() / "client_key"));
    editor->testConnection();
    EXPECT(h.waitUntil([&] { return hostKeyPrompt() != nullptr; }, 10000));
    EXPECT(hostKeyPrompt()->prompt.status == vfs::HostKeyPrompt::Status::kUnknown);
    EXPECT(hostKeyPrompt()->prompt.fingerprint == sshd.hostFingerprint());
    h.pump(300);
    sheets.answerHostKey(vfs::HostKeyDecision::kAcceptAndSave);
    EXPECT(h.waitUntil([&] { return editor->testState.peek() == app::ServerEditor::TestState::passed; }, 10000));
    EXPECT(editor->testMessage.peek().starts_with("Logged in."));
    // The sheet has gone (it slides out).
    EXPECT(h.waitUntil([&] { return !sheets.showing(); }));
    EXPECT(readFile(d.files / "known_hosts").find("[127.0.0.1]:" + std::to_string(sshd.port())) != std::string::npos);
    EXPECT(editor->save());
    h.pump(100);

    // Opening it connects without asking again and browses its home directory.
    app::openServer(h.app->context(), "local");
    EXPECT(h.waitUntil([&] { return h.state().peekActive()->current().kind == app::Route::Kind::browse; }, 10000));
    EXPECT(!sheets.showing() || h.waitUntil([&] { return !sheets.showing(); }));
    EXPECT(sheets.current() == nullptr);
    const auto location = *h.state().peekActive()->current().location;
    EXPECT(location.kind() == vfs::BackendKind::kSftp && location.serverId() == "local");

    // The server's key changes: the sheet warns, and closing it rejects.
    {
        std::string known = readFile(d.files / "known_hosts");
        std::istringstream other(readFile(sshdDir.path() / "client_key.pub"));
        std::string type, blob;
        other >> type >> blob;
        const auto start = known.find("AAAA");
        const auto end = known.find_first_of(" \n", start);
        known.replace(start, end - start, blob);
        std::ofstream(d.files / "known_hosts") << known;
    }
    h.sftp->disconnect("local");
    h.state().navigateHome();
    h.pump(50);
    app::openServer(h.app->context(), "local");
    EXPECT(h.waitUntil([&] { return hostKeyPrompt() != nullptr; }, 10000));
    EXPECT(hostKeyPrompt()->prompt.status == vfs::HostKeyPrompt::Status::kChanged);
    h.pump(300);
    EXPECT(h.app->back());
    EXPECT(h.waitUntil([&] {
        const auto &current = h.app->context().overlays.snackbars().current();
        return current && current->text.starts_with("Couldn't connect to Local");
    }, 10000));
    EXPECT(h.state().peekActive()->current().kind == app::Route::Kind::home);

    // A locked key: the passphrase is asked for, again after a wrong one.
    std::ofstream(d.files / "known_hosts") << "";
    editor = openEditor(h);
    type(editor->field("Name"), "Locked");
    type(editor->field("Host"), "127.0.0.1");
    type(editor->field("Port"), std::to_string(sshd.port()));
    type(editor->field("Username"), user);
    editor->setPrivateKey(std::nullopt, true);
    h.pump(30);
    // The key from a key file; encrypted, so the card says so instead of a fingerprint.
    editor->pickKey(std::nullopt);
    h.documents.deliver(h.platform.picks.back().first, {.status = runtime::PickedDocument::Status::picked,
                                                        .name = "locked_key",
                                                        .bytes = readFile(sshdDir.path() / "locked_key")});
    h.pump(30);
    EXPECT(editor->draft().server.key == readFile(sshdDir.path() / "locked_key"));
    EXPECT(findLabel(*h.app->screenNode(), "Encrypted private key, locked_key") != nullptr);
    editor->testConnection();
    EXPECT(h.waitUntil([&] { return hostKeyPrompt() != nullptr; }, 10000));
    h.pump(300);
    sheets.answerHostKey(vfs::HostKeyDecision::kAcceptOnce);
    const auto credential = [&]() -> const app::PromptSheets::Credential * {
        return sheets.current() ? std::get_if<app::PromptSheets::Credential>(sheets.current()) : nullptr;
    };
    EXPECT(h.waitUntil([&] { return credential() != nullptr; }, 10000));
    EXPECT(credential()->prompt.kind == vfs::CredentialPrompt::Kind::kPassphrase && !credential()->prompt.retry);
    h.pump(300);
    sheets.answerSecret("wrong");
    EXPECT(h.waitUntil([&] { return credential() != nullptr && credential()->prompt.retry; }, 10000));
    h.pump(300);
    sheets.answerSecret("secret phrase");
    EXPECT(h.waitUntil([&] { return editor->testState.peek() == app::ServerEditor::TestState::passed; }, 10000));
    // Accepted once: not saved.
    EXPECT(readFile(d.files / "known_hosts").empty());
}

// Jump hosts set up in the editor, saved, loaded again and connected through: three sshds, each
// with its own host key and its own client key, so every hop needs its own key and host key.
void testSftpJumpHosts() {
    Dirs d;
    TempDir dirA, dirB, dirC;
    Sshd a(dirA.path());
    Sshd b(dirB.path());
    Sshd c(dirC.path());
    const std::string user = getenv("USER") ? getenv("USER") : "root";
    const auto keyOf = [](const TempDir &dir) { return readFile(dir.path() / "client_key"); };

    // Fills an endpoint's fields: "" for the server, "Jump host N " for hop N.
    const auto fill = [&](app::ServerEditor *editor, AppHarness &h, std::optional<std::size_t> jump,
                          const Sshd &sshd, const std::string &key) {
        const std::string prefix = jump ? "Jump host " + std::to_string(*jump + 1) + " " : std::string();
        type(editor->field(prefix + "Host"), "127.0.0.1");
        type(editor->field(prefix + "Port"), std::to_string(sshd.port()));
        type(editor->field(prefix + "Username"), user);
        if (!key.empty()) {
            editor->setPrivateKey(jump, true);
            h.pump(30);
            type(editor->field(prefix + "Private key"), key);
        }
    };
    // Accepts (and saves) the host keys asked about, expecting these hops in this order, until
    // the test settles.
    const auto runTest = [&](AppHarness &h, app::ServerEditor *editor, std::vector<std::size_t> hops,
                             std::size_t chain) {
        app::PromptSheets &sheets = h.app->prompts();
        editor->testConnection();
        for (const std::size_t hop : hops) {
            EXPECT(h.waitUntil([&] { return sheets.current() && std::get_if<app::PromptSheets::HostKey>(sheets.current()); },
                               10000));
            const auto &prompt = std::get_if<app::PromptSheets::HostKey>(sheets.current())->prompt;
            if (prompt.hop != hop) {
                std::fprintf(stderr, "host key asked for hop %zu, expected %zu\n", prompt.hop, hop);
            }
            EXPECT(prompt.hop == hop && prompt.isJumpHost == (hop + 1 < chain));
            h.pump(300);
            sheets.answerHostKey(vfs::HostKeyDecision::kAcceptAndSave);
            EXPECT(h.waitUntil([&] { return !sheets.showing(); }));
        }
        const bool done =
                h.waitUntil([&] { return editor->testState.peek() != app::ServerEditor::TestState::running; }, 30000);
        if (!done) {
            std::fprintf(stderr, "still testing: %s; a sheet showing: %d\n", editor->testMessage.peek().c_str(),
                         sheets.current() != nullptr);
            if (const auto *key = sheets.current() ? std::get_if<app::PromptSheets::HostKey>(sheets.current()) : nullptr) {
                std::fprintf(stderr, "host key asked for hop %zu\n", key->prompt.hop);
            }
            if (const auto *login = sheets.current() ? std::get_if<app::PromptSheets::Credential>(sheets.current())
                                                     : nullptr) {
                std::fprintf(stderr, "%s asked for hop %zu (%s)\n",
                             login->prompt.kind == vfs::CredentialPrompt::Kind::kPassword ? "password" : "passphrase",
                             login->prompt.hop, login->prompt.host.c_str());
                std::fprintf(stderr, "server %s retry %d\n", login->prompt.serverId.c_str(), login->prompt.retry);
            }
        }
        EXPECT(done);
        return editor->testMessage.peek();
    };

    {
        AppHarness h(d.home.string(), d.files.string());
        // One hop, A, to the server, C; a key on each.
        auto *editor = openEditor(h);
        type(editor->field("Name"), "Inner");
        fill(editor, h, std::nullopt, c, keyOf(dirC));
        editor->addJump();
        h.pump(30);
        fill(editor, h, 0, a, keyOf(dirA));
        EXPECT(editor->draft().jumps.size() == 1 && editor->draft().jumps[0].privateKey);
        EXPECT(editor->draft().jumps[0].keyText() == keyOf(dirA) && editor->draft().server.keyText() == keyOf(dirC));
        std::string message = runTest(h, editor, {0, 1}, 2);
        if (editor->testState.peek() != app::ServerEditor::TestState::passed) {
            std::fprintf(stderr, "one hop: %s\n", message.c_str());
        }
        EXPECT(editor->testState.peek() == app::ServerEditor::TestState::passed);
        EXPECT(editor->save());
        h.pump(100);
        h.app->flushSettings();
        const std::string file = readFile(d.files / "settings.txt");
        EXPECT(file.find("secret inner 0 key ") != std::string::npos);
        EXPECT(file.find("secret inner 1 key ") != std::string::npos);
    }
    {
        // After a restart: the keys come back from the settings, sealed, and the saved host keys
        // are trusted without asking.
        AppHarness h(d.home.string(), d.files.string());
        const auto *server = saved(h, "inner");
        EXPECT(server != nullptr && server->jumpHosts.size() == 1);
        EXPECT(server->jumpHosts[0].authentication.privateKey == keyOf(dirA));
        EXPECT(server->server.authentication.privateKey == keyOf(dirC));
        app::openServer(h.app->context(), "inner");
        EXPECT(h.waitUntil([&] {
            const auto &route = h.state().peekActive()->current();
            return h.app->prompts().current() != nullptr ||
                   (route.kind == app::Route::Kind::browse && route.location->kind() == vfs::BackendKind::kSftp);
        }, 15000));
        EXPECT(h.app->prompts().current() == nullptr);
        EXPECT(h.state().peekActive()->current().location->serverId() == "inner");

        // A second hop, B, between A and C: only its host key is new.
        h.state().navigateHome();
        h.pump(50);
        h.state().openScreen(app::kServersScreen);
        h.pump(100);
        auto *list = screen<app::ServersScreen>(h, "ServersScreen");
        if (list == nullptr) {
            std::fprintf(stderr, "showing %s, route %d %s\n",
                         h.app->screenNode() ? std::string(h.app->screenNode()->debugName()).c_str() : "nothing",
                         static_cast<int>(h.state().peekActive()->current().kind),
                         h.state().peekActive()->current().screenId.c_str());
        }
        EXPECT(list != nullptr);
        list->edit("inner");
        h.pump(100);
        auto *editor = screen<app::ServerEditor>(h, "ServerEditor");
        EXPECT(editor != nullptr && editor->draft().jumps.size() == 1 && editor->draft().jumps[0].privateKey);
        editor->addJump();
        h.pump(30);
        fill(editor, h, 1, b, keyOf(dirB));
        EXPECT(editor->draft().jumps.size() == 2 && editor->draft().jumps[1].privateKey);
        EXPECT(editor->draft().jumps[1].keyText() == keyOf(dirB));
        std::string message = runTest(h, editor, {1}, 3);
        if (editor->testState.peek() != app::ServerEditor::TestState::passed) {
            std::fprintf(stderr, "two hops: %s\n", message.c_str());
        }
        EXPECT(editor->testState.peek() == app::ServerEditor::TestState::passed);

        // Mixed: the second hop logs in with a password, which B (key logins only) refuses. The
        // message names that hop and the step.
        editor->setPrivateKey(1, false);
        h.pump(30);
        type(editor->field("Jump host 2 Password"), "not-a-password");
        // Not asked again: no password would do.
        message = runTest(h, editor, {}, 3);
        EXPECT(editor->testState.peek() == app::ServerEditor::TestState::failed);
        std::fprintf(stderr, "password on hop 2: %s\n", message.c_str());
        EXPECT(message.starts_with("Jump host 2 (" + user + "@127.0.0.1:" + std::to_string(b.port()) + "): "));
        EXPECT(message.find("couldn't log in: it doesn't take passwords") != std::string::npos);

        // A key the first hop doesn't know: refused there.
        editor->setPrivateKey(1, true);
        h.pump(30);
        type(editor->field("Jump host 1 Private key"), keyOf(dirB));
        message = runTest(h, editor, {}, 3);
        std::fprintf(stderr, "wrong key on hop 1: %s\n", message.c_str());
        EXPECT(message.starts_with("Jump host 1 (" + user + "@127.0.0.1:" + std::to_string(a.port()) + "): "));
        EXPECT(message.find("log in") != std::string::npos);

        // A second hop nothing listens on: the tunnel through the first hop fails, and says so.
        type(editor->field("Jump host 1 Private key"), keyOf(dirA));
        type(editor->field("Jump host 2 Port"), std::to_string(freePort()));
        message = runTest(h, editor, {}, 3);
        std::fprintf(stderr, "closed port behind hop 1: %s\n", message.c_str());
        EXPECT(message.starts_with("Jump host 2 ("));
        EXPECT(message.find("through jump host 1") != std::string::npos);

        // A first hop nothing listens on.
        type(editor->field("Jump host 1 Port"), std::to_string(freePort()));
        message = runTest(h, editor, {}, 3);
        std::fprintf(stderr, "closed first hop: %s\n", message.c_str());
        EXPECT(message.starts_with("Jump host 1 ("));
        EXPECT(message.find("connect") != std::string::npos);
    }
}
#endif

}  // namespace

// Settings' hidden items entry and its screen: each with its path and Unhide, and Unhide all
// after a confirmation.
void testHiddenItemsScreen() {
    Dirs d;
    AppHarness h(d.home.string(), d.files.string());
    const auto local = [&](const std::string &relative) { return vfs::Location::local((d.home / relative).string()); };
    h.state().setHidden({local("Download/secret.txt"), local("Music"), vfs::Location::sftp("nas", "/srv/old")}, true);
    h.state().openScreen(app::kSettingsScreen);
    h.pump(100);
    ui::Node *link = findControl(*h.app->screenNode(), "Hidden items, 3 items", ui::Role::button);
    EXPECT(link != nullptr);
    EXPECT(findLabel(*h.app->screenNode(), "Dot files and items you hid") != nullptr);
    static_cast<ui::ScrollNode *>(h.app->screenNode()->children()[0].get())->scrollTo(400);
    h.pump(50);
    h.tap(*link);
    h.pump(100);
    ui::Node *screen = h.app->screenNode();
    EXPECT(screen != nullptr && screen->debugName() == "HiddenItemsScreen");
    EXPECT(findLabel(*screen, "secret.txt, Internal / Download / secret.txt") != nullptr);
    EXPECT(findLabel(*screen, "old, SFTP · nas / srv / old") != nullptr);
    ui::Node *unhide = findControl(*screen, "Unhide Music", ui::Role::button);
    EXPECT(unhide != nullptr);
    h.tap(*unhide);
    h.pump(50);
    EXPECT(h.state().settings.peek().hiddenItems.size() == 2 && findLabel(*screen, "Unhide Music") == nullptr);
    // Unhide all asks first; Cancel keeps them.
    h.tap(*findControl(*screen, "Unhide all", ui::Role::button));
    h.pump(300);
    ui::Node *cancel = findControl(*h.app->root(), "Cancel", ui::Role::button);
    EXPECT(cancel != nullptr && findLabel(*h.app->root(), "Unhide all?") != nullptr);
    h.tap(*cancel);
    EXPECT(h.waitUntil([&] { return h.app->context().overlays.openCount.peek() == 0; }));
    EXPECT(h.state().settings.peek().hiddenItems.size() == 2);
    h.tap(*findControl(*screen, "Unhide all", ui::Role::button));
    h.pump(300);
    h.tap(*findControl(*h.app->root(), "Unhide all 2 items", ui::Role::button));
    h.pump(300);
    EXPECT(h.state().settings.peek().hiddenItems.empty());
    EXPECT(findLabel(*screen, "Nothing hidden. Hide files and folders from their ⋮ menu; they show again, dimmed, "
                              "with Show hidden files.") != nullptr);
    // Back on settings, the entry says so.
    h.app->back();
    h.pump(100);
    EXPECT(findControl(*h.app->screenNode(), "Hidden items, None yet", ui::Role::button) != nullptr);
}

// Switching to Clean while a folder is open lays the screens out again in place: the folder, the
// selection and the rows stay, the rows take one line with their columns, the bars go to the
// edges, and the status line counts what's there. Back to Acid undoes it.
void testLiveCleanLayout() {
    Dirs dirs;
    makeFile(dirs.home / "Download" / "notes.txt", 3 * 1024, 2026, 10, 2);
    AppHarness h(dirs.home.string(), dirs.files.string());
    const vfs::Location download = vfs::Location::local(h.app->context().services.homePath + "/Download");
    h.state().navigate(download);
    h.settle(100);
    h.state().setSelection({"notes.txt"});
    h.pump(50);
    ui::Node *row = h.rowFor("notes.txt");
    EXPECT(row != nullptr && row->frame().height == 60);
    const float acidBar = h.app->context().barHeight.peek();
    EXPECT(!h.app->browse().statusText().parent()->style().display);

    h.state().updateSettings([](app::Settings &s) { s.preset = ui::ThemePreset::clean; });
    h.pump(100);
    row = h.rowFor("notes.txt");
    EXPECT(row != nullptr && row->frame().height == 44);
    EXPECT(h.app->context().barHeight.peek() < acidBar);
    EXPECT(h.state().active()->current().location == download);
    EXPECT(h.state().active()->selection.peek() == std::vector<std::string>{"notes.txt"});
    EXPECT(h.app->browse().statusText().parent()->style().display);
    EXPECT(h.app->browse().statusText().text() == "1 selected · 3.0 KiB");
    h.state().setSelection({});
    h.pump(50);
    EXPECT(h.app->browse().statusText().text().starts_with("1 item · 3.0 KiB"));

    h.state().updateSettings([](app::Settings &s) { s.preset = ui::ThemePreset::acid; });
    h.pump(100);
    row = h.rowFor("notes.txt");
    EXPECT(row != nullptr && row->frame().height == 60);
    EXPECT(h.app->context().barHeight.peek() == acidBar);
    EXPECT(!h.app->browse().statusText().parent()->style().display);
}

int main() {
    // FILEPAN_TEST_ONLY=<substring> runs only the matching tests.
    const char *only = std::getenv("FILEPAN_TEST_ONLY");
    const auto run = [only](const char *name, void (*test)()) {
        if (only != nullptr && std::string_view(name).find(only) == std::string_view::npos) {
            return;
        }
        std::fprintf(stderr, "screens_test: %s\n", name);
        test();
    };
    run("servers", testServerCrud);
    run("secrets", testSecretsMigrationAndNoKeyStore);
    run("old key paths", testOldKeyPaths);
    run("prompts", testPromptSheets);
    run("security key sheet", testSecurityKeySheet);
    run("openpgp card sheet", testOpenPgpCardSheet);
    run("card login", testCardLogin);
    run("archive password", testArchivePasswordRetry);
    run("settings", testSettingsLive);
    run("live clean layout", testLiveCleanLayout);
    run("hidden items", testHiddenItemsScreen);
    run("licenses", testLicenses);
    run("onboarding", testOnboarding);
    run("backup", testBackupRestore);
    run("restore without secrets", testRestoreWithoutSecrets);
#ifdef FILEPAN_SSHD
    run("sftp", testSftpAgainstSshd);
    run("sftp jump hosts", testSftpJumpHosts);
#endif
    std::puts("screens_test: OK");
    return 0;
}
