# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# Tests for the Vulkan renderer. renderer_test compares against the PNGs in golden/ and needs a
# Vulkan device (lavapipe in the dev shell); without one it reports itself skipped, except on CI.

if (NOT TARGET filepan_gfx_vulkan)
    return()
endif ()

find_package(ZLIB REQUIRED)

add_library(filepan_gfx_test_util STATIC png.cpp)
target_include_directories(filepan_gfx_test_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(filepan_gfx_test_util PUBLIC filepan_gfx_vulkan ZLIB::ZLIB)
target_compile_definitions(filepan_gfx_test_util PUBLIC
        FILEPAN_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
        FILEPAN_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/golden-out")

add_executable(frame_plan_test frame_plan_test.cpp)
target_link_libraries(frame_plan_test PRIVATE filepan_gfx_test_util)
add_test(NAME frame_plan_test COMMAND frame_plan_test)

add_executable(renderer_test renderer_test.cpp)
target_link_libraries(renderer_test PRIVATE filepan_gfx_test_util)
# Once with the validation layer (when it's installed), and once without: the layer wraps
# handles and changes timing, which has hidden bugs before.
add_test(NAME renderer_test COMMAND renderer_test)
add_test(NAME renderer_test_no_validation COMMAND renderer_test)
set_tests_properties(renderer_test renderer_test_no_validation PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(renderer_test_no_validation PROPERTIES ENVIRONMENT FILEPAN_VK_VALIDATION=0)

add_executable(renderer_benchmark renderer_benchmark.cpp)
target_link_libraries(renderer_benchmark PRIVATE filepan_gfx_test_util)

# Swapchain sizing for Android's surfaces, which needs no device.
add_executable(swapchain_test swapchain_test.cpp)
target_link_libraries(swapchain_test PRIVATE filepan_gfx_test_util)
add_test(NAME swapchain_test COMMAND swapchain_test)
