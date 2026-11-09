// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/sftp_client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <libssh/sk_api.h>

#include "filepan/base64.hpp"
#include "filepan/ssh_agent.hpp"

namespace filepan {

namespace {

// Requests kept in flight per transfer; enough to fill a high-latency link.
constexpr std::size_t kMaxRequestsInFlight = 64;

// Prefer AES-GCM: phones have AES instructions, and it's what OpenSSH negotiates by default on
// such hardware too.
constexpr const char *kCiphers =
    "aes128-gcm@openssh.com,aes256-gcm@openssh.com,chacha20-poly1305@openssh.com,"
    "aes128-ctr,aes256-ctr";

struct LibsshInitializer {
    LibsshInitializer() { ssh_init(); }
};

void ensureLibsshInitialized() { static LibsshInitializer initializer; }

std::string sessionError(ssh_session session) {
    const char *message = ssh_get_error(session);
    return message ? message : "";
}

// Asks the verifier about the session's host key.
bool verifyHostKey(ssh_session session, std::size_t hop, const SshHost &host,
                   SshHostKeyVerifier &verifier, std::string &error) {
    ssh_key key = nullptr;
    if (ssh_get_server_publickey(session, &key) != SSH_OK) {
        error = sessionError(session);
        return false;
    }
    unsigned char *hash = nullptr;
    size_t hashLength = 0;
    char *fingerprint = nullptr;
    char *publicKey = nullptr;
    bool accepted = false;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLength) == SSH_OK
        && (fingerprint = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLength))
        && ssh_pki_export_pubkey_base64(key, &publicKey) == SSH_OK) {
        const char *keyType = ssh_key_type_to_char(ssh_key_type(key));
        accepted = verifier.verify(hop, host, keyType ? keyType : "", fingerprint, publicKey);
        if (!accepted) {
            error = "host key " + std::string(fingerprint) + " not trusted";
        }
    } else {
        error = "couldn't read its host key";
    }
    ssh_string_free_char(fingerprint);
    ssh_string_free_char(publicKey);
    ssh_clean_pubkey_hash(&hash);
    ssh_key_free(key);
    return accepted;
}

// Answers keyboard-interactive prompts with the password, for servers that only offer password
// login that way.
int authenticateKeyboardInteractive(ssh_session session, const std::string &password) {
    int rc = ssh_userauth_kbdint(session, nullptr, nullptr);
    while (rc == SSH_AUTH_INFO) {
        const int prompts = ssh_userauth_kbdint_getnprompts(session);
        for (int i = 0; i < prompts; ++i) {
            char echo = 0;
            ssh_userauth_kbdint_getprompt(session, static_cast<unsigned>(i), &echo);
            if (ssh_userauth_kbdint_setanswer(session, static_cast<unsigned>(i),
                                              echo ? "" : password.c_str())
                < 0) {
                return SSH_AUTH_ERROR;
            }
        }
        rc = ssh_userauth_kbdint(session, nullptr, nullptr);
    }
    return rc;
}

// libssh's security key callbacks take no user data, so the signer for the authentication in
// progress on this thread is kept here.
struct SecurityKeyContext {
    SecurityKeySigner *signer;
    std::size_t hop;
};
thread_local const SecurityKeyContext *currentSecurityKeyContext = nullptr;

// Reads a DER INTEGER as an unsigned big-endian number without leading zeros.
bool readDerInteger(const std::uint8_t *&p, const std::uint8_t *end, std::string &integer) {
    if (end - p < 2 || *p != 0x02) {
        return false;
    }
    const std::size_t length = p[1];
    p += 2;
    if (length == 0 || length >= 0x80 || static_cast<std::size_t>(end - p) < length) {
        return false;
    }
    const std::uint8_t *begin = p;
    p += length;
    while (begin < p - 1 && *begin == 0) {
        ++begin;
    }
    integer.assign(begin, p);
    return true;
}

// Splits a DER ECDSA-Sig-Value into r and s.
bool parseEcdsaSignature(const std::string &der, std::string &r, std::string &s) {
    const auto *p = reinterpret_cast<const std::uint8_t *>(der.data());
    const std::uint8_t *end = p + der.size();
    if (end - p < 2 || p[0] != 0x30 || p[1] >= 0x80
        || static_cast<std::size_t>(end - p - 2) != p[1]) {
        return false;
    }
    p += 2;
    return readDerInteger(p, end, r) && readDerInteger(p, end, s) && p == end;
}

std::uint8_t *copyToMalloc(const std::string &bytes) {
    auto *copy = static_cast<std::uint8_t *>(std::malloc(std::max<std::size_t>(bytes.size(), 1)));
    if (copy) {
        std::memcpy(copy, bytes.data(), bytes.size());
    }
    return copy;
}

