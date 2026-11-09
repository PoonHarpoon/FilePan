// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

// A FIDO authenticator in software, written from the authenticator's side of CTAP 2.1 (and
// U2F), to test the security key client against; and an SSH "sk-" key enrolled on it.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/sk_api.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>

#include "filepan/securitykey/cbor.hpp"
#include "filepan/securitykey/ctap.hpp"
#include "filepan/securitykey/pin_protocol.hpp"
#include "filepan/securitykey/transport.hpp"
#include "test_util.hpp"

namespace filepan::test {

using namespace filepan::securitykey;

inline Bytes bytesOf(std::string_view text) { return {text.begin(), text.end()}; }

inline Bytes concat(std::initializer_list<std::span<const std::uint8_t>> parts) {
    Bytes out;
    for (const auto part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

inline Bytes bigEndian32(std::uint32_t value) {
    return {static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
            static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
}

inline int random(void *, unsigned char *buffer, std::size_t size) {
    fillRandom({buffer, size});
    return 0;
}

inline Bytes ecdsaSign(const P256KeyPair &key, std::span<const std::uint8_t> message) {
    const Bytes hash = sha256(message);
    mbedtls_ecp_keypair keypair;
    mbedtls_ecp_keypair_init(&keypair);
    EXPECT(mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &keypair, key.privateKey.data(), 32) == 0);
    unsigned char der[MBEDTLS_ECDSA_MAX_LEN];
    std::size_t length = 0;
    EXPECT(mbedtls_ecdsa_write_signature(&keypair, MBEDTLS_MD_SHA256, hash.data(), hash.size(), der,
                                         sizeof(der), &length, random, nullptr)
           == 0);
    mbedtls_ecp_keypair_free(&keypair);
    return {der, der + length};
}

inline bool ecdsaVerifies(const P256KeyPair &key, std::span<const std::uint8_t> message,
                   std::span<const std::uint8_t> signature) {
    const Bytes hash = sha256(message);
    const Bytes point = concat({Bytes{0x04}, key.x, key.y});
    mbedtls_ecp_keypair keypair;
    mbedtls_ecp_keypair_init(&keypair);
    mbedtls_ecp_point q;
    mbedtls_ecp_point_init(&q);
    mbedtls_ecp_group group;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    EXPECT(mbedtls_ecp_point_read_binary(&group, &q, point.data(), point.size()) == 0);
    EXPECT(mbedtls_ecp_set_public_key(MBEDTLS_ECP_DP_SECP256R1, &keypair, &q) == 0);
    const bool verified = mbedtls_ecdsa_read_signature(&keypair, hash.data(), hash.size(),
                                                       signature.data(), signature.size())
                          == 0;
    mbedtls_ecp_group_free(&group);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_keypair_free(&keypair);
    return verified;
}

struct Failure {
    int status;
};

struct AuthenticatorOptions {
    bool ctap2 = true;
    std::optional<std::string> pin = std::nullopt;
    std::vector<int> protocols{2, 1};
    bool supportsPermissions = true;
    bool alwaysUv = false;
    // Over NFC, a U2F-only key rejects CTAP2 at the APDU level, which the transport
    // reports as an I/O failure.
    bool ctap2FailsAsIo = false;
};

class SoftwareAuthenticator final : public CtapTransport {
public:
    using Options = AuthenticatorOptions;

    explicit SoftwareAuthenticator(Options options = {}) : options_(std::move(options)) {}

    std::map<Bytes, P256KeyPair> credentials;
    int pinRetries = 8;
    std::uint32_t counter = 0;
    std::vector<int> commands;
    int touches = 0;

    Bytes addCredential(std::size_t length = 64) {
        Bytes handle(length);
        fillRandom(handle);
        credentials[handle] = generateP256KeyPair();
        return handle;
    }

    [[nodiscard]] bool mayHaveCtap2() const override {
        return options_.ctap2 || options_.ctap2FailsAsIo;
    }
    [[nodiscard]] bool isUserPresent() const override { return false; }
    void close() override { closed_ = true; }
    [[nodiscard]] bool isClosed() const override { return closed_; }

    KeyResult<Bytes> sendCbor(int command, std::span<const std::uint8_t> data) override {
        commands.push_back(command);
        if (!options_.ctap2) {
            return keyFailure(KeyError::Kind::kIo, "CTAP2 not supported");
        }
        std::optional<CborValue> request = data.empty() ? CborValue(CborMap{}) : cborDecode(data);
        EXPECT(request && request->map());
        std::variant<CborValue, Failure> response = Failure{ctap_status::kInvalidCommand};
        switch (command) {
        case ctap_command::kGetInfo:
            response = getInfo();
            break;
        case ctap_command::kGetAssertion:
            response = getAssertion(*request);
            break;
        case ctap_command::kClientPin:
            response = clientPin(*request);
            break;
        default:
            break;
        }
        if (const auto *failure = std::get_if<Failure>(&response)) {
            return Bytes{static_cast<std::uint8_t>(failure->status)};
        }
        Bytes out{0};
        cborEncode(std::get<CborValue>(response), out);
        return out;
    }

    KeyResult<ApduResponse> sendApdu(int ins, int p1, int, std::span<const std::uint8_t> data) override {
        if (ins != U2f::kAuthenticate) {
            return ApduResponse{{}, U2f::kSwInsNotSupported};
        }
        const auto challenge = data.subspan(0, 32);
        const auto applicationHash = data.subspan(32, 32);
        const Bytes keyHandle(data.begin() + 65, data.begin() + 65 + data[64]);
        const auto found = credentials.find(keyHandle);
        if (found == credentials.end()) {
            return ApduResponse{{}, U2f::kSwWrongData};
        }
        const std::uint8_t flags = p1 == 0x03 ? 0x01 : 0x00;
        if (flags && onUserPresenceNeeded) {
            onUserPresenceNeeded();
        }
        ++touches;
        const Bytes count = bigEndian32(++counter);
        const Bytes signature =
            ecdsaSign(found->second, concat({applicationHash, Bytes{flags}, count, challenge}));
        return ApduResponse{concat({Bytes{flags}, count, signature}), U2f::kSwNoError};
    }

private:
    using Response = std::variant<CborValue, Failure>;

    Response getInfo() {
        CborMap options{{"up", true}, {"rk", true}, {"clientPin", options_.pin.has_value()}};
        if (options_.supportsPermissions) {
            options.emplace_back("pinUvAuthToken", true);
        }
        if (options_.alwaysUv) {
            options.emplace_back("alwaysUv", true);
        }
        CborArray protocols;
        for (const int protocol : options_.protocols) {
            protocols.emplace_back(protocol);
        }
        return CborValue(CborMap{
            {1, CborArray{"FIDO_2_0", "FIDO_2_1", "U2F_V2"}},
            {3, Bytes(16, 0)},
            {4, std::move(options)},
            {6, std::move(protocols)},
        });
    }

    Response getAssertion(const CborValue &request) {
        const std::string &rpId = *request.find(1)->text();
        const Bytes &clientDataHash = *request.find(2)->bytes();
        const auto &allowList = *request.find(3)->array();
        const CborValue *options = request.find(5);
        const CborValue *pinUvAuthParam = request.find(6);
        const CborValue *up = options ? options->find("up") : nullptr;
        const bool userPresence = up ? *up->boolean() : true;
        const CborValue *uv = options ? options->find("uv") : nullptr;
        bool userVerified = false;
        if (pinUvAuthParam) {
            const auto *protocol = this->protocol(static_cast<int>(*request.find(7)->integer()));
            if (!protocol) {
                return Failure{ctap_status::kInvalidParameter};
            }
            if (!pinToken_) {
                return Failure{ctap_status::kPinAuthInvalid};
            }
            if (protocol->authenticate(*pinToken_, clientDataHash) != *pinUvAuthParam->bytes()) {
                return Failure{ctap_status::kPinAuthInvalid};
            }
            userVerified = true;
        } else if (uv && *uv->boolean()) {
            // No built-in user verification.
            return Failure{ctap_status::kInvalidOption};
        } else if (options_.alwaysUv) {
            return Failure{ctap_status::kPuatRequired};
        }
        EXPECT(allowList.size() == 1);
        const Bytes &credentialId = *allowList[0].find("id")->bytes();
        const auto found = credentials.find(credentialId);
        if (found == credentials.end()) {
            return Failure{ctap_status::kNoCredentials};
        }
        std::uint8_t flags = 0;
        if (userPresence) {
            if (onUserPresenceNeeded) {
                onUserPresenceNeeded();
            }
            ++touches;
            flags |= 0x01;
        }
        if (userVerified) {
            flags |= 0x04;
        }
        const Bytes authenticatorData =
            concat({sha256(bytesOf(rpId)), Bytes{flags}, bigEndian32(++counter)});
        return CborValue(CborMap{
            {1, CborMap{{"id", credentialId}, {"type", "public-key"}}},
            {2, authenticatorData},
            {3, ecdsaSign(found->second, concat({authenticatorData, clientDataHash}))},
        });
    }

    Response clientPin(const CborValue &request) {
        switch (*request.find(2)->integer()) {
        case client_pin::kGetPinRetries:
            return CborValue(CborMap{{3, pinRetries}});
        case client_pin::kGetKeyAgreement:
            if (!protocol(static_cast<int>(*request.find(1)->integer()))) {
                return Failure{ctap_status::kInvalidParameter};
            }
            return CborValue(CborMap{{1, encodeCoseKey(keyAgreement_.x, keyAgreement_.y)}});
        case client_pin::kGetPinToken:
            return issueToken(request);
        case client_pin::kGetPinUvAuthTokenUsingPinWithPermissions: {
            if (!options_.supportsPermissions) {
                return Failure{ctap_status::kInvalidCommand};
            }
            const auto *permissions = request.find(9);
            const auto *rpId = request.find(10);
            if (!permissions || *permissions->integer() != client_pin::kPermissionGetAssertion
                || !rpId || !rpId->text()) {
                return Failure{ctap_status::kUnauthorizedPermission};
            }
            return issueToken(request);
        }
        default:
            return Failure{ctap_status::kInvalidCommand};
        }
    }

    Response issueToken(const CborValue &request) {
        const auto *protocol = this->protocol(static_cast<int>(*request.find(1)->integer()));
        if (!protocol) {
            return Failure{ctap_status::kInvalidParameter};
        }
        if (!options_.pin) {
            return Failure{ctap_status::kPinNotSet};
        }
        if (pinRetries == 0) {
            return Failure{ctap_status::kPinBlocked};
        }
        const auto platformKey = decodeCoseKey(*request.find(3));
        EXPECT(platformKey);
        const auto z = p256Ecdh(keyAgreement_.privateKey, platformKey->first, platformKey->second);
        EXPECT(z);
        const Bytes sharedSecret = protocol->kdf(*z);
        const auto pinHash = protocol->decrypt(sharedSecret, *request.find(6)->bytes());
        EXPECT(pinHash);
        Bytes expected = sha256(bytesOf(*options_.pin));
        expected.resize(16);
        if (*pinHash != expected) {
            --pinRetries;
            return Failure{pinRetries == 0 ? ctap_status::kPinBlocked : ctap_status::kPinInvalid};
        }
        pinRetries = 8;
        Bytes token(32);
        fillRandom(token);
        pinToken_ = token;
        return CborValue(CborMap{{2, protocol->encrypt(sharedSecret, token)}});
    }

    const PinUvAuthProtocol *protocol(int version) const {
        if (!std::ranges::contains(options_.protocols, version)) {
            return nullptr;
        }
        return version == 2 ? &PinUvAuthProtocol::two() : &PinUvAuthProtocol::one();
    }

    Options options_;
    P256KeyPair keyAgreement_ = generateP256KeyPair();
    std::optional<Bytes> pinToken_;
    bool closed_ = false;
};

// A connection to a SoftwareAuthenticator that stays put, as a key plugged in again is the same
// key: what the backend gets from a SecurityKeySource.
class AuthenticatorLink final : public CtapTransport {
public:
    explicit AuthenticatorLink(SoftwareAuthenticator &authenticator) : authenticator_(authenticator) {}

    [[nodiscard]] bool mayHaveCtap2() const override { return authenticator_.mayHaveCtap2(); }
    [[nodiscard]] bool isUserPresent() const override { return false; }
    KeyResult<Bytes> sendCbor(int command, std::span<const std::uint8_t> data) override {
        if (closed_) {
            return keyFailure(KeyError::Kind::kClosed);
        }
        authenticator_.onUserPresenceNeeded = onUserPresenceNeeded;
        return authenticator_.sendCbor(command, data);
    }
    KeyResult<ApduResponse> sendApdu(int ins, int p1, int p2, std::span<const std::uint8_t> data) override {
        if (closed_) {
            return keyFailure(KeyError::Kind::kClosed);
        }
        authenticator_.onUserPresenceNeeded = onUserPresenceNeeded;
        return authenticator_.sendApdu(ins, p1, p2, data);
    }
    void close() override { closed_ = true; }
    [[nodiscard]] bool isClosed() const override { return closed_; }

private:
    SoftwareAuthenticator &authenticator_;
    bool closed_ = false;
};

// Enrolls a new credential on the authenticator as an OpenSSH sk-ecdsa key: its private key
// text and its authorized_keys line. flags: SSH_SK_USER_PRESENCE_REQD and
// SSH_SK_USER_VERIFICATION_REQD.
inline std::pair<std::string, std::string> enrollSshKey(SoftwareAuthenticator &authenticator,
                                                        std::uint8_t flags = SSH_SK_USER_PRESENCE_REQD) {
    // libssh's callbacks take no user data.
    static SoftwareAuthenticator *enrolling;
    enrolling = &authenticator;
    ssh_sk_callbacks_struct callbacks{};
    ssh_callbacks_init(&callbacks);
    callbacks.api_version = []() -> uint32_t { return SSH_SK_VERSION_MAJOR; };
    callbacks.enroll = [](uint32_t algorithm, const uint8_t *, size_t, const char *, uint8_t keyFlags,
                          const char *, struct sk_option **, struct sk_enroll_response **response) -> int {
        EXPECT(algorithm == SSH_SK_ECDSA);
        const Bytes handle = enrolling->addCredential();
        const P256KeyPair &key = enrolling->credentials.at(handle);
        const Bytes publicKey = concat({Bytes{0x04}, key.x, key.y});
        const auto copy = [](const Bytes &bytes) {
            auto *out = static_cast<uint8_t *>(std::malloc(bytes.size()));
            std::memcpy(out, bytes.data(), bytes.size());
            return out;
        };
        auto *result = static_cast<sk_enroll_response *>(std::calloc(1, sizeof(sk_enroll_response)));
        result->flags = keyFlags;
        result->public_key = copy(publicKey);
        result->public_key_len = publicKey.size();
        result->key_handle = copy(handle);
        result->key_handle_len = handle.size();
        *response = result;
        return SSH_OK;
    };
    ssh_pki_ctx context = ssh_pki_ctx_new();
    EXPECT(ssh_pki_ctx_options_set(context, SSH_PKI_OPTION_SK_CALLBACKS, &callbacks) == SSH_OK);
    EXPECT(ssh_pki_ctx_options_set(context, SSH_PKI_OPTION_SK_APPLICATION, "ssh:") == SSH_OK);
    EXPECT(ssh_pki_ctx_options_set(context, SSH_PKI_OPTION_SK_FLAGS, &flags) == SSH_OK);
    ssh_key key = nullptr;
    EXPECT(ssh_pki_generate_key(SSH_KEYTYPE_SK_ECDSA, context, &key) == SSH_OK);
    char *privateKey = nullptr;
    EXPECT(ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &privateKey) == SSH_OK);
    char *publicKey = nullptr;
    EXPECT(ssh_pki_export_pubkey_base64(key, &publicKey) == SSH_OK);
    std::pair<std::string, std::string> result{
            privateKey, std::string(ssh_key_type_to_char(ssh_key_type(key))) + " " + publicKey + " sk\n"};
    ssh_string_free_char(privateKey);
    ssh_string_free_char(publicKey);
    ssh_key_free(key);
    ssh_pki_ctx_free(context);
    return result;
}

}  // namespace filepan::test
