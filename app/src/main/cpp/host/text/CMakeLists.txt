# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# Text system tests and benchmark, using the bundled fonts from the app's assets. Fallback,
# bidi and color glyphs use fonts from HarfBuzz's test suite, since the host has no system fonts.

get_filename_component(FILEPAN_REPO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
set(FILEPAN_TEXT_TEST_DEFINITIONS
        FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts"
        FILEPAN_HB_TEST_FONTS_DIR="${FILEPAN_REPO_DIR}/third_party/harfbuzz/test")

add_executable(text_test text_test.cpp)
target_include_directories(text_test PRIVATE ..)
target_compile_definitions(text_test PRIVATE ${FILEPAN_TEXT_TEST_DEFINITIONS})
target_link_libraries(text_test PRIVATE filepan_text)
add_test(NAME text_test COMMAND text_test)

# Not run by ctest: build/host/text/text_benchmark
add_executable(text_benchmark text_benchmark.cpp)
target_include_directories(text_benchmark PRIVATE ..)
target_compile_definitions(text_benchmark PRIVATE ${FILEPAN_TEXT_TEST_DEFINITIONS})
target_link_libraries(text_benchmark PRIVATE filepan_text)

add_executable(text_document_test text_document_test.cpp)
target_include_directories(text_document_test PRIVATE ..)
target_link_libraries(text_document_test PRIVATE filepan_text_document)
add_test(NAME text_document_test COMMAND text_document_test)
