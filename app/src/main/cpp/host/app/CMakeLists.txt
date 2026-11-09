# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# The app's behavior tests (app_test, file_operations_test, viewer_test) and golden renders
# (app_golden_test), on a temporary directory tree with a fake Platform and the real text system:
#
#   FILEPAN_UPDATE_GOLDENS=1 build/host/app/app_golden_test   rewrites golden/*.png

if (NOT TARGET filepan_app)
    return()
endif ()

get_filename_component(FILEPAN_REPO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

add_executable(app_test app_test.cpp)
target_include_directories(app_test PRIVATE ..)
target_compile_definitions(app_test PRIVATE FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts"
        FILEPAN_ASSETS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets")
target_link_libraries(app_test PRIVATE filepan_app)
add_test(NAME app_test COMMAND app_test)

add_executable(file_operations_test file_operations_test.cpp)
target_include_directories(file_operations_test PRIVATE ..)
target_compile_definitions(file_operations_test PRIVATE FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts"
        FILEPAN_ASSETS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets")
target_link_libraries(file_operations_test PRIVATE filepan_app)
add_test(NAME file_operations_test COMMAND file_operations_test)

add_executable(viewer_test viewer_test.cpp)
target_include_directories(viewer_test PRIVATE ..)
target_compile_definitions(viewer_test PRIVATE FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts"
        FILEPAN_ASSETS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets")
target_link_libraries(viewer_test PRIVATE filepan_app)
add_test(NAME viewer_test COMMAND viewer_test)

find_package(ZLIB REQUIRED)
add_executable(app_golden_test app_golden_test.cpp ../gfx/png.cpp)
target_include_directories(app_golden_test PRIVATE .. ../gfx)
target_compile_definitions(app_golden_test PRIVATE
        FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts"
        FILEPAN_ASSETS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets"
        FILEPAN_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
        FILEPAN_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/golden-out")
target_link_libraries(app_golden_test PRIVATE filepan_app ZLIB::ZLIB)
add_test(NAME app_golden_test COMMAND app_golden_test)
set_tests_properties(app_golden_test PROPERTIES SKIP_RETURN_CODE 77)

# The SFTP screens, prompt sheets, settings, secrets and licenses (screens_test), and their golden
# renders (screens_golden_test, same switches as app_golden_test). The SFTP flows need OpenSSH's
# sshd and ssh-keygen, which the dev shell provides.
add_executable(screens_test screens_test.cpp)
target_include_directories(screens_test PRIVATE ..)
target_compile_definitions(screens_test PRIVATE
        FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts"
        FILEPAN_ASSETS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets")
find_program(SSHD_EXECUTABLE sshd)
find_program(SSH_KEYGEN_EXECUTABLE ssh-keygen)
if (SSHD_EXECUTABLE AND SSH_KEYGEN_EXECUTABLE)
    target_compile_definitions(screens_test PRIVATE
            FILEPAN_SSHD="${SSHD_EXECUTABLE}" FILEPAN_SSH_KEYGEN="${SSH_KEYGEN_EXECUTABLE}")
else ()
    message(WARNING "sshd not found; screens_test skips the SFTP flows")
endif ()
target_link_libraries(screens_test PRIVATE filepan_app)
add_test(NAME screens_test COMMAND screens_test)

add_executable(screens_golden_test screens_golden_test.cpp ../gfx/png.cpp)
target_include_directories(screens_golden_test PRIVATE .. ../gfx)
target_compile_definitions(screens_golden_test PRIVATE
        FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts"
        FILEPAN_ASSETS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets"
        FILEPAN_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
        FILEPAN_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/golden-out")
target_link_libraries(screens_golden_test PRIVATE filepan_app ZLIB::ZLIB)
add_test(NAME screens_golden_test COMMAND screens_golden_test)
set_tests_properties(screens_golden_test PROPERTIES SKIP_RETURN_CODE 77)

# The app turned between portrait and landscape mid-session, rendered on the real renderer when
# there's a Vulkan device (layout only without one).
add_executable(rotation_test rotation_test.cpp)
target_include_directories(rotation_test PRIVATE ..)
target_compile_definitions(rotation_test PRIVATE FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts"
        FILEPAN_ASSETS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets")
target_link_libraries(rotation_test PRIVATE filepan_app filepan_gfx_test_util)
add_test(NAME rotation_test COMMAND rotation_test)

# The thumbnail cache's disk tier, with a fake decoder on a temporary directory.
add_executable(thumbnails_test thumbnails_test.cpp)
target_include_directories(thumbnails_test PRIVATE ..)
target_link_libraries(thumbnails_test PRIVATE filepan_app)
add_test(NAME thumbnails_test COMMAND thumbnails_test)

# The viewer's remote reads: the block cache, read-ahead and prefetching, against an in-memory
# server (fake_media_backend.hpp).
add_executable(remote_media_test remote_media_test.cpp)
target_include_directories(remote_media_test PRIVATE ..)
if (SSHD_EXECUTABLE AND SSH_KEYGEN_EXECUTABLE)
    # Random reads over a real sshd, against the file.
    target_compile_definitions(remote_media_test PRIVATE
            FILEPAN_SSHD="${SSHD_EXECUTABLE}" FILEPAN_SSH_KEYGEN="${SSH_KEYGEN_EXECUTABLE}")
endif ()
target_link_libraries(remote_media_test PRIVATE filepan_app)
add_test(NAME remote_media_test COMMAND remote_media_test)

# Settings backups: the format, the password sealing and restoring.
add_executable(backup_test backup_test.cpp)
target_include_directories(backup_test PRIVATE ..)
target_link_libraries(backup_test PRIVATE filepan_app)
add_test(NAME backup_test COMMAND backup_test)

add_executable(text_screen_test text_screen_test.cpp)
target_include_directories(text_screen_test PRIVATE ..)
target_compile_definitions(text_screen_test PRIVATE FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts"
        FILEPAN_ASSETS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets")
target_link_libraries(text_screen_test PRIVATE filepan_app)
add_test(NAME text_screen_test COMMAND text_screen_test)

# The disk usage screen, when the adirstat module is built.
if (TARGET filepan_adirstat)
    add_executable(disk_usage_test disk_usage_test.cpp)
    target_include_directories(disk_usage_test PRIVATE ..)
    target_compile_definitions(disk_usage_test PRIVATE FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts"
            FILEPAN_ASSETS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets")
    target_link_libraries(disk_usage_test PRIVATE filepan_app)
    add_test(NAME disk_usage_test COMMAND disk_usage_test)
endif ()
if (TARGET filepan_adirstat)
    add_executable(disk_usage_golden_test disk_usage_golden_test.cpp ../gfx/png.cpp)
    target_include_directories(disk_usage_golden_test PRIVATE .. ../gfx)
    target_compile_definitions(disk_usage_golden_test PRIVATE
            FILEPAN_FONTS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets/fonts"
            FILEPAN_ASSETS_DIR="${FILEPAN_REPO_DIR}/app/src/main/assets"
            FILEPAN_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
            FILEPAN_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/golden-out")
    target_link_libraries(disk_usage_golden_test PRIVATE filepan_app ZLIB::ZLIB)
    add_test(NAME disk_usage_golden_test COMMAND disk_usage_golden_test)
    set_tests_properties(disk_usage_golden_test PROPERTIES SKIP_RETURN_CODE 77)
endif ()
