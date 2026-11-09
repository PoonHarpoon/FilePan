// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Runs the SFTP client against a real OpenSSH sshd on localhost.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>
#include <libssh/sk_api.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/sha256.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "filepan/sftp_client.hpp"
#include "sshd_harness.hpp"
#include "test_util.hpp"

using filepan::SecurityKeySignature;
using filepan::SecurityKeySigner;
using filepan::SecurityKeySignRequest;
using filepan::SftpAttributes;
using filepan::SftpClient;
using filepan::SftpError;
using filepan::SshAuthentication;
using filepan::SshHost;
using filepan::test::read;
using filepan::test::run;
using filepan::test::Sshd;
using filepan::test::TempDir;
namespace fs = std::filesystem;

namespace {

class Verifier : public filepan::SshHostKeyVerifier {
public:
    bool accept = true;
    std::vector<std::size_t> hops;
    std::vector<std::string> fingerprints;

    bool verify(std::size_t hop, const SshHost &, const std::string &keyType,
                const std::string &fingerprint, const std::string &publicKey) override {
        EXPECT(keyType == "ssh-ed25519");
        EXPECT(!publicKey.empty());
        hops.push_back(hop);
        fingerprints.push_back(fingerprint);
        return accept;
    }
};

std::unique_ptr<SftpClient> connect(const std::vector<SshHost> &chain, Verifier &verifier,
                                    SecurityKeySigner *signer = nullptr) {
    SftpError error;
    auto client = SftpClient::connect(chain, verifier, signer, 10, error);
    if (!client) {
        std::fprintf(stderr, "connect failed: %d hop %d %s\n", error.code, error.hop,
                     error.message.c_str());
    }
    return client;
}

void testHostKeysAndAuthentication(const Sshd &sshd, const fs::path &dir) {
    Verifier verifier;
    EXPECT(connect({sshd.host()}, verifier) != nullptr);
    EXPECT(verifier.hops == std::vector<std::size_t>{0});
    EXPECT(verifier.fingerprints[0] == sshd.hostFingerprint());

    Verifier rejecting;
    rejecting.accept = false;
    SftpError error;
    EXPECT(SftpClient::connect({sshd.host()}, rejecting, nullptr, 10, error) == nullptr);
    EXPECT(error.code == SftpError::kHostKeyRejected);
    EXPECT(error.hop == 0);

    // A key the server doesn't know.
    run(std::string(FILEPAN_SSH_KEYGEN) + " -q -t ed25519 -N '' -f " + (dir / "stranger").string());
    EXPECT(SftpClient::connect({sshd.host("stranger")}, verifier, nullptr, 10, error) == nullptr);
    EXPECT(error.code == SftpError::kAuthenticationFailed);

    // Passphrase-protected key, with the wrong and the right passphrase.
    EXPECT(SftpClient::connect({sshd.host("locked_key", "wrong")}, verifier, nullptr, 10,
                               error)
           == nullptr);
    EXPECT(error.code == SftpError::kAuthenticationFailed);
    EXPECT(connect({sshd.host("locked_key", "secret phrase")}, verifier) != nullptr);
}

void testJumpChain(const Sshd &sshd) {
    // Jump through the same server twice: hop 0 and hop 1 are jumps, hop 2 is the target.
    Verifier verifier;
    auto client = connect({sshd.host(), sshd.host(), sshd.host()}, verifier);
    EXPECT(client != nullptr);
    std::sort(verifier.hops.begin(), verifier.hops.end());
    EXPECT((verifier.hops == std::vector<std::size_t>{0, 1, 2}));
    SftpAttributes attributes;
    EXPECT(!client->stat("/", true, attributes));

    // A rejected key on the second jump is reported against that hop.
    class RejectSecond : public Verifier {
    public:
        bool verify(std::size_t hop, const SshHost &host, const std::string &keyType,
                    const std::string &fingerprint, const std::string &publicKey) override {
            Verifier::verify(hop, host, keyType, fingerprint, publicKey);
            return hop != 1;
        }
    } rejectSecond;
    SftpError error;
    EXPECT(SftpClient::connect({sshd.host(), sshd.host(), sshd.host()}, rejectSecond, nullptr, 10,
                               error)
           == nullptr);
    EXPECT(error.code == SftpError::kHostKeyRejected);
    EXPECT(error.hop == 1);

    // The user takes longer over a jump host's key than the timeout: only network waits count.
    class Slow : public Verifier {
    public:
        bool verify(std::size_t hop, const SshHost &host, const std::string &keyType,
                    const std::string &fingerprint, const std::string &publicKey) override {
            if (hop == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            }
            return Verifier::verify(hop, host, keyType, fingerprint, publicKey);
        }
    } slow;
    auto patient = SftpClient::connect({sshd.host(), sshd.host()}, slow, nullptr, 1, error);
    if (!patient) {
        std::fprintf(stderr, "slow jump host key: %s\n", error.message.c_str());
    }
    EXPECT(patient != nullptr);
    EXPECT(!patient->stat("/", true, attributes));

    // A failed step names itself; a tunnel to a closed port is put down to the host behind it.
    SshHost closed = sshd.host();
    closed.port = filepan::test::freePort();
    EXPECT(SftpClient::connect({sshd.host(), closed}, verifier, nullptr, 10, error) == nullptr);
    EXPECT(error.code == SftpError::kConnectFailed && error.hop == 1);
    EXPECT(error.message.starts_with("couldn't reach 127.0.0.1:" + std::to_string(closed.port)
                                     + " through jump host 1: "));
    EXPECT(SftpClient::connect({closed, sshd.host()}, verifier, nullptr, 10, error) == nullptr);
    EXPECT(error.code == SftpError::kConnectFailed && error.hop == 0);
    EXPECT(error.message.starts_with("couldn't connect: "));

    // Many connections through jump hosts come and go without disturbing each other.
    for (int i = 0; i < 3; ++i) {
        auto other = connect({sshd.host(), sshd.host()}, verifier);
        EXPECT(other != nullptr);
    }
    EXPECT(!client->stat("/", true, attributes));
    EXPECT(!patient->stat("/", true, attributes));
}

// Up-to-date OpenSSH servers locked down to post-quantum key exchange and modern algorithms:
// directly, and as jump hosts through one another. Then one whose ciphers we don't offer, which
// says so.
void testStrictAlgorithms() {
    const std::string strict =
        "KexAlgorithms mlkem768x25519-sha256,sntrup761x25519-sha512,"
        "sntrup761x25519-sha512@openssh.com\n"
        "HostKeyAlgorithms ssh-ed25519\n"
        "PubkeyAcceptedAlgorithms ssh-ed25519\n"
        "Ciphers chacha20-poly1305@openssh.com,aes256-gcm@openssh.com\n"
        "MACs hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com\n";
    // Each key exchange on its own, so both are known to work.
    for (const char *kex : {"mlkem768x25519-sha256", "sntrup761x25519-sha512"}) {
        TempDir dir;
        Sshd only(dir.path(), std::string("KexAlgorithms ") + kex
                                  + "\nCiphers aes256-gcm@openssh.com\n");
        Verifier verifier;
        if (connect({only.host()}, verifier) == nullptr) {
            std::fprintf(stderr, "with only %s\n", kex);
            std::exit(1);
        }
    }
    TempDir a, b, c;
    Sshd first(a.path(), strict);
    Sshd second(b.path(), strict);
    Sshd server(c.path(), strict);
    Verifier verifier;
    SftpAttributes attributes;
    for (const auto &chain : {std::vector<SshHost>{server.host()},
                              std::vector<SshHost>{first.host(), server.host()},
                              std::vector<SshHost>{first.host(), second.host(), server.host()}}) {
        auto client = connect(chain, verifier);
        EXPECT(client != nullptr);
        EXPECT(!client->stat("/", true, attributes));
    }

    // Nothing in common: the handshake says so, against the host it happened at.
    TempDir d;
    Sshd old(d.path(), "Ciphers aes128-cbc\n");
    SftpError error;
    EXPECT(SftpClient::connect({first.host(), old.host()}, verifier, nullptr, 10, error) == nullptr);
    std::fprintf(stderr, "no common cipher: hop %d: %s\n", error.hop, error.message.c_str());
    EXPECT(error.code == SftpError::kConnectFailed && error.hop == 1);
    EXPECT(error.message.starts_with("SSH handshake through jump host 1: no algorithms in common: "));
}

void testFileOperations(const Sshd &sshd, const fs::path &dir) {
    Verifier verifier;
    auto client = connect({sshd.host()}, verifier);
    EXPECT(client != nullptr);
    const std::string root = (dir / "remote").string();
    EXPECT(!client->makeDirectory(root, 0755));
    EXPECT(client->makeDirectory(root, 0755).code == SSH_FX_FAILURE
           || client->makeDirectory(root, 0755).code == SSH_FX_FILE_ALREADY_EXISTS);

    // Pipelined write and read of a file spanning many requests.
    std::string content(24 * 1024 * 1024 + 321, '\0');
    std::mt19937 random(3);
    for (auto &c : content) {
        c = static_cast<char>(random());
    }
    std::uint64_t handle;
    const std::string file = root + "/big.bin";
    EXPECT(!client->open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644, handle));
    // In two calls, the second at an offset.
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(content.data());
    EXPECT(!client->write(handle, 0, bytes, 10'000'000));
    EXPECT(!client->write(handle, 10'000'000, bytes + 10'000'000, content.size() - 10'000'000));
    EXPECT(!client->close(handle));
    EXPECT(read(file) == content);

