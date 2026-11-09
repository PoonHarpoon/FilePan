// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/vfs/sftp_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <libssh/sk_api.h>
#include <sys/stat.h>
#include <unistd.h>

#include "filepan/byte_writer.hpp"
#include "filepan/sftp_client.hpp"

namespace filepan::vfs {

namespace {

// Reads smaller than this go through the stream's read-ahead buffer.
constexpr std::size_t kDirectReadSize = 256 * 1024;
constexpr std::size_t kStreamBufferSize = 1024 * 1024;
// Removed entries between progress reports.
constexpr std::uint64_t kRemoveProgressEvery = 32;

std::string joinMessage(std::string_view context, std::string_view message) {
    if (context.empty()) {
        return std::string(message);
    }
    if (message.empty()) {
        return std::string(context);
    }
    return std::string(context) + ": " + std::string(message);
}

// --- Known hosts, in OpenSSH's format: "host[,host...] keytype base64" per line. -------------

struct KnownHost {
    std::string hosts;
    std::string keyType;
    std::string key;
};

std::string hostPattern(const std::string &host, int port) {
    return port == 22 ? host : "[" + host + "]:" + std::to_string(port);
}

std::optional<KnownHost> parseKnownHost(std::string_view line) {
    std::istringstream in{std::string(line)};
    KnownHost entry;
    if (!(in >> entry.hosts >> entry.keyType >> entry.key) || entry.hosts.starts_with('#')
        || entry.hosts.starts_with('@')) {
        return std::nullopt;
    }
    return entry;
}

bool matchesHost(const KnownHost &entry, std::string_view pattern) {
    std::string_view hosts = entry.hosts;
    while (!hosts.empty()) {
        const auto comma = hosts.find(',');
        if (hosts.substr(0, comma) == pattern) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        hosts.remove_prefix(comma + 1);
    }
    return false;
}

std::vector<std::string> readLines(const std::string &path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        lines.push_back(std::move(line));
    }
    return lines;
}

// Replaces the host's keys of this type and writes the file atomically.
Result<void> saveKnownHost(const std::string &path, const std::string &pattern,
                           const std::string &keyType, const std::string &key) {
    std::vector<std::string> lines = readLines(path);
    std::erase_if(lines, [&](const std::string &line) {
        const auto entry = parseKnownHost(line);
        return entry && entry->keyType == keyType && matchesHost(*entry, pattern);
    });
    lines.push_back(pattern + " " + keyType + " " + key);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        for (const auto &line : lines) {
            out << line << '\n';
        }
        if (!out.flush()) {
            return fail(EIO, "can't write " + temporary);
        }
    }
    if (::rename(temporary.c_str(), path.c_str()) == -1) {
        return failErrno(path);
    }
    return {};
}

std::string fingerprintOf(const std::string &keyType, const std::string &base64) {
    ssh_key key = nullptr;
    if (ssh_pki_import_pubkey_base64(base64.c_str(), ssh_key_type_from_name(keyType.c_str()), &key)
        != SSH_OK) {
        return {};
    }
    unsigned char *hash = nullptr;
    size_t length = 0;
    std::string result;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &length) == SSH_OK) {
        if (char *fingerprint = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, length)) {
            result = fingerprint;
            ssh_string_free_char(fingerprint);
        }
        ssh_clean_pubkey_hash(&hash);
    }
    ssh_key_free(key);
    return result;
}

// --- Attributes to Entries. --------------------------------------------------------------

EntryType typeOf(const SftpAttributes &attributes) {
    if ((attributes.flags & SSH_FILEXFER_ATTR_PERMISSIONS) && (attributes.permissions & S_IFMT)) {
        switch (attributes.permissions & S_IFMT) {
            case S_IFREG: return EntryType::kFile;
            case S_IFDIR: return EntryType::kDirectory;
            case S_IFLNK: return EntryType::kSymlink;
            default: return EntryType::kOther;
        }
    }
    switch (attributes.type) {
        case SSH_FILEXFER_TYPE_REGULAR: return EntryType::kFile;
        case SSH_FILEXFER_TYPE_DIRECTORY: return EntryType::kDirectory;
        case SSH_FILEXFER_TYPE_SYMLINK: return EntryType::kSymlink;
        default: return EntryType::kOther;
    }
}

void fillEntry(Entry &entry, const SftpAttributes &attributes) {
    entry.hasStat = true;
    entry.type = entry.targetType = typeOf(attributes);
    if (attributes.flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
        entry.mode = attributes.permissions;
    }
    if (attributes.flags & SSH_FILEXFER_ATTR_SIZE) {
        entry.size = attributes.size;
    }
    if (attributes.flags & SSH_FILEXFER_ATTR_UIDGID) {
        entry.uid = attributes.uid;
        entry.gid = attributes.gid;
    }
    if (attributes.flags & SSH_FILEXFER_ATTR_ACMODTIME) {
        entry.accessTime = {static_cast<std::int64_t>(attributes.atime), 0};
        entry.modifiedTime = {static_cast<std::int64_t>(attributes.mtime), 0};
    }
}

void fillTarget(Entry &entry, const SftpAttributes *target) {
    if (target) {
        entry.targetType = typeOf(*target);
        if (target->flags & SSH_FILEXFER_ATTR_SIZE) {
            entry.size = target->size;
        }
    } else {
        entry.targetType = EntryType::kOther;
        entry.brokenLink = true;
    }
}

// Parses SftpClient::listDirectory()'s format.
class ListingParser {
public:
    explicit ListingParser(const ByteWriter &listing)
        : data_(listing.data()), end_(listing.data() + listing.size()) {}

    [[nodiscard]] bool done() const { return data_ >= end_; }

    bool next(Entry &entry) {
        std::uint16_t nameLength;
        if (!get(nameLength) || !getBytes(entry.name, nameLength)) {
            return false;
        }
        entry.hidden = Entry::isHiddenName(entry.name);
        SftpAttributes attributes;
        std::uint8_t flags;
        if (!getAttributes(attributes) || !get(flags)) {
            return false;
        }
        fillEntry(entry, attributes);
        if (flags & 1) {
            std::uint16_t targetLength;
            if (!get(targetLength) || !getBytes(entry.linkTarget, targetLength)) {
                return false;
            }
        }
        SftpAttributes target;
        if ((flags & 2) && !getAttributes(target)) {
            return false;
        }
        if (entry.type == EntryType::kSymlink) {
            fillTarget(entry, (flags & 2) ? &target : nullptr);
        }
        return true;
    }

private:
    template <typename T>
    bool get(T &value) {
        if (end_ - data_ < static_cast<std::ptrdiff_t>(sizeof(T))) {
            return false;
        }
        std::memcpy(&value, data_, sizeof(T));
        data_ += sizeof(T);
        return true;
    }

    bool getBytes(std::string &value, std::size_t length) {
        if (static_cast<std::size_t>(end_ - data_) < length) {
            return false;
        }
        value.assign(reinterpret_cast<const char *>(data_), length);
        data_ += length;
        return true;
    }

    bool getAttributes(SftpAttributes &attributes) {
        return get(attributes.flags) && get(attributes.type) && get(attributes.size)
               && get(attributes.uid) && get(attributes.gid) && get(attributes.permissions)
               && get(attributes.atime) && get(attributes.mtime);
    }

    const std::uint8_t *data_;
    const std::uint8_t *end_;
};

