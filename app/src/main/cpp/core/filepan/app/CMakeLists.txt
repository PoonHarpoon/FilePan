# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# FilePan's screens and state: the App root, the storage home, the browsing screen, settings,
# SFTP servers, the prompt sheets, about and licenses.

if (NOT TARGET filepan_runtime)
    return()
endif ()

add_library(filepan_app STATIC
        about_screen.cpp
        app.cpp
        app_state.cpp
        backup.cpp
        backup_sheets.cpp
        browse_screen.cpp
        decision_sheets.cpp
        document_trees.cpp
        directory_model.cpp
        favorites_sheet.cpp
        file_actions.cpp
        file_dialogs.cpp
        file_operations.cpp
        format.cpp
        hidden_items_screen.cpp
        home_screen.cpp
        jobs_ui.cpp
        licenses.cpp
        onboarding_screen.cpp
        places.cpp
        prompt_sheets.cpp
        properties_sheet.cpp
        remote_media.cpp
        screen_parts.cpp
        secrets.cpp
        settings.cpp
        settings_screen.cpp
        sftp_screens.cpp
        sheet_parts.cpp
        thumbnail_store.cpp
        text_screen.cpp
        thumbnails.cpp
        user_items.cpp
        video_thumbnails.cpp
        viewer_screen.cpp)
target_compile_options(filepan_app PRIVATE -fvisibility=hidden)
target_link_libraries(filepan_app PUBLIC filepan_runtime filepan_ui_components filepan_vfs filepan_text
        filepan_media)
# The disk tier of the thumbnail cache compresses with zstd.
target_link_libraries(filepan_app PRIVATE filepan::zstd)
# Backups are sealed with AES-GCM under a PBKDF2 key.
target_link_libraries(filepan_app PRIVATE filepan::mbedcrypto)
# The disk usage analyzer's screens build into this library, so the engine (filepan_adirstat)
# never depends on the app.
if (TARGET filepan_adirstat)
    target_sources(filepan_app PRIVATE adirstat/disk_usage_screen.cpp)
    target_link_libraries(filepan_app PUBLIC filepan_adirstat)
    target_compile_definitions(filepan_app PUBLIC FILEPAN_ADIRSTAT=1)
endif ()
if (ANDROID)
    # AImageDecoder for thumbnails.
    target_link_libraries(filepan_app PRIVATE jnigraphics)
endif ()