int securityKeySign(uint32_t algorithm, const uint8_t *data, size_t dataLength,
                    const char *application, const uint8_t *keyHandle, size_t keyHandleLength,
                    uint8_t flags, const char *, struct sk_option **,
                    struct sk_sign_response **response) {
    *response = nullptr;
    const SecurityKeyContext *context = currentSecurityKeyContext;
    if (!context || !context->signer) {
        return SSH_SK_ERR_DEVICE_NOT_FOUND;
    }
    SecurityKeySignRequest request;
    request.hop = context->hop;
    request.algorithm = algorithm;
    request.data.assign(reinterpret_cast<const char *>(data), dataLength);
    request.application = application;
    request.keyHandle.assign(reinterpret_cast<const char *>(keyHandle), keyHandleLength);
    request.flags = flags;
    SecurityKeySignature signature;
    if (const int rc = context->signer->sign(request, signature); rc != 0) {
        return rc;
    }
    std::string r;
    std::string s;
    if (algorithm == SSH_SK_ECDSA) {
        if (!parseEcdsaSignature(signature.signature, r, s)) {
            return SSH_SK_ERR_GENERAL;
        }
    } else {
        r = signature.signature;
    }
    // libssh frees the response and its buffers with free().
    auto *result = static_cast<sk_sign_response *>(std::calloc(1, sizeof(sk_sign_response)));
    if (!result) {
        return SSH_SK_ERR_GENERAL;
    }
    result->flags = signature.flags;
    result->counter = signature.counter;
    result->sig_r = copyToMalloc(r);
    result->sig_r_len = r.size();
    if (!s.empty()) {
        result->sig_s = copyToMalloc(s);
        result->sig_s_len = s.size();
    }
    if (!result->sig_r || (!s.empty() && !result->sig_s)) {
        std::free(result->sig_r);
        std::free(result->sig_s);
        std::free(result);
        return SSH_SK_ERR_GENERAL;
    }
    *response = result;
    return SSH_OK;
}

const ssh_sk_callbacks_struct *securityKeyCallbacks() {
    static const ssh_sk_callbacks_struct callbacks = [] {
        ssh_sk_callbacks_struct callbacks{};
        ssh_callbacks_init(&callbacks);
        callbacks.api_version = []() -> uint32_t { return SSH_SK_VERSION_MAJOR; };
        callbacks.sign = securityKeySign;
        return callbacks;
    }();
    return &callbacks;
}

bool isSecurityKey(ssh_key key) {
    const auto type = ssh_key_type(key);
    return type == SSH_KEYTYPE_SK_ECDSA || type == SSH_KEYTYPE_SK_ED25519;
}

// Makes the session sign with security keys through our callbacks.
bool useSecurityKeyCallbacks(ssh_session session) {
    ssh_pki_ctx context = ssh_pki_ctx_new();
    if (!context) {
        return false;
    }
    const bool set =
        ssh_pki_ctx_options_set(context, SSH_PKI_OPTION_SK_CALLBACKS, securityKeyCallbacks())
            == SSH_OK
        && ssh_options_set(session, SSH_OPTIONS_PKI_CONTEXT, context) == SSH_OK;
    ssh_pki_ctx_free(context);
    return set;
}

// On failure, code is SftpError::kAuthenticationFailed, or kLoginMethodRefused when the host
// doesn't take this kind of login at all.
bool authenticate(ssh_session session, std::size_t hop, const SshHost &host,
                  SecurityKeySigner *signer, std::string &error, int &code) {
    code = SftpError::kAuthenticationFailed;
    const auto &authentication = host.authentication;
    int rc;
    if (authentication.type == SshAuthentication::Type::kPrivateKey) {
        ssh_key key = nullptr;
        if (ssh_pki_import_privkey_base64(
                authentication.privateKey.c_str(),
                authentication.passphrase.empty() ? nullptr : authentication.passphrase.c_str(),
                nullptr, nullptr, &key)
            != SSH_OK) {
            error = "the private key didn't open; check its passphrase";
            return false;
        }
        if (isSecurityKey(key)) {
            if (!useSecurityKeyCallbacks(session)) {
                ssh_key_free(key);
                error = "Couldn't set up the security key";
                return false;
            }
            // Signing happens on this thread, within ssh_userauth_publickey().
            const SecurityKeyContext context{signer, hop};
            currentSecurityKeyContext = &context;
            rc = ssh_userauth_publickey(session, nullptr, key);
            currentSecurityKeyContext = nullptr;
        } else {
            rc = ssh_userauth_publickey(session, nullptr, key);
        }
        ssh_key_free(key);
    } else if (authentication.type == SshAuthentication::Type::kOpenPgpCard) {
        auto key = parsePublicKey(authentication.cardPublicKey);
        if (!key) {
            error = "the OpenPGP card's key isn't set up; read it from the card again";
            return false;
        }
        const std::string blob(key->blob.begin(), key->blob.end());
        // libssh can't sign with a key it doesn't hold, except through an agent: signing happens
        // on the agent's thread, within ssh_userauth_agent().
        auto agent = InProcessAgent::start(
            key->blob, key->comment,
            [&](std::span<const std::uint8_t> data,
                std::uint32_t flags) -> std::optional<std::vector<std::uint8_t>> {
                const CardSignRequest request{.hop = hop,
                                              .publicKey = blob,
                                              .data = std::string(data.begin(), data.end()),
                                              .flags = flags};
                std::string signature;
                if (signer == nullptr || !signer->signWithCard(request, signature)) {
                    return std::nullopt;
                }
                return std::vector<std::uint8_t>(signature.begin(), signature.end());
            });
        const int socket = agent ? agent->takeClientSocket() : -1;
        if (socket < 0 || ssh_set_agent_socket(session, socket) != SSH_OK) {
            if (socket >= 0) {
                ::close(socket);
            }
            error = "Couldn't set up the OpenPGP card";
            return false;
        }
        rc = ssh_userauth_agent(session, nullptr);
        agent.reset();
    } else {
        // Learn the allowed methods first.
        rc = ssh_userauth_none(session, nullptr);
        if (rc == SSH_AUTH_SUCCESS) {
            return true;
        }
        const int methods = ssh_userauth_list(session, nullptr);
        if ((methods & (SSH_AUTH_METHOD_PASSWORD | SSH_AUTH_METHOD_INTERACTIVE)) == 0) {
            // Asking for the password again wouldn't help.
            code = SftpError::kLoginMethodRefused;
            error = (methods & SSH_AUTH_METHOD_PUBLICKEY)
                        ? "it doesn't take passwords, only keys; choose Private key for it"
                        : "it doesn't take passwords";
            return false;
        }
        rc = SSH_AUTH_DENIED;
        if (methods & SSH_AUTH_METHOD_PASSWORD) {
            rc = ssh_userauth_password(session, nullptr, authentication.password.c_str());
        }
        if (rc != SSH_AUTH_SUCCESS && (methods & SSH_AUTH_METHOD_INTERACTIVE)) {
            rc = authenticateKeyboardInteractive(session, authentication.password);
        }
    }
    if (rc != SSH_AUTH_SUCCESS) {
        error = sessionError(session);
        if (error.empty()) {
            error = "Authentication failed";
        }
        return false;
    }
    return true;
}