static_assert(securitykey::kAlgorithmEcdsa == SSH_SK_ECDSA
              && securitykey::kAlgorithmEd25519 == SSH_SK_ED25519
              && securitykey::kUserPresenceRequired == SSH_SK_USER_PRESENCE_REQD
              && securitykey::kUserVerificationRequired == SSH_SK_USER_VERIFICATION_REQD);

// One security key signing (or card reading) at a time: connections made in parallel wait their
// turn.
std::mutex &securityKeySigning() {
    static std::mutex signing;
    return signing;
}

// Signs for "sk-" keys with the security key the user presents, and for OpenPGP card logins with
// the card they present, noting how it went so the error can say why. Without prompts (an extra
// connection) or a source, it refuses.
class SecurityKeyPromptSigner : public SecurityKeySigner {
public:
    SecurityKeyPromptSigner(const std::string &serverId, const std::vector<SftpEndpoint> &endpoints,
                            SftpPromptProvider *prompts, SecurityKeySource *source,
                            CardPinCache &cardPins)
        : serverId_(serverId), endpoints_(endpoints), prompts_(prompts), source_(source),
          cardPins_(cardPins) {}

    int sign(const SecurityKeySignRequest &request, SecurityKeySignature &signature) override {
        used = true;
        hop = request.hop;
        if (!prompts_ || !source_ || request.hop >= endpoints_.size()) {
            unavailable = true;
            return SSH_SK_ERR_DEVICE_NOT_FOUND;
        }
        std::lock_guard lock(securityKeySigning());
        auto session = std::make_shared<SecurityKeySession>();
        prompts_->requestSecurityKey(
            prompt(request.hop, (request.flags & SSH_SK_USER_VERIFICATION_REQD) != 0), session);
        source_->start(session);
        const securitykey::SignRequest signRequest{
            .algorithm = request.algorithm,
            .data = {reinterpret_cast<const std::uint8_t *>(request.data.data()),
                     request.data.size()},
            .application = request.application,
            .keyHandle = {reinterpret_cast<const std::uint8_t *>(request.keyHandle.data()),
                          request.keyHandle.size()},
            .flags = request.flags,
        };
        auto result = signWithPresentedKey(*session, signRequest);
        source_->stop();
        session->finish();
        if (!result) {
            error = std::move(result.error());
            switch (error->kind) {
            case securitykey::KeyError::Kind::kUnsupported: return SSH_SK_ERR_UNSUPPORTED;
            case securitykey::KeyError::Kind::kPinRequired: return SSH_SK_ERR_PIN_REQUIRED;
            case securitykey::KeyError::Kind::kWrongKey: return SSH_SK_ERR_DEVICE_NOT_FOUND;
            default: return SSH_SK_ERR_GENERAL;
            }
        }
        signature.flags = result->flags;
        signature.counter = result->counter;
        signature.signature.assign(result->signature.begin(), result->signature.end());
        return 0;
    }

    bool signWithCard(const CardSignRequest &request, std::string &signature) override {
        used = true;
        card = true;
        hop = request.hop;
        if (!prompts_ || !source_ || request.hop >= endpoints_.size()) {
            unavailable = true;
            return false;
        }
        std::lock_guard lock(securityKeySigning());
        auto session = std::make_shared<SecurityKeySession>(SecurityKeySession::Kind::kOpenPgpCard);
        prompts_->requestSecurityKey(prompt(request.hop, true), session);
        source_->start(session);
        auto result = signWithPresentedCard(
            *session,
            {reinterpret_cast<const std::uint8_t *>(request.publicKey.data()),
             request.publicKey.size()},
            {reinterpret_cast<const std::uint8_t *>(request.data.data()), request.data.size()},
            request.flags, cardPins_);
        source_->stop();
        session->finish();
        if (!result) {
            error = std::move(result.error());
            return false;
        }
        signature.assign(result->begin(), result->end());
        return true;
    }

    // Why the login failed, if it was down to the security key or card.
    [[nodiscard]] std::optional<Error> failure(std::string_view hopName) const {
        const auto withHop = [&](int code, std::string_view message) {
            return Error{code, std::string(hopName) + ": " + std::string(message)};
        };
        const std::string_view what = card ? "the OpenPGP card" : "the security key";
        if (unavailable) {
            return withHop(EACCES, "the login needs " + std::string(what)
                                       + ", which can't be asked for here");
        }
        if (!error) {
            return std::nullopt;
        }
        using Kind = securitykey::KeyError::Kind;
        switch (error->kind) {
        case Kind::kCancelled:
            return withHop(ECANCELED, "signing in with " + std::string(what) + " was cancelled");
        case Kind::kWrongKey:
            return withHop(EACCES, card ? "the card doesn't have the key saved for this server"
                                        : "the security key doesn't have this SSH key");
        case Kind::kPinRequired:
            return withHop(EACCES, "the SSH key needs user verification, but the security key "
                                   "has no PIN set");
        case Kind::kPinBlocked:
            return withHop(EACCES, card ? "the card's PIN is blocked; unblock it with its admin "
                                          "PIN or reset code"
                                        : "the security key's PIN is blocked");
        case Kind::kPinWrong:
            return withHop(EACCES, std::string(what) + "'s PIN was wrong");
        case Kind::kUnsupported:
            return withHop(ENOTSUP, card ? "the card's key can't be used for SSH"
                                         : "the security key doesn't support this SSH key");
        case Kind::kCtap:
            return withHop(EIO, "the security key failed (error " + std::to_string(error->status)
                                    + ")");
        case Kind::kIo:
        case Kind::kClosed:
            break;
        }
        if (error->message.empty()) {
            return withHop(EIO, std::string(what) + " failed");
        }
        return withHop(EIO, (card ? "OpenPGP card: " : "security key: ") + error->message);
    }

    bool used = false;
    bool card = false;
    bool unavailable = false;
    std::size_t hop = 0;
    std::optional<securitykey::KeyError> error;

private:
    [[nodiscard]] SecurityKeyPrompt prompt(std::size_t hop, bool userVerification) const {
        const auto &endpoint = endpoints_[hop];
        return SecurityKeyPrompt{.serverId = serverId_,
                                 .hop = hop,
                                 .isJumpHost = hop + 1 < endpoints_.size(),
                                 .host = endpoint.host,
                                 .port = endpoint.port,
                                 .username = endpoint.username,
                                 .userVerification = userVerification};
    }

    const std::string &serverId_;
    const std::vector<SftpEndpoint> &endpoints_;
    SftpPromptProvider *prompts_;
    SecurityKeySource *source_;
    CardPinCache &cardPins_;
};

bool isConnectionError(const SftpError &error) {
    return error.code == SftpError::kConnectionLost || error.code == SftpError::kInvalidHandle;
}

}  // namespace

