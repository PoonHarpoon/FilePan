# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# Host tests for core/filepan/jobs.

function(filepan_jobs_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(${name} PRIVATE filepan_jobs)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

filepan_jobs_test(job_kinds_test)
filepan_jobs_test(job_queue_test)