bool setCommonOptions(ssh_session session, const SshHost &host, int timeoutSeconds) {
    const long timeout = timeoutSeconds;
    const int noDelay = 1;
    const bool processConfig = false;
    const unsigned int port = static_cast<unsigned int>(host.port);
    return ssh_options_set(session, SSH_OPTIONS_HOST, host.host.c_str()) == SSH_OK
           && ssh_options_set(session, SSH_OPTIONS_PORT, &port) == SSH_OK
           && ssh_options_set(session, SSH_OPTIONS_USER, host.username.c_str()) == SSH_OK
           && ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout) == SSH_OK
           && ssh_options_set(session, SSH_OPTIONS_NODELAY, &noDelay) == SSH_OK
           && ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &processConfig) == SSH_OK
           && ssh_options_set(session, SSH_OPTIONS_CIPHERS_C_S, kCiphers) == SSH_OK
           && ssh_options_set(session, SSH_OPTIONS_CIPHERS_S_C, kCiphers) == SSH_OK;
}

void toAttributes(const sftp_attributes source, SftpAttributes &attributes) {
    attributes.flags = source->flags;
    attributes.type = source->type;
    attributes.size = source->size;
    attributes.uid = source->uid;
    attributes.gid = source->gid;
    attributes.permissions = source->permissions;
    attributes.atime = source->atime64 ? source->atime64 : source->atime;
    attributes.mtime = source->mtime64 ? source->mtime64 : source->mtime;
}


// Frees a session made by ssh_new(). Every session's socket is handed over with SSH_OPTIONS_FD,
// so none is sent a disconnect: ssh_disconnect() would forget that socket without closing it,
// while ssh_free() closes it (unless libssh already did, after an error).
void freeSession(ssh_session session) {
    if (session != nullptr) {
        ssh_free(session);
    }
}

// "jump host 2", for the hop's position in the chain.
std::string jumpName(std::size_t hop) { return "jump host " + std::to_string(hop + 1); }

std::string hostPort(const SshHost &host) {
    const bool v6 = host.host.find(':') != std::string::npos;
    return (v6 ? "[" + host.host + "]" : host.host) + ":" + std::to_string(host.port);
}

// A connected TCP socket to the host (blocking, close-on-exec, no Nagle), trying each address it
// resolves to within the timeout; -1 with why ("couldn't find ...", "couldn't connect: ...").
int connectTcp(const SshHost &host, int timeoutSeconds, std::string &why) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    addrinfo *addresses = nullptr;
    const std::string port = std::to_string(host.port);
    if (const int rc = ::getaddrinfo(host.host.c_str(), port.c_str(), &hints, &addresses); rc != 0) {
        why = "couldn't find " + host.host + ": "
              + (rc == EAI_SYSTEM ? std::string(std::strerror(errno)) : ::gai_strerror(rc));
        return -1;
    }
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(timeoutSeconds > 0 ? timeoutSeconds : 15);
    int error = ETIMEDOUT;
    int connected = -1;
    for (addrinfo *a = addresses; a != nullptr && connected < 0; a = a->ai_next) {
        const int fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                                a->ai_protocol);
        if (fd < 0) {
            error = errno;
            continue;
        }
        int rc = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            pollfd p{fd, POLLOUT, 0};
            rc = left.count() > 0 ? ::poll(&p, 1, static_cast<int>(left.count())) : 0;
            if (rc == 1) {
                socklen_t length = sizeof error;
                rc = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0 ? 0
                                                                                             : -1;
            } else {
                error = rc == 0 ? ETIMEDOUT : errno;
                rc = -1;
            }
        } else if (rc != 0) {
            error = errno;
        }
        if (rc != 0) {
            ::close(fd);
            continue;
        }
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        connected = fd;
    }
    ::freeaddrinfo(addresses);
    if (connected < 0) {
        why = "couldn't connect: " + std::string(std::strerror(error));
    }
    return connected;
}

// Why ssh_connect() failed on a connected socket: the version exchange or key exchange, said as
// such, and a key exchange with nothing in common with what the server offers.
std::string handshakeError(ssh_session session, std::size_t via) {
    std::string message = sessionError(session);
    if (message.empty()) {
        message = "the connection closed";
    }
    const std::string through = via > 0 ? " through " + jumpName(via - 1) : std::string();
    if (message.find("no match for method") != std::string::npos) {
        // libssh: "kex error : no match for method kex algos: server [...], client [...]".
        return "SSH handshake" + through + ": no algorithms in common: " + message;
    }
    return "SSH handshake" + through + " failed: " + message;
}

}  // namespace