Error toError(const SftpError &error, std::string_view context) {
    int code;
    switch (error.code) {
        case SSH_FX_OK: code = 0; break;
        case SSH_FX_EOF: code = EIO; break;
        case SSH_FX_NO_SUCH_FILE:
        case SSH_FX_NO_SUCH_PATH: code = ENOENT; break;
        case SSH_FX_PERMISSION_DENIED: code = EACCES; break;
        case SSH_FX_BAD_MESSAGE: code = EBADMSG; break;
        case SSH_FX_NO_CONNECTION: code = ENOTCONN; break;
        case SSH_FX_CONNECTION_LOST: code = ECONNRESET; break;
        case SSH_FX_OP_UNSUPPORTED: code = ENOTSUP; break;
        case SSH_FX_INVALID_HANDLE: code = EBADF; break;
        case SSH_FX_FILE_ALREADY_EXISTS: code = EEXIST; break;
        case SSH_FX_WRITE_PROTECT: code = EROFS; break;
        case SSH_FX_NO_MEDIA: code = ENOMEDIUM; break;
        case SftpError::kConnectFailed: code = ECONNREFUSED; break;
        case SftpError::kHostKeyRejected:
        case SftpError::kAuthenticationFailed:
        case SftpError::kLoginMethodRefused: code = EACCES; break;
        case SftpError::kConnectionLost: code = ENOTCONN; break;
        case SftpError::kInvalidHandle: code = EBADF; break;
        default: code = EIO; break;  // SSH_FX_FAILURE and anything newer.
    }
    return {code, joinMessage(context, error.message)};
}

struct SftpBackend::Server {
    explicit Server(SftpServerConfig config) : config(std::move(config)) {}

    SftpServerConfig config;
    std::mutex clientMutex;
    std::shared_ptr<SftpClient> client;
    // The extra connections for openReadParallel(), guarded by clientMutex too.
    struct Pooled {
        std::shared_ptr<SftpClient> client;
        // Streams reading through it.
        std::size_t streams = 0;
        std::chrono::steady_clock::time_point lastUsed;
    };
    std::vector<Pooled> pool;
    // Extra connections being made now.
    std::size_t connecting = 0;
    // How many extra connections the server took before refusing one, once it has.
    std::optional<std::size_t> poolLimit;
    bool poolLimitReported = false;
    // Counts disconnect()s, so that a connection made meanwhile isn't pooled.
    std::uint64_t generation = 0;
    // Held while connecting, which may wait for the user; guards the fields below.
    std::mutex connectMutex;
    // Passwords and passphrases entered this session, by hop.
    std::map<std::size_t, std::string> credentials;
    // Keys accepted for this session only, or for every key without a known hosts file.
    std::vector<KnownHost> sessionKnownHosts;
    // The main connection logged in with a security key or an OpenPGP card, which needs the user
    // for every connection: extra ones aren't made. Several SFTP channels over the one session
    // could stand in for them.
    std::atomic<bool> usesSecurityKey = false;

    [[nodiscard]] std::vector<SftpEndpoint> endpoints() const {
        auto chain = config.jumpHosts;
        chain.push_back(config.server);
        return chain;
    }
};

namespace {

// Checks host keys against the known hosts, asking the user about unknown and changed ones.
class HostKeyVerifier : public SshHostKeyVerifier {
public:
    HostKeyVerifier(SftpBackend::Server &server, SftpPromptProvider *prompts)
        : server_(server), prompts_(prompts) {}

    bool verify(std::size_t hop, const SshHost &host, const std::string &keyType,
                const std::string &fingerprint, const std::string &publicKey) override {
        const std::string pattern = hostPattern(host.host, host.port);
        std::vector<KnownHost> known = server_.sessionKnownHosts;
        if (!server_.config.knownHostsPath.empty()) {
            for (const auto &line : readLines(server_.config.knownHostsPath)) {
                if (auto entry = parseKnownHost(line)) {
                    known.push_back(std::move(*entry));
                }
            }
        }
        const KnownHost *changed = nullptr;
        for (const auto &entry : known) {
            if (entry.keyType != keyType || !matchesHost(entry, pattern)) {
                continue;
            }
            if (entry.key == publicKey) {
                return true;
            }
            changed = &entry;
        }
        if (!prompts_) {
            return false;
        }
        HostKeyPrompt prompt;
        prompt.serverId = server_.config.id;
        prompt.hop = hop;
        prompt.isJumpHost = hop < server_.config.jumpHosts.size();
        prompt.host = host.host;
        prompt.port = host.port;
        prompt.status = changed ? HostKeyPrompt::Status::kChanged : HostKeyPrompt::Status::kUnknown;
        prompt.keyType = keyType;
        prompt.fingerprint = fingerprint;
        if (changed) {
            prompt.knownFingerprint = fingerprintOf(changed->keyType, changed->key);
        }
        auto [promise, future] = makeDecision<HostKeyDecision>();
        prompts_->requestHostKey(std::move(prompt), std::move(promise));
        const auto decision = future.wait(CancellationToken()).value_or(HostKeyDecision::kReject);
        if (decision == HostKeyDecision::kReject) {
            return false;
        }
        if (decision == HostKeyDecision::kAcceptAndSave && !server_.config.knownHostsPath.empty()) {
            // A key that can't be saved still lets this connection go ahead.
            (void)saveKnownHost(server_.config.knownHostsPath, pattern, keyType, publicKey);
        }
        std::erase_if(server_.sessionKnownHosts, [&](const KnownHost &entry) {
            return entry.keyType == keyType && matchesHost(entry, pattern);
        });
        server_.sessionKnownHosts.push_back({pattern, keyType, publicKey});
        return true;
    }

private:
    SftpBackend::Server &server_;
    SftpPromptProvider *prompts_;
};

}  // namespace

// --- Streams. ------------------------------------------------------------------------------

class SftpBackend::InputStreamImpl final : public InputStream {
public:
    // parallel: on an extra connection (see openReadParallel()).
    InputStreamImpl(SftpBackend &backend, std::shared_ptr<Server> server, std::string path,
                    bool parallel)
        : backend_(backend), server_(std::move(server)), path_(std::move(path)),
          parallel_(parallel) {}

    ~InputStreamImpl() override {
        if (client_) {
            (void)client_->close(handle_);
            release(client_);
        }
    }

    Result<void> open() {
        auto client = acquire();
        if (!client) {
            return std::unexpected(client.error());
        }
        std::uint64_t handle;
        SftpError error = (*client)->open(path_, O_RDONLY, 0, handle);
        if (error.code == SftpError::kConnectionLost) {
            backend_.dropClient(*server_, *client);
            client = acquire();
            if (!client) {
                return std::unexpected(client.error());
            }
            error = (*client)->open(path_, O_RDONLY, 0, handle);
        }
        if (error) {
            release(*client);
            return std::unexpected(toError(error, path_));
        }
        client_ = std::move(*client);
        handle_ = handle;
        return {};
    }

    Result<std::size_t> read(std::span<std::uint8_t> buffer) override {
        if (bufferOffset_ < buffer_.size()) {
            const std::size_t length = std::min(buffer.size(), buffer_.size() - bufferOffset_);
            std::memcpy(buffer.data(), buffer_.data() + bufferOffset_, length);
            bufferOffset_ += length;
            position_ += length;
            return length;
        }
        if (buffer.size() >= kDirectReadSize) {
            auto length = readAt(position_, buffer.data(), buffer.size());
            if (length) {
                position_ += *length;
            }
            return length;
        }
        buffer_.resize(kStreamBufferSize);
        auto filled = readAt(position_, buffer_.data(), buffer_.size());
        if (!filled) {
            buffer_.clear();
            return std::unexpected(filled.error());
        }
        buffer_.resize(*filled);
        bufferStart_ = position_;
        bufferOffset_ = 0;
        if (buffer_.empty()) {
            return 0;
        }
        return read(buffer);
    }

