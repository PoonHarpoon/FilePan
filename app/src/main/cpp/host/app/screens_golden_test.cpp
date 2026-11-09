// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Golden images of the SFTP screens, the prompt sheets, settings, about and licenses, rendered as
// app_golden_test renders: the real Vulkan renderer (lavapipe) and the bundled fonts, on a
// 1080x2400 phone at density 2.625, in Acid (the default), a few of them in Ultraviolet too.
//
//   FILEPAN_UPDATE_GOLDENS=1 build/host/app/screens_golden_test   rewrites golden/screens_*.png
//   FILEPAN_GOLDEN_ONLY=<substring>                                 renders only matching pages

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app_harness.hpp"
#include "filepan/app/about_screen.hpp"
#include "filepan/app/backup_sheets.hpp"
#include "filepan/app/format.hpp"
#include "filepan/app/hidden_items_screen.hpp"
#include "filepan/app/onboarding_screen.hpp"
#include "filepan/app/prompt_sheets.hpp"
#include "filepan/app/settings_screen.hpp"
#include "filepan/app/sftp_screens.hpp"
#include "filepan/app/text_screen.hpp"
#include "filepan/gfx/vulkan/renderer.hpp"
#include "gfx_test_util.hpp"

using namespace filepan;
using namespace filepan::test;
namespace fs = std::filesystem;

namespace {

struct Dirs {
    TempDir dir;
    fs::path home;
    fs::path files;

