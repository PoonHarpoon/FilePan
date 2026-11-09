// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Force-included into every FreeType source file (-include), since the angle brackets can't be
// passed through a shell command line as a -D flag.

#define FT_CONFIG_MODULES_H <filepan_ftmodule.h>
