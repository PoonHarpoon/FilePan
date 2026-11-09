# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# The disk usage analyzer's engine tests; skipped when the module is off (-DFILEPAN_ADIRSTAT=OFF).

if (NOT TARGET filepan_adirstat)
    return()
endif ()

foreach (name scanner_test file_types_test treemap_test)
    add_executable(adirstat_${name} ${name}.cpp)
    target_include_directories(adirstat_${name} PRIVATE ..)
    target_link_libraries(adirstat_${name} PRIVATE filepan_adirstat)
    add_test(NAME adirstat_${name} COMMAND adirstat_${name})
endforeach ()
