# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# Host tests for core/filepan/vfs.

function(filepan_vfs_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(${name} PRIVATE filepan_vfs)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

filepan_vfs_test(archive_backend_test)
target_link_libraries(archive_backend_test PRIVATE filepan_jobs)
filepan_vfs_test(directory_watcher_test)
filepan_vfs_test(document_backend_test)
filepan_vfs_test(local_backend_test)
filepan_vfs_test(location_test)
filepan_vfs_test(mime_test)
# Checks invariants on the generated tables themselves.
target_include_directories(mime_test PRIVATE $<TARGET_PROPERTY:filepan_vfs,BINARY_DIR>/generated)
filepan_vfs_test(save_test)
filepan_vfs_test(sort_test)

# Needs OpenSSH's sshd and ssh-keygen, which the dev shell provides.
find_program(SSHD_EXECUTABLE sshd)
find_program(SSH_KEYGEN_EXECUTABLE ssh-keygen)
if (SSHD_EXECUTABLE AND SSH_KEYGEN_EXECUTABLE)
    filepan_vfs_test(sftp_backend_test)
    target_link_libraries(sftp_backend_test PRIVATE filepan_jobs)
    target_compile_definitions(sftp_backend_test PRIVATE
            FILEPAN_SSHD="${SSHD_EXECUTABLE}" FILEPAN_SSH_KEYGEN="${SSH_KEYGEN_EXECUTABLE}")
else ()
    message(WARNING "sshd not found; skipping sftp_backend_test")
endif ()

# Not a test: build/host/vfs/vfs_benchmark [directory] [entry count]
add_executable(vfs_benchmark vfs_benchmark.cpp)
target_include_directories(vfs_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(vfs_benchmark PRIVATE filepan_vfs)
