// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "filepan/byte_writer.hpp"

struct ssh_session_struct;
struct sftp_session_struct;
struct sftp_file_struct;
struct sftp_limits_struct;

namespace filepan {

struct SshAuthentication {
    enum class Type : std::int32_t { kPassword = 0, kPrivateKey = 1, kOpenPgpCard = 2 };

    Type type = Type::kPassword;
    std::string password;
    // OpenSSH or PEM private key text. An "sk-" key from a FIDO security key signs through the
    // SecurityKeySigner.
    std::string privateKey;
    std::string passphrase;
    // For kOpenPgpCard: the card's authentication key as an OpenSSH public key line, which
    // signs through the SecurityKeySigner.
    std::string cardPublicKey;
};

struct SshHost {
    std::string host;
    int port = 22;
    std::string username;
    SshAuthentication authentication;
};

class SshHostKeyVerifier {
public:
    virtual ~SshHostKeyVerifier() = default;

    // Decides whether to trust a host's key; hop is the host's position in the connection chain.
    // fingerprint is OpenSSH's "SHA256:..." form.
    virtual bool verify(std::size_t hop, const SshHost &host, const std::string &keyType,
                        const std::string &fingerprint, const std::string &publicKey) = 0;
};

// What a FIDO2/U2F security key is asked to sign, as for OpenSSH's security key middleware.
struct SecurityKeySignRequest {
    // The authenticating host's position in the connection chain.
    std::size_t hop = 0;
    // libssh's SSH_SK_ECDSA or SSH_SK_ED25519.
    std::uint32_t algorithm = 0;
    // The client data; its SHA-256 is the client data hash (or U2F challenge).
    std::string data;
    // The relying party ID, usually "ssh:".
    std::string application;
    std::string keyHandle;
    // libssh's SSH_SK_USER_PRESENCE_REQD and SSH_SK_USER_VERIFICATION_REQD.
    std::uint8_t flags = 0;
};

// What an OpenPGP card is asked to sign, as an ssh-agent would be.
struct CardSignRequest {
    // The authenticating host's position in the connection chain.
    std::size_t hop = 0;
    // The SSH public key blob of the key to sign with.
    std::string publicKey;
    std::string data;
    // The agent request's flags, which choose the hash for RSA (securitykey::agent_flags).
    std::uint32_t flags = 0;
};

struct SecurityKeySignature {
    // From the authenticator data.
    std::uint8_t flags = 0;
    std::uint32_t counter = 0;
    // DER for ECDSA; the raw 64 bytes for Ed25519.
    std::string signature;
};

class SecurityKeySigner {
public:
    virtual ~SecurityKeySigner() = default;

    // Returns 0, or one of libssh's SSH_SK_ERR_* codes. Called on the thread connecting, for
    // jump hosts too.
    virtual int sign(const SecurityKeySignRequest &request, SecurityKeySignature &signature) = 0;
    // Signs as ssh-agent would with an OpenPGP card's authentication key, setting the SSH
    // signature blob; false if it couldn't. Called on a thread of its own while the connecting
    // one waits.
    virtual bool signWithCard(const CardSignRequest &request, std::string &signature) {
        (void)request;
        (void)signature;
        return false;
    }
};

// A private key as the user supplied it, pasted or read from a file, checked with libssh.
struct SshPrivateKey {
    // The key as SshAuthentication::privateKey takes it: no byte order mark, LF line endings, no
    // blank space around it, and a final newline.
    std::string text;
    // libssh's name for its type ("ssh-ed25519", "ssh-rsa", "ecdsa-sha2-nistp256", ...). Empty
    // for an encrypted PEM key, which hides its type too.
    std::string type;
    // The public half's "SHA256:..." fingerprint; empty while the key is encrypted.
    std::string fingerprint;
    // It needs a passphrase, which is asked for when connecting if none is saved.
    bool encrypted = false;
};

// Longer than any private key: a 16384-bit RSA key in PEM is about 13 KB.
inline constexpr std::size_t kMaxPrivateKeyBytes = 64 * 1024;

// Reads an OpenSSH ("BEGIN OPENSSH PRIVATE KEY") or PEM (PKCS#1, SEC1 or PKCS#8) private key
// from its text or a key file's bytes. The error is a sentence for the user: empty, not text, a
// public key, a PuTTY key, or a key libssh can't read. An encrypted key is checked as far as it
// can be without its passphrase: an OpenSSH one up to its encrypted part, a PEM one by its
// markers only. Thread-safe.
std::expected<SshPrivateKey, std::string> parsePrivateKey(std::string_view bytes);

// A public key as an authorized_keys line has it.
struct SshPublicKey {
    // libssh's name for its type, as the line starts.
    std::string type;
    std::vector<std::uint8_t> blob;
    std::string comment;
    // "SHA256:...".
    std::string fingerprint;
};

// Reads "<type> <base64> [comment]", checked with libssh. The error is a sentence for the user.
std::expected<SshPublicKey, std::string> parsePublicKey(std::string_view line);
// The authorized_keys line for a key blob.
std::string formatPublicKey(std::span<const std::uint8_t> blob, std::string_view comment);

// An SFTP status code (SSH_FX_*) or one of the negative connection-level codes.
struct SftpError {
    static constexpr int kOk = 0;
    static constexpr int kConnectFailed = -1;
    static constexpr int kHostKeyRejected = -2;
    static constexpr int kAuthenticationFailed = -3;
    // The connection broke or timed out; reconnecting may help.
    static constexpr int kConnectionLost = -4;
    static constexpr int kInvalidHandle = -5;
    // The host doesn't offer the kind of login configured for it (a password where it only takes
    // keys): asking for the password again won't help.
    static constexpr int kLoginMethodRefused = -6;

