// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "filepan/decision_promise.hpp"
#include "filepan/vfs/backend.hpp"
#include "filepan/vfs/security_key.hpp"

namespace filepan {
class SftpClient;
struct SftpError;
}  // namespace filepan

namespace filepan::vfs {

struct SftpAuthentication {
    enum class Type : std::uint8_t { kPassword, kPrivateKey, kOpenPgpCard };

    Type type = Type::kPassword;
    // Empty: asked for through the SftpPromptProvider when connecting.
    std::string password;
    // The private key itself, as OpenSSH or PEM text (see filepan::parsePrivateKey), not a path
    // to it. A FIDO security key's "sk-" key signs with the key the user presents, through the
    // SftpBackendOptions' SecurityKeySource.
    std::string privateKey;
    // For an encrypted key; empty: asked for if the key turns out to need one.
    std::string passphrase;
    // For kOpenPgpCard: the card's authentication key as an OpenSSH public key line (an
    // authorized_keys one), from SftpBackend::readCardKey(). It signs with the card the user
    // presents, through the SftpBackendOptions' SecurityKeySource.
    std::string cardPublicKey;
};

struct SftpEndpoint {
    std::string host;
    int port = 22;
    std::string username;
    SftpAuthentication authentication;
};

// One server in the app's server list; Location::sftp() names it by id.
struct SftpServerConfig {
    std::string id;
    // What the app calls it; the backend doesn't use it.
    std::string name;
    SftpEndpoint server;
    // Hosts to connect through, in order, before the server (as OpenSSH's ProxyJump).
    std::vector<SftpEndpoint> jumpHosts;
    // Where trusted host keys are kept, in OpenSSH's known_hosts format. Empty keeps them for the
    // session only.
    std::string knownHostsPath;
    int timeoutSeconds = 15;
};

struct HostKeyPrompt {
    enum class Status : std::uint8_t {
        kUnknown,  // First connection: trust on first use.
        kChanged,  // A different key than the one trusted: possibly an attack. Warn hard.
    };

    std::string serverId;
    // The host's position in the connection chain; the last one is the server itself.
    std::size_t hop = 0;
    bool isJumpHost = false;
    std::string host;
    int port = 22;
    Status status = Status::kUnknown;
    std::string keyType;
    std::string fingerprint;  // "SHA256:..."
    // For kChanged: the fingerprint of the key trusted until now.
    std::string knownFingerprint;
};

enum class HostKeyDecision : std::uint8_t {
    kReject,
    kAcceptOnce,      // For this connection only.
    kAcceptAndSave,   // Saved to the known hosts, replacing a changed key.
};

struct CredentialPrompt {
    enum class Kind : std::uint8_t { kPassword, kPassphrase };

    std::string serverId;
    // As in HostKeyPrompt.
    std::size_t hop = 0;
    bool isJumpHost = false;
    std::string host;
    int port = 22;
    std::string username;
    Kind kind = Kind::kPassword;
    // The previous answer was wrong.
    bool retry = false;
};

// Asks the user while connecting. Called on an I/O thread, which then blocks until the promise
// is fulfilled: post the request to the UI thread and return. Dropping a promise unfulfilled
// rejects the key or gives up on the login.
class SftpPromptProvider {
public:
    virtual ~SftpPromptProvider() = default;
    virtual void requestHostKey(HostKeyPrompt prompt, DecisionPromise<HostKeyDecision> promise) = 0;
    // Fulfil with the password or passphrase, or nullopt to give up.
    virtual void requestCredential(CredentialPrompt prompt,
                                   DecisionPromise<std::optional<std::string>> promise) = 0;
    // Shows how signing with a security key goes, following the session's state until it's
    // kDone, and asks for its PIN. The default cancels, giving up.
    virtual void requestSecurityKey(SecurityKeyPrompt prompt,
                                    std::shared_ptr<SecurityKeySession> session) {
        (void)prompt;
        session->cancel();
    }
};

// An OpenPGP card's authentication key, as SftpBackend::readCardKey() reads it.
struct CardKey {
    // For SftpAuthentication::cardPublicKey, and the server's authorized_keys: commented as gpg
    // does, "cardno:<serial>".
    std::string publicKey;
    // "ssh-ed25519", "ecdsa-sha2-nistp256", "ssh-rsa", ...
    std::string type;
    // "SHA256:...".
    std::string fingerprint;
};

struct SftpBackendOptions {
    // Logins attempted per connection before giving up with EACCES.
    int maxCredentialAttempts = 5;
    // Where "sk-" keys and OpenPGP card logins find the key to sign with; null: they can't log in.
    std::shared_ptr<SecurityKeySource> securityKeys;
};

// A server refused a connection beyond those it already had (sshd's MaxStartups or MaxSessions, a
// firewall's rate limit), so reads alongside others share the ones that work from then on.
struct PoolLimit {
    std::string serverId;
    // Connections to the server that work, the main one included.
    std::size_t working = 0;
    // Why the next one failed, as the error message.
    std::string reason;
};

// Files on SFTP servers (BackendKind::kSftp): see the design note in backend.hpp. Each server
// has one main connection, made on first use and made again when it drops; a call that fails
// because the connection was lost is retried once on a new one. Open streams reopen their file
// on the new connection and carry on at their offset. Streams from openReadParallel() use a pool
// of extra connections instead, made with the credentials and host keys the main one was given,
// without prompting; idle ones close after a while. Streams must not outlive the backend.
class SftpBackend final : public Backend {
public:
    // prompts may be null: unknown host keys are then rejected and missing credentials fail.
    explicit SftpBackend(std::shared_ptr<SftpPromptProvider> prompts,
                         SftpBackendOptions options = {});
    ~SftpBackend() override;

