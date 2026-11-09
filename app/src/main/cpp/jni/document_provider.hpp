// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <jni.h>

#include <memory>

#include "filepan/vfs/document_backend.hpp"

namespace filepan::jni {

// The DocumentProvider behind document trees, calling app.phsp.filepan.DocumentTrees (trees) on
// whichever I/O thread asks. Null if its methods can't be found.
std::shared_ptr<vfs::DocumentProvider> createDocumentProvider(JNIEnv *env, jobject trees);

}  // namespace filepan::jni