    Result<std::uint64_t> seek(std::int64_t offset, SeekOrigin origin) override {
        std::int64_t base = 0;
        if (origin == SeekOrigin::kCurrent) {
            base = static_cast<std::int64_t>(position_);
        } else if (origin == SeekOrigin::kEnd) {
            auto size = this->size();
            if (!size) {
                return std::unexpected(size.error());
            }
            base = static_cast<std::int64_t>(*size);
        }
        if (base + offset < 0) {
            return fail(EINVAL);
        }
        position_ = static_cast<std::uint64_t>(base + offset);
        if (position_ >= bufferStart_ && position_ <= bufferStart_ + buffer_.size()) {
            bufferOffset_ = static_cast<std::size_t>(position_ - bufferStart_);
        } else {
            buffer_.clear();
            bufferOffset_ = 0;
        }
        return position_;
    }

    Result<std::uint64_t> size() override {
        SftpAttributes attributes;
        auto result = retry([&] { return client_->fileAttributes(handle_, attributes); });
        if (!result) {
            return std::unexpected(result.error());
        }
        return attributes.size;
    }

private:
    Result<std::shared_ptr<SftpClient>> acquire() {
        return parallel_ ? backend_.parallelClient(*server_) : backend_.client(*server_);
    }

    void release(const std::shared_ptr<SftpClient> &client) {
        if (parallel_) {
            backend_.releaseParallel(*server_, client);
        }
    }

    Result<std::size_t> readAt(std::uint64_t offset, std::uint8_t *buffer, std::size_t size) {
        std::size_t length = 0;
        auto result = retry([&] { return client_->read(handle_, offset, buffer, size, length); });
        if (!result) {
            return std::unexpected(result.error());
        }
        return length;
    }

    // Runs f, reopening the file on a new connection once if the connection was lost.
    template <typename F>
    Result<void> retry(F &&f) {
        SftpError error = f();
        if (isConnectionError(error)) {
            backend_.dropClient(*server_, client_);
            client_.reset();
            if (auto reopened = open(); !reopened) {
                return reopened;
            }
            error = f();
        }
        if (error) {
            return std::unexpected(toError(error, path_));
        }
        return {};
    }

    SftpBackend &backend_;
    std::shared_ptr<Server> server_;
    std::string path_;
    bool parallel_;
    std::shared_ptr<SftpClient> client_;
    std::uint64_t handle_ = 0;
    std::uint64_t position_ = 0;
    // Read-ahead for small reads: the bytes from bufferStart_ on.
    std::vector<std::uint8_t> buffer_;
    std::uint64_t bufferStart_ = 0;
    std::size_t bufferOffset_ = 0;
};

class SftpBackend::OutputStreamImpl final : public OutputStream {
public:
    OutputStreamImpl(SftpBackend &backend, std::shared_ptr<Server> server, std::string path)
        : backend_(backend), server_(std::move(server)), path_(std::move(path)) {}

    ~OutputStreamImpl() override { (void)close(); }

    Result<void> open(int flags, std::uint32_t mode) {
        auto client = backend_.client(*server_);
        if (!client) {
            return std::unexpected(client.error());
        }
        std::uint64_t handle;
        SftpError error = (*client)->open(path_, flags, mode, handle);
        if (error.code == SftpError::kConnectionLost) {
            backend_.dropClient(*server_, *client);
            client = backend_.client(*server_);
            if (!client) {
                return std::unexpected(client.error());
            }
            error = (*client)->open(path_, flags, mode, handle);
        }
        if (error) {
            return std::unexpected(toError(error, path_));
        }
        client_ = std::move(*client);
        handle_ = handle;
        return {};
    }

    Result<void> write(std::span<const std::uint8_t> buffer) override {
        if (!client_) {
            return fail(EBADF, path_);
        }
        while (!buffer.empty()) {
            const std::size_t length = std::min(buffer.size(), kStreamBufferSize - pending_.size());
            pending_.insert(pending_.end(), buffer.begin(),
                            buffer.begin() + static_cast<std::ptrdiff_t>(length));
            buffer = buffer.subspan(length);
            if (pending_.size() >= kStreamBufferSize) {
                if (auto flushed = flush(); !flushed) {
                    return flushed;
                }
            }
        }
        return {};
    }

    Result<std::uint64_t> seek(std::int64_t offset, SeekOrigin origin) override {
        if (auto flushed = flush(); !flushed) {
            return std::unexpected(flushed.error());
        }
        std::int64_t base = 0;
        if (origin == SeekOrigin::kCurrent) {
            base = static_cast<std::int64_t>(position_);
        } else if (origin == SeekOrigin::kEnd) {
            auto size = this->size();
            if (!size) {
                return std::unexpected(size.error());
            }
            base = static_cast<std::int64_t>(*size);
        }
        if (base + offset < 0) {
            return fail(EINVAL);
        }
        position_ = static_cast<std::uint64_t>(base + offset);
        return position_;
    }

    Result<std::uint64_t> size() override {
        if (auto flushed = flush(); !flushed) {
            return std::unexpected(flushed.error());
        }
        SftpAttributes attributes;
        auto result = retry([&] { return client_->fileAttributes(handle_, attributes); });
        if (!result) {
            return std::unexpected(result.error());
        }
        return attributes.size;
    }

    Result<void> close() override {
        if (!client_) {
            return {};
        }
        auto flushed = flush();
        const SftpError error = client_->close(handle_);
        client_.reset();
        if (!flushed) {
            return flushed;
        }
        if (error) {
            return std::unexpected(toError(error, path_));
        }
        return {};
    }

private:
    Result<void> flush() {
        if (pending_.empty()) {
            return {};
        }
        auto result = retry([&] {
            return client_->write(handle_, position_, pending_.data(), pending_.size());
        });
        if (!result) {
            return result;
        }
        position_ += pending_.size();
        pending_.clear();
        return {};
    }

    // Runs f, reopening the file (without truncating it) on a new connection once if the
    // connection was lost. Rewriting the same bytes at the same offset is harmless.
    template <typename F>
    Result<void> retry(F &&f) {
        SftpError error = f();
        if (isConnectionError(error)) {
            backend_.dropClient(*server_, client_);
            client_.reset();
            if (auto reopened = open(O_WRONLY, 0); !reopened) {
                return reopened;
            }
            error = f();
        }
        if (error) {
            return std::unexpected(toError(error, path_));
        }
        return {};
    }

    SftpBackend &backend_;
    std::shared_ptr<Server> server_;
    std::string path_;
    std::shared_ptr<SftpClient> client_;
    std::uint64_t handle_ = 0;
    std::uint64_t position_ = 0;
    std::vector<std::uint8_t> pending_;
};

// --- The backend. -------------------------------------------------------------------------

SftpBackend::SftpBackend(std::shared_ptr<SftpPromptProvider> prompts, SftpBackendOptions options)
    : prompts_(std::move(prompts)), options_(options) {}

SftpBackend::~SftpBackend() = default;

Capabilities SftpBackend::capabilities() const {
    return {Capability::kWrite,        Capability::kSetTimes, Capability::kChmod,
            Capability::kSymlinks,     Capability::kSeekableRead,
            Capability::kSeekableWrite};
}

void SftpBackend::setServer(SftpServerConfig config) {
    std::shared_ptr<Server> replaced;
    {
        std::lock_guard lock(mutex_);
        auto id = config.id;
        auto &slot = servers_[id];
        replaced = std::exchange(slot, std::make_shared<Server>(std::move(config)));
    }
    if (replaced) {
        // Open streams keep it, and their connections, alive until they close.
        dropConnections(*replaced);
    }
}