    Dirs() {
        home = dir.path() / "emulated";
        files = dir.path() / "files";
        fs::create_directories(files);
        makeFolder(home / "Download", 2026, 10, 3);
    }
};

vfs::SftpEndpoint endpoint(std::string host, int port, std::string user, bool key = false) {
    vfs::SftpEndpoint e;
    e.host = std::move(host);
    e.port = port;
    e.username = std::move(user);
    if (key) {
        e.authentication.type = vfs::SftpAuthentication::Type::kPrivateKey;
        e.authentication.privateKey = kTestPrivateKey;
    }
    return e;
}

void servers(AppHarness &h) {
    vfs::SftpServerConfig nas{.id = "home-nas", .name = "Home NAS", .server = endpoint("nas.local", 22, "me")};
    vfs::SftpServerConfig build{.id = "build-box",
                                .name = "Build box",
                                .server = endpoint("10.0.4.17", 2222, "ci", true),
                                .jumpHosts = {endpoint("bastion.example.org", 22, "ops", true)}};
    vfs::SftpServerConfig seedbox{.id = "seedbox", .name = "Seedbox", .server = endpoint("eu-7.seedhost.net", 2022, "anna")};
    h.state().updateSettings([&](app::Settings &s) { s.servers = {nas, build, seedbox}; });
}

void hostKey(AppHarness &h, bool changed) {
    servers(h);
    h.state().openScreen(app::kServersScreen);
    vfs::HostKeyPrompt prompt{.serverId = "build-box",
                              .host = "10.0.4.17",
                              .port = 2222,
                              .status = changed ? vfs::HostKeyPrompt::Status::kChanged : vfs::HostKeyPrompt::Status::kUnknown,
                              .keyType = "ssh-ed25519",
                              .fingerprint = "SHA256:q2Vd8m3RkT1yXw0bZ7nLh4Cj9eFsPuAo6GiKtYxN5Ec",
                              .knownFingerprint = "SHA256:Fh2kQ9pLm4ZsW8vTn1cRb7eYu3XaDo6GjKiNx0Pq5Ms"};
    auto [promise, future] = makeDecision<vfs::HostKeyDecision>();
    h.prompts->requestHostKey(prompt, promise);
    // Keep the request alive through the render.
    static std::vector<DecisionFuture<vfs::HostKeyDecision>> keep;
    keep.push_back(std::move(future));
    h.waitUntil([&] { return h.app->prompts().showing(); });
}

// A key that's there, for the security key sheet's touch step.
class PresentKey final : public securitykey::CtapTransport {
public:
    [[nodiscard]] bool mayHaveCtap2() const override { return true; }
    [[nodiscard]] bool isUserPresent() const override { return false; }
    securitykey::KeyResult<securitykey::Bytes> sendCbor(int, std::span<const std::uint8_t>) override {
        return securitykey::keyFailure(securitykey::KeyError::Kind::kClosed);
    }
    securitykey::KeyResult<securitykey::ApduResponse> sendApdu(int, int, int,
                                                               std::span<const std::uint8_t>) override {
        return securitykey::keyFailure(securitykey::KeyError::Kind::kClosed);
    }
    void close() override {}
    [[nodiscard]] bool isClosed() const override { return false; }
};

// The security key sheet for the build box's jump host: waiting for a touch, or for the PIN after
// a wrong one.
void securityKey(AppHarness &h, bool pin) {
    servers(h);
    h.state().openScreen(app::kServersScreen);
    // Kept, with whatever waits on it, through the render.
    auto *session = new std::shared_ptr<vfs::SecurityKeySession>(std::make_shared<vfs::SecurityKeySession>());
    h.prompts->requestSecurityKey({.serverId = "build-box",
                                   .hop = 0,
                                   .isJumpHost = true,
                                   .host = "bastion.example.org",
                                   .username = "ops",
                                   .userVerification = pin},
                                  *session);
    (*session)->offerTransport(std::make_unique<PresentKey>());
    static std::vector<std::unique_ptr<securitykey::PresentedKey>> keep;
    keep.push_back((*session)->awaitTransport());
    if (pin) {
        std::thread([session] { (void)(*session)->awaitPin(3, true); }).detach();
        h.waitUntil([&] { return (*session)->state().step == vfs::SecurityKeySession::Step::kPin; });
    }
    h.waitUntil([&] { return h.app->prompts().showing(); });
}

// The OpenPGP card sheet for the build box, asking for the card's PIN after a wrong one.
void openPgpCard(AppHarness &h) {
    servers(h);
    h.state().openScreen(app::kServersScreen);
    // Kept, with whatever waits on it, through the render.
    auto *session = new std::shared_ptr<vfs::SecurityKeySession>(
            std::make_shared<vfs::SecurityKeySession>(vfs::SecurityKeySession::Kind::kOpenPgpCard));
    h.prompts->requestSecurityKey({.serverId = "build-box",
                                   .hop = 1,
                                   .isJumpHost = false,
                                   .host = "build.example.org",
                                   .username = "ops",
                                   .userVerification = true},
                                  *session);
    std::thread([session] { (void)(*session)->awaitPin(2, true); }).detach();
    h.waitUntil([&] { return (*session)->state().step == vfs::SecurityKeySession::Step::kPin; });
    h.waitUntil([&] { return h.app->prompts().showing(); });
}

// The intro on page, still: under reduced motion the orb holds and pages don't slide.
void intro(AppHarness &h, std::size_t page) {
    h.root->env().reducedMotion.set(true);
    h.app->context().showOnboarding();
    h.pump(50);
    h.app->onboarding()->goTo(page);
}

struct Page {
    const char *name;
    std::function<void(AppHarness &)> show;
};

const Page kPages[] = {
        {"sftp_list",
         [](AppHarness &h) {
             servers(h);
             h.state().openScreen(app::kServersScreen);
         }},
        {"sftp_empty", [](AppHarness &h) { h.state().openScreen(app::kServersScreen); }},
        {"sftp_editor",
         [](AppHarness &h) {
             servers(h);
             h.state().openScreen(app::kServersScreen);
             h.pump(50);
             static_cast<app::ServersScreen *>(h.app->screenNode())->edit("build-box");
         }},
        {"sftp_editor_errors",
         [](AppHarness &h) {
             h.state().openScreen(app::kServersScreen);
             h.pump(50);
             static_cast<app::ServersScreen *>(h.app->screenNode())->add();
             h.pump(50);
             auto *editor = static_cast<app::ServerEditor *>(h.app->screenNode());
             editor->save();
             h.app->context().overlays.snackbars().dismissCurrent();
         }},
        {"sftp_editor_key",
         [](AppHarness &h) {
             // A new server's key login, with a key cut short pasted: the error under the field.
             h.state().openScreen(app::kServersScreen);
             h.pump(50);
             static_cast<app::ServersScreen *>(h.app->screenNode())->add();
             h.pump(50);
             auto *editor = static_cast<app::ServerEditor *>(h.app->screenNode());
             editor->setPrivateKey(std::nullopt, true);
             h.pump(50);
             const std::string key = kTestPrivateKey;
             const std::string cut = key.substr(0, key.size() / 2);
             editor->field("Private key")->editor().updateEditingValue(
                     {cut, {static_cast<std::int32_t>(cut.size()), static_cast<std::int32_t>(cut.size())}, {}});
         }},
        {"sftp_editor_card",
         [](AppHarness &h) {
             // A new server logging in with an OpenPGP card, its key read.
             h.state().openScreen(app::kServersScreen);
             h.pump(50);
             static_cast<app::ServersScreen *>(h.app->screenNode())->add();
             h.pump(50);
             auto *editor = static_cast<app::ServerEditor *>(h.app->screenNode());
             editor->setLogin(std::nullopt, app::ServerEditor::Login::card);
             h.pump(50);
             const std::string key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIPLaWF6s6nZOOUpOUGJyViqrU38Gsjh50aX787/W18xU "
                                     "cardno:000612345678";
             const auto parsed = parsePublicKey(key);
             editor->cardRead(std::nullopt, vfs::CardKey{.publicKey = key,
                                                         .type = parsed->type,
                                                         .fingerprint = parsed->fingerprint});
         }},
        {"hostkey_unknown", [](AppHarness &h) { hostKey(h, false); }},
        {"hostkey_changed", [](AppHarness &h) { hostKey(h, true); }},
        {"security_key_touch", [](AppHarness &h) { securityKey(h, false); }},
        {"security_key_pin", [](AppHarness &h) { securityKey(h, true); }},
        {"openpgp_card_pin", [](AppHarness &h) { openPgpCard(h); }},
        {"password_sheet",
         [](AppHarness &h) {
             auto [promise, future] = makeDecision<std::optional<std::string>>();
             h.prompts->requestPassword({.archive = vfs::Location::local("/storage/emulated/0/Download/backup-2026.zip"),
                                         .entryPath = "/photos/IMG_0042.jpg",
                                         .retry = true},
                                        promise);
             static std::vector<DecisionFuture<std::optional<std::string>>> keep;
             keep.push_back(std::move(future));
             h.waitUntil([&] { return h.app->prompts().showing(); });
         }},
        {"settings", [](AppHarness &h) { h.state().openScreen(app::kSettingsScreen); }},
        {"settings_end",
         [](AppHarness &h) {
             h.state().openScreen(app::kSettingsScreen);
             h.pump(50);
             static_cast<ui::ScrollNode *>(h.app->screenNode()->children()[0].get())->scrollTo(100000);
         }},
        {"hidden_items",
         [](AppHarness &h) {
             const std::string home = h.app->context().services.homePath;
             h.state().setHidden({vfs::Location::local(home + "/Download/old-drafts"),
                                  vfs::Location::local(home + "/DCIM/Camera/2026/october-coast-trip-raw-exports"),
                                  vfs::Location::sftp("home-nas", "/srv/media/backups")},
                                 true);
             h.state().openScreen(app::kHiddenItemsScreen);
         }},
        {"accent_picker",
         [](AppHarness &h) {
             h.state().updateSettings([](app::Settings &s) { s.accent = gfx::Color::fromRgb(0x7C5CFF); });
             h.state().openScreen(app::kSettingsScreen);
             h.pump(50);
             app::showAccentPicker(h.app->context());
         }},
        {"backup_sheet",
         [](AppHarness &h) {
             h.state().openScreen(app::kSettingsScreen);
             h.pump(50);
             app::showBackupSheet(h.app->context());
         }},
        {"restore_confirm",
         [](AppHarness &h) {
             app::Settings backup;
             vfs::SftpServerConfig server;
             server.id = "home-nas";
             server.server = {.host = "nas.local", .port = 22, .username = "me"};
             backup.servers = {server, server};
             backup.servers[1].id = "build-box";
             backup.favorites = {{"Papers", vfs::Location::local("/storage/emulated/0/Download/Papers")}};
             h.state().openScreen(app::kSettingsScreen);
             h.pump(50);
             app::startRestore(h.app->context());
             h.documents.deliver(h.platform.picks.back().first,
                                 {.status = runtime::PickedDocument::Status::picked, .bytes = *app::writeBackup(backup, {})});
         }},
        {"onboarding_welcome", [](AppHarness &h) { intro(h, 0); }},
        {"onboarding_access",
         [](AppHarness &h) {
             h.services->allFilesAccess.set(false);
             intro(h, 1);
         }},
        {"onboarding_look", [](AppHarness &h) { intro(h, 3); }},
        {"onboarding_data", [](AppHarness &h) { intro(h, 4); }},
        {"text_editor",
         [](AppHarness &h) {
             // A short source file, opened in the editor, a word selected and a line wrapping.
             const std::string home = h.app->context().services.homePath;
             std::ofstream(home + "/Download/render.cpp")
                     << "// Draws the list.\n#include <vector>\n\nvoid render(const std::vector<Row> &rows) {\n"
                        "\tfor (const Row &row : rows) {\n\t\tdraw(row.name, row.size, row.modified, row.icon, "
                        "row.selected);\n\t}\n}\n";
             const vfs::Location folder = vfs::Location::local(home + "/Download");
             const auto entry = h.app->context().services.backends.backendFor(folder)->stat(folder.child("render.cpp"));
             EXPECT(entry.has_value());
             h.app->context().openText({folder, {*entry}});
             h.waitUntil([&] {
                 return h.app->textScreen() != nullptr &&
                        h.app->textScreen()->status() == app::TextScreen::Status::ready;
             });
             h.pump(50);
             h.app->textScreen()->editor()->select(64, 70);
         }},
        {"about", [](AppHarness &h) { h.state().openScreen(app::kAboutScreen); }},
        {"licenses",
         [](AppHarness &h) {
             h.state().openScreen(app::kLicensesScreen);
             h.waitUntil([&] {
                 auto *node = h.app->screenNode();
                 return node && static_cast<app::LicensesScreen *>(node)->licenses().peek().done;
             });
         }},
        {"license_text",
         [](AppHarness &h) {
             h.state().openScreen(app::kLicensesScreen);
             h.waitUntil([&] { return static_cast<app::LicensesScreen *>(h.app->screenNode())->licenses().peek().done; });
             h.pump(50);
             auto *list = static_cast<app::LicensesScreen *>(h.app->screenNode());
             for (const auto &notice : list->licenses().peek().value.notices) {
                 if (notice.name == "libssh") {
                     list->open(notice);
                 }
             }
             h.waitUntil([&] {
                 auto *node = h.app->screenNode();
                 return node && node->debugName() == "LicenseScreen" &&
                        static_cast<app::LicenseScreen *>(node)->text().peek().done;
             });
         }},
};

// Every page in Acid, the default, and in Clean, whose layout is its own; only kOtherLookPages in
// Ultraviolet, which shares Acid's.
constexpr std::string_view kOtherLookPages[] = {"settings", "sftp_list", "password_sheet", "onboarding_look",
                                                "text_editor"};

struct Look {
    ui::ThemePreset preset;
    bool night;
    const char *name;
};

constexpr Look kLooks[] = {{ui::ThemePreset::acid, true, "acid"},
                           {ui::ThemePreset::ultraviolet, true, "ultraviolet"},
                           {ui::ThemePreset::clean, true, "clean_dark"},
                           {ui::ThemePreset::clean, false, "clean_light"}};

}  // namespace

