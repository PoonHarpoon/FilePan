// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Appended to Mbed TLS's default configuration, both when building it and in the installed
// mbedtls_config.h, so that every consumer sees the same configuration.

#ifndef FILEPAN_MBEDTLS_USER_CONFIG_H
#define FILEPAN_MBEDTLS_USER_CONFIG_H

// Use the ARMv8 SHA-256 and SHA-512 instructions when the CPU has them, detected at run time;
// ignored on other architectures.
#define MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT
#define MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT

// libssh shares one random generator across sessions, which run on different threads.
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD

#endif