void SftpBackend::removeServer(const std::string &id) {
    std::shared_ptr<Server> removed;
    {
        std::lock_guard lock(mutex_);
        if (const auto found = servers_.find(id); found != servers_.end()) {
            removed = std::move(found->second);
            servers_.erase(found);
        }
    }
    if (removed) {
        dropConnections(*removed);
    }
}

void SftpBackend::disconnect(const std::string &id) {
    if (auto found = server(id)) {
        dropConnections(**found);
    }
}

void SftpBackend::dropConnections(Server &server) {
    std::shared_ptr<SftpClient> client;
    std::vector<Server::Pooled> pool;
    // Destroyed after unlocking, as disconnecting sends a message.
    std::lock_guard lock(server.clientMutex);
    client = std::move(server.client);
    pool = std::move(server.pool);
    server.pool.clear();
    server.poolLimit.reset();
    server.poolLimitReported = false;
    ++server.generation;
}

void SftpBackend::setParallelConnections(std::size_t count) {
    parallelConnections_ = std::clamp<std::size_t>(count, 1, 6);
}

void SftpBackend::setIdleTimeout(std::chrono::steady_clock::duration timeout) {
    idleTimeout_ = timeout.count();
}

void SftpBackend::setOnPoolLimited(std::function<void(const PoolLimit &)> callback) {
    std::lock_guard lock(mutex_);
    onPoolLimited_ = std::move(callback);
}

std::size_t SftpBackend::parallelConnectionCount(const std::string &id) {
    auto found = server(id);
    if (!found) {
        return 0;
    }
    std::vector<std::shared_ptr<SftpClient>> closing;
    reapParallel(**found, closing);
    std::lock_guard lock((*found)->clientMutex);
    return (*found)->pool.size();
}

Result<void> SftpBackend::connect(const std::string &id) {
    auto found = server(id);
    if (!found) {
        return std::unexpected(found.error());
    }
    auto connected = client(**found);
    return connected ? Result<void>() : std::unexpected(connected.error());
}

Result<std::string> SftpBackend::homeDirectory(const std::string &id) {
    std::string path;
    auto result = call(id, [&](SftpClient &client) { return client.realPath(".", path); });
    if (!result) {
        return std::unexpected(result.error());
    }
    return path;
}

Result<CardKey> SftpBackend::readCardKey(const SecurityKeyPrompt &prompt) {
    if (!prompts_ || !options_.securityKeys) {
        return fail(EACCES, "OpenPGP cards can't be read here");
    }
    std::lock_guard lock(securityKeySigning());
    auto session = std::make_shared<SecurityKeySession>(SecurityKeySession::Kind::kOpenPgpCard);
    prompts_->requestSecurityKey(prompt, session);
    options_.securityKeys->start(session);
    auto read = readPresentedCardKey(*session);
    options_.securityKeys->stop();
    session->finish();
    if (!read) {
        using Kind = securitykey::KeyError::Kind;
        switch (read.error().kind) {
        case Kind::kCancelled: return fail(ECANCELED, "reading the card was cancelled");
        case Kind::kUnsupported:
            return fail(ENOTSUP, read.error().message.empty() ? "the card's key can't be used for SSH"
                                                              : read.error().message);
        default:
            return fail(EIO, read.error().message.empty() ? "the card couldn't be read"
                                                          : "OpenPGP card: " + read.error().message);
        }
    }
    const std::string line = formatPublicKey(read->publicKey, "cardno:" + read->serial);
    auto key = parsePublicKey(line);
    if (!key) {
        return fail(EIO, key.error());
    }
    return CardKey{.publicKey = line, .type = key->type, .fingerprint = key->fingerprint};
}

Result<std::shared_ptr<SftpBackend::Server>> SftpBackend::server(const std::string &id) {
    std::lock_guard lock(mutex_);
    const auto found = servers_.find(id);
    if (found == servers_.end()) {
        return fail(ENODEV, "unknown server " + id);
    }
    return found->second;
}

Result<std::shared_ptr<SftpClient>> SftpBackend::client(Server &server) {
    std::vector<std::shared_ptr<SftpClient>> closing;
    reapParallel(server, closing);
    {
        std::lock_guard lock(server.clientMutex);
        if (server.client) {
            return server.client;
        }
    }
    std::lock_guard connectLock(server.connectMutex);
    {
        // Another thread may have connected meanwhile.
        std::lock_guard lock(server.clientMutex);
        if (server.client) {
            return server.client;
        }
    }
    auto connected = connectServer(server);
    if (!connected) {
        return connected;
    }
    std::lock_guard lock(server.clientMutex);
    server.client = *connected;
    return server.client;
}

void SftpBackend::dropClient(Server &server, const std::shared_ptr<SftpClient> &client) {
    std::lock_guard lock(server.clientMutex);
    if (server.client == client) {
        server.client.reset();
    }
    std::erase_if(server.pool,
                  [&](const Server::Pooled &pooled) { return pooled.client == client; });
}

void SftpBackend::reapParallel(Server &server, std::vector<std::shared_ptr<SftpClient>> &closing) {
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::steady_clock::duration timeout(idleTimeout_.load());
    const std::size_t most = parallelConnections_;
    std::lock_guard lock(server.clientMutex);
    std::size_t kept = 0;
    std::erase_if(server.pool, [&](Server::Pooled &pooled) {
        // Idle too long, or more than setParallelConnections() now allows.
        if (pooled.streams == 0 && (now - pooled.lastUsed >= timeout || kept >= most)) {
            closing.push_back(std::move(pooled.client));
            return true;
        }
        ++kept;
        return false;
    });
}

Result<std::shared_ptr<SftpClient>> SftpBackend::parallelClient(Server &server) {
    // The main connection first, so that whatever has to be asked is asked there.
    auto main = client(server);
    if (!main) {
        return main;
    }
    if (server.usesSecurityKey) {
        return main;
    }
    const auto leastBusy = [&]() -> Server::Pooled * {
        Server::Pooled *least = nullptr;
        for (auto &pooled : server.pool) {
            if (!least || pooled.streams < least->streams) {
                least = &pooled;
            }
        }
        return least;
    };
    const auto use = [](Server::Pooled &pooled) {
        ++pooled.streams;
        pooled.lastUsed = std::chrono::steady_clock::now();
        return pooled.client;
    };
    std::uint64_t generation;
    {
        std::lock_guard lock(server.clientMutex);
        const std::size_t most = std::min<std::size_t>(
            parallelConnections_,
            server.poolLimit.value_or(std::numeric_limits<std::size_t>::max()));
        Server::Pooled *least = leastBusy();
        if (server.pool.size() + server.connecting >= most || (least && least->streams == 0)) {
            // Shared: there's an idle one, or no room for another.
            return least ? use(*least) : *main;
        }
        ++server.connecting;
        generation = server.generation;
    }

    // With what the main connection was given, so the user isn't asked again; jump hosts and
    // all go the same way.
    Server copy(server.config);
    {
        std::lock_guard connectLock(server.connectMutex);
        copy.credentials = server.credentials;
        copy.sessionKnownHosts = server.sessionKnownHosts;
    }
    auto connected = connectServer(copy, false);
    // Refused while the main connection still answers is a limit; with it gone too, the network
    // or the server is down, and the stream fails as any would.
    bool down = false;
    if (!connected) {
        if (!(*main)->sendKeepAlive()) {
            dropClient(server, *main);
            down = true;
        }
    }

    std::optional<PoolLimit> limited;
    std::shared_ptr<SftpClient> result;
    {
        std::lock_guard lock(server.clientMutex);
        --server.connecting;
        if (down) {
            return std::unexpected(connected.error());
        }
        if (connected) {
            result = *connected;
            if (server.generation == generation) {
                server.pool.push_back({result, 1, std::chrono::steady_clock::now()});
                if (server.poolLimit && server.pool.size() > *server.poolLimit) {
                    server.poolLimit = server.pool.size();  // One made at the same time worked.
                }
            }
        } else {
            // It works with fewer: the server or something on the way limits connections
            // (sshd's MaxStartups or MaxSessions, a firewall). Don't try for more this session.
            if (!server.poolLimit) {
                server.poolLimit = server.pool.size();
            }
            if (!server.poolLimitReported) {
                server.poolLimitReported = true;
                limited = PoolLimit{.serverId = server.config.id,
                                    .working = server.pool.size() + 1,
                                    .reason = connected.error().message};
            }
            Server::Pooled *least = leastBusy();
            result = least ? use(*least) : *main;
        }
    }
    if (limited) {
        std::function<void(const PoolLimit &)> callback;
        {
            std::lock_guard lock(mutex_);
            callback = onPoolLimited_;
        }
        if (callback) {
            callback(*limited);
        }
    }
    return result;
}

