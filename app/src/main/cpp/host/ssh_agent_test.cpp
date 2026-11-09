// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// The in-process ssh-agent: what it answers, and how it frames it over its socket.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include "filepan/securitykey/ssh_wire.hpp"
#include "filepan/ssh_agent.hpp"
#include "test_util.hpp"

namespace {

using filepan::InProcessAgent;
using Bytes = std::vector<std::uint8_t>;
namespace ssh_wire = filepan::securitykey::ssh_wire;

const Bytes kKey = {0, 0, 0, 3, 'k', 'e', 'y', 1, 2, 3};

std::optional<Bytes> signer(std::span<const std::uint8_t> data, std::uint32_t flags) {
    if (data.empty()) {
        return std::nullopt;
    }
    Bytes signature(data.begin(), data.end());
    signature.push_back(static_cast<std::uint8_t>(flags));
    return signature;
}

Bytes signRequest(const Bytes &key, const Bytes &data, std::uint32_t flags) {
    Bytes request{InProcessAgent::kSignRequest};
    ssh_wire::appendString(request, key);
    ssh_wire::appendString(request, data);
    ssh_wire::appendU32(request, flags);
    return request;
}

void testAnswers() {
    const InProcessAgent::Sign sign = signer;
    const Bytes identities = InProcessAgent::answer(Bytes{InProcessAgent::kRequestIdentities}, kKey,
                                                    "cardno:1", sign);
    ssh_wire::Reader reader(identities);
    EXPECT(reader.u8() == InProcessAgent::kIdentitiesAnswer);
    EXPECT(reader.u32() == 1u);
    const auto key = reader.string();
    EXPECT(key && Bytes(key->begin(), key->end()) == kKey);
    const auto comment = reader.string();
    EXPECT(comment && std::string(comment->begin(), comment->end()) == "cardno:1");
    EXPECT(reader.empty());

    const Bytes signed_ = InProcessAgent::answer(signRequest(kKey, {7, 8}, 4), kKey, "", sign);
    ssh_wire::Reader signedReader(signed_);
    EXPECT(signedReader.u8() == InProcessAgent::kSignResponse);
    const auto signature = signedReader.string();
    EXPECT(signature && Bytes(signature->begin(), signature->end()) == Bytes({7, 8, 4}));

    const Bytes failure{InProcessAgent::kFailure};
    // Another key, a refusal, a malformed request and one it doesn't know.
    EXPECT(InProcessAgent::answer(signRequest({1}, {7}, 0), kKey, "", sign) == failure);
    EXPECT(InProcessAgent::answer(signRequest(kKey, {}, 0), kKey, "", sign) == failure);
    EXPECT(InProcessAgent::answer(Bytes{InProcessAgent::kSignRequest, 0, 0}, kKey, "", sign) == failure);
    EXPECT(InProcessAgent::answer(Bytes{17}, kKey, "", sign) == failure);
    EXPECT(InProcessAgent::answer(Bytes{}, kKey, "", sign) == failure);
    EXPECT(InProcessAgent::answer(signRequest(kKey, {7}, 0), kKey, "", nullptr) == failure);
}

Bytes roundTrip(int fd, const Bytes &request) {
    Bytes message;
    ssh_wire::appendString(message, request);
    EXPECT(::send(fd, message.data(), message.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(message.size()));
    std::uint8_t header[4];
    EXPECT(::recv(fd, header, 4, MSG_WAITALL) == 4);
    const std::size_t size = (std::size_t{header[0]} << 24) | (std::size_t{header[1]} << 16)
                             | (std::size_t{header[2]} << 8) | header[3];
    Bytes reply(size);
    EXPECT(::recv(fd, reply.data(), size, MSG_WAITALL) == static_cast<ssize_t>(size));
    return reply;
}

void testServesOverItsSocket() {
    auto agent = InProcessAgent::start(kKey, "", signer);
    EXPECT(agent != nullptr);
    const int fd = agent->takeClientSocket();
    EXPECT(fd >= 0 && agent->takeClientSocket() == -1);
    EXPECT(roundTrip(fd, Bytes{InProcessAgent::kRequestIdentities})[0] == InProcessAgent::kIdentitiesAnswer);
    EXPECT(roundTrip(fd, signRequest(kKey, {9}, 2))[0] == InProcessAgent::kSignResponse);
    EXPECT(roundTrip(fd, Bytes{99}) == Bytes{InProcessAgent::kFailure});

    // A message longer than any libssh sends: it hangs up.
    const std::uint8_t oversized[] = {0x01, 0x00, 0x00, 0x00};
    EXPECT(::send(fd, oversized, sizeof oversized, MSG_NOSIGNAL) == 4);
    std::uint8_t byte;
    EXPECT(::recv(fd, &byte, 1, 0) == 0);
    ::close(fd);
}

void testHangsUpWhenDestroyed() {
    int fd;
    {
        auto agent = InProcessAgent::start(kKey, "", signer);
        fd = agent->takeClientSocket();
    }
    std::uint8_t byte;
    EXPECT(::recv(fd, &byte, 1, 0) == 0);
    ::close(fd);
    // Never taken: the agent closes both ends.
    auto agent = InProcessAgent::start(kKey, "", signer);
    agent.reset();
}

}  // namespace

int main() {
    testAnswers();
    testServesOverItsSocket();
    testHangsUpWhenDestroyed();
    std::puts("ssh_agent_test passed");
    return 0;
}
