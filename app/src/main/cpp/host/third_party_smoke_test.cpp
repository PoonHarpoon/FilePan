// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 FilePan contributors

// Checks that libarchive was built with every compressor FilePan relies on, and that the text
// libraries link.

#include <cstdio>
#include <cstring>

#include <archive.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <libssh/libssh.h>

#include "test_util.hpp"

int main() {
    const char *details = archive_version_details();
    std::puts(details);
    for (const char *library : {"zlib", "liblzma", "bz2lib", "liblz4", "libzstd", "mbedtls"}) {
        EXPECT(std::strstr(details, library) != nullptr);
    }
    std::printf("libssh %s\n", ssh_version(0));
    EXPECT(ssh_version(SSH_VERSION_INT(0, 12, 0)) != nullptr);

    FT_Library freetype = nullptr;
    EXPECT(FT_Init_FreeType(&freetype) == 0);
    FT_Int major = 0, minor = 0, patch = 0;
    FT_Library_Version(freetype, &major, &minor, &patch);
    std::printf("FreeType %d.%d.%d\n", major, minor, patch);
    FT_Done_FreeType(freetype);
    std::printf("HarfBuzz %s\n", hb_version_string());
    EXPECT(hb_version_atleast(15, 0, 0));
    return 0;
}