    // Adds or replaces a server, dropping its connection and the credentials entered for it.
    void setServer(SftpServerConfig config);
    void removeServer(const std::string &id);
    // Closes the server's connection; the next call connects again.
    void disconnect(const std::string &id);
    // Connects now, prompting as needed, e.g. to test a server's settings.
    Result<void> connect(const std::string &id);
    // The login directory, to start browsing from.
    Result<std::string> homeDirectory(const std::string &id);

    // Reads the authentication key of the OpenPGP card the user presents, prompting through the
    // SftpPromptProvider as logging in with it would, for the server editor to save. Blocks.
    Result<CardKey> readCardKey(const SecurityKeyPrompt &prompt);

    // The most extra connections openReadParallel() makes per server, clamped to 1-6 (3).
    void setParallelConnections(std::size_t count);
    [[nodiscard]] std::size_t parallelConnections() const { return parallelConnections_; }
    // How long an extra connection with no open streams stays open (60 s). Idle ones are closed
    // on the next call to the server rather than on a timer.
    void setIdleTimeout(std::chrono::steady_clock::duration timeout);
    // Called on the I/O thread when a server first refuses an extra connection: once per server
    // until disconnect() or setServer(). Not if the main connection has gone too (the network
    // or the server is down): the stream fails then.
    void setOnPoolLimited(std::function<void(const PoolLimit &)> callback);
    // The extra connections open to the server now.
    [[nodiscard]] std::size_t parallelConnectionCount(const std::string &id);

    [[nodiscard]] BackendKind kind() const override { return BackendKind::kSftp; }
    [[nodiscard]] Capabilities capabilities() const override;

    Result<void> list(const Location &directory, const ListOptions &options,
                      const ListCallback &onBatch, const CancellationToken &cancel) override;
    Result<Entry> stat(const Location &location) override;
    Result<Entry> lstat(const Location &location) override;
    Result<void> makeDirectory(const Location &location, std::uint32_t mode = 0777) override;
    Result<void> createFile(const Location &location, std::uint32_t mode = 0666) override;
    // EXDEV between servers.
    Result<void> rename(const Location &source, const Location &target, bool replace) override;
    Result<RemoveStats> remove(const Location &location, bool recursive,
                               RemoveCallbacks *callbacks,
                               const CancellationToken &cancel) override;
    Result<std::unique_ptr<InputStream>> openRead(const Location &location) override;
    // On the least busy extra connection, or a new one while there are fewer than
    // setParallelConnections() allows. Connects the main connection first if needed, prompting
    // as openRead() would. If the server refuses an extra connection, the stream shares one that
    // works, and setOnPoolLimited() hears of it.
    Result<std::unique_ptr<InputStream>> openReadParallel(const Location &location) override;
    Result<std::unique_ptr<OutputStream>> openWrite(const Location &location,
                                                    const OpenWriteOptions &options) override;
    Result<std::string> readLink(const Location &location) override;
    Result<void> createSymlink(const Location &link, std::string_view target) override;
    // SFTP v3 sets times on a symlink's target, and in whole seconds.
    Result<void> setTimes(const Location &location, std::optional<Timestamp> accessTime,
                          std::optional<Timestamp> modifiedTime) override;
    Result<void> chmod(const Location &location, std::uint32_t mode) override;

    struct Server;

private:
    class InputStreamImpl;
    class OutputStreamImpl;
    friend class InputStreamImpl;
    friend class OutputStreamImpl;

    Result<std::shared_ptr<Server>> server(const std::string &id);
    Result<std::shared_ptr<SftpClient>> client(Server &server);
    // interactive false: fails instead of asking the user anything.
    Result<std::shared_ptr<SftpClient>> connectServer(Server &server, bool interactive = true);
    // An extra connection for a stream, counted as in use by it until releaseParallel().
    Result<std::shared_ptr<SftpClient>> parallelClient(Server &server);
    void releaseParallel(Server &server, const std::shared_ptr<SftpClient> &client);
    // Takes the extra connections idle for too long, or beyond setParallelConnections(), out of
    // the pool into closing, which the caller destroys (disconnecting them) after unlocking.
    void reapParallel(Server &server, std::vector<std::shared_ptr<SftpClient>> &closing);
    // Forgets a lost connection, main or extra.
    void dropClient(Server &server, const std::shared_ptr<SftpClient> &client);
    void dropConnections(Server &server);
    Result<std::unique_ptr<InputStream>> openStream(const Location &location, bool parallel);
    // Runs call on the server's connection, connecting first if needed, and once more on a new
    // connection if the connection was lost.
    template <typename F>
    Result<void> call(const std::string &serverId, F &&call);
    Result<void> removeTree(const Location &location, RemoveCallbacks *callbacks,
                            const CancellationToken &cancel, RemoveStats &stats,
                            std::uint64_t &reported);

    std::shared_ptr<SftpPromptProvider> prompts_;
    SftpBackendOptions options_;
    // PINs of OpenPGP cards that logged in, for a while.
    CardPinCache cardPins_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Server>> servers_;
    std::function<void(const PoolLimit &)> onPoolLimited_;
    std::atomic<std::size_t> parallelConnections_ = 3;
    std::atomic<std::chrono::steady_clock::rep> idleTimeout_ =
        std::chrono::steady_clock::duration(std::chrono::seconds(60)).count();
};

// Maps an SftpClient error to an errno-based Error, keeping the server's message.
Error toError(const SftpError &error, std::string_view context = {});

}  // namespace filepan::vfs
