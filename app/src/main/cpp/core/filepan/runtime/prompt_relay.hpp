// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "filepan/vfs/archive_backend.hpp"
#include "filepan/vfs/sftp_backend.hpp"

// The archive and SFTP backends are made before the app, and outlive it, so they ask a relay:
// it hands each request to whatever the app installed (sheets that ask the user), or declines it
// while nothing is installed. Thread-safe: the backends call it on I/O threads.
namespace filepan::runtime {

class PromptRelay final : public vfs::PasswordProvider, public vfs::SftpPromptProvider {
public:
    // Null declines: the promise is dropped, which gives up.
    void setPasswordProvider(std::shared_ptr<vfs::PasswordProvider> provider);
    void setSftpPrompts(std::shared_ptr<vfs::SftpPromptProvider> prompts);

    void requestPassword(vfs::PasswordRequest request,
                         DecisionPromise<std::optional<std::string>> promise) override;
    void requestHostKey(vfs::HostKeyPrompt prompt, DecisionPromise<vfs::HostKeyDecision> promise) override;
    void requestCredential(vfs::CredentialPrompt prompt,
                           DecisionPromise<std::optional<std::string>> promise) override;
    void requestSecurityKey(vfs::SecurityKeyPrompt prompt,
                            std::shared_ptr<vfs::SecurityKeySession> session) override;

private:
    std::mutex mutex_;
    std::shared_ptr<vfs::PasswordProvider> passwords_;
    std::shared_ptr<vfs::SftpPromptProvider> sftp_;
};

}  // namespace filepan::runtime