int main() {
    // Dates in the renders leave out this year whatever the clock says.
    app::setCurrentYearForTesting(2026);
    std::unique_ptr<gfx::TextSystem> text =
            text::createTextSystem(text::createDirectoryFontProvider(FILEPAN_FONTS_DIR), {.locale = "en"});
    auto created = gfx::vulkan::Renderer::create(*text);
    if (!created) {
        skipOrFail(created.error().message);
    }
    auto renderer = std::move(*created);
    auto target = renderer->createOffscreenTarget(AppHarness::kWidth, AppHarness::kHeight);
    EXPECT(target.has_value());
    const char *onlyEnv = std::getenv("FILEPAN_GOLDEN_ONLY");
    const std::string only = onlyEnv ? onlyEnv : "";
    int failures = 0;
    for (const Look &look : kLooks) {
        for (const Page &page : kPages) {
            if (look.preset == ui::ThemePreset::ultraviolet &&
                std::ranges::find(kOtherLookPages, std::string_view(page.name)) == std::end(kOtherLookPages)) {
                continue;
            }
            const std::string name = std::string("screens_") + page.name + "_" + look.name;
            if (!only.empty() && name.find(only) == std::string::npos) {
                continue;
            }
            Dirs dirs;
            AppHarness h(dirs.home.string(), dirs.files.string(), {}, true, text.get());
            h.root->env().night.set(look.night);
            h.state().updateSettings([&](app::Settings &s) { s.preset = look.preset; });
            // Applied before the page opens, as it would be by the time anyone opens one.
            h.pump(30);
            page.show(h);
            h.pump(900);
            EXPECT(h.lastFrame != nullptr);
            auto rendered = renderer->render(**target, *h.lastFrame, h.root->env().theme.peek().colors.ground);
            if (!rendered) {
                std::fprintf(stderr, "render failed: %s\n", rendered.error().message.c_str());
                return 1;
            }
            auto pixels = renderer->readPixels(**target);
            EXPECT(pixels.has_value());
            const RgbaImage image{AppHarness::kWidth, AppHarness::kHeight, std::move(*pixels)};
            const fs::path out(FILEPAN_TEST_OUTPUT_DIR);
            fs::create_directories(out);
            writePng(out / (name + ".png"), image);
            if (!matchesGolden(name, image)) {
                ++failures;
            }
        }
    }
    if (failures > 0) {
        std::fprintf(stderr, "%d golden(s) differ\n", failures);
        return 1;
    }
    std::puts("screens_golden_test: OK");
    return 0;
}
