# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# The disk usage analyzer's engine, built when FILEPAN_ADIRSTAT is on: the local scanner, the scan
# tree, file type statistics and the treemap layout. Plain C++ with no UI; its screens live in
# core/filepan/app/adirstat/.

add_library(filepan_adirstat STATIC
        file_types.cpp
        scanner.cpp
        tree.cpp
        treemap.cpp)
target_compile_options(filepan_adirstat PRIVATE -fvisibility=hidden)
target_link_libraries(filepan_adirstat PUBLIC filepan_core)