// A tunnel through a jump host: a direct-tcpip channel to the next host, whose bytes go to and
// from one end of a socket pair on a thread of its own. The next host's session runs over the
// other end. It owns the jump host's session, which only its thread uses once it starts.
class SshTunnel {
public:
    // Opens the channel to host:port; on success, nextFd is the socket for the next session. On
    // failure, error says why and the session is freed.
    static std::unique_ptr<SshTunnel> open(ssh_session session,
                                           const SshHost &next, int &nextFd, std::string &error) {
        std::unique_ptr<SshTunnel> tunnel(new SshTunnel(session));
        tunnel->channel_ = ssh_channel_new(session);
        if (tunnel->channel_ == nullptr) {
            error = sessionError(session);
            return nullptr;
        }
        // The originator address is only for the server's logs, as OpenSSH sends it.
        if (ssh_channel_open_forward(tunnel->channel_, next.host.c_str(), next.port, "127.0.0.1",
                                     0)
            != SSH_OK) {
            error = sessionError(session);
            if (error.empty()) {
                error = "the channel was refused";
            }
            return nullptr;
        }
        int pair[2];
        if (::pipe2(tunnel->wake_, O_CLOEXEC) != 0
            || ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
            error = std::strerror(errno);
            return nullptr;
        }
        tunnel->fd_ = pair[0];
        nextFd = pair[1];
        tunnel->thread_ = std::thread([raw = tunnel.get()] { raw->run(); });
        return tunnel;
    }

    ~SshTunnel() {
        if (thread_.joinable()) {
            stop_.store(true);
            const char byte = 0;
            (void)!::write(wake_[1], &byte, 1);
            thread_.join();
        }
        if (channel_ != nullptr) {
            ssh_channel_free(channel_);
        }
        freeSession(session_);
        for (const int fd : {fd_, wake_[0], wake_[1]}) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }

    SshTunnel(const SshTunnel &) = delete;
    SshTunnel &operator=(const SshTunnel &) = delete;

private:
    explicit SshTunnel(ssh_session session) : session_(session) {}

    static int wake(socket_t, int, void *userdata) {
        auto *self = static_cast<SshTunnel *>(userdata);
        char buffer[16];
        (void)!::read(self->wake_[0], buffer, sizeof buffer);
        return 0;
    }

    void run() {
        ssh_event event = ssh_event_new();
        ssh_connector in = ssh_connector_new(session_);
        ssh_connector out = ssh_connector_new(session_);
        if (event != nullptr && in != nullptr && out != nullptr) {
            ssh_connector_set_in_fd(in, fd_);
            ssh_connector_set_out_channel(in, channel_, SSH_CONNECTOR_STDINOUT);
            ssh_connector_set_in_channel(out, channel_, SSH_CONNECTOR_STDINOUT);
            ssh_connector_set_out_fd(out, fd_);
            ssh_event_add_connector(event, in);
            ssh_event_add_connector(event, out);
            ssh_event_add_fd(event, wake_[0], POLLIN, wake, this);
            while (!stop_.load() && ssh_channel_is_open(channel_)) {
                if (ssh_event_dopoll(event, 60000) == SSH_ERROR) {
                    break;
                }
            }
            ssh_event_remove_fd(event, wake_[0]);
            ssh_event_remove_connector(event, in);
            ssh_event_remove_connector(event, out);
        }
        ssh_connector_free(in);
        ssh_connector_free(out);
        ssh_event_free(event);
        // The jump host's side is gone (or we're stopping): the next session sees the end.
        ::shutdown(fd_, SHUT_RDWR);
    }

