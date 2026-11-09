// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// An ssh-agent with one identity, inside the process, for keys libssh can't sign with itself:
// an OpenPGP card's authentication key. libssh talks to it over one end of a socket pair
// (ssh_set_agent_socket), and it answers on a thread of its own.
namespace filepan {

class InProcessAgent {
public:
    // The SSH signature blob over data, given the sign request's flags; nullopt refuses.
    using Sign = std::function<std::optional<std::vector<std::uint8_t>>(
        std::span<const std::uint8_t> data, std::uint32_t flags)>;

    static constexpr std::uint8_t kFailure = 5;
    static constexpr std::uint8_t kRequestIdentities = 11;
    static constexpr std::uint8_t kIdentitiesAnswer = 12;
    static constexpr std::uint8_t kSignRequest = 13;
    static constexpr std::uint8_t kSignResponse = 14;
    // Longer than any request libssh makes; a longer one hangs up.
    static constexpr std::size_t kMaxMessage = 256 * 1024;

    // publicKey is the identity's SSH public key blob. Null if no socket pair could be made.
    static std::unique_ptr<InProcessAgent> start(std::vector<std::uint8_t> publicKey,
                                                 std::string comment, Sign sign);
    // Hangs up, and waits for a request in progress.
    ~InProcessAgent();
    InProcessAgent(const InProcessAgent &) = delete;
    InProcessAgent &operator=(const InProcessAgent &) = delete;

    // The client's end, for ssh_set_agent_socket, which then owns it; -1 once taken.
    int takeClientSocket();

    // The answer to one request, both without their length.
    static std::vector<std::uint8_t> answer(std::span<const std::uint8_t> request,
                                            std::span<const std::uint8_t> publicKey,
                                            std::string_view comment, const Sign &sign);

private:
    InProcessAgent(int server, int client, std::vector<std::uint8_t> publicKey,
                   std::string comment, Sign sign);

    void serve();
    void serveRequests();

    int server_;
    int client_;
    std::vector<std::uint8_t> publicKey_;
    std::string comment_;
    Sign sign_;
    std::thread thread_;
};

}  // namespace filepan
