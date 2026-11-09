# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# The file job queue: copy, move, delete and the rest, over the VFS backends.

add_library(filepan_jobs STATIC
        archive_jobs.cpp
        decision.cpp
        file_jobs.cpp
        job_context.cpp
        job_queue.cpp)
target_compile_options(filepan_jobs PRIVATE -fvisibility=hidden)
target_link_libraries(filepan_jobs PUBLIC filepan_vfs)
