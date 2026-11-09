# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# Host tests for the UI framework. They need neither a GPU nor fonts: text goes through a fake
# TextSystem with simple metrics (see ui_test_util.hpp).

function(filepan_ui_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE filepan_ui)
    target_include_directories(${name} PRIVATE ..)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

filepan_ui_test(reactive_test)
filepan_ui_test(layout_test)
filepan_ui_test(for_each_test)
filepan_ui_test(node_test)
filepan_ui_test(gesture_test)
filepan_ui_test(virtual_list_test)
filepan_ui_test(semantics_test)
filepan_ui_test(theme_test)
filepan_ui_test(ui_root_test)
filepan_ui_test(example_test)

# Not in ctest: build/host/ui/virtual_list_benchmark
add_executable(virtual_list_benchmark virtual_list_benchmark.cpp)
target_link_libraries(virtual_list_benchmark PRIVATE filepan_ui)
target_include_directories(virtual_list_benchmark PRIVATE ..)
