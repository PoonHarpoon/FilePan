# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# The text system: gfx::TextSystem on FreeType, HarfBuzz and ICU. Included from the main
# CMakeLists.txt after ThirdParty.cmake, which defines filepan::freetype and filepan::harfbuzz.

# gfx::ShapedText's caret queries, which need no fonts: the UI framework can link just this.
add_library(filepan_shaped_text STATIC shaped_text.cpp)
target_include_directories(filepan_shaped_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(filepan_shaped_text PUBLIC filepan_options)

add_library(filepan_text STATIC
        font_provider.cpp
        collator.cpp
        font_registry.cpp
        png_decoder.cpp
        text_system.cpp
        unicode.cpp)
target_include_directories(filepan_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_compile_options(filepan_text PRIVATE -fvisibility=hidden)
target_link_libraries(filepan_text
        PUBLIC filepan_options filepan_shaped_text
        PRIVATE filepan::harfbuzz_raster filepan::harfbuzz filepan::freetype)

# ICU: Android's system ICU4C through the NDK (API 31+), the dev shell's on the host. See icu.hpp.
if (ANDROID)
    target_link_libraries(filepan_text PRIVATE icu android jnigraphics)
else ()
    find_package(ICU REQUIRED COMPONENTS uc i18n data)
    target_compile_definitions(filepan_text PRIVATE U_SHOW_CPLUSPLUS_API=0)
    target_link_libraries(filepan_text PRIVATE ICU::uc ICU::i18n ICU::data)
endif ()

# TextDocument: a text file's contents, lines and encoding, which needs no ICU either: the text
# editor component links just this.
add_library(filepan_text_document STATIC text_document.cpp)
target_include_directories(filepan_text_document PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(filepan_text_document PUBLIC filepan_options)