    ssh_session session_;
    ssh_channel channel_ = nullptr;
    int fd_ = -1;
    int wake_[2] = {-1, -1};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

std::unique_ptr<SftpClient> SftpClient::connect(const std::vector<SshHost> &chain,
                                                SshHostKeyVerifier &verifier,
                                                SecurityKeySigner *signer, int timeoutSeconds,
                                                SftpError &error) {
    ensureLibsshInitialized();
    if (chain.empty()) {
        error = {SftpError::kConnectFailed, "No host to connect to", -1};
        return nullptr;
    }
    std::unique_ptr<SftpClient> client(new SftpClient());
    const std::size_t targetHop = chain.size() - 1;
    // Each host is connected, checked and logged in to in turn, on this thread, the next one
    // through a tunnel in the one before (as OpenSSH's ProxyJump does). So every step can say
    // which host failed, and each host's timeout only counts its own network waits, not the
    // time the user takes over a host key.
    // The socket to the next host: through the last tunnel, or (for the first) one we connect
    // ourselves, so that a failure to reach a host is told apart from a failed SSH handshake.
    int fd = -1;
    for (std::size_t hop = 0; hop <= targetHop; ++hop) {
        const SshHost &host = chain[hop];
        const int h = static_cast<int>(hop);
        const bool tunneled = hop > 0;
        if (!tunneled) {
            std::string why;
            fd = connectTcp(host, timeoutSeconds, why);
            if (fd < 0) {
                error = {SftpError::kConnectFailed, why, h};
                return nullptr;
            }
        }
        ssh_session session = ssh_new();
        if (session == nullptr) {
            ::close(fd);
            error = {SftpError::kConnectFailed, "Out of memory", h};
            return nullptr;
        }
        bool owned = true;
        struct Guard {
            ssh_session session;
            bool *owned;
            ~Guard() {
                if (*owned) {
                    freeSession(session);
                }
            }
        } guard{session, &owned};
        if (ssh_options_set(session, SSH_OPTIONS_FD, &fd) != SSH_OK) {
            ::close(fd);
            error = {SftpError::kConnectFailed, sessionError(session), h};
            return nullptr;
        }
        fd = -1;  // The session's now.
        if (!setCommonOptions(session, host, timeoutSeconds)) {
            error = {SftpError::kConnectFailed, "couldn't set up: " + sessionError(session), h};
            return nullptr;
        }
        if (ssh_connect(session) != SSH_OK) {
            error = {SftpError::kConnectFailed, handshakeError(session, tunneled ? hop : 0), h};
            return nullptr;
        }
        std::string message;
        if (!verifyHostKey(session, hop, host, verifier, message)) {
            error = {SftpError::kHostKeyRejected, message, h};
            return nullptr;
        }
        int code = SftpError::kAuthenticationFailed;
        if (!authenticate(session, hop, host, signer, message, code)) {
            error = {code, "couldn't log in: " + message, h};
            return nullptr;
        }
        if (hop == targetHop) {
            owned = false;
            client->session_ = session;
            break;
        }
        owned = false;  // The tunnel's, even if it fails.
        const SshHost &next = chain[hop + 1];
        auto tunnel = SshTunnel::open(session, next, fd, message);
        if (!tunnel) {
            error = {SftpError::kConnectFailed,
                     "couldn't reach " + hostPort(next) + " through " + jumpName(hop) + ": "
                         + message,
                     h + 1};
            return nullptr;
        }
        client->tunnels_.push_back(std::move(tunnel));
    }

    ssh_session session = client->session_;
    client->sftp_ = sftp_new(session);
    if (!client->sftp_ || sftp_init(client->sftp_) != SSH_OK) {
        error = {SftpError::kConnectFailed, "couldn't start SFTP: " + sessionError(session),
                 static_cast<int>(targetHop)};
        return nullptr;
    }
    if (sftp_limits_t limits = sftp_limits(client->sftp_)) {
        client->maxReadLength_ = std::max<std::size_t>(limits->max_read_length, 4096);
        client->maxWriteLength_ = std::max<std::size_t>(limits->max_write_length, 4096);
        sftp_limits_free(limits);
    }
    error = {};
    return client;
}

SftpClient::~SftpClient() {
    for (auto &[handle, file] : files_) {
        sftp_close(file);
    }
    if (sftp_) {
        sftp_free(sftp_);
    }
    freeSession(session_);
    // From the server back: each tunnel goes once the session through it has.
    while (!tunnels_.empty()) {
        tunnels_.pop_back();
    }
}

bool SftpClient::isConnected() {
    std::lock_guard lock(mutex_);
    return ssh_is_connected(session_);
}

bool SftpClient::sendKeepAlive() {
    std::lock_guard lock(mutex_);
    if (!ssh_is_connected(session_)) {
        return false;
    }
    // A round trip, unlike SSH_MSG_IGNORE, notices a dead connection (within the timeout).
    char *path = sftp_canonicalize_path(sftp_, ".");
    ssh_string_free_char(path);
    return path != nullptr;
}

SftpError SftpClient::lastError(const std::string &context) {
    if (!ssh_is_connected(session_)) {
        return {SftpError::kConnectionLost, context + ": " + sessionError(session_), -1};
    }
    const int status = sftp_get_error(sftp_);
    if (status == SSH_FX_OK) {
        // A transport-level failure, such as a timeout.
        return {SftpError::kConnectionLost, context + ": " + sessionError(session_), -1};
    }
    return {status, context, -1};
}

SftpError SftpClient::stat(const std::string &path, bool followLinks,
                           SftpAttributes &attributes) {
    std::lock_guard lock(mutex_);
    sftp_attributes result = followLinks ? sftp_stat(sftp_, path.c_str())
                                         : sftp_lstat(sftp_, path.c_str());
    if (!result) {
        return lastError(path);
    }
    toAttributes(result, attributes);
    sftp_attributes_free(result);
    return {};
}

void SftpClient::putAttributes(ByteWriter &out, const SftpAttributes &attributes) {
    out.put<std::uint32_t>(attributes.flags);
    out.put<std::uint8_t>(attributes.type);
    out.put<std::uint64_t>(attributes.size);
    out.put<std::uint32_t>(attributes.uid);
    out.put<std::uint32_t>(attributes.gid);
    out.put<std::uint32_t>(attributes.permissions);
    out.put<std::uint64_t>(attributes.atime);
    out.put<std::uint64_t>(attributes.mtime);
}

SftpError SftpClient::listDirectory(const std::string &path, ByteWriter &out) {
    std::lock_guard lock(mutex_);
    struct Entry {
        std::string name;
        SftpAttributes attributes;
        bool hasLinkTarget = false;
        std::string linkTarget;
        bool hasTargetAttributes = false;
        SftpAttributes targetAttributes;
    };
    std::vector<Entry> entries;
    sftp_dir dir = sftp_opendir(sftp_, path.c_str());
    if (!dir) {
        return lastError(path);
    }
    while (sftp_attributes attributes = sftp_readdir(sftp_, dir)) {
        std::string name = attributes->name ? attributes->name : "";
        if (!name.empty() && name != "." && name != "..") {
            auto &entry = entries.emplace_back();
            entry.name = std::move(name);
            toAttributes(attributes, entry.attributes);
        }
        sftp_attributes_free(attributes);
    }
    const bool isComplete = sftp_dir_eof(dir);
    sftp_closedir(dir);
    if (!isComplete) {
        return lastError(path);
    }
    for (auto &entry : entries) {
        if (entry.attributes.type != SSH_FILEXFER_TYPE_SYMLINK) {
            continue;
        }
        const std::string linkPath = (path.ends_with('/') ? path : path + "/") + entry.name;
        if (char *target = sftp_readlink(sftp_, linkPath.c_str())) {
            entry.hasLinkTarget = true;
            entry.linkTarget = target;
            ssh_string_free_char(target);
        }
        if (sftp_attributes targetAttributes = sftp_stat(sftp_, linkPath.c_str())) {
            entry.hasTargetAttributes = true;
            toAttributes(targetAttributes, entry.targetAttributes);
            sftp_attributes_free(targetAttributes);
        }
        if (!ssh_is_connected(session_)) {
            return lastError(path);
        }
    }
    for (const auto &entry : entries) {
        out.put<std::uint16_t>(static_cast<std::uint16_t>(entry.name.size()));
        out.putBytes(entry.name);
        putAttributes(out, entry.attributes);
        out.put<std::uint8_t>((entry.hasLinkTarget ? 1 : 0) | (entry.hasTargetAttributes ? 2 : 0));
        if (entry.hasLinkTarget) {
            out.put<std::uint16_t>(static_cast<std::uint16_t>(entry.linkTarget.size()));
            out.putBytes(entry.linkTarget);
        }
        if (entry.hasTargetAttributes) {
            putAttributes(out, entry.targetAttributes);
        }
    }
    return {};
}

SftpError SftpClient::makeDirectory(const std::string &path, std::uint32_t mode) {
    std::lock_guard lock(mutex_);
    return sftp_mkdir(sftp_, path.c_str(), mode) == SSH_OK ? SftpError{} : lastError(path);
}

SftpError SftpClient::removeDirectory(const std::string &path) {
    std::lock_guard lock(mutex_);
    return sftp_rmdir(sftp_, path.c_str()) == SSH_OK ? SftpError{} : lastError(path);
}

SftpError SftpClient::unlink(const std::string &path) {
    std::lock_guard lock(mutex_);
    return sftp_unlink(sftp_, path.c_str()) == SSH_OK ? SftpError{} : lastError(path);
}

SftpError SftpClient::rename(const std::string &path, const std::string &newPath) {
    std::lock_guard lock(mutex_);
    return sftp_rename(sftp_, path.c_str(), newPath.c_str()) == SSH_OK ? SftpError{}
                                                                       : lastError(path);
}

SftpError SftpClient::symlink(const std::string &target, const std::string &link) {
    std::lock_guard lock(mutex_);
    return sftp_symlink(sftp_, target.c_str(), link.c_str()) == SSH_OK ? SftpError{}
                                                                       : lastError(link);
}

SftpError SftpClient::readLink(const std::string &path, std::string &target) {
    std::lock_guard lock(mutex_);
    char *result = sftp_readlink(sftp_, path.c_str());
    if (!result) {
        return lastError(path);
    }
    target = result;
    ssh_string_free_char(result);
    return {};
}

SftpError SftpClient::realPath(const std::string &path, std::string &realPath) {
    std::lock_guard lock(mutex_);
    char *result = sftp_canonicalize_path(sftp_, path.c_str());
    if (!result) {
        return lastError(path);
    }
    realPath = result;
    ssh_string_free_char(result);
    return {};
}

SftpError SftpClient::setAttributes(const std::string &path, const SftpAttributes &attributes) {
    std::lock_guard lock(mutex_);
    struct sftp_attributes_struct values {};
    values.flags = attributes.flags;
    values.size = attributes.size;
    values.uid = attributes.uid;
    values.gid = attributes.gid;
    values.permissions = attributes.permissions;
    values.atime = static_cast<std::uint32_t>(attributes.atime);
    values.mtime = static_cast<std::uint32_t>(attributes.mtime);
    return sftp_setstat(sftp_, path.c_str(), &values) == SSH_OK ? SftpError{} : lastError(path);
}

SftpError SftpClient::open(const std::string &path, int flags, std::uint32_t mode,
                           std::uint64_t &handle) {
    std::lock_guard lock(mutex_);
    sftp_file file = sftp_open(sftp_, path.c_str(), flags, mode);
    if (!file) {
        return lastError(path);
    }
    handle = nextHandle_++;
    files_.emplace(handle, file);
    paths_.emplace(handle, path);
    return {};
}

sftp_file_struct *SftpClient::findFile(std::uint64_t handle) {
    const auto it = files_.find(handle);
    return it != files_.end() ? it->second : nullptr;
}

SftpError SftpClient::read(std::uint64_t handle, std::uint64_t offset, std::uint8_t *buffer,
                           std::size_t size, std::size_t &readSize) {
    std::lock_guard lock(mutex_);
    readSize = 0;
    sftp_file file = findFile(handle);
    if (!file) {
        return {SftpError::kInvalidHandle, "Invalid handle", -1};
    }
    if (sftp_seek64(file, offset) != SSH_OK) {
        return lastError("seek");
    }
    // Requests in flight, each for up to maxReadLength_ bytes at increasing offsets.
    std::deque<std::pair<sftp_aio, std::size_t>> inFlight;
    std::size_t requested = 0;
    SftpError error;
    bool reachedEnd = false;
    auto request = [&] {
        while (!reachedEnd && !error && requested < size && inFlight.size() < kMaxRequestsInFlight) {
            const std::size_t length = std::min(maxReadLength_, size - requested);
            sftp_aio aio = nullptr;
            if (sftp_aio_begin_read(file, length, &aio) == SSH_ERROR) {
                error = lastError("read");
                return;
            }
            inFlight.emplace_back(aio, requested);
            requested += length;
        }
    };
    request();
    while (!inFlight.empty()) {
        auto [aio, position] = inFlight.front();
        inFlight.pop_front();
        // Responses arrive in order; a short one means the end of the file, after which the
        // remaining requests return nothing but must still be collected.
        const std::size_t length = std::min(maxReadLength_, size - position);
        const ssize_t received = sftp_aio_wait_read(&aio, buffer + position, length);
        if (received == SSH_ERROR) {
            if (!error) {
                error = lastError("read");
            }
        } else if (!reachedEnd && !error) {
            readSize = position + static_cast<std::size_t>(received);
            if (static_cast<std::size_t>(received) < length) {
                reachedEnd = true;
            }
        }
        if (inFlight.empty() || inFlight.size() < kMaxRequestsInFlight / 2) {
            request();
        }
    }
    return error;
}

SftpError SftpClient::write(std::uint64_t handle, std::uint64_t offset,
                            const std::uint8_t *buffer, std::size_t size) {
    std::lock_guard lock(mutex_);
    sftp_file file = findFile(handle);
    if (!file) {
        return {SftpError::kInvalidHandle, "Invalid handle", -1};
    }
    if (sftp_seek64(file, offset) != SSH_OK) {
        return lastError("seek");
    }
    std::deque<sftp_aio> inFlight;
    std::size_t written = 0;
    SftpError error;
    while ((written < size && !error) || !inFlight.empty()) {
        while (!error && written < size && inFlight.size() < kMaxRequestsInFlight) {
            const std::size_t length = std::min(maxWriteLength_, size - written);
            sftp_aio aio = nullptr;
            const ssize_t queued = sftp_aio_begin_write(file, buffer + written, length, &aio);
            if (queued == SSH_ERROR) {
                error = lastError("write");
                break;
            }
            inFlight.push_back(aio);
            written += static_cast<std::size_t>(queued);
        }
        if (!inFlight.empty()) {
            sftp_aio aio = inFlight.front();
            inFlight.pop_front();
            if (sftp_aio_wait_write(&aio) == SSH_ERROR && !error) {
                error = lastError("write");
            }
        }
    }
    return error;
}

SftpError SftpClient::fileAttributes(std::uint64_t handle, SftpAttributes &attributes) {
    std::lock_guard lock(mutex_);
    sftp_file file = findFile(handle);
    if (!file) {
        return {SftpError::kInvalidHandle, "Invalid handle", -1};
    }
    sftp_attributes result = sftp_fstat(file);
    if (!result) {
        return lastError("fstat");
    }
    toAttributes(result, attributes);
    sftp_attributes_free(result);
    return {};
}

SftpError SftpClient::truncate(std::uint64_t handle, std::uint64_t size) {
    std::string path;
    {
        std::lock_guard lock(mutex_);
        const auto it = paths_.find(handle);
        if (it == paths_.end()) {
            return {SftpError::kInvalidHandle, "Invalid handle", -1};
        }
        path = it->second;
    }
    SftpAttributes attributes;
    attributes.flags = SSH_FILEXFER_ATTR_SIZE;
    attributes.size = size;
    return setAttributes(path, attributes);
}

SftpError SftpClient::close(std::uint64_t handle) {
    std::lock_guard lock(mutex_);
    sftp_file file = findFile(handle);
    if (!file) {
        return {SftpError::kInvalidHandle, "Invalid handle", -1};
    }
    files_.erase(handle);
    paths_.erase(handle);
    return sftp_close(file) == SSH_OK ? SftpError{} : lastError("close");
}

// --- Private keys. --------------------------------------------------------------------------

namespace {

bool isBlank(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Records that libssh wanted a passphrase, and gives it none.
int noPassphrase(const char *, char *, size_t, int, int, void *userdata) {
    *static_cast<bool *>(userdata) = true;
    return SSH_ERROR;
}

constexpr const char *kPublicKeyError =
    "A public key. Use its private half: the key file without .pub.";

}  // namespace

std::expected<SshPrivateKey, std::string> parsePrivateKey(std::string_view bytes) {
    if (bytes.size() > kMaxPrivateKeyBytes) {
        return std::unexpected("Too large to be a private key.");
    }
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.remove_prefix(3);
    }
    std::string text;
    text.reserve(bytes.size() + 1);
    for (const char c : bytes) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '\r') {
            continue;  // CRLF from Windows: PEM has no use for it.
        }
        if ((u < 0x20 && c != '\n' && c != '\t') || u == 0x7F) {
            return std::unexpected("Not a text key file. Use an OpenSSH or PEM private key.");
        }
        text += c;
    }
    text.erase(text.begin(), std::ranges::find_if_not(text, isBlank));
    while (!text.empty() && isBlank(text.back())) {
        text.pop_back();
    }
    if (text.empty()) {
        return std::unexpected("Empty. Paste a private key or choose a key file.");
    }
    if (text.starts_with("PuTTY-User-Key-File-")) {
        return std::unexpected("A PuTTY key. Convert it to OpenSSH format first (puttygen).");
    }
    // An OpenSSH public key line: "ssh-ed25519 AAAA... comment".
    bool publicLine = false;
    for (const char *prefix : {"ssh-", "ecdsa-", "sk-ssh-", "sk-ecdsa-"}) {
        publicLine = publicLine
                     || (text.starts_with(prefix) && text.contains(' ') && !text.contains('\n'));
    }
    if (publicLine || text.contains("PUBLIC KEY-----")) {
        return std::unexpected(kPublicKeyError);
    }
    if (text.contains("-----BEGIN CERTIFICATE-----")) {
        return std::unexpected("A certificate, not a private key.");
    }
    if (!text.starts_with("-----BEGIN ") || !text.contains("PRIVATE KEY-----")
        || !text.contains("\n-----END ")) {
        return std::unexpected("Not an OpenSSH or PEM private key, or one cut short.");
    }
    text += '\n';

    SshPrivateKey key;
    const bool openSsh = text.starts_with("-----BEGIN OPENSSH PRIVATE KEY-----");
    if (!openSsh
        && (text.contains("Proc-Type: 4,ENCRYPTED")
            || text.starts_with("-----BEGIN ENCRYPTED PRIVATE KEY-----"))) {
        // libssh can't open an encrypted PEM key without its passphrase, nor tell what's inside.
        key.text = std::move(text);
        key.encrypted = true;
        return key;
    }

    ensureLibsshInitialized();
    ssh_key imported = nullptr;
    bool wantedPassphrase = false;
    // The callback only for OpenSSH keys: with Mbed TLS, libssh calls it for every PEM key.
    const int rc = ssh_pki_import_privkey_base64(text.c_str(), nullptr,
                                                 openSsh ? noPassphrase : nullptr,
                                                 &wantedPassphrase, &imported);
    if (rc != SSH_OK) {
        ssh_key_free(imported);
        if (wantedPassphrase) {
            key.text = std::move(text);
            key.encrypted = true;
            return key;
        }
        return std::unexpected("Couldn't read this private key: damaged, or of an unsupported kind.");
    }
    if (!ssh_key_is_private(imported)) {
        ssh_key_free(imported);
        return std::unexpected(kPublicKeyError);
    }
    const char *type = ssh_key_type_to_char(ssh_key_type(imported));
    key.type = type ? type : "";
    unsigned char *hash = nullptr;
    size_t hashLength = 0;
    if (ssh_get_publickey_hash(imported, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLength) == SSH_OK) {
        if (char *fingerprint = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLength)) {
            key.fingerprint = fingerprint;
            ssh_string_free_char(fingerprint);
        }
        ssh_clean_pubkey_hash(&hash);
    }
    ssh_key_free(imported);
    key.text = std::move(text);
    return key;
}

