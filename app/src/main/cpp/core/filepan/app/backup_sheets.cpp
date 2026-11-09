// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/app/backup_sheets.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "filepan/app/format.hpp"
#include "filepan/app/sftp_screens.hpp"
#include "filepan/app/sheet_parts.hpp"

namespace filepan::app {

namespace {

using namespace ui;
using namespace ui::components;

// Backups are read whole; settings are a few kB, so this is plenty.
constexpr std::size_t kMaxBackupBytes = 4 << 20;

std::vector<std::string> readLines(const std::string &path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

// Adds the lines path doesn't have yet, replacing it atomically.
void mergeLines(const std::string &path, const std::vector<std::string> &add) {
    std::vector<std::string> lines = readLines(path);
    bool changed = false;
    for (const std::string &line : add) {
        if (std::ranges::find(lines, line) == lines.end()) {
            lines.push_back(line);
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        for (const std::string &line : lines) {
            out << line << '\n';
        }
        if (!out.flush()) {
            std::remove(temporary.c_str());
            return;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
    }
}

std::string backupName() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char date[16];
    std::strftime(date, sizeof date, "%Y-%m-%d", &local);
    return std::string("filepan-settings-") + date + kBackupExtension;
}

NodePtr button(std::string label, ButtonVariant variant, std::function<void()> onTap, Prop<bool> enabled = true,
               std::string icon = {}) {
    return Button::create({.label = std::move(label), .variant = variant, .icon = std::move(icon),
                           .enabled = std::move(enabled), .onTap = std::move(onTap)});
}

bool hasSecrets(const vfs::SftpServerConfig &server) {
    const auto has = [](const vfs::SftpAuthentication &auth) {
        return !auth.password.empty() || !auth.privateKey.empty() || !auth.passphrase.empty();
    };
    return has(server.server.authentication) || std::ranges::any_of(server.jumpHosts, [&](const auto &hop) {
               return has(hop.authentication);
           });
}

bool sameEndpoint(const vfs::SftpEndpoint &a, const vfs::SftpEndpoint &b) {
    return a.host == b.host && a.port == b.port && a.username == b.username;
}

std::string countOf(std::size_t count, const char *one, const char *many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

std::string summary(const Settings &s) {
    std::vector<std::string> parts;
    parts.push_back(countOf(s.servers.size(), "server", "servers"));
    parts.push_back(countOf(s.favorites.size(), "favorite", "favorites"));
    parts.push_back(countOf(s.hiddenItems.size(), "hidden item", "hidden items"));
    std::string out;
    for (const std::string &part : parts) {
        out += (out.empty() ? "" : " · ") + part;
    }
    return out;
}

void showConfirm(AppContext &context, RestoredBackup backup, bool secretsLeftOut) {
    auto column = parts::sheetColumn(10);
    column->setDebugName("RestoreSheet");
    column->appendChild(parts::monoText(summary(backup.settings)));
    std::string message = "Your settings, favorites, hidden items and servers are replaced with the backup's. "
                          "Open tabs stay as they are.";
    if (secretsLeftOut && !backup.settings.servers.empty()) {
        message += " Passwords and keys aren't restored; ones saved here for the same servers are kept.";
    }
    column->appendChild(parts::bodyText(message));
    auto holder = std::make_shared<BottomSheet *>(nullptr);
    auto shared = std::make_shared<RestoredBackup>(std::move(backup));
    AppContext *ctx = &context;
    auto cancel = button("Cancel", ButtonVariant::glass, [holder] { (*holder)->dismiss(); });
    auto replace = button(
            "Replace", ButtonVariant::danger,
            [holder, shared, ctx] {
                (*holder)->dismiss();
                applyRestore(*ctx, std::move(*shared));
            },
            true, "settings_backup_restore");
    auto buttons = parts::buttonRow({cancel, replace});
    buttons->updateStyle([](Style &s) { s.margin = {0, 8, 0, 0}; });
    column->appendChild(buttons);
    *holder = &context.overlays.showSheet("Replace current settings?", column);
}

void showUnlock(AppContext &context, std::shared_ptr<BackupFile> file) {
    auto column = parts::sheetColumn(12);
    column->setDebugName("UnlockBackupSheet");
    column->appendChild(parts::bodyText("This backup's passwords and keys are sealed with a password."));
    Signal<std::string> error{std::string()};
    Signal<bool> busy{false};
    auto holder = std::make_shared<BottomSheet *>(nullptr);
    auto fieldHolder = std::make_shared<TextField *>(nullptr);
    AppContext *ctx = &context;
    const auto unlock = [ctx, file, holder, fieldHolder, error, busy] {
        if (busy.peek() || *fieldHolder == nullptr) {
            return;
        }
        busy.set(true);
        error.set({});
        std::string password = (*fieldHolder)->editor().editingValue().text;
        std::weak_ptr<const void> alive = ctx->lifetime;
        ctx->services.io.submit([file, password = std::move(password)] { return openBackup(*file, password); },
                                [ctx, alive, holder, error, busy](Result<RestoredBackup> opened) {
                                    if (alive.expired()) {
                                        return;
                                    }
                                    busy.set(false);
                                    if (!opened) {
                                        error.set(opened.error().code == EACCES ? "That didn't work. Try again."
                                                                                : opened.error().message);
                                        return;
                                    }
                                    (*holder)->dismiss();
                                    showConfirm(*ctx, std::move(*opened), false);
                                });
    };
    auto field = TextField::create({.label = "Backup password",
                                    .icon = "key",
                                    .error = [error] { return error(); },
                                    .config = {.keyboard = KeyboardType::password, .action = ImeAction::done},
                                    .onSubmit = unlock});
    *fieldHolder = field.get();
    column->appendChild(field);
    auto skip = button("Without them", ButtonVariant::glass, [ctx, file, holder] {
        (*holder)->dismiss();
        auto opened = openBackup(*file);
        if (!opened) {
            ctx->notify(opened.error().message);
            return;
        }
        showConfirm(*ctx, std::move(*opened), true);
    });
    auto ok = button("Unlock", ButtonVariant::primary, unlock, [busy] { return !busy(); }, "lock_open");
    auto buttons = parts::buttonRow({skip, ok});
    buttons->updateStyle([](Style &s) { s.margin = {0, 4, 0, 0}; });
    column->appendChild(buttons);
    *holder = &context.overlays.showSheet("Restore settings", parts::scrolling(column));
    field->editor().requestFocus();
}

}  // namespace

void showBackupSheet(AppContext &context) {
    auto column = parts::sheetColumn(12);
    column->setDebugName("BackupSheet");
    column->appendChild(parts::bodyText("Your settings, favorites, hidden items and SFTP servers. Their passwords "
                                        "and keys go in only if you set a password; keep it somewhere safe."));
    Signal<std::string> password{std::string()};
    Signal<std::string> repeat{std::string()};
    const auto mismatch = [password, repeat] { return !repeat().empty() && password() != repeat(); };
    auto holder = std::make_shared<BottomSheet *>(nullptr);
    AppContext *ctx = &context;
    const auto save = [ctx, holder, password, repeat] {
        if (password.peek() != repeat.peek()) {
            return;
        }
        (*holder)->dismiss();
        BackupOptions options;
        if (!password.peek().empty()) {
            options.password = password.peek();
        }
        const Settings settings = ctx->state.settings.peek();
        const std::string knownHosts = knownHostsPath(*ctx);
        std::weak_ptr<const void> alive = ctx->lifetime;
        ctx->services.io.submit(
                [settings, knownHosts, options = std::move(options)] {
                    return writeBackup(settings, knownHosts.empty() ? std::vector<std::string>() : readLines(knownHosts),
                                       options);
                },
                [ctx, alive](Result<std::string> written) {
                    if (alive.expired()) {
                        return;
                    }
                    if (!written || ctx->services.saver == nullptr) {
                        ctx->notify("Couldn't make the backup");
                        return;
                    }
                    const bool asked = ctx->services.saver->save(
                            {.name = backupName(), .bytes = std::move(*written)},
                            [ctx, alive](runtime::SavedDocument saved) {
                                if (alive.expired() || saved.status == runtime::SavedDocument::Status::cancelled) {
                                    return;
                                }
                                ctx->notify(saved.status == runtime::SavedDocument::Status::saved
                                                    ? std::string("Settings backed up")
                                                    : std::string("Couldn't save the backup"));
                            });
                    if (!asked) {
                        ctx->notify("Couldn't save the backup");
                    }
                });
    };
    column->appendChild(TextField::create({.label = "Password",
                                           .placeholder = "Optional",
                                           .icon = "key",
                                           .config = {.keyboard = KeyboardType::password, .action = ImeAction::next},
                                           .onChange = [password](const std::string &text) { password.set(text); }}));
    column->appendChild(TextField::create({.label = "Repeat password",
                                           .icon = "key",
                                           .error = [mismatch] { return mismatch() ? std::string("Doesn't match")
                                                                                   : std::string(); },
                                           .config = {.keyboard = KeyboardType::password, .action = ImeAction::done},
                                           .onChange = [repeat](const std::string &text) { repeat.set(text); },
                                           .onSubmit = save}));
    auto cancel = button("Cancel", ButtonVariant::glass, [holder] { (*holder)->dismiss(); });
    auto ok = button("Save backup", ButtonVariant::primary, save, [password, repeat] { return password() == repeat(); },
                     "backup");
    auto buttons = parts::buttonRow({cancel, ok});
    buttons->updateStyle([](Style &s) { s.margin = {0, 4, 0, 0}; });
    column->appendChild(buttons);
    *holder = &context.overlays.showSheet("Back up settings", parts::scrolling(column));
}

void startRestore(AppContext &context) {
    if (context.services.documents == nullptr) {
        context.notify("There's no file picker here");
        return;
    }
    AppContext *ctx = &context;
    std::weak_ptr<const void> alive = ctx->lifetime;
    const bool asked = context.services.documents->pick(
            {.mimeTypes = {"*/*"}, .maxBytes = kMaxBackupBytes}, [ctx, alive](runtime::PickedDocument picked) {
                using Status = runtime::PickedDocument::Status;
                if (alive.expired() || picked.status == Status::cancelled) {
                    return;
                }
                if (picked.status != Status::picked) {
                    ctx->notify(picked.status == Status::tooLarge ? "That isn't a FilePan backup"
                                                                  : "Couldn't read that file");
                    return;
                }
                auto file = readBackup(picked.bytes);
                if (!file) {
                    ctx->notify(file.error().message);
                    return;
                }
                if (file->encrypted) {
                    showUnlock(*ctx, std::make_shared<BackupFile>(std::move(*file)));
                    return;
                }
                auto opened = openBackup(*file);
                if (!opened) {
                    ctx->notify(opened.error().message);
                    return;
                }
                showConfirm(*ctx, std::move(*opened), true);
            });
    if (!asked) {
        context.notify("There's no file picker here");
    }
}

void applyRestore(AppContext &context, RestoredBackup backup) {
    const std::string knownHosts = knownHostsPath(context);
    context.state.updateSettings([&](Settings &s) {
        Settings restored = std::move(backup.settings);
        restored.tabs = std::move(s.tabs);
        restored.activeTab = s.activeTab;
        restored.onboarded = s.onboarded;
        restored.trees = std::move(s.trees);
        for (vfs::SftpServerConfig &server : restored.servers) {
            server.knownHostsPath = knownHosts;
            if (hasSecrets(server)) {
                continue;
            }
            // The same server saved here keeps its passwords and keys.
            const auto here = std::ranges::find_if(s.servers, [&](const auto &other) { return other.id == server.id; });
            if (here == s.servers.end()) {
                continue;
            }
            if (sameEndpoint(server.server, here->server)) {
                server.server.authentication = here->server.authentication;
            }
            for (std::size_t i = 0; i < server.jumpHosts.size() && i < here->jumpHosts.size(); ++i) {
                if (sameEndpoint(server.jumpHosts[i], here->jumpHosts[i])) {
                    server.jumpHosts[i].authentication = here->jumpHosts[i].authentication;
                }
            }
        }
        s = std::move(restored);
    });
    if (!knownHosts.empty() && !backup.knownHosts.empty()) {
        context.services.io.execute(
                [knownHosts, lines = std::move(backup.knownHosts)] { mergeLines(knownHosts, lines); });
    }
    context.notify("Settings restored");
}

}  // namespace filepan::app