    EXPECT(!client->open(file, O_RDONLY, 0, handle));
    std::string back(content.size() + 1000, '\0');
    std::size_t readSize = 0;
    const auto start = std::chrono::steady_clock::now();
    EXPECT(!client->read(handle, 0, reinterpret_cast<std::uint8_t *>(back.data()), back.size(),
                         readSize));
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("sftp_client_test: read %zu MiB in %.2f s\n", readSize >> 20, seconds);
    EXPECT(readSize == content.size());
    back.resize(readSize);
    EXPECT(back == content);
    // A read in the middle, and one past the end.
    std::string middle(100, '\0');
    EXPECT(!client->read(handle, 5'000'000, reinterpret_cast<std::uint8_t *>(middle.data()), 100,
                         readSize));
    EXPECT(readSize == 100 && middle == content.substr(5'000'000, 100));
    EXPECT(!client->read(handle, content.size() + 5, reinterpret_cast<std::uint8_t *>(middle.data()),
                         100, readSize));
    EXPECT(readSize == 0);
    SftpAttributes attributes;
    EXPECT(!client->fileAttributes(handle, attributes));
    EXPECT(attributes.size == content.size());
    EXPECT(!client->truncate(handle, 10));
    EXPECT(!client->close(handle));
    EXPECT(fs::file_size(file) == 10);
    EXPECT(client->close(handle).code == SftpError::kInvalidHandle);