std::expected<SshPublicKey, std::string> parsePublicKey(std::string_view line) {
    constexpr std::string_view kBlank = " \t\r\n";
    const auto trim = [&](std::string_view text) {
        const auto start = text.find_first_not_of(kBlank);
        if (start == std::string_view::npos) {
            return std::string_view{};
        }
        return text.substr(start, text.find_last_not_of(kBlank) - start + 1);
    };
    line = trim(line);
    const auto typeEnd = line.find_first_of(kBlank);
    const std::string type(line.substr(0, typeEnd));
    const std::string_view rest = typeEnd == std::string_view::npos ? std::string_view{}
                                                                    : trim(line.substr(typeEnd));
    const auto dataEnd = rest.find_first_of(kBlank);
    const std::string data(rest.substr(0, dataEnd));
    const std::string comment(dataEnd == std::string_view::npos ? std::string_view{}
                                                                : trim(rest.substr(dataEnd)));
    auto blob = base64Decode(data);
    if (type.empty() || !blob) {
        return std::unexpected("This isn't an SSH public key.");
    }
    ensureLibsshInitialized();
    const enum ssh_keytypes_e keyType = ssh_key_type_from_name(type.c_str());
    ssh_key imported = nullptr;
    if (keyType == SSH_KEYTYPE_UNKNOWN
        || ssh_pki_import_pubkey_base64(data.c_str(), keyType, &imported) != SSH_OK) {
        ssh_key_free(imported);
        return std::unexpected("Couldn't read this public key: damaged, or of an unsupported kind.");
    }
    SshPublicKey key{.type = type, .blob = std::move(*blob), .comment = comment, .fingerprint = {}};
    unsigned char *hash = nullptr;
    size_t hashLength = 0;
    if (ssh_get_publickey_hash(imported, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLength) == SSH_OK) {
        if (char *fingerprint = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLength)) {
            key.fingerprint = fingerprint;
            ssh_string_free_char(fingerprint);
        }
        ssh_clean_pubkey_hash(&hash);
    }
    ssh_key_free(imported);
    return key;
}

std::string formatPublicKey(std::span<const std::uint8_t> blob, std::string_view comment) {
    // The blob starts with its type, as an SSH string.
    std::string line;
    if (blob.size() >= 4) {
        const std::size_t size = (std::size_t{blob[0]} << 24) | (std::size_t{blob[1]} << 16)
                                 | (std::size_t{blob[2]} << 8) | blob[3];
        if (size <= blob.size() - 4) {
            line.assign(reinterpret_cast<const char *>(blob.data() + 4), size);
        }
    }
    line += ' ';
    line += base64Encode(blob);
    if (!comment.empty()) {
        line += ' ';
        line += comment;
    }
    return line;
}

}  // namespace filepan
