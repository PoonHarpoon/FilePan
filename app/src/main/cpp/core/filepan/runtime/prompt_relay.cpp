// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/runtime/prompt_relay.hpp"

namespace filepan::runtime {

void PromptRelay::setPasswordProvider(std::shared_ptr<vfs::PasswordProvider> provider) {
    std::lock_guard lock(mutex_);
    passwords_ = std::move(provider);
}

void PromptRelay::setSftpPrompts(std::shared_ptr<vfs::SftpPromptProvider> prompts) {
    std::lock_guard lock(mutex_);
    sftp_ = std::move(prompts);
}

void PromptRelay::requestPassword(vfs::PasswordRequest request,
                                  DecisionPromise<std::optional<std::string>> promise) {
    std::shared_ptr<vfs::PasswordProvider> provider;
    {
        std::lock_guard lock(mutex_);
        provider = passwords_;
    }
    if (provider) {
        provider->requestPassword(std::move(request), std::move(promise));
    }
}

void PromptRelay::requestHostKey(vfs::HostKeyPrompt prompt, DecisionPromise<vfs::HostKeyDecision> promise) {
    std::shared_ptr<vfs::SftpPromptProvider> prompts;
    {
        std::lock_guard lock(mutex_);
        prompts = sftp_;
    }
    if (prompts) {
        prompts->requestHostKey(std::move(prompt), std::move(promise));
    }
}

void PromptRelay::requestCredential(vfs::CredentialPrompt prompt,
                                    DecisionPromise<std::optional<std::string>> promise) {
    std::shared_ptr<vfs::SftpPromptProvider> prompts;
    {
        std::lock_guard lock(mutex_);
        prompts = sftp_;
    }
    if (prompts) {
        prompts->requestCredential(std::move(prompt), std::move(promise));
    }
}

void PromptRelay::requestSecurityKey(vfs::SecurityKeyPrompt prompt,
                                     std::shared_ptr<vfs::SecurityKeySession> session) {
    std::shared_ptr<vfs::SftpPromptProvider> prompts;
    {
        std::lock_guard lock(mutex_);
        prompts = sftp_;
    }
    if (prompts) {
        prompts->requestSecurityKey(std::move(prompt), std::move(session));
    } else {
        session->cancel();
    }
}

}  // namespace filepan::runtime