    int code = kOk;
    std::string message;
    // For kConnectFailed, kHostKeyRejected, kAuthenticationFailed and kLoginMethodRefused: which
    // host in the chain. The message then says which step failed: "couldn't connect: ...",
    // "couldn't log in: ...", "couldn't reach host:port through jump host 1: ...".
    int hop = -1;

    explicit operator bool() const { return code != kOk; }
};

// SFTP v3 attributes; fields are valid according to flags (SSH_FILEXFER_ATTR_*).
struct SftpAttributes {
    std::uint32_t flags = 0;
    std::uint8_t type = 0;  // SSH_FILEXFER_TYPE_*
    std::uint64_t size = 0;
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    std::uint32_t permissions = 0;
    std::uint64_t atime = 0;
    std::uint64_t mtime = 0;
};

// A connection to one SFTP server, possibly through jump hosts. Methods are thread-safe and
// serialized; long transfers are split into chunks so that other calls can interleave.
class SshTunnel;

class SftpClient {
public:
    // chain lists the jump hosts in order, then the server. signer may be null if no host uses
    // a security key.
    static std::unique_ptr<SftpClient> connect(const std::vector<SshHost> &chain,
                                               SshHostKeyVerifier &verifier,
                                               SecurityKeySigner *signer, int timeoutSeconds,
                                               SftpError &error);

    ~SftpClient();
    SftpClient(const SftpClient &) = delete;
    SftpClient &operator=(const SftpClient &) = delete;

    bool isConnected();
    // Returns false if the connection is gone.
    bool sendKeepAlive();

    SftpError stat(const std::string &path, bool followLinks, SftpAttributes &attributes);
    // Appends every entry of the directory in this format, repeated (little-endian):
    //   u16 name length, name, attributes (see putAttributes()), u8 flags (1: has link target,
    //   2: has target attributes), [u16 target length, target], [target attributes]
    // Symbolic links get their target and the target's attributes so that listing needs no
    // further round trips.
    //
    // Attributes: u32 flags, u8 type, u64 size, u32 uid, u32 gid, u32 permissions, u64 atime,
    // u64 mtime.
    SftpError listDirectory(const std::string &path, ByteWriter &out);
    SftpError makeDirectory(const std::string &path, std::uint32_t mode);
    SftpError removeDirectory(const std::string &path);
    SftpError unlink(const std::string &path);
    SftpError rename(const std::string &path, const std::string &newPath);
    SftpError symlink(const std::string &target, const std::string &link);
    SftpError readLink(const std::string &path, std::string &target);
    SftpError realPath(const std::string &path, std::string &realPath);
    SftpError setAttributes(const std::string &path, const SftpAttributes &attributes);

    // flags are O_* open flags.
    SftpError open(const std::string &path, int flags, std::uint32_t mode, std::uint64_t &handle);
    // Reads up to size bytes at offset with many requests in flight; fewer only at the end of
    // the file.
    SftpError read(std::uint64_t handle, std::uint64_t offset, std::uint8_t *buffer,
                   std::size_t size, std::size_t &readSize);
    SftpError write(std::uint64_t handle, std::uint64_t offset, const std::uint8_t *buffer,
                    std::size_t size);
    SftpError fileAttributes(std::uint64_t handle, SftpAttributes &attributes);
    SftpError truncate(std::uint64_t handle, std::uint64_t size);
    SftpError close(std::uint64_t handle);

    static void putAttributes(ByteWriter &out, const SftpAttributes &attributes);

private:
    SftpClient() = default;

    SftpError lastError(const std::string &context);
    sftp_file_struct *findFile(std::uint64_t handle);

    std::mutex mutex_;
    ssh_session_struct *session_ = nullptr;
    sftp_session_struct *sftp_ = nullptr;
    std::size_t maxReadLength_ = 32 * 1024;
    std::size_t maxWriteLength_ = 32 * 1024;
    std::map<std::uint64_t, sftp_file_struct *> files_;
    // For truncate(), since SFTP v3 in libssh has no handle-based setstat.
    std::map<std::uint64_t, std::string> paths_;
    std::uint64_t nextHandle_ = 1;
    // Through jump hosts: session_ runs over the last tunnel, and each tunnel over the one before.
    std::vector<std::unique_ptr<SshTunnel>> tunnels_;
};

}  // namespace filepan
