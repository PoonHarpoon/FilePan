// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#include <jni.h>

namespace filepan::jni {
jint registerNativeHost(JavaVM *vm, JNIEnv *env);
jint registerSecurityKeys(JNIEnv *env);
}  // namespace filepan::jni

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    if (filepan::jni::registerNativeHost(vm, env) != JNI_OK) {
        return JNI_ERR;
    }
    if (filepan::jni::registerSecurityKeys(env) != JNI_OK) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
