# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# Host tests for media: the timing and A/V sync logic, file byte sources, the players driven by
# fake decoders, and the pure part of video thumbnails. No GPU needed; the ImageStore only queues.

if (NOT TARGET filepan_media)
    return()
endif ()

add_executable(byte_source_test byte_source_test.cpp)
target_include_directories(byte_source_test PRIVATE ..)
target_link_libraries(byte_source_test PRIVATE filepan_media)
add_test(NAME byte_source_test COMMAND byte_source_test)

add_executable(image_player_test image_player_test.cpp)
target_include_directories(image_player_test PRIVATE ..)
target_link_libraries(image_player_test PRIVATE filepan_media)
add_test(NAME image_player_test COMMAND image_player_test)

add_executable(video_player_test video_player_test.cpp)
target_include_directories(video_player_test PRIVATE ..)
target_link_libraries(video_player_test PRIVATE filepan_media)
add_test(NAME video_player_test COMMAND video_player_test)

add_executable(video_thumbnail_test video_thumbnail_test.cpp)
target_include_directories(video_thumbnail_test PRIVATE ..)
target_link_libraries(video_thumbnail_test PRIVATE filepan_media)
add_test(NAME video_thumbnail_test COMMAND video_thumbnail_test)
