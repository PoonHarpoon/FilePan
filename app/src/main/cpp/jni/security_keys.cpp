// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "security_keys.hpp"

#include <android/log.h>

#include <cstdint>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

#include "filepan/securitykey/ccid.hpp"
#include "filepan/securitykey/ctaphid.hpp"
#include "filepan/securitykey/nfc.hpp"
#include "filepan/securitykey/openpgp_card.hpp"
#include "jni_util.hpp"

namespace filepan::jni {

namespace {

using securitykey::Bytes;
using Kind = vfs::SecurityKeySession::Kind;
using Problem = vfs::SecurityKeySession::Problem;

constexpr const char *kSecurityKeysClass = "app/phsp/filepan/SecurityKeys";

struct Methods {
    jmethodID start = nullptr;
    jmethodID stop = nullptr;
    jmethodID transceive = nullptr;
    jmethodID closeKey = nullptr;
};

// Closes the key on the Kotlin side, from any thread.
void closeKey(jobject keys, jmethodID method, jint token) {
    JNIEnv *env = currentEnv();
    if (env == nullptr) {
        return;
    }
    env->CallVoidMethod(keys, method, token);
    clearException(env, "closeKey");
}

// An NFC key's IsoDep, through SecurityKeys.transceive.
class JniApduChannel final : public securitykey::ApduChannel {
public:
    JniApduChannel(jobject keys, const Methods &methods, jint token, bool extendedLength)
        : keys_(keys), methods_(methods), token_(token), extendedLength_(extendedLength) {}
    ~JniApduChannel() override { close(); }

    [[nodiscard]] bool supportsExtendedLength() const override { return extendedLength_; }

    std::optional<Bytes> transceive(std::span<const std::uint8_t> apdu) override {
        JNIEnv *env = currentEnv();
        if (env == nullptr) {
            return std::nullopt;
        }
        LocalFrame frame(env);
        auto response = static_cast<jbyteArray>(
                env->CallObjectMethod(keys_, methods_.transceive, token_, newByteArray(env, apdu)));
        if (clearException(env, "transceive") || response == nullptr) {
            return std::nullopt;
        }
        const std::string bytes = bytesToString(env, response);
        return Bytes(bytes.begin(), bytes.end());
    }

    void close() override { closeKey(keys_, methods_.closeKey, token_); }

private:
    jobject keys_;
    const Methods &methods_;
    jint token_;
    bool extendedLength_;
};

class JniSecurityKeySource final : public vfs::SecurityKeySource {
public:
    JniSecurityKeySource(JNIEnv *env, jobject keys, Methods methods)
        : keys_(env->NewGlobalRef(keys)), methods_(methods) {}

    void start(std::shared_ptr<vfs::SecurityKeySession> session) override {
        const Kind kind = session->kind();
        {
            std::lock_guard lock(mutex_);
            session_ = std::move(session);
        }
        JNIEnv *env = currentEnv();
        if (env != nullptr) {
            // Over USB, an OpenPGP card is behind the key's CCID interface, not its FIDO one.
            const jboolean card = kind == Kind::kOpenPgpCard ? JNI_TRUE : JNI_FALSE;
            env->CallVoidMethod(keys_, methods_.start, reinterpret_cast<jlong>(this), card);
            clearException(env, "SecurityKeys.start");
        }
    }

    void stop() override {
        {
            std::lock_guard lock(mutex_);
            session_.reset();
        }
        JNIEnv *env = currentEnv();
        if (env != nullptr) {
            env->CallVoidMethod(keys_, methods_.stop);
            clearException(env, "SecurityKeys.stop");
        }
    }

    // On SecurityKeys' opener thread. Opening talks to the key, which may take a moment.
    void usbKey(int fd, int inEndpoint, int outEndpoint, int packetSize, jint token) {
        const auto session = current();
        auto pipe = std::make_unique<securitykey::UsbfsHidPipe>(
                fd, static_cast<std::uint8_t>(inEndpoint), static_cast<std::uint8_t>(outEndpoint),
                static_cast<std::size_t>(packetSize),
                [keys = keys_, method = methods_.closeKey, token] { closeKey(keys, method, token); });
        if (!session) {
            return;  // Gone meanwhile: the pipe releases the key.
        }
        auto transport = securitykey::CtapHidTransport::open(std::move(pipe));
        if (!transport) {
            session->offerProblem(vfs::SecurityKeySession::Problem::kNotASecurityKey);
            return;
        }
        session->offerTransport(std::move(*transport));
    }

    // An OpenPGP card's CCID interface, claimed; on the opener thread too.
    void usbCard(int fd, int inEndpoint, int outEndpoint, int packetSize, jint token) {
        const auto session = current();
        auto pipe = std::make_unique<securitykey::UsbfsBulkPipe>(
                fd, static_cast<std::uint8_t>(inEndpoint), static_cast<std::uint8_t>(outEndpoint),
                static_cast<std::size_t>(packetSize),
                [keys = keys_, method = methods_.closeKey, token] { closeKey(keys, method, token); });
        if (!session) {
            return;
        }
        auto reader = securitykey::CcidReader::open(std::move(pipe));
        if (!reader) {
            session->offerProblem(Problem::kConnectionLost);
            return;
        }
        offerCard(*session, std::move(*reader), false);
    }