void SftpBackend::releaseParallel(Server &server, const std::shared_ptr<SftpClient> &client) {
    std::vector<std::shared_ptr<SftpClient>> closing;
    {
        std::lock_guard lock(server.clientMutex);
        for (auto &pooled : server.pool) {
            if (pooled.client == client) {
                pooled.streams -= pooled.streams > 0 ? 1 : 0;
                pooled.lastUsed = std::chrono::steady_clock::now();
            }
        }
    }
    reapParallel(server, closing);
}

Result<std::shared_ptr<SftpClient>> SftpBackend::connectServer(Server &server, bool interactive) {
    SftpPromptProvider *const prompts = interactive ? prompts_.get() : nullptr;
    const auto endpoints = server.endpoints();
    const std::size_t jumps = endpoints.size() - 1;
    // Which host, for the messages: "Jump host 2 (me@bastion:22)", or the server itself.
    const auto hopName = [&](std::size_t hop) {
        const auto &e = endpoints[hop];
        const bool v6 = e.host.find(':') != std::string::npos;
        const std::string where = (e.username.empty() ? std::string() : e.username + "@")
                                  + (v6 ? "[" + e.host + "]" : e.host) + ":"
                                  + std::to_string(e.port);
        if (hop < jumps) {
            return "Jump host " + std::to_string(hop + 1) + " (" + where + ")";
        }
        return jumps > 0 ? "Server (" + where + ")" : where;
    };
    // A connection error, put down to the host it happened at.
    const auto failure = [&](const SftpError &error) {
        const bool atHop =
            error.hop >= 0 && static_cast<std::size_t>(error.hop) < endpoints.size();
        return std::unexpected(toError(
            error, atHop ? hopName(static_cast<std::size_t>(error.hop)) : server.config.id));
    };
    const auto ask = [&](std::size_t hop, CredentialPrompt::Kind kind,
                         bool retry) -> std::optional<std::string> {
        if (!prompts) {
            return std::nullopt;
        }
        auto [promise, future] = makeDecision<std::optional<std::string>>();
        prompts->requestCredential(CredentialPrompt{.serverId = server.config.id,
                                                     .hop = hop,
                                                     .isJumpHost = hop < jumps,
                                                     .host = endpoints[hop].host,
                                                     .port = endpoints[hop].port,
                                                     .username = endpoints[hop].username,
                                                     .kind = kind,
                                                     .retry = retry},
                                    std::move(promise));
        auto answer = future.wait(CancellationToken());
        return answer ? *answer : std::nullopt;
    };
    // What the user entered overrides what the config has.
    const auto secretFor = [&](std::size_t hop) -> std::string {
        const auto entered = server.credentials.find(hop);
        if (entered != server.credentials.end()) {
            return entered->second;
        }
        const auto &authentication = endpoints[hop].authentication;
        return authentication.type == SftpAuthentication::Type::kPassword
                   ? authentication.password
                   : authentication.passphrase;
    };

    for (int attempt = 0; attempt < std::max(options_.maxCredentialAttempts, 1); ++attempt) {
        std::vector<SshHost> chain;
        for (std::size_t hop = 0; hop < endpoints.size(); ++hop) {
            const auto &endpoint = endpoints[hop];
            SshHost &host = chain.emplace_back();
            host.host = endpoint.host;
            host.port = endpoint.port;
            host.username = endpoint.username;
            if (endpoint.authentication.type == SftpAuthentication::Type::kPrivateKey) {
                host.authentication.type = SshAuthentication::Type::kPrivateKey;
                if (endpoint.authentication.privateKey.empty()) {
                    // Not saved (the platform couldn't seal it, or it was saved as a path to a
                    // key file, from before keys were kept themselves), or never given.
                    return fail(ENOKEY, hopName(hop)
                                            + ": no private key saved for it; edit the server "
                                              "to choose or paste one");
                }
                host.authentication.privateKey = endpoint.authentication.privateKey;
                host.authentication.passphrase = secretFor(hop);
            } else if (endpoint.authentication.type == SftpAuthentication::Type::kOpenPgpCard) {
                host.authentication.type = SshAuthentication::Type::kOpenPgpCard;
                if (endpoint.authentication.cardPublicKey.empty()) {
                    return fail(ENOKEY, hopName(hop)
                                            + ": no OpenPGP card key saved for it; edit the "
                                              "server to read it from the card");
                }
                host.authentication.cardPublicKey = endpoint.authentication.cardPublicKey;
            } else {
                host.authentication.type = SshAuthentication::Type::kPassword;
                host.authentication.password = secretFor(hop);
                if (host.authentication.password.empty()) {
                    auto password = ask(hop, CredentialPrompt::Kind::kPassword, false);
                    if (!password) {
                        return fail(EACCES, hopName(hop) + ": a password is needed to log in");
                    }
                    server.credentials[hop] = *password;
                    host.authentication.password = std::move(*password);
                }
            }
        }
        SecurityKeyPromptSigner securityKeys(server.config.id, endpoints, prompts,
                                             options_.securityKeys.get(), cardPins_);
        HostKeyVerifier verifier(server, prompts);
        SftpError error;
        auto client = SftpClient::connect(chain, verifier, &securityKeys,
                                          server.config.timeoutSeconds, error);
        if (client) {
            if (securityKeys.used) {
                server.usesSecurityKey = true;
            }
            return std::shared_ptr<SftpClient>(std::move(client));
        }
        if (securityKeys.used) {
            if (auto keyFailure = securityKeys.failure(hopName(securityKeys.hop))) {
                return std::unexpected(std::move(*keyFailure));
            }
        }
        if (error.code != SftpError::kAuthenticationFailed || error.hop < 0
            || static_cast<std::size_t>(error.hop) >= endpoints.size()) {
            return failure(error);
        }
        const auto hop = static_cast<std::size_t>(error.hop);
        if (endpoints[hop].authentication.type == SftpAuthentication::Type::kOpenPgpCard) {
            // The server refused the card's key: there's nothing to ask for.
            return failure(error);
        }
        const bool isKey =
            endpoints[hop].authentication.type == SftpAuthentication::Type::kPrivateKey;
        if (isKey && error.message.find("passphrase") == std::string::npos) {
            // The server refused the key itself; no passphrase will change that.
            return failure(error);
        }
        auto secret = ask(hop,
                          isKey ? CredentialPrompt::Kind::kPassphrase
                                : CredentialPrompt::Kind::kPassword,
                          !secretFor(hop).empty());
        if (!secret) {
            return failure(error);
        }
        server.credentials[hop] = std::move(*secret);
    }
    return fail(EACCES, "too many failed logins to " + server.config.id);
}

