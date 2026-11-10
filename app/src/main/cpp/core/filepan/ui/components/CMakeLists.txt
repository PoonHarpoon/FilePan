# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# The Glass Signal component kit, on the UI framework. Like filepan_ui it records display lists
# and talks to text only through gfx::TextSystem, so it builds and tests on a Linux host.

add_library(filepan_ui_components STATIC
        buttons.cpp
        common.cpp
        controls.cpp
        hero.cpp
        marks.cpp
        navigation.cpp
        overlays.cpp
        recipes.cpp
        rows.cpp
        scroll_thumb.cpp
        surfaces.cpp
        text_editor.cpp
        text_field.cpp)
target_compile_options(filepan_ui_components PRIVATE -fvisibility=hidden)
# filepan_shaped_text: ShapedText's caret queries, for the text field and editor.
# filepan_text_document: the text editor's document.
target_link_libraries(filepan_ui_components PUBLIC filepan_ui filepan_shaped_text filepan_text_document)