    // Listing with a symbolic link and a broken one.
    EXPECT(!client->symlink("big.bin", root + "/link"));
    EXPECT(!client->symlink("missing", root + "/broken"));
    EXPECT(!client->makeDirectory(root + "/sub", 0700));
    filepan::ByteWriter listing;
    EXPECT(!client->listDirectory(root, listing));
    std::map<std::string, std::pair<std::uint8_t, std::string>> entries;
    for (std::size_t offset = 0; offset < listing.size();) {
        auto get = [&](auto &value) {
            std::memcpy(&value, listing.data() + offset, sizeof(value));
            offset += sizeof(value);
        };
        std::uint16_t nameLength;
        get(nameLength);
        std::string name(reinterpret_cast<const char *>(listing.data() + offset), nameLength);
        offset += nameLength;
        offset += 4 + 1 + 8 + 4 + 4 + 4 + 8 + 8;
        const std::uint8_t type = listing.data()[offset - (8 + 8 + 4 + 4 + 4 + 8 + 1)];
        std::uint8_t flags;
        get(flags);
        std::string target;
        if (flags & 1) {
            std::uint16_t targetLength;
            get(targetLength);
            target.assign(reinterpret_cast<const char *>(listing.data() + offset), targetLength);
            offset += targetLength;
        }
        if (flags & 2) {
            offset += 4 + 1 + 8 + 4 + 4 + 4 + 8 + 8;
        }
        entries[name] = {flags, target};
        (void)type;
    }
    EXPECT(entries.size() == 4);
    EXPECT(entries.at("big.bin").first == 0);
    EXPECT(entries.at("link").first == 3 && entries.at("link").second == "big.bin");
    EXPECT(entries.at("broken").first == 1 && entries.at("broken").second == "missing");

    std::string realPath;
    EXPECT(!client->realPath(root + "/sub/..", realPath));
    EXPECT(realPath == fs::canonical(root).string());
    EXPECT(!client->rename(file, root + "/renamed"));
    EXPECT(client->stat(file, false, attributes).code == SSH_FX_NO_SUCH_FILE);
    EXPECT(!client->unlink(root + "/renamed"));
    EXPECT(!client->removeDirectory(root + "/sub"));
    SftpAttributes mode;
    mode.flags = SSH_FILEXFER_ATTR_PERMISSIONS;
    mode.permissions = 0700;
    EXPECT(!client->setAttributes(root, mode));
    EXPECT((fs::status(root).permissions() & fs::perms::all) == fs::perms::owner_all);
    EXPECT(client->sendKeepAlive());
}

