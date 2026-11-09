// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "filepan/securitykey/cbor.hpp"
#include "filepan/securitykey/pin_protocol.hpp"
#include "filepan/securitykey/transport.hpp"

// The CTAP2 commands and the U2F request that signing needs.
namespace filepan::securitykey {

namespace ctap_command {
inline constexpr int kGetAssertion = 0x02;
inline constexpr int kGetInfo = 0x04;
inline constexpr int kClientPin = 0x06;
}  // namespace ctap_command

namespace client_pin {
inline constexpr int kGetPinRetries = 0x01;
inline constexpr int kGetKeyAgreement = 0x02;
inline constexpr int kGetPinToken = 0x05;
inline constexpr int kGetPinUvAuthTokenUsingPinWithPermissions = 0x09;
inline constexpr int kPermissionGetAssertion = 0x02;
}  // namespace client_pin

class Ctap2 {
public:
    struct Info {
        std::vector<std::string> versions;
        std::vector<std::pair<std::string, bool>> options;
        std::vector<int> pinUvAuthProtocols;

        // Whether the option is present and true.
        [[nodiscard]] bool option(std::string_view name) const;
    };

    struct Assertion {
        Bytes authenticatorData;
        Bytes signature;

        [[nodiscard]] std::uint8_t flags() const { return authenticatorData[32]; }
        [[nodiscard]] std::uint32_t counter() const;
    };

    explicit Ctap2(CtapTransport &transport) : transport_(transport) {}

    KeyResult<Info> getInfo();
    // pinUvAuthParam and pinUvAuthProtocol go together.
    KeyResult<Assertion> getAssertion(const std::string &rpId,
                                      std::span<const std::uint8_t> clientDataHash,
                                      std::span<const std::uint8_t> credentialId,
                                      bool userPresence, bool userVerification,
                                      const std::optional<Bytes> &pinUvAuthParam,
                                      std::optional<int> pinUvAuthProtocol);
    KeyResult<int> getPinRetries();
    // Exchanges the PIN for a token for signing for rpId, with permissions if the authenticator
    // supports them.
    KeyResult<Bytes> getPinToken(const PinUvAuthProtocol &protocol, const std::string &pin,
                                 const std::string &rpId, bool withPermissions);

private:
    KeyResult<CborValue> send(int command, const std::optional<CborValue> &request);

    CtapTransport &transport_;
};

// U2F (CTAP1) authentication, for authenticators without CTAP2.
class U2f {
public:
    static constexpr int kAuthenticate = 0x02;
    static constexpr int kSwNoError = 0x9000;
    static constexpr int kSwConditionsNotSatisfied = 0x6985;
    static constexpr int kSwWrongData = 0x6A80;
    static constexpr int kSwInsNotSupported = 0x6D00;

    struct Response {
        std::uint8_t flags = 0;
        std::uint32_t counter = 0;
        Bytes signature;
    };

    explicit U2f(CtapTransport &transport) : transport_(transport) {}

    KeyResult<Response> authenticate(std::span<const std::uint8_t> challenge,
                                     std::span<const std::uint8_t> applicationHash,
                                     std::span<const std::uint8_t> keyHandle, bool userPresence);

private:
    CtapTransport &transport_;
};

}  // namespace filepan::securitykey
