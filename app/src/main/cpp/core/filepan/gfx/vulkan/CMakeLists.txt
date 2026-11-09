# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# The Vulkan renderer. Shaders are compiled to SPIR-V at build time and #embed-ded: with the NDK's
# glslc for Android, and with glslc from PATH (shaderc, in the dev shell) on the host.

if (ANDROID)
    if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        set(FILEPAN_NDK_HOST_TAG linux-x86_64)
    elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(FILEPAN_NDK_HOST_TAG darwin-x86_64)
    else ()
        set(FILEPAN_NDK_HOST_TAG windows-x86_64)
    endif ()
    find_program(FILEPAN_GLSLC glslc
            PATHS ${ANDROID_NDK}/shader-tools/${FILEPAN_NDK_HOST_TAG}
            NO_DEFAULT_PATH REQUIRED)
else ()
    # Optional on the host: without Vulkan or glslc, the renderer and its tests are skipped.
    find_package(Vulkan)
    find_program(FILEPAN_GLSLC glslc)
    if (NOT Vulkan_FOUND OR NOT FILEPAN_GLSLC)
        message(WARNING "Vulkan or glslc not found; skipping the Vulkan renderer")
        return()
    endif ()
endif ()

set(FILEPAN_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(FILEPAN_SPIRV_DIR ${CMAKE_CURRENT_BINARY_DIR}/spirv)
file(MAKE_DIRECTORY ${FILEPAN_SPIRV_DIR})

set(FILEPAN_SPIRV)
foreach (shader ui.vert ui.frag external.frag fullscreen.vert blur_down.frag blur_up.frag)
    set(output ${FILEPAN_SPIRV_DIR}/${shader}.spv)
    add_custom_command(
            OUTPUT ${output}
            COMMAND ${FILEPAN_GLSLC} --target-env=vulkan1.1 -O -Werror
                    -MD -MF ${output}.d -o ${output} ${FILEPAN_SHADER_DIR}/${shader}
            DEPENDS ${FILEPAN_SHADER_DIR}/${shader}
            DEPFILE ${output}.d
            COMMENT "Compiling ${shader}"
            VERBATIM)
    list(APPEND FILEPAN_SPIRV ${output})
endforeach ()
add_custom_target(filepan_gfx_vulkan_shaders DEPENDS ${FILEPAN_SPIRV})

add_library(filepan_gfx_vulkan STATIC
        device.cpp
        external_images.cpp
        frame_plan.cpp
        glyph_atlas.cpp
        recorder.cpp
        resources.cpp
        surface_android.cpp)
add_dependencies(filepan_gfx_vulkan filepan_gfx_vulkan_shaders)
# resources.cpp embeds the SPIR-V.
set_source_files_properties(resources.cpp PROPERTIES OBJECT_DEPENDS "${FILEPAN_SPIRV}")
target_compile_options(filepan_gfx_vulkan PRIVATE "--embed-dir=${FILEPAN_SPIRV_DIR}"
        -fvisibility=hidden)
# core/, for the filepan/gfx/ headers.
target_include_directories(filepan_gfx_vulkan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
target_link_libraries(filepan_gfx_vulkan PUBLIC filepan_options)

if (ANDROID)
    find_library(FILEPAN_VULKAN_LIBRARY vulkan REQUIRED)
    find_library(FILEPAN_ANDROID_LIBRARY android REQUIRED)
    find_library(FILEPAN_LOG_LIBRARY log REQUIRED)
    # AHardwareBuffer, for video frames.
    find_library(FILEPAN_NATIVEWINDOW_LIBRARY nativewindow REQUIRED)
    target_compile_definitions(filepan_gfx_vulkan PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
    target_link_libraries(filepan_gfx_vulkan PUBLIC ${FILEPAN_VULKAN_LIBRARY}
            ${FILEPAN_ANDROID_LIBRARY} ${FILEPAN_LOG_LIBRARY} ${FILEPAN_NATIVEWINDOW_LIBRARY})
else ()
    target_link_libraries(filepan_gfx_vulkan PUBLIC Vulkan::Vulkan)
endif ()
