// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/ssh_agent.hpp"

#include <algorithm>
#include <cerrno>

#include <sys/socket.h>
#include <unistd.h>

#include "filepan/securitykey/ssh_wire.hpp"

namespace filepan {

namespace {

namespace ssh_wire = securitykey::ssh_wire;

bool readFully(int fd, std::uint8_t *data, std::size_t size) {
    while (size > 0) {
        const ssize_t got = ::recv(fd, data, size, 0);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return false;
        }
        data += got;
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

bool writeFully(int fd, const std::uint8_t *data, std::size_t size) {
    while (size > 0) {
        // Without SIGPIPE if libssh has hung up.
        const ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

}  // namespace

std::unique_ptr<InProcessAgent> InProcessAgent::start(std::vector<std::uint8_t> publicKey,
                                                      std::string comment, Sign sign) {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
        return nullptr;
    }
    return std::unique_ptr<InProcessAgent>(
        new InProcessAgent(fds[0], fds[1], std::move(publicKey), std::move(comment), std::move(sign)));
}

InProcessAgent::InProcessAgent(int server, int client, std::vector<std::uint8_t> publicKey,
                               std::string comment, Sign sign)
    : server_(server), client_(client), publicKey_(std::move(publicKey)),
      comment_(std::move(comment)), sign_(std::move(sign)), thread_([this] { serve(); }) {}

InProcessAgent::~InProcessAgent() {
    ::shutdown(server_, SHUT_RDWR);
    thread_.join();
    ::close(server_);
    if (client_ >= 0) {
        ::close(client_);
    }
}

int InProcessAgent::takeClientSocket() {
    return std::exchange(client_, -1);
}

void InProcessAgent::serve() {
    serveRequests();
    // Hangs up, so that libssh sees the end rather than waiting for an answer.
    ::shutdown(server_, SHUT_RDWR);
}

void InProcessAgent::serveRequests() {
    while (true) {
        std::uint8_t header[4];
        if (!readFully(server_, header, sizeof header)) {
            return;
        }
        const std::size_t size = (std::size_t{header[0]} << 24) | (std::size_t{header[1]} << 16)
                                 | (std::size_t{header[2]} << 8) | header[3];
        if (size == 0 || size > kMaxMessage) {
            return;
        }
        std::vector<std::uint8_t> request(size);
        if (!readFully(server_, request.data(), size)) {
            return;
        }
        const auto reply = answer(request, publicKey_, comment_, sign_);
        std::vector<std::uint8_t> message;
        ssh_wire::appendString(message, reply);
        if (!writeFully(server_, message.data(), message.size())) {
            return;
        }
    }
}

std::vector<std::uint8_t> InProcessAgent::answer(std::span<const std::uint8_t> request,
                                                 std::span<const std::uint8_t> publicKey,
                                                 std::string_view comment, const Sign &sign) {
    const std::vector<std::uint8_t> failure{kFailure};
    ssh_wire::Reader reader(request);
    const auto type = reader.u8();
    if (type == kRequestIdentities) {
        std::vector<std::uint8_t> reply{kIdentitiesAnswer};
        ssh_wire::appendU32(reply, 1);
        ssh_wire::appendString(reply, publicKey);
        ssh_wire::appendString(reply, comment);
        return reply;
    }
    if (type != kSignRequest) {
        return failure;
    }
    const auto key = reader.string();
    const auto data = reader.string();
    const auto flags = reader.u32();
    if (!key || !data || !flags || !std::ranges::equal(*key, publicKey) || !sign) {
        return failure;
    }
    const auto signature = sign(*data, *flags);
    if (!signature) {
        return failure;
    }
    std::vector<std::uint8_t> reply{kSignResponse};
    ssh_wire::appendString(reply, *signature);
    return reply;
}

}  // namespace filepan
