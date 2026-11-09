# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# The runtime: AppHost (the UI and render threads, frame pacing, surface handoff) and the
# Platform interface the Android shell implements. Builds on a host for headless tests.

if (NOT TARGET filepan_gfx_vulkan)
    message(WARNING "No Vulkan renderer; skipping the runtime")
    return()
endif ()

add_library(filepan_runtime STATIC
        app_host.cpp
        assets.cpp
        job_events.cpp
        document_picker.cpp
        prompt_relay.cpp
        vsync.cpp)
target_compile_options(filepan_runtime PRIVATE -fvisibility=hidden)
target_link_libraries(filepan_runtime PUBLIC filepan_ui filepan_text filepan_gfx_vulkan filepan_jobs)
if (ANDROID)
    # AAssetManager for the bundled assets.
    target_link_libraries(filepan_runtime PRIVATE android)
endif ()
