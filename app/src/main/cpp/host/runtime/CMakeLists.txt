# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# AppHost run headless: the real text system and renderer (lavapipe in the dev shell) into an
# offscreen target, a fake Platform, and the app. Without a Vulkan device it reports
# itself skipped, except on CI.

if (NOT TARGET filepan_app)
    return()
endif ()

get_filename_component(FILEPAN_REPO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

add_executable(app_host_test app_host_test.cpp)
target_include_directories(app_host_test PRIVATE ..)
target_compile_definitions(app_host_test PRIVATE
        FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts")
target_link_libraries(app_host_test PRIVATE filepan_app filepan_gfx_test_util)
add_test(NAME app_host_test COMMAND app_host_test)
set_tests_properties(app_host_test PROPERTIES SKIP_RETURN_CODE 77)

# The IME bridge's per-field sessions, without a renderer.
add_executable(ime_bridge_test ime_bridge_test.cpp)
target_include_directories(ime_bridge_test PRIVATE ..)
target_link_libraries(ime_bridge_test PRIVATE filepan_runtime)
add_test(NAME ime_bridge_test COMMAND ime_bridge_test)
