// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

#pragma once

#include <cerrno>

// Re-evaluates a call returning -1 while it fails with EINTR. A macro so that the expression is
// re-evaluated on each attempt.
#define FILEPAN_RETRY(exp) \
    ({ \
        __typeof__(exp) _rc; \
        do { \
            _rc = (exp); \
        } while (_rc == -1 && errno == EINTR); \
        _rc; \
    })
