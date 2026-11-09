# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# Media for the viewer: image, animation and video decoding and playback, plus video frame grabs
# for thumbnails. The decoders are Android's (AImageDecoder, the media NDK, AAudio); a host build
# stubs them, and tests drive the logic with fakes.

if (NOT TARGET filepan_gfx_vulkan)
    message(WARNING "No Vulkan renderer; skipping media")
    return()
endif ()

add_library(filepan_media STATIC
        av_sync.cpp
        byte_source.cpp
        frame_scheduler.cpp
        image_player.cpp
        image_source.cpp
        video_backend_android.cpp
        video_player.cpp
        video_thumbnail.cpp
        video_thumbnail_android.cpp)
target_compile_options(filepan_media PRIVATE -fvisibility=hidden)
target_link_libraries(filepan_media PUBLIC filepan_ui filepan_gfx_vulkan)
if (ANDROID)
    # AImageDecoder; the media NDK (extractor, codecs, image reader) and AAudio for video and
    # video thumbnails.
    target_link_libraries(filepan_media PRIVATE jnigraphics mediandk aaudio)
endif ()