// A software FIDO authenticator holding P-256 keys, whose key handle is simply the private key.
// It signs like a real one: ECDSA over the authenticator data (SHA-256 of the application, flags,
// big-endian counter) followed by the SHA-256 of the client data.
class SoftwareSecurityKey : public SecurityKeySigner {
public:
    std::vector<SecurityKeySignRequest> requests;
    std::vector<std::thread::id> threads;
    int failure = 0;

    int sign(const SecurityKeySignRequest &request, SecurityKeySignature &signature) override {
        requests.push_back(request);
        threads.push_back(std::this_thread::get_id());
        if (failure) {
            return failure;
        }
        if (request.algorithm != SSH_SK_ECDSA || request.keyHandle.size() != 32) {
            return SSH_SK_ERR_UNSUPPORTED;
        }
        signature.flags = 0x01;  // User present.
        signature.counter = ++counter_;
        std::string authData = sha256(request.application);
        authData += static_cast<char>(signature.flags);
        for (int shift = 24; shift >= 0; shift -= 8) {
            authData += static_cast<char>(signature.counter >> shift);
        }
        const std::string hash = sha256(authData + sha256(request.data));

        mbedtls_ecp_keypair key;
        mbedtls_ecp_keypair_init(&key);
        EXPECT(mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &key,
                                    reinterpret_cast<const unsigned char *>(
                                        request.keyHandle.data()),
                                    request.keyHandle.size())
               == 0);
        mbedtls_ecdsa_context ecdsa;
        mbedtls_ecdsa_init(&ecdsa);
        EXPECT(mbedtls_ecdsa_from_keypair(&ecdsa, &key) == 0);
        unsigned char der[MBEDTLS_ECDSA_MAX_LEN];
        size_t derLength = 0;
        EXPECT(mbedtls_ecdsa_write_signature(
                   &ecdsa, MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char *>(hash.data()),
                   hash.size(), der, sizeof(der), &derLength, random, nullptr)
               == 0);
        mbedtls_ecdsa_free(&ecdsa);
        mbedtls_ecp_keypair_free(&key);
        signature.signature.assign(reinterpret_cast<const char *>(der), derLength);
        return 0;
    }

    // Makes a new key with libssh, through an enrollment callback, and returns it in OpenSSH's
    // private key format along with the authorized_keys line for it.
    static std::pair<std::string, std::string> enroll() {
        ssh_sk_callbacks_struct callbacks{};
        ssh_callbacks_init(&callbacks);
        callbacks.api_version = []() -> uint32_t { return SSH_SK_VERSION_MAJOR; };
        callbacks.enroll = enrollCallback;
        ssh_pki_ctx context = ssh_pki_ctx_new();
        const uint8_t flags = SSH_SK_USER_PRESENCE_REQD;
        EXPECT(ssh_pki_ctx_options_set(context, SSH_PKI_OPTION_SK_CALLBACKS, &callbacks)
               == SSH_OK);
        EXPECT(ssh_pki_ctx_options_set(context, SSH_PKI_OPTION_SK_APPLICATION, "ssh:") == SSH_OK);
        EXPECT(ssh_pki_ctx_options_set(context, SSH_PKI_OPTION_SK_FLAGS, &flags) == SSH_OK);
        ssh_key key = nullptr;
        EXPECT(ssh_pki_generate_key(SSH_KEYTYPE_SK_ECDSA, context, &key) == SSH_OK);
        char *privateKey = nullptr;
        EXPECT(ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &privateKey)
               == SSH_OK);
        char *publicKey = nullptr;
        EXPECT(ssh_pki_export_pubkey_base64(key, &publicKey) == SSH_OK);
        std::pair<std::string, std::string> result{
            privateKey,
            std::string(ssh_key_type_to_char(ssh_key_type(key))) + " " + publicKey + " sk\n"};
        ssh_string_free_char(privateKey);
        ssh_string_free_char(publicKey);
        ssh_key_free(key);
        ssh_pki_ctx_free(context);
        return result;
    }

