// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// A throwaway OpenSSH sshd on localhost for the SFTP tests. Needs FILEPAN_SSHD and
// FILEPAN_SSH_KEYGEN defined to the programs' paths.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "filepan/sftp_client.hpp"
#include "test_util.hpp"

namespace filepan::test {

inline std::string read(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

inline std::string run(const std::string &command) {
    std::string output;
    FILE *pipe = popen(command.c_str(), "r");
    EXPECT(pipe != nullptr);
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }
    EXPECT(pclose(pipe) == 0);
    return output;
}

inline int freePort() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length);
    close(fd);
    return ntohs(address.sin_port);
}

// Generates a host key and client keys (client_key, and locked_key with the passphrase
// "secret phrase") in dir, and runs sshd with public key login for the current user.
class Sshd {
public:
    // extraConfig: more sshd_config lines, such as algorithm restrictions. sftp: the command for
    // the sftp subsystem.
    explicit Sshd(const std::filesystem::path &dir, const std::string &extraConfig = "",
                  const std::string &sftp = "internal-sftp")
        : dir_(dir), port_(freePort()) {
        run(std::string(FILEPAN_SSH_KEYGEN) + " -q -t ed25519 -N '' -f " + (dir / "host_key").string());
        run(std::string(FILEPAN_SSH_KEYGEN) + " -q -t ed25519 -N '' -C client -f "
            + (dir / "client_key").string());
        run(std::string(FILEPAN_SSH_KEYGEN) + " -q -t ed25519 -N 'secret phrase' -C locked -f "
            + (dir / "locked_key").string());
        std::ofstream(dir / "authorized_keys")
            << read(dir / "client_key.pub") << read(dir / "locked_key.pub");
        std::ofstream(dir / "sshd_config")
            << "ListenAddress 127.0.0.1\n"
            << "Port " << port_ << "\n"
            << "HostKey " << (dir / "host_key").string() << "\n"
            << "AuthorizedKeysFile " << (dir / "authorized_keys").string() << "\n"
            << "PidFile " << (dir / "sshd.pid").string() << "\n"
            << "StrictModes no\nUsePAM no\nPermitRootLogin yes\nPasswordAuthentication no\n"
            << "KbdInteractiveAuthentication no\nAllowTcpForwarding yes\n"
            // Tests fail logins on purpose; sshd mustn't then shut out 127.0.0.1.
            << "PerSourcePenalties no\n"
            << "Subsystem sftp " << sftp << "\nLogLevel ERROR\n" << extraConfig;
        pid_ = fork();
        if (pid_ == 0) {
            // Don't outlive a test that fails and exits without running destructors: sshd
            // would keep the test's output pipe open and hang whoever reads it.
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            execl(FILEPAN_SSHD, FILEPAN_SSHD, "-D", "-e", "-f", (dir / "sshd_config").c_str(),
                  nullptr);
            _exit(127);
        }
        // Wait until it accepts connections.
        for (int i = 0; i < 100; ++i) {
            const int fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<std::uint16_t>(port_));
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            const bool connected =
                ::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
            close(fd);
            if (connected) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::fputs("sshd didn't start\n", stderr);
        std::exit(1);
    }

    ~Sshd() {
        kill(pid_, SIGTERM);
        waitpid(pid_, nullptr, 0);
    }

    int port() const { return port_; }
    pid_t pid() const { return pid_; }

    std::string hostFingerprint() const {
        // "256 SHA256:... comment (ED25519)"
        std::istringstream in(run(std::string(FILEPAN_SSH_KEYGEN) + " -l -f "
                                  + (dir_ / "host_key.pub").string()));
        std::string bits, fingerprint;
        in >> bits >> fingerprint;
        return fingerprint;
    }

    SshHost host(const std::string &key = "client_key", const std::string &passphrase = "") const {
        SshHost host;
        host.host = "127.0.0.1";
        host.port = port_;
        host.username = getenv("USER") ? getenv("USER") : "root";
        host.authentication.type = SshAuthentication::Type::kPrivateKey;
        host.authentication.privateKey = read(dir_ / key);
        host.authentication.passphrase = passphrase;
        return host;
    }

    // Kills the per-connection sshd processes, dropping every open connection while the server
    // keeps accepting new ones.
    void dropConnections() const {
        // Their own children too: after login, a child of each runs the session and holds the
        // socket, and outlives its parent.
        std::vector<pid_t> doomed = children();
        for (std::size_t i = 0; i < doomed.size(); ++i) {
            for (const pid_t child : childrenOf(doomed[i])) {
                doomed.push_back(child);
            }
        }
        for (const pid_t pid : doomed) {
            kill(pid, SIGKILL);
        }
    }

    // The connections open now: sshd runs a process for each.
    std::size_t connectionCount() const { return children().size(); }

    // Waits up to 10 s for connectionCount() to be count; returns whether it got there.
    bool awaitConnectionCount(std::size_t count) const {
        for (int i = 0; i < 200; ++i) {
            if (connectionCount() == count) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::fprintf(stderr, "sshd has %zu connections, not %zu\n", connectionCount(), count);
        return false;
    }

private:
    std::vector<pid_t> children() const { return childrenOf(pid_); }

    static std::vector<pid_t> childrenOf(pid_t pid) {
        std::vector<pid_t> children;
        for (const auto &item : std::filesystem::directory_iterator("/proc")) {
            const std::string name = item.path().filename().string();
            if (name.find_first_not_of("0123456789") != std::string::npos) {
                continue;
            }
            // "pid (comm) state ppid ..."; comm may contain spaces, so parse after the ')'.
            const std::string stat = read(item.path() / "stat");
            const auto close = stat.rfind(')');
            if (close == std::string::npos) {
                continue;
            }
            std::istringstream fields(stat.substr(close + 1));
            std::string state;
            int parent = 0;
            fields >> state >> parent;
            // A zombie has gone already; only its parent hasn't noticed yet.
            if (parent == pid && state != "Z") {
                children.push_back(std::stoi(name));
            }
        }
        return children;
    }

    std::filesystem::path dir_;
    int port_;
    pid_t pid_ = -1;
};

}  // namespace filepan::test
