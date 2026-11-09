# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# Tests for the Glass Signal component kit.
#
# Behavior tests need neither a GPU nor fonts. gallery_test renders the kit and a browsing
# screen with the real Vulkan renderer (lavapipe in the dev shell) and the real text system with
# the bundled fonts, and compares them with the PNGs in golden/:
#
#   FILEPAN_UPDATE_GOLDENS=1 build/host/ui_components/gallery_test   rewrites golden/*.png

function(filepan_ui_components_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE filepan_ui_components)
    target_include_directories(${name} PRIVATE .. ../ui)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

filepan_ui_components_test(components_test)
filepan_ui_components_test(text_field_test)
filepan_ui_components_test(text_editor_test)
filepan_ui_components_test(containers_test)

if (NOT TARGET filepan_gfx_vulkan)
    return()
endif ()

find_package(ZLIB REQUIRED)
get_filename_component(FILEPAN_REPO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

add_executable(gallery_test gallery_test.cpp gallery.cpp ../gfx/png.cpp)
target_include_directories(gallery_test PRIVATE .. ../gfx)
target_compile_definitions(gallery_test PRIVATE
        FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts"
        FILEPAN_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
        FILEPAN_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/golden-out")
target_link_libraries(gallery_test PRIVATE filepan_ui_components filepan_text filepan_gfx_vulkan ZLIB::ZLIB)
add_test(NAME gallery_test COMMAND gallery_test)
set_tests_properties(gallery_test PROPERTIES SKIP_RETURN_CODE 77)
