// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <jni.h>

#include <memory>

#include "filepan/vfs/security_key.hpp"

namespace filepan::jni {

// The SecurityKeySource behind SFTP "sk-" keys, with app.phsp.filepan.SecurityKeys (keys)
// finding the key over USB or NFC. Null if its methods can't be found.
std::shared_ptr<vfs::SecurityKeySource> createSecurityKeySource(JNIEnv *env, jobject keys);

// SecurityKeys' native methods, for JNI_OnLoad.
jint registerSecurityKeys(JNIEnv *env);

}  // namespace filepan::jni
