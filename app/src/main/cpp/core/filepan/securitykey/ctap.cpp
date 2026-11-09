// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/securitykey/ctap.hpp"

#include <chrono>
#include <thread>

namespace filepan::securitykey {

bool Ctap2::Info::option(std::string_view name) const {
    for (const auto &[key, value] : options) {
        if (key == name) {
            return value;
        }
    }
    return false;
}

std::uint32_t Ctap2::Assertion::counter() const {
    std::uint32_t counter = 0;
    for (int i = 33; i < 37; ++i) {
        counter = (counter << 8) | authenticatorData[i];
    }
    return counter;
}

KeyResult<Ctap2::Info> Ctap2::getInfo() {
    auto response = send(ctap_command::kGetInfo, std::nullopt);
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    Info info;
    if (const auto *versions = response->find(1); versions && versions->array()) {
        for (const auto &version : *versions->array()) {
            if (const auto *text = version.text()) {
                info.versions.push_back(*text);
            }
        }
    }
    if (const auto *options = response->find(4); options && options->map()) {
        for (const auto &[key, value] : *options->map()) {
            if (key.text() && value.boolean()) {
                info.options.emplace_back(*key.text(), *value.boolean());
            }
        }
    }
    if (const auto *protocols = response->find(6); protocols && protocols->array()) {
        for (const auto &protocol : *protocols->array()) {
            if (const auto *version = protocol.integer()) {
                info.pinUvAuthProtocols.push_back(static_cast<int>(*version));
            }
        }
    } else {
        // CTAP 2.0 authenticators without the field support protocol one.
        info.pinUvAuthProtocols.push_back(1);
    }
    return info;
}

KeyResult<Ctap2::Assertion> Ctap2::getAssertion(const std::string &rpId,
                                                std::span<const std::uint8_t> clientDataHash,
                                                std::span<const std::uint8_t> credentialId,
                                                bool userPresence, bool userVerification,
                                                const std::optional<Bytes> &pinUvAuthParam,
                                                std::optional<int> pinUvAuthProtocol) {
    CborMap options;
    // Presence is the default; say so only to turn it off.
    if (!userPresence) {
        options.emplace_back("up", false);
    }
    if (userVerification) {
        options.emplace_back("uv", true);
    }
    CborMap request{
        {1, rpId},
        {2, Bytes(clientDataHash.begin(), clientDataHash.end())},
        {3, CborArray{CborMap{{"id", Bytes(credentialId.begin(), credentialId.end())},
                              {"type", "public-key"}}}},
    };
    if (!options.empty()) {
        request.emplace_back(5, std::move(options));
    }
    if (pinUvAuthParam && pinUvAuthProtocol) {
        request.emplace_back(6, *pinUvAuthParam);
        request.emplace_back(7, *pinUvAuthProtocol);
    }
    auto response = send(ctap_command::kGetAssertion, CborValue(std::move(request)));
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    const auto *authenticatorData = response->find(2);
    const auto *signature = response->find(3);
    if (!authenticatorData || !authenticatorData->bytes()
        || authenticatorData->bytes()->size() < 37 || !signature || !signature->bytes()) {
        return keyFailure(KeyError::Kind::kIo, "Malformed assertion");
    }
    return Assertion{*authenticatorData->bytes(), *signature->bytes()};
}

KeyResult<int> Ctap2::getPinRetries() {
    auto response =
        send(ctap_command::kClientPin, CborValue(CborMap{{1, 1}, {2, client_pin::kGetPinRetries}}));
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    const auto *retries = response->find(3);
    if (!retries || !retries->integer()) {
        return keyFailure(KeyError::Kind::kIo, "Malformed PIN retries");
    }
    return static_cast<int>(*retries->integer());
}

KeyResult<Bytes> Ctap2::getPinToken(const PinUvAuthProtocol &protocol, const std::string &pin,
                                    const std::string &rpId, bool withPermissions) {
    auto agreement = send(ctap_command::kClientPin,
                          CborValue(CborMap{{1, protocol.version()},
                                            {2, client_pin::kGetKeyAgreement}}));
    if (!agreement) {
        return std::unexpected(std::move(agreement.error()));
    }
    const auto *keyAgreement = agreement->find(1);
    if (!keyAgreement || !keyAgreement->map()) {
        return keyFailure(KeyError::Kind::kIo, "Malformed key agreement");
    }
    auto encapsulation = protocol.encapsulate(*keyAgreement);
    if (!encapsulation) {
        return std::unexpected(std::move(encapsulation.error()));
    }
    Bytes pinHash = sha256({reinterpret_cast<const std::uint8_t *>(pin.data()), pin.size()});
    pinHash.resize(16);
    CborMap request{
        {1, protocol.version()},
        {3, std::move(encapsulation->platformKey)},
        {6, protocol.encrypt(encapsulation->sharedSecret, pinHash)},
    };
    if (withPermissions) {
        request.emplace_back(2, client_pin::kGetPinUvAuthTokenUsingPinWithPermissions);
        request.emplace_back(9, client_pin::kPermissionGetAssertion);
        request.emplace_back(10, rpId);
    } else {
        request.emplace_back(2, client_pin::kGetPinToken);
    }
    auto response = send(ctap_command::kClientPin, CborValue(std::move(request)));
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    const auto *encryptedToken = response->find(2);
    if (!encryptedToken || !encryptedToken->bytes()) {
        return keyFailure(KeyError::Kind::kIo, "Malformed PIN token");
    }
    return protocol.decrypt(encapsulation->sharedSecret, *encryptedToken->bytes());
}

KeyResult<CborValue> Ctap2::send(int command, const std::optional<CborValue> &request) {
    const Bytes data = request ? cborEncode(*request) : Bytes();
    auto response = transport_.sendCbor(command, data);
    if (!response) {
        return std::unexpected(std::move(response.error()));
    }
    if (response->empty()) {
        return keyFailure(KeyError::Kind::kIo, "Empty CTAP2 response");
    }
    if (const int status = (*response)[0]; status != 0) {
        return ctapFailure(status);
    }
    if (response->size() == 1) {
        return CborValue(CborMap{});
    }
    auto value = cborDecode(std::span(*response).subspan(1));
    if (!value || !value->map()) {
        return keyFailure(KeyError::Kind::kIo, "Malformed response");
    }
    return std::move(*value);
}

KeyResult<U2f::Response> U2f::authenticate(std::span<const std::uint8_t> challenge,
                                           std::span<const std::uint8_t> applicationHash,
                                           std::span<const std::uint8_t> keyHandle,
                                           bool userPresence) {
    if (keyHandle.size() > 255) {
        return keyFailure(KeyError::Kind::kUnsupported, "Key handle too long for U2F");
    }
    Bytes data(challenge.begin(), challenge.end());
    data.insert(data.end(), applicationHash.begin(), applicationHash.end());
    data.push_back(static_cast<std::uint8_t>(keyHandle.size()));
    data.insert(data.end(), keyHandle.begin(), keyHandle.end());
    // "Enforce user presence and sign", or "don't enforce".
    const int p1 = userPresence ? 0x03 : 0x08;
    while (true) {
        auto response = transport_.sendApdu(kAuthenticate, p1, 0, data);
        if (!response) {
            return std::unexpected(std::move(response.error()));
        }
        switch (response->status) {
        case kSwNoError: {
            const Bytes &bytes = response->data;
            if (bytes.size() < 6) {
                return keyFailure(KeyError::Kind::kIo, "Malformed U2F response");
            }
            Response result;
            result.flags = bytes[0];
            for (int i = 1; i < 5; ++i) {
                result.counter = (result.counter << 8) | bytes[i];
            }
            result.signature.assign(bytes.begin() + 5, bytes.end());
            return result;
        }
        case kSwConditionsNotSatisfied:
            // Waiting for a touch.
            if (transport_.onUserPresenceNeeded) {
                transport_.onUserPresenceNeeded();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (transport_.isClosed()) {
                return keyFailure(KeyError::Kind::kClosed);
            }
            break;
        case kSwWrongData:
            return ctapFailure(ctap_status::kNoCredentials);
        default:
            return keyFailure(KeyError::Kind::kIo, "U2F error " + std::to_string(response->status));
        }
    }
}

}  // namespace filepan::securitykey
