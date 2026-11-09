// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include "jni_util.hpp"

#include <android/log.h>

namespace filepan::jni {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

void appendUtf16(std::u16string &out, char32_t c) {
    if (c >= 0x10000) {
        c -= 0x10000;
        out.push_back(static_cast<char16_t>(0xD800 + (c >> 10)));
        out.push_back(static_cast<char16_t>(0xDC00 + (c & 0x3FF)));
    } else {
        out.push_back(static_cast<char16_t>(c));
    }
}

void appendUtf8(std::string &out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

}  // namespace

std::u16string utf8ToUtf16(std::string_view utf8) {
    std::u16string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto b0 = static_cast<unsigned char>(utf8[i]);
        std::size_t length;
        char32_t c;
        char32_t minimum;
        if (b0 < 0x80) {
            out.push_back(b0);
            ++i;
            continue;
        } else if ((b0 & 0xE0) == 0xC0) {
            length = 2;
            c = b0 & 0x1F;
            minimum = 0x80;
        } else if ((b0 & 0xF0) == 0xE0) {
            length = 3;
            c = b0 & 0x0F;
            minimum = 0x800;
        } else if ((b0 & 0xF8) == 0xF0) {
            length = 4;
            c = b0 & 0x07;
            minimum = 0x10000;
        } else {
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        bool valid = i + length <= utf8.size();
        for (std::size_t k = 1; valid && k < length; ++k) {
            const auto b = static_cast<unsigned char>(utf8[i + k]);
            valid = (b & 0xC0) == 0x80;
            c = (c << 6) | (b & 0x3F);
        }
        if (!valid || c < minimum || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
            // One replacement per invalid lead byte, as Kotlin's decoder in Utf8 does.
            out.push_back(kReplacement);
            ++i;
            continue;
        }
        appendUtf16(out, c);
        i += length;
    }
    return out;
}

std::string utf16ToUtf8(std::u16string_view utf16) {
    std::string out;
    out.reserve(utf16.size());
    for (std::size_t i = 0; i < utf16.size(); ++i) {
        const char16_t u = utf16[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < utf16.size() && utf16[i + 1] >= 0xDC00 &&
            utf16[i + 1] <= 0xDFFF) {
            appendUtf8(out, 0x10000 + ((char32_t{u} - 0xD800) << 10) + (utf16[i + 1] - 0xDC00));
            ++i;
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            appendUtf8(out, kReplacement);
        } else {
            appendUtf8(out, u);
        }
    }
    return out;
}

jstring newString(JNIEnv *env, std::string_view utf8) {
    const std::u16string utf16 = utf8ToUtf16(utf8);
    return env->NewString(reinterpret_cast<const jchar *>(utf16.data()),
                          static_cast<jsize>(utf16.size()));
}

std::string toUtf8(JNIEnv *env, jstring string) {
    if (string == nullptr) {
        return {};
    }
    const jsize length = env->GetStringLength(string);
    std::u16string utf16(static_cast<std::size_t>(length), u'\0');
    env->GetStringRegion(string, 0, length, reinterpret_cast<jchar *>(utf16.data()));
    return utf16ToUtf8(utf16);
}

jbyteArray newByteArray(JNIEnv *env, std::span<const std::uint8_t> bytes) {
    jbyteArray array = env->NewByteArray(static_cast<jsize>(bytes.size()));
    if (array != nullptr) {
        env->SetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()),
                                reinterpret_cast<const jbyte *>(bytes.data()));
    }
    return array;
}

std::string bytesToString(JNIEnv *env, jbyteArray bytes) {
    if (bytes == nullptr) {
        return {};
    }
    const jsize length = env->GetArrayLength(bytes);
    std::string out(static_cast<std::size_t>(length), '\0');
    env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte *>(out.data()));
    return out;
}

bool clearException(JNIEnv *env, const char *where) {
    if (!env->ExceptionCheck()) {
        return false;
    }
    __android_log_print(ANDROID_LOG_ERROR, "FilePan", "Java exception in %s", where);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

}  // namespace filepan::jni
