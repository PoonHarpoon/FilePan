// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cstdio>

// Error logging for the runtime: logcat on Android, stderr on a host.
#ifdef __ANDROID__
#include <android/log.h>
#define FILEPAN_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "FilePan", __VA_ARGS__)
#define FILEPAN_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "FilePan", __VA_ARGS__)
#else
#define FILEPAN_LOGE(...) (std::fprintf(stderr, "E FilePan: " __VA_ARGS__), std::fputc('\n', stderr))
#define FILEPAN_LOGW(...) (std::fprintf(stderr, "W FilePan: " __VA_ARGS__), std::fputc('\n', stderr))
#endif