    void nfcKey(jint token, bool extendedLength) {
        const auto session = current();
        if (token == 0) {
            if (session) {
                session->offerProblem(vfs::SecurityKeySession::Problem::kNotASecurityKey);
            }
            return;
        }
        auto channel = std::make_unique<JniApduChannel>(keys_, methods_, token, extendedLength);
        if (!session) {
            return;
        }
        if (session->kind() == Kind::kOpenPgpCard) {
            offerCard(*session, std::move(channel), true);
            return;
        }
        auto transport = securitykey::NfcTransport::open(std::move(channel));
        if (!transport) {
            session->offerProblem(transport.error().kind == securitykey::KeyError::Kind::kIo &&
                                                  transport.error().message == "Not a security key"
                                          ? vfs::SecurityKeySession::Problem::kNotASecurityKey
                                          : vfs::SecurityKeySession::Problem::kConnectionLost);
            return;
        }
        session->offerTransport(std::move(*transport));
    }

private:
    static void offerCard(vfs::SecurityKeySession &session, std::unique_ptr<securitykey::ApduChannel> channel,
                          bool userPresent) {
        auto card = securitykey::OpenPgpCard::open(std::move(channel), userPresent);
        if (!card) {
            session.offerProblem(card.error().kind == securitykey::KeyError::Kind::kIo &&
                                                 card.error().message == "Not an OpenPGP card"
                                         ? Problem::kNotASecurityKey
                                         : Problem::kConnectionLost);
            return;
        }
        session.offerTransport(std::move(*card));
    }

    std::shared_ptr<vfs::SecurityKeySession> current() {
        std::lock_guard lock(mutex_);
        return session_;
    }

    jobject keys_;
    Methods methods_;
    std::mutex mutex_;
    std::shared_ptr<vfs::SecurityKeySession> session_;
};

// Sources live as long as the process (NativeHost's does), so a handle is never stale.
JniSecurityKeySource *source(jlong handle) { return reinterpret_cast<JniSecurityKeySource *>(handle); }

void nativeUsbKey(JNIEnv *, jobject, jlong handle, jint fd, jint inEndpoint, jint outEndpoint, jint packetSize,
                  jint token) {
    if (handle != 0) {
        source(handle)->usbKey(fd, inEndpoint, outEndpoint, packetSize, token);
    }
}

void nativeUsbCard(JNIEnv *, jobject, jlong handle, jint fd, jint inEndpoint, jint outEndpoint, jint packetSize,
                   jint token) {
    if (handle != 0) {
        source(handle)->usbCard(fd, inEndpoint, outEndpoint, packetSize, token);
    }
}

void nativeNfcKey(JNIEnv *, jobject, jlong handle, jint token, jboolean extendedLength) {
    if (handle != 0) {
        source(handle)->nfcKey(token, extendedLength == JNI_TRUE);
    }
}

}  // namespace

std::shared_ptr<vfs::SecurityKeySource> createSecurityKeySource(JNIEnv *env, jobject keys) {
    if (keys == nullptr) {
        return nullptr;
    }
    jclass type = env->GetObjectClass(keys);
    Methods methods{
            .start = env->GetMethodID(type, "start", "(JZ)V"),
            .stop = env->GetMethodID(type, "stop", "()V"),
            .transceive = env->GetMethodID(type, "transceive", "(I[B)[B"),
            .closeKey = env->GetMethodID(type, "closeKey", "(I)V"),
    };
    env->DeleteLocalRef(type);
    if (clearException(env, "SecurityKeys methods") || !methods.start || !methods.stop || !methods.transceive ||
        !methods.closeKey) {
        __android_log_print(ANDROID_LOG_ERROR, "FilePan", "SecurityKeys' methods are missing");
        return nullptr;
    }
    // Never freed, as Kotlin may hand a key to it at any time: see source().
    static std::shared_ptr<JniSecurityKeySource> kept;
    kept = std::make_shared<JniSecurityKeySource>(env, keys, methods);
    return kept;
}

jint registerSecurityKeys(JNIEnv *env) {
    jclass type = env->FindClass(kSecurityKeysClass);
    if (type == nullptr) {
        clearException(env, "FindClass SecurityKeys");
        return JNI_ERR;
    }
    const JNINativeMethod methods[] = {
            {"nativeUsbKey", "(JIIIII)V", reinterpret_cast<void *>(nativeUsbKey)},
            {"nativeUsbCard", "(JIIIII)V", reinterpret_cast<void *>(nativeUsbCard)},
            {"nativeNfcKey", "(JIZ)V", reinterpret_cast<void *>(nativeNfcKey)},
    };
    const jint result = env->RegisterNatives(type, methods, std::size(methods));
    env->DeleteLocalRef(type);
    if (result != JNI_OK) {
        clearException(env, "RegisterNatives SecurityKeys");
        return JNI_ERR;
    }
    return JNI_OK;
}

}  // namespace filepan::jni