template <typename F>
Result<void> SftpBackend::call(const std::string &serverId, F &&f) {
    auto found = server(serverId);
    if (!found) {
        return std::unexpected(found.error());
    }
    for (int attempt = 0;; ++attempt) {
        auto client = this->client(**found);
        if (!client) {
            return std::unexpected(client.error());
        }
        const SftpError error = f(**client);
        if (!error) {
            return {};
        }
        if (error.code == SftpError::kConnectionLost) {
            dropClient(**found, *client);
            if (attempt == 0) {
                continue;
            }
        }
        return std::unexpected(toError(error));
    }
}

Result<void> SftpBackend::list(const Location &directory, const ListOptions &options,
                               const ListCallback &onBatch, const CancellationToken &cancel) {
    ByteWriter listing;
    auto result = call(directory.serverId(), [&](SftpClient &client) {
        listing.clear();
        return client.listDirectory(directory.path(), listing);
    });
    if (!result) {
        if (result.error().code == EIO || result.error().code == ENOENT) {
            // A file opened as a directory: OpenSSH reports ENOTDIR as no such file.
            if (auto entry = stat(directory); entry && !entry->isDirectoryLike()) {
                return fail(ENOTDIR, directory.toString());
            }
        }
        result.error().message = joinMessage(directory.toString(), result.error().message);
        return result;
    }
    const std::size_t batchSize = std::max<std::size_t>(options.minBatchSize, 1);
    std::vector<Entry> batch;
    ListingParser parser(listing);
    while (!parser.done()) {
        if (cancel.isCancelled()) {
            return fail(ECANCELED);
        }
        Entry entry;
        if (!parser.next(entry)) {
            return fail(EBADMSG, directory.toString());
        }
        batch.push_back(std::move(entry));
        if (batch.size() >= batchSize) {
            onBatch(std::exchange(batch, {}));
        }
    }
    if (!batch.empty()) {
        onBatch(std::move(batch));
    }
    return {};
}

Result<Entry> SftpBackend::stat(const Location &location) {
    SftpAttributes attributes;
    auto result = call(location.serverId(), [&](SftpClient &client) {
        return client.stat(location.path(), true, attributes);
    });
    if (!result) {
        return std::unexpected(Error{result.error().code, location.toString()});
    }
    Entry entry;
    entry.name = std::string(location.name());
    entry.hidden = Entry::isHiddenName(entry.name);
    fillEntry(entry, attributes);
    return entry;
}

Result<Entry> SftpBackend::lstat(const Location &location) {
    SftpAttributes attributes;
    std::string target;
    bool hasTarget = false;
    SftpAttributes targetAttributes;
    bool resolved = false;
    auto result = call(location.serverId(), [&](SftpClient &client) {
        SftpError error = client.stat(location.path(), false, attributes);
        if (error || typeOf(attributes) != EntryType::kSymlink) {
            return error;
        }
        hasTarget = !client.readLink(location.path(), target);
        const SftpError targetError = client.stat(location.path(), true, targetAttributes);
        resolved = !targetError;
        // A broken link is fine; a lost connection isn't.
        return targetError.code == SftpError::kConnectionLost ? targetError : SftpError{};
    });
    if (!result) {
        return std::unexpected(Error{result.error().code, location.toString()});
    }
    Entry entry;
    entry.name = std::string(location.name());
    entry.hidden = Entry::isHiddenName(entry.name);
    fillEntry(entry, attributes);
    if (entry.type == EntryType::kSymlink) {
        if (hasTarget) {
            entry.linkTarget = std::move(target);
        }
        fillTarget(entry, resolved ? &targetAttributes : nullptr);
    }
    return entry;
}

namespace {

// SFTP v3 reports most failures as SSH_FX_FAILURE (EIO here); an existing target is the usual
// reason when creating something.
Result<void> refineCreateError(Result<void> result, Backend &backend, const Location &location) {
    if (!result && result.error().code == EIO && backend.lstat(location)) {
        return fail(EEXIST, location.toString());
    }
    if (!result) {
        result.error().message = joinMessage(location.toString(), result.error().message);
    }
    return result;
}

}  // namespace

Result<void> SftpBackend::makeDirectory(const Location &location, std::uint32_t mode) {
    auto result = call(location.serverId(), [&](SftpClient &client) {
        return client.makeDirectory(location.path(), mode & 07777);
    });
    return refineCreateError(std::move(result), *this, location);
}

Result<void> SftpBackend::createFile(const Location &location, std::uint32_t mode) {
    auto result = call(location.serverId(), [&](SftpClient &client) {
        std::uint64_t handle;
        SftpError error =
            client.open(location.path(), O_WRONLY | O_CREAT | O_EXCL, mode & 07777, handle);
        return error ? error : client.close(handle);
    });
    return refineCreateError(std::move(result), *this, location);
}

Result<void> SftpBackend::rename(const Location &source, const Location &target, bool replace) {
    if (source.serverId() != target.serverId()) {
        return fail(EXDEV, source.toString());
    }
    // OpenSSH's server replaces the target through posix-rename@openssh.com, which libssh uses
    // when offered; plain SFTP v3 rename never replaces.
    if (!replace && lstat(target)) {
        return fail(EEXIST, target.toString());
    }
    const auto doRename = [&] {
        return call(source.serverId(), [&](SftpClient &client) {
            return client.rename(source.path(), target.path());
        });
    };
    auto result = doRename();
    if (!result && replace) {
        if (const auto existing = lstat(target)) {
            const auto removed = call(target.serverId(), [&](SftpClient &client) {
                return existing->type == EntryType::kDirectory ? client.removeDirectory(target.path())
                                                               : client.unlink(target.path());
            });
            if (!removed) {
                return std::unexpected(Error{removed.error().code, target.toString()});
            }
            result = doRename();
        }
    }
    if (!result) {
        if (result.error().code == EIO && lstat(target)) {
            return fail(EEXIST, target.toString());
        }
        result.error().message = joinMessage(source.toString(), result.error().message);
    }
    return result;
}

Result<RemoveStats> SftpBackend::remove(const Location &location, bool recursive,
                                        RemoveCallbacks *callbacks,
                                        const CancellationToken &cancel) {
    const auto entry = lstat(location);
    if (!entry) {
        return std::unexpected(entry.error());
    }
    if (entry->type != EntryType::kDirectory || !recursive) {
        auto result = call(location.serverId(), [&](SftpClient &client) {
            return entry->type == EntryType::kDirectory ? client.removeDirectory(location.path())
                                                        : client.unlink(location.path());
        });
        if (!result) {
            if (result.error().code == EIO && entry->type == EntryType::kDirectory) {
                return fail(ENOTEMPTY, location.toString());
            }
            result.error().message = joinMessage(location.toString(), result.error().message);
            return std::unexpected(result.error());
        }
        return RemoveStats{1, 0};
    }
    RemoveStats stats;
    std::uint64_t reported = 0;
    auto result = removeTree(location, callbacks, cancel, stats, reported);
    if (result && callbacks && stats.removedCount > reported) {
        callbacks->onProgress(stats.removedCount - reported, location.path());
    }
    if (!result) {
        return std::unexpected(result.error());
    }
    return stats;
}

