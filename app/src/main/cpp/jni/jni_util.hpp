// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <jni.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Small JNI helpers. Strings cross as UTF-16 (NewString/GetStringChars), never through JNI's
// modified UTF-8, which mangles characters outside the BMP.
namespace filepan::jni {

// Invalid UTF-8 and lone surrogates become U+FFFD.
std::u16string utf8ToUtf16(std::string_view utf8);
std::string utf16ToUtf8(std::u16string_view utf16);

jstring newString(JNIEnv *env, std::string_view utf8);
std::string toUtf8(JNIEnv *env, jstring string);
jbyteArray newByteArray(JNIEnv *env, std::span<const std::uint8_t> bytes);
std::string bytesToString(JNIEnv *env, jbyteArray bytes);

// This thread's JNIEnv, attaching it (an I/O pool worker) on its first call and detaching it when
// it exits; null if it can't be attached.
JNIEnv *currentEnv();

// Logs and clears a pending Java exception; returns whether there was one.
bool clearException(JNIEnv *env, const char *where);

// Frees the local references made inside it: the native UI thread stays attached with no Java
// frame of its own, so nothing else would.
class LocalFrame {
public:
    explicit LocalFrame(JNIEnv *env, jint capacity = 16) : env_(env) {
        env_->PushLocalFrame(capacity);
    }
    ~LocalFrame() { env_->PopLocalFrame(nullptr); }
    LocalFrame(const LocalFrame &) = delete;
    LocalFrame &operator=(const LocalFrame &) = delete;

private:
    JNIEnv *env_;
};

}  // namespace filepan::jni
