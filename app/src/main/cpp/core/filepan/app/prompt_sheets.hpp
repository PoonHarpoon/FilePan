// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "filepan/app/app.hpp"
#include "filepan/vfs/archive_backend.hpp"
#include "filepan/vfs/sftp_backend.hpp"

// The sheets that answer the backends' questions while they connect or read: an SFTP host key
// to trust, an SFTP password or key passphrase, a security key to sign in with, and an archive
// password. The backends ask on I/O
// threads; each request is posted to the UI thread and queued, and the sheets show one at a time.
// Closing a sheet any other way than its buttons (back, the scrim, dragging it down) declines.
//
//   Host key, unknown:  the fingerprint in mono with a barcode; reject, accept once, or accept
//                       and save it to the known hosts.
//   Host key, changed:  a hard warning in the destructive color with the trusted and the new
//                       fingerprints; reject is the default (the primary button, focused first).
//   Credential:         the password or passphrase, saying so when the last one was wrong.
//   Security key:       follows the signing: present the key, touch it, its PIN; closes by
//                       itself when the signing ends.
//   Archive password:   the entry that needs it, saying so when the last one was wrong.
namespace filepan::app {

class PromptSheets final : public vfs::PasswordProvider,
                           public vfs::SftpPromptProvider,
                           public std::enable_shared_from_this<PromptSheets> {
public:
    // serverName: what to call a server by id (its saved name).
    static std::shared_ptr<PromptSheets> create(AppContext &context,
                                                std::function<std::string(const std::string &id)> serverName);

    // From the backends, on I/O threads.
    void requestPassword(vfs::PasswordRequest request,
                         DecisionPromise<std::optional<std::string>> promise) override;
    void requestHostKey(vfs::HostKeyPrompt prompt, DecisionPromise<vfs::HostKeyDecision> promise) override;
    void requestCredential(vfs::CredentialPrompt prompt,
                           DecisionPromise<std::optional<std::string>> promise) override;
    void requestSecurityKey(vfs::SecurityKeyPrompt prompt,
                            std::shared_ptr<vfs::SecurityKeySession> session) override;

    // --- UI thread ---
    // The prompt showing, if any, and how many wait behind it.
    [[nodiscard]] bool showing() const { return current_.has_value(); }
    [[nodiscard]] std::size_t waiting() const { return queue_.size(); }
    [[nodiscard]] ui::components::BottomSheet *sheet() const { return sheet_; }
    struct HostKey;
    struct Credential;
    struct ArchivePassword;
    struct SecurityKey;
    using Request = std::variant<HostKey, Credential, ArchivePassword, SecurityKey>;
    // The request showing, if any.
    [[nodiscard]] const Request *current() const { return current_ ? &*current_ : nullptr; }
    // What the showing sheet's buttons do, for tests and keyboard users.
    void answerHostKey(vfs::HostKeyDecision decision);
    // A password or passphrase; or a security key's PIN, which keeps the sheet open.
    void answerSecret(std::string secret);
    // Declines the showing prompt.
    void cancel();
    // Drops everything: the App is going.
    void shutdown();

    struct HostKey {
        vfs::HostKeyPrompt prompt;
        DecisionPromise<vfs::HostKeyDecision> promise;
    };
    struct Credential {
        vfs::CredentialPrompt prompt;
        DecisionPromise<std::optional<std::string>> promise;
    };
    struct ArchivePassword {
        vfs::PasswordRequest request;
        DecisionPromise<std::optional<std::string>> promise;
    };
    struct SecurityKey {
        vfs::SecurityKeyPrompt prompt;
        std::shared_ptr<vfs::SecurityKeySession> session;
    };

    PromptSheets(AppContext &context, std::function<std::string(const std::string &)> serverName)
        : context_(context), serverName_(std::move(serverName)) {}

private:
    void post(Request request);
    void enqueue(Request request);
    void showNext();
    void show(Request &request);
    ui::NodePtr hostKeyContent(const HostKey &request);
    ui::NodePtr secretContent(const std::string &message, const std::string &detail, bool retry,
                              const std::string &fieldLabel, const std::string &submitLabel);
    ui::NodePtr securityKeyContent(const SecurityKey &request);
    // Shows the session's state in the security key sheet, or closes it when it's done.
    void securityKeyChanged(const vfs::SecurityKeySession *session);
    void fillSecurityKeyBody(const vfs::SecurityKeySession::State &state);
    void finish();

    AppContext &context_;
    std::function<std::string(const std::string &)> serverName_;
    std::deque<Request> queue_;
    std::optional<Request> current_;
    ui::components::BottomSheet *sheet_ = nullptr;
    // The secret field of the showing sheet.
    ui::components::TextField *field_ = nullptr;
    // What the security key sheet shows, below its header, and the state it shows.
    std::shared_ptr<ui::Box> securityBody_;
    std::optional<vfs::SecurityKeySession::State> securityState_;
    std::string pinError_;
    // The PIN field takes letters, not just digits; and its text, kept while the keyboard switches.
    bool pinLetters_ = false;
    std::string pinDraft_;
    // The security key sheet is for an OpenPGP card, not a FIDO key.
    bool securityCard_ = false;
    std::uint64_t generation_ = 0;
    bool shutDown_ = false;
};

}  // namespace filepan::app
