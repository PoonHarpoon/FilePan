// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "filepan/securitykey/assertion.hpp"

#include <algorithm>
#include <utility>

#include "filepan/securitykey/ctap.hpp"
#include "filepan/securitykey/pin_protocol.hpp"

namespace filepan::securitykey {

namespace {

using Kind = KeyError::Kind;

// What the user can act on, from the key's status.
KeyError mapError(KeyError error) {
    if (error.kind != Kind::kCtap) {
        return error;
    }
    switch (error.status) {
    case ctap_status::kNoCredentials:
        return {Kind::kWrongKey, error.status, {}};
    case ctap_status::kOperationDenied:
    case ctap_status::kUserActionTimeout:
        return {Kind::kCancelled, error.status, {}};
    default:
        return error;
    }
}

KeyResult<Bytes> getPinToken(Ctap2 &ctap2, const PinUvAuthProtocol &protocol,
                             const std::string &rpId, bool withPermissions,
                             const PinSource &pinSource) {
    while (true) {
        auto retries = ctap2.getPinRetries();
        if (!retries && retries.error().kind != Kind::kCtap) {
            return std::unexpected(std::move(retries.error()));
        }
        const int left = retries ? *retries : -1;
        if (left == 0) {
            return keyFailure(Kind::kPinBlocked);
        }
        const auto pin = pinSource(left);
        if (!pin) {
            return keyFailure(Kind::kCancelled);
        }
        auto token = ctap2.getPinToken(protocol, *pin, rpId, withPermissions);
        if (token) {
            return token;
        }
        if (token.error().kind == Kind::kCtap) {
            switch (token.error().status) {
            case ctap_status::kPinInvalid:
                continue;
            case ctap_status::kPinBlocked:
            case ctap_status::kPinAuthBlocked:
                return keyFailure(Kind::kPinBlocked);
            default:
                break;
            }
        }
        return std::unexpected(std::move(token.error()));
    }
}

}  // namespace

KeyResult<SignResult> signWithSecurityKey(CtapTransport &transport, const SignRequest &request,
                                          const PinSource &pinSource) {
    const Bytes clientDataHash = sha256(request.data);
    const bool userPresence = (request.flags & kUserPresenceRequired) != 0;
    const bool userVerification = (request.flags & kUserVerificationRequired) != 0;
    const auto &application = request.application;

    std::optional<Ctap2::Info> info;
    if (transport.mayHaveCtap2()) {
        auto got = Ctap2(transport).getInfo();
        if (got) {
            info = std::move(*got);
        } else if (got.error().kind == Kind::kClosed) {
            return std::unexpected(std::move(got.error()));
        }
        // Otherwise a U2F-only key over NFC.
    }
    if (!info) {
        // U2F can only do ECDSA, and has no user verification.
        if (request.algorithm != kAlgorithmEcdsa || userVerification) {
            return keyFailure(Kind::kUnsupported);
        }
        const Bytes applicationHash = sha256(
            {reinterpret_cast<const std::uint8_t *>(application.data()), application.size()});
        auto response = U2f(transport).authenticate(clientDataHash, applicationHash,
                                                    request.keyHandle, userPresence);
        if (!response) {
            return std::unexpected(mapError(std::move(response.error())));
        }
        return SignResult{response->flags, response->counter, std::move(response->signature)};
    }

    Ctap2 ctap2(transport);
    const bool hasPin = info->option("clientPin");
    const bool hasBuiltInUv = info->option("uv");
    const auto &protocol = std::ranges::contains(info->pinUvAuthProtocols, 2)
                               ? PinUvAuthProtocol::two()
                               : PinUvAuthProtocol::one();
    const bool withPermissions = info->option("pinUvAuthToken");
    std::optional<Bytes> pinToken;
    if (userVerification) {
        if (hasPin) {
            auto token = getPinToken(ctap2, protocol, application, withPermissions, pinSource);
            if (!token) {
                return std::unexpected(std::move(token.error()));
            }
            pinToken = std::move(*token);
        } else if (!hasBuiltInUv) {
            return keyFailure(Kind::kPinRequired);
        }
    }
    while (true) {
        std::optional<Bytes> pinUvAuthParam;
        std::optional<int> pinUvAuthProtocol;
        if (pinToken) {
            pinUvAuthParam = protocol.authenticate(*pinToken, clientDataHash);
            pinUvAuthProtocol = protocol.version();
        }
        auto assertion = ctap2.getAssertion(application, clientDataHash, request.keyHandle,
                                            userPresence, userVerification && !pinToken,
                                            pinUvAuthParam, pinUvAuthProtocol);
        if (assertion) {
            return SignResult{assertion->flags(), assertion->counter(),
                              std::move(assertion->signature)};
        }
        // A key that always wants user verification.
        if (assertion.error().kind == Kind::kCtap
            && assertion.error().status == ctap_status::kPuatRequired && !pinToken && hasPin) {
            auto token = getPinToken(ctap2, protocol, application, withPermissions, pinSource);
            if (!token) {
                return std::unexpected(std::move(token.error()));
            }
            pinToken = std::move(*token);
            continue;
        }
        return std::unexpected(mapError(std::move(assertion.error())));
    }
}

}  // namespace filepan::securitykey
