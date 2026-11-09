# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# bzip2 1.0.8 ships only a Makefile; this builds and installs libbz2 for libarchive to find.

cmake_minimum_required(VERSION 3.22)
project(bzip2 C)

set(BZIP2_SOURCE_DIR "" CACHE PATH "bzip2 source directory")

add_library(bz2 STATIC
        ${BZIP2_SOURCE_DIR}/blocksort.c
        ${BZIP2_SOURCE_DIR}/bzlib.c
        ${BZIP2_SOURCE_DIR}/compress.c
        ${BZIP2_SOURCE_DIR}/crctable.c
        ${BZIP2_SOURCE_DIR}/decompress.c
        ${BZIP2_SOURCE_DIR}/huffman.c
        ${BZIP2_SOURCE_DIR}/randtable.c)
target_compile_definitions(bz2 PRIVATE _FILE_OFFSET_BITS=64)
install(TARGETS bz2 ARCHIVE DESTINATION lib)
install(FILES ${BZIP2_SOURCE_DIR}/bzlib.h DESTINATION include)
