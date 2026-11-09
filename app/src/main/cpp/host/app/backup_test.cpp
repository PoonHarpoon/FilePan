// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Settings backups: the key derivation against a known vector, round trips with and without a
// password, a wrong password, tampering, files that aren't backups, and what's left out.

#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

#include "filepan/app/backup.hpp"
#include "filepan/app/secrets.hpp"
#include "test_util.hpp"

using namespace filepan;

namespace {

// Few iterations, so the tests are quick; readBackup accepts no fewer.
constexpr std::uint32_t kFast = app::kBackupMinIterations;

std::string hex(std::span<const std::uint8_t> bytes) {
    std::string out;
    for (const std::uint8_t b : bytes) {
        char digits[3];
        std::snprintf(digits, sizeof digits, "%02x", b);
        out += digits;
    }
    return out;
}

app::Settings sample() {
    app::Settings s;
    s.preset = ui::ThemePreset::ultraviolet;
    s.accent = gfx::Color{1, 0, 1, 1};  // Exact in #RRGGBB.
    s.dataSaver = true;
    s.favorites = {{"Papers", vfs::Location::local("/storage/emulated/0/Download/Papers"), app::Favorite::Kind::folder}};
    s.hiddenItems = {vfs::Location::local("/storage/emulated/0/.thumbs")};
    s.tabs = {{{vfs::Location::local("/storage/emulated/0")}}, {{vfs::Location::sftp("nas", "/srv")}}};
    s.activeTab = 1;
    vfs::SftpServerConfig server;
    server.id = "nas";
    server.name = "Home NAS";
    server.server = {.host = "nas.local", .port = 2222, .username = "me",
                     .authentication = {.type = vfs::SftpAuthentication::Type::kPrivateKey,
                                        .privateKey = "-----BEGIN OPENSSH PRIVATE KEY-----\nAAAA\n"
                                                      "-----END OPENSSH PRIVATE KEY-----\n",
                                        .passphrase = "key pass"}};
    server.jumpHosts = {{.host = "jump", .port = 22, .username = "j", .authentication = {.password = "hop secret"}}};
    server.knownHostsPath = "/data/old/files/known_hosts";
    server.timeoutSeconds = 30;
    s.servers = {server};
    return s;
}

const std::vector<std::string> kKnownHosts = {"nas.local ssh-ed25519 AAAAC3Nz", "[jump]:22 ssh-rsa AAAAB3 x y"};

void testKeyDerivation() {
    // RFC 7914 section 11: PBKDF2-HMAC-SHA256, "passwd", "salt", 1 iteration (its first 32 bytes).
    const std::string salt = "salt";
    auto key = app::deriveBackupKey("passwd", {reinterpret_cast<const std::uint8_t *>(salt.data()), salt.size()}, 1);
    EXPECT(key && hex(*key) == "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc");
}

void testRoundTripWithPassword() {
    auto text = app::writeBackup(sample(), kKnownHosts, {.password = "correct horse", .iterations = kFast});
    EXPECT(text.has_value());
    EXPECT(text->starts_with("filepan-backup 1\nbackup-kdf pbkdf2-sha256 1000 "));
    // The secrets are sealed, not written as entered.
    EXPECT(text->find("hop secret") == std::string::npos && text->find("key pass") == std::string::npos &&
           text->find("OPENSSH") == std::string::npos);
    EXPECT(text->find("\nsecret ") != std::string::npos);
    // The tabs aren't in it.
    EXPECT(text->find("\ntab ") == std::string::npos);
    auto file = app::readBackup(*text);
    EXPECT(file && file->encrypted && file->iterations == kFast && file->knownHosts == kKnownHosts);
    auto restored = app::openBackup(*file, "correct horse");
    EXPECT(restored.has_value());
    app::Settings expected = sample();
    expected.tabs.clear();
    expected.activeTab = 0;
    EXPECT(restored->settings == expected);
    EXPECT(restored->knownHosts == kKnownHosts);
    // A wrong password is told apart at once.
    auto wrong = app::openBackup(*file, "wrong horse");
    EXPECT(!wrong && wrong.error().code == EACCES);
    // Without the password, it opens without the secrets.
    auto bare = app::openBackup(*file);
    EXPECT(bare && bare->settings.servers.size() == 1);
    EXPECT(bare->settings.servers[0].server.authentication.privateKey.empty() &&
           bare->settings.servers[0].jumpHosts[0].authentication.password.empty());
    EXPECT(bare->settings.servers[0].server.host == "nas.local" && bare->settings.preset == ui::ThemePreset::ultraviolet);
}

void testWithoutPassword() {
    auto text = app::writeBackup(sample(), {});
    EXPECT(text.has_value());
    // No secrets at all, sealed or not.
    EXPECT(text->find("backup-kdf") == std::string::npos && text->find("\nsecret ") == std::string::npos);
    EXPECT(text->find("hop secret") == std::string::npos && text->find("OPENSSH") == std::string::npos);
    auto file = app::readBackup(*text);
    EXPECT(file && !file->encrypted);
    // A password given anyway changes nothing.
    auto restored = app::openBackup(*file, "anything");
    EXPECT(restored && restored->settings.servers.size() == 1 && restored->settings.favorites.size() == 1);
    EXPECT(restored->settings.servers[0].jumpHosts[0].authentication.password.empty());
    EXPECT(restored->settings.dataSaver && restored->settings.accent.has_value());
    // The document trees' switch goes with it, but not the trees: grants stay on the device.
    app::Settings withTrees = sample();
    withTrees.documentTrees = true;
    withTrees.trees = {{"Shared", "content://com.android.externalstorage.documents/tree/primary%3AShared"}};
    auto treesText = app::writeBackup(withTrees, {});
    EXPECT(treesText && treesText->find("content://") == std::string::npos);
    auto treesBack = app::openBackup(*app::readBackup(*treesText), {});
    EXPECT(treesBack && treesBack->settings.documentTrees && treesBack->settings.trees.empty());
}

void testTampered() {
    auto text = app::writeBackup(sample(), {}, {.password = "pw", .iterations = kFast});
    EXPECT(text.has_value());
    // Flip a character inside the first secret's blob: that secret is dropped, the rest opens.
    const std::size_t secret = text->find("\nsecret ");
    EXPECT(secret != std::string::npos);
    const std::size_t blob = text->rfind(' ', text->find('\n', secret + 1)) + 6;
    (*text)[blob] = (*text)[blob] == 'A' ? 'B' : 'A';
    auto file = app::readBackup(*text);
    EXPECT(file.has_value());
    auto restored = app::openBackup(*file, "pw");
    EXPECT(restored.has_value());
    const auto &server = restored->settings.servers[0];
    const int kept = (server.server.authentication.privateKey.empty() ? 0 : 1) +
                     (server.server.authentication.passphrase.empty() ? 0 : 1) +
                     (server.jumpHosts[0].authentication.password.empty() ? 0 : 1);
    EXPECT(kept == 2);
    // A check that doesn't open reads as a wrong password.
    std::string badCheck = *text;
    const std::size_t check = badCheck.find("backup-check ") + 20;
    badCheck[check] = badCheck[check] == 'A' ? 'B' : 'A';
    auto checked = app::readBackup(badCheck);
    EXPECT(checked && !app::openBackup(*checked, "pw") && app::openBackup(*checked, "pw").error().code == EACCES);
}

void testNotBackups() {
    for (const char *text : {"", "hello\n", "filepan-settings 1\ntheme acid\n", "filepan-backup x\n",
                             "filepan-backup 1\ntheme acid\n"}) {
        auto file = app::readBackup(text);
        EXPECT(!file && file.error().code == EINVAL);
    }
    auto newer = app::readBackup("filepan-backup 2\nfilepan-settings 1\n");
    EXPECT(!newer && newer.error().code == ENOTSUP);
    // Too few iterations, or too many, isn't opened.
    for (const char *kdf : {"backup-kdf pbkdf2-sha256 999 c2FsdA==\n", "backup-kdf pbkdf2-sha256 10000001 c2FsdA==\n"}) {
        auto file = app::readBackup(std::string("filepan-backup 1\n") + kdf + "filepan-settings 1\n");
        EXPECT(!file && file.error().code == EINVAL);
    }
    // Windows line ends and unknown lines are fine.
    auto crlf = app::readBackup("filepan-backup 1\r\nsomething new 1\r\nfilepan-settings 1\r\ntheme ultraviolet\r\n");
    EXPECT(crlf && !crlf->encrypted);
    auto opened = app::openBackup(*crlf);
    EXPECT(opened && opened->settings.preset == ui::ThemePreset::ultraviolet);
}

}  // namespace

int main() {
    testKeyDerivation();
    testRoundTripWithPassword();
    testWithoutPassword();
    testTampered();
    testNotBackups();
    std::puts("backup_test: OK");
    return 0;
}