// Removes a directory's contents depth first, then the directory. Returns ECANCELED when
// cancelled or aborted, after reporting the error through callbacks.
Result<void> SftpBackend::removeTree(const Location &directory, RemoveCallbacks *callbacks,
                                     const CancellationToken &cancel, RemoveStats &stats,
                                     std::uint64_t &reported) {
    const auto progress = [&](const Location &current) {
        if (cancel.isCancelled()) {
            return false;
        }
        if (callbacks && stats.removedCount - reported >= kRemoveProgressEvery) {
            const auto count = stats.removedCount - reported;
            reported = stats.removedCount;
            return callbacks->onProgress(count, current.path());
        }
        return true;
    };
    // Runs op until it works or the callbacks skip (false) or abort (error).
    const auto attempt = [&](const Location &location, auto &&op) -> Result<bool> {
        while (true) {
            auto result = op();
            if (result) {
                return true;
            }
            if (result.error().code == ECANCELED || cancel.isCancelled()) {
                return fail(ECANCELED);
            }
            result.error().message = joinMessage(location.toString(), result.error().message);
            if (!callbacks) {
                return std::unexpected(result.error());
            }
            switch (callbacks->onError(location.path(), result.error())) {
                case RemoveCallbacks::ErrorAction::kRetry: continue;
                case RemoveCallbacks::ErrorAction::kSkip: return false;
                case RemoveCallbacks::ErrorAction::kAbort: return std::unexpected(result.error());
            }
        }
    };

    std::vector<Entry> children;
    auto listed = attempt(directory, [&] {
        children.clear();
        return list(
            directory, {.minBatchSize = 4096},
            [&](std::vector<Entry> batch) {
                std::ranges::move(batch, std::back_inserter(children));
            },
            cancel);
    });
    if (!listed) {
        return std::unexpected(listed.error());
    }
    if (!*listed) {
        ++stats.skippedCount;
        return {};
    }
    const std::uint64_t skippedBefore = stats.skippedCount;
    for (const auto &child : children) {
        const Location location = directory.child(child.name);
        if (child.type == EntryType::kDirectory) {
            if (auto nested = removeTree(location, callbacks, cancel, stats, reported); !nested) {
                return nested;
            }
            continue;
        }
        auto removed = attempt(location, [&] {
            return call(location.serverId(),
                        [&](SftpClient &client) { return client.unlink(location.path()); });
        });
        if (!removed) {
            return std::unexpected(removed.error());
        }
        if (*removed) {
            ++stats.removedCount;
        } else {
            ++stats.skippedCount;
        }
        if (!progress(location)) {
            return fail(ECANCELED);
        }
    }
    if (stats.skippedCount != skippedBefore) {
        return {};  // Something inside stays, so the directory does too.
    }
    auto removed = attempt(directory, [&] {
        return call(directory.serverId(), [&](SftpClient &client) {
            return client.removeDirectory(directory.path());
        });
    });
    if (!removed) {
        return std::unexpected(removed.error());
    }
    if (*removed) {
        ++stats.removedCount;
    } else {
        ++stats.skippedCount;
    }
    if (!progress(directory)) {
        return fail(ECANCELED);
    }
    return {};
}

Result<std::unique_ptr<InputStream>> SftpBackend::openRead(const Location &location) {
    return openStream(location, false);
}

Result<std::unique_ptr<InputStream>> SftpBackend::openReadParallel(const Location &location) {
    return openStream(location, true);
}

Result<std::unique_ptr<InputStream>> SftpBackend::openStream(const Location &location,
                                                             bool parallel) {
    auto found = server(location.serverId());
    if (!found) {
        return std::unexpected(found.error());
    }
    auto stream =
        std::make_unique<InputStreamImpl>(*this, std::move(*found), location.path(), parallel);
    if (auto opened = stream->open(); !opened) {
        if (opened.error().code == EIO) {
            if (auto entry = stat(location); entry && entry->isDirectoryLike()) {
                return fail(EISDIR, location.toString());
            }
        }
        return std::unexpected(opened.error());
    }
    return std::unique_ptr<InputStream>(std::move(stream));
}

Result<std::unique_ptr<OutputStream>> SftpBackend::openWrite(const Location &location,
                                                            const OpenWriteOptions &options) {
    auto found = server(location.serverId());
    if (!found) {
        return std::unexpected(found.error());
    }
    int flags = O_WRONLY;
    switch (options.mode) {
        case WriteMode::kCreateNew: flags |= O_CREAT | O_EXCL; break;
        case WriteMode::kReplace: flags |= O_CREAT | O_TRUNC; break;
        case WriteMode::kExisting: break;
    }
    auto stream = std::make_unique<OutputStreamImpl>(*this, std::move(*found), location.path());
    if (auto opened = stream->open(flags, options.permissions & 07777); !opened) {
        if (options.mode == WriteMode::kCreateNew) {
            auto refined = refineCreateError(std::move(opened), *this, location);
            return std::unexpected(refined.error());
        }
        return std::unexpected(opened.error());
    }
    return std::unique_ptr<OutputStream>(std::move(stream));
}

Result<std::string> SftpBackend::readLink(const Location &location) {
    std::string target;
    auto result = call(location.serverId(), [&](SftpClient &client) {
        return client.readLink(location.path(), target);
    });
    if (!result) {
        return std::unexpected(Error{result.error().code, location.toString()});
    }
    return target;
}

Result<void> SftpBackend::createSymlink(const Location &link, std::string_view target) {
    auto result = call(link.serverId(), [&](SftpClient &client) {
        return client.symlink(std::string(target), link.path());
    });
    return refineCreateError(std::move(result), *this, link);
}

Result<void> SftpBackend::setTimes(const Location &location, std::optional<Timestamp> accessTime,
                                   std::optional<Timestamp> modifiedTime) {
    if (!accessTime && !modifiedTime) {
        return {};
    }
    if (!accessTime || !modifiedTime) {
        // SFTP v3 sets both at once.
        const auto current = stat(location);
        if (!current) {
            return std::unexpected(current.error());
        }
        accessTime = accessTime.value_or(current->accessTime);
        modifiedTime = modifiedTime.value_or(current->modifiedTime);
    }
    SftpAttributes attributes;
    attributes.flags = SSH_FILEXFER_ATTR_ACMODTIME;
    attributes.atime = static_cast<std::uint64_t>(accessTime->seconds);
    attributes.mtime = static_cast<std::uint64_t>(modifiedTime->seconds);
    auto result = call(location.serverId(), [&](SftpClient &client) {
        return client.setAttributes(location.path(), attributes);
    });
    if (!result) {
        return std::unexpected(Error{result.error().code, location.toString()});
    }
    return {};
}

Result<void> SftpBackend::chmod(const Location &location, std::uint32_t mode) {
    SftpAttributes attributes;
    attributes.flags = SSH_FILEXFER_ATTR_PERMISSIONS;
    attributes.permissions = mode & 07777;
    auto result = call(location.serverId(), [&](SftpClient &client) {
        return client.setAttributes(location.path(), attributes);
    });
    if (!result) {
        return std::unexpected(Error{result.error().code, location.toString()});
    }
    return {};
}

}  // namespace filepan::vfs
