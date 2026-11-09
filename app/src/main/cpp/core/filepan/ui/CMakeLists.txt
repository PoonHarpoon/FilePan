# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# The native UI framework: reactivity, the retained node tree, layout, input, animation,
# semantics and theme tokens. Depends only on the gfx/ contract headers, never on Vulkan or the
# text system's implementation, so it builds and tests on a Linux host.

find_package(Threads REQUIRED)

add_library(filepan_ui STATIC
        animation.cpp
        focus.cpp
        gesture.cpp
        layout.cpp
        node.cpp
        reactive.cpp
        scroll.cpp
        semantics.cpp
        text_input.cpp
        theme.cpp
        ui_loop.cpp
        ui_root.cpp
        virtual_list.cpp)
target_include_directories(filepan_ui PUBLIC ../..)
target_compile_options(filepan_ui PRIVATE -fvisibility=hidden)
# Styles, decorations and text styles are aggregates with defaults meant to be filled with
# designated initializers naming only what differs: Box::create({.direction = Direction::row}).
# Linked after filepan_options so the flag comes after its -Wextra.
add_library(filepan_ui_warnings INTERFACE)
target_compile_options(filepan_ui_warnings INTERFACE -Wno-missing-designated-field-initializers)
target_link_libraries(filepan_ui PUBLIC filepan_options Threads::Threads filepan_ui_warnings)
