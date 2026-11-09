# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# FilePan's native core. core/ is plain C++ that also builds on a Linux host (see bench/);
# jni/ is the Android-only glue.

# Language level and warnings for FilePan's own C++ (never third_party/). The NDK's clang sets the
# feature floor: the host build also uses clang and libc++ so both accept the same code.
# Optional modules, chosen at configure time. Gradle sets them per build type (app/build.gradle).
option(FILEPAN_ADIRSTAT "Build the disk usage analyzer (adirstat)" OFF)

add_library(filepan_options INTERFACE)
target_compile_features(filepan_options INTERFACE cxx_std_26)
target_compile_options(filepan_options INTERFACE
        -Wall -Wextra -Werror -fno-exceptions -fno-rtti
        # #embed is standard C++26 but clang 21 still flags it as an extension.
        -Wno-c23-extensions)

add_library(filepan_core STATIC
        core/filepan/archive_stream.cpp
        core/filepan/base64.cpp
        core/filepan/checksum.cpp
        core/filepan/copy_file.cpp
        core/filepan/create_archive.cpp
        core/filepan/delete_tree.cpp
        core/filepan/directory_reader.cpp
        core/filepan/extract_archive.cpp
        core/filepan/move.cpp
        core/filepan/securitykey/assertion.cpp
        core/filepan/securitykey/cbor.cpp
        core/filepan/securitykey/ccid.cpp
        core/filepan/securitykey/ctap.cpp
        core/filepan/securitykey/ctaphid.cpp
        core/filepan/securitykey/iso7816.cpp
        core/filepan/securitykey/nfc.cpp
        core/filepan/securitykey/openpgp_card.cpp
        core/filepan/securitykey/pin_protocol.cpp
        core/filepan/securitykey/usbfs.cpp
        core/filepan/sftp_client.cpp
        core/filepan/ssh_agent.cpp
        core/filepan/verify_archive.cpp)
target_include_directories(filepan_core PUBLIC core)
target_compile_options(filepan_core PRIVATE -fvisibility=hidden)

add_subdirectory(core/filepan/gfx/vulkan)
add_subdirectory(core/filepan/ui)

include(third_party/ThirdParty.cmake)
target_link_libraries(filepan_core PUBLIC filepan_options filepan::archive filepan::ssh filepan::lzma filepan::mbedcrypto
        ZLIB::ZLIB)

add_subdirectory(core/filepan/text)
add_subdirectory(core/filepan/ui/components)
add_subdirectory(core/filepan/vfs)
add_subdirectory(core/filepan/jobs)
add_subdirectory(core/filepan/runtime)
add_subdirectory(core/filepan/media)
if (FILEPAN_ADIRSTAT)
    add_subdirectory(core/filepan/adirstat)
endif ()
add_subdirectory(core/filepan/app)

if (ANDROID)
    find_library(LOG_LIBRARY log)
    add_library(filepan SHARED
            jni/document_provider.cpp
            jni/jni_onload.cpp
            jni/jni_util.cpp
            jni/native_host.cpp
            jni/security_keys.cpp)
    target_compile_options(filepan PRIVATE -fvisibility=hidden)
    target_link_libraries(filepan PRIVATE filepan_app filepan_runtime filepan_core filepan_text
            filepan_gfx_vulkan filepan_media filepan_vfs filepan_jobs filepan_ui filepan_ui_components ${LOG_LIBRARY})
    target_link_options(filepan PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL)
endif ()
