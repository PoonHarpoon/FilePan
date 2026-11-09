# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# The virtual file system: locations, entries, backends, watching, MIME types and sorting.

# The MIME tables come from freedesktop.org shared-mime-info (a submodule) and FilePan's
# mime_policy.txt, through generate_mime_table.py.
set(FILEPAN_SHARED_MIME_INFO_DIR ${FILEPAN_THIRD_PARTY_DIR}/shared-mime-info)
if (NOT EXISTS "${FILEPAN_SHARED_MIME_INFO_DIR}/data/freedesktop.org.xml.in")
    message(FATAL_ERROR "shared-mime-info is missing; run: git submodule update --init --recursive")
endif ()
# Prefer the python3 on PATH, which is the dev shell's.
set(Python3_FIND_STRATEGY LOCATION)
find_package(Python3 3.11 COMPONENTS Interpreter)
if (NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "Python 3.11 or later is needed to generate the MIME type table from "
            "shared-mime-info (vfs/generate_mime_table.py); the Nix dev shell provides it.")
endif ()
set(FILEPAN_MIME_TABLE ${CMAKE_CURRENT_BINARY_DIR}/generated/mime_table.inc)
add_custom_command(
        OUTPUT ${FILEPAN_MIME_TABLE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/generate_mime_table.py
                --smi-dir ${FILEPAN_SHARED_MIME_INFO_DIR}
                --policy ${CMAKE_CURRENT_SOURCE_DIR}/mime_policy.txt
                --output ${FILEPAN_MIME_TABLE}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generate_mime_table.py
                ${CMAKE_CURRENT_SOURCE_DIR}/mime_policy.txt
                ${FILEPAN_SHARED_MIME_INFO_DIR}/data/freedesktop.org.xml.in
                ${FILEPAN_SHARED_MIME_INFO_DIR}/meson.build
        COMMENT "Generating the MIME type table"
        VERBATIM)

add_library(filepan_vfs STATIC
        archive_backend.cpp
        backend.cpp
        directory_watcher.cpp
        document_backend.cpp
        local_backend.cpp
        location.cpp
        mime.cpp
        save.cpp
        security_key.cpp
        sftp_backend.cpp
        sort.cpp
        ${FILEPAN_MIME_TABLE})
target_include_directories(filepan_vfs PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(filepan_vfs PRIVATE -fvisibility=hidden)
target_link_libraries(filepan_vfs PUBLIC filepan_core)