private:
    static std::string sha256(const std::string &data) {
        unsigned char hash[32];
        EXPECT(mbedtls_sha256(reinterpret_cast<const unsigned char *>(data.data()), data.size(),
                              hash, 0)
               == 0);
        return {reinterpret_cast<const char *>(hash), sizeof(hash)};
    }

    static int random(void *, unsigned char *buffer, size_t size) {
        static std::mt19937 generator(std::random_device{}());
        for (size_t i = 0; i < size; ++i) {
            buffer[i] = static_cast<unsigned char>(generator());
        }
        return 0;
    }

    static uint8_t *copy(const unsigned char *bytes, size_t size) {
        auto *copy = static_cast<uint8_t *>(std::malloc(size));
        std::memcpy(copy, bytes, size);
        return copy;
    }

    static int enrollCallback(uint32_t algorithm, const uint8_t *, size_t, const char *,
                              uint8_t flags, const char *, struct sk_option **,
                              struct sk_enroll_response **response) {
        EXPECT(algorithm == SSH_SK_ECDSA);
        mbedtls_ecp_keypair key;
        mbedtls_ecp_keypair_init(&key);
        EXPECT(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &key, random, nullptr) == 0);
        unsigned char privateKey[32];
        size_t privateKeyLength = 0;
        EXPECT(mbedtls_ecp_write_key_ext(&key, &privateKeyLength, privateKey, sizeof(privateKey))
               == 0);
        EXPECT(privateKeyLength == sizeof(privateKey));
        unsigned char publicKey[65];
        size_t publicKeyLength = 0;
        EXPECT(mbedtls_ecp_write_public_key(&key, MBEDTLS_ECP_PF_UNCOMPRESSED, &publicKeyLength,
                                            publicKey, sizeof(publicKey))
               == 0);
        mbedtls_ecp_keypair_free(&key);
        auto *result =
            static_cast<sk_enroll_response *>(std::calloc(1, sizeof(sk_enroll_response)));
        result->flags = flags;
        result->public_key = copy(publicKey, publicKeyLength);
        result->public_key_len = publicKeyLength;
        result->key_handle = copy(privateKey, sizeof(privateKey));
        result->key_handle_len = sizeof(privateKey);
        *response = result;
        return SSH_OK;
    }

    std::uint32_t counter_ = 0;
};

void testSecurityKey(const Sshd &sshd, const fs::path &dir) {
    const auto [privateKey, authorizedKey] = SoftwareSecurityKey::enroll();
    EXPECT(authorizedKey.starts_with("sk-ecdsa-sha2-nistp256@openssh.com "));
    std::ofstream(dir / "sk_key") << privateKey;
    std::ofstream(dir / "authorized_keys", std::ios::app) << authorizedKey;

    // Through a jump host, which authenticates on the connecting thread too.
    SoftwareSecurityKey securityKey;
    Verifier verifier;
    auto client = connect({sshd.host("sk_key"), sshd.host("sk_key")}, verifier, &securityKey);
    EXPECT(client != nullptr);
    EXPECT(securityKey.requests.size() == 2);
    EXPECT(securityKey.requests[0].hop == 0);
    EXPECT(securityKey.requests[1].hop == 1);
    EXPECT(securityKey.threads[0] == securityKey.threads[1]);
    for (const auto &request : securityKey.requests) {
        EXPECT(request.application == "ssh:");
        EXPECT(request.flags == SSH_SK_USER_PRESENCE_REQD);
        EXPECT(!request.data.empty());
    }
    SftpAttributes attributes;
    EXPECT(!client->stat(dir.string(), true, attributes));

    // A missing or failing security key fails authentication without crashing.
    SftpError error;
    EXPECT(SftpClient::connect({sshd.host("sk_key")}, verifier, nullptr, 10, error) == nullptr);
    EXPECT(error.code == SftpError::kAuthenticationFailed);
    securityKey.failure = SSH_SK_ERR_DEVICE_NOT_FOUND;
    EXPECT(SftpClient::connect({sshd.host("sk_key")}, verifier, &securityKey, 10, error)
           == nullptr);
    EXPECT(error.code == SftpError::kAuthenticationFailed);
}

}  // namespace

int main() {
    TempDir dir;
    Sshd sshd(dir.path());
    testHostKeysAndAuthentication(sshd, dir.path());
    testJumpChain(sshd);
    testStrictAlgorithms();
    testFileOperations(sshd, dir.path());
    testSecurityKey(sshd, dir.path());
    std::puts("sftp_client_test: OK");
    return 0;
}
