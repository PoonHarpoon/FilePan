# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# Host build of the native core for tests and synthetic benchmarks:
#
#   cmake -S app/src/main/cpp/host -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/host && ctest --test-dir build/host --output-on-failure

cmake_minimum_required(VERSION 3.22)
project(FilePanHost C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The tests cover the optional modules unless configured with -DFILEPAN_ADIRSTAT=OFF.
set(FILEPAN_ADIRSTAT ON CACHE BOOL "Build the disk usage analyzer (adirstat)")

add_subdirectory(.. core)

enable_testing()

add_subdirectory(jobs)
add_subdirectory(vfs)
add_subdirectory(ui)

add_executable(directory_reader_test directory_reader_test.cpp)
target_link_libraries(directory_reader_test PRIVATE filepan_core)
add_test(NAME directory_reader_test COMMAND directory_reader_test)

add_executable(delete_tree_test delete_tree_test.cpp)
target_link_libraries(delete_tree_test PRIVATE filepan_core)
add_test(NAME delete_tree_test COMMAND delete_tree_test)

add_executable(copy_file_test copy_file_test.cpp)
target_link_libraries(copy_file_test PRIVATE filepan_core)
add_test(NAME copy_file_test COMMAND copy_file_test)

add_executable(move_test move_test.cpp)
target_link_libraries(move_test PRIVATE filepan_core)
add_test(NAME move_test COMMAND move_test)

add_executable(core_benchmark core_benchmark.cpp)
target_link_libraries(core_benchmark PRIVATE filepan_core)

add_subdirectory(gfx)


add_executable(third_party_smoke_test third_party_smoke_test.cpp)
target_link_libraries(third_party_smoke_test PRIVATE filepan::archive filepan::ssh filepan::freetype
        filepan::harfbuzz)
add_test(NAME third_party_smoke_test COMMAND third_party_smoke_test)

add_executable(extract_archive_test extract_archive_test.cpp)
target_link_libraries(extract_archive_test PRIVATE filepan_core)
add_test(NAME extract_archive_test COMMAND extract_archive_test)

add_executable(create_archive_test create_archive_test.cpp)
target_link_libraries(create_archive_test PRIVATE filepan_core)
add_test(NAME create_archive_test COMMAND create_archive_test)

add_executable(archive_stream_test archive_stream_test.cpp)
target_link_libraries(archive_stream_test PRIVATE filepan_core)
add_test(NAME archive_stream_test COMMAND archive_stream_test)

add_executable(verify_archive_test verify_archive_test.cpp)
target_link_libraries(verify_archive_test PRIVATE filepan_core)
add_test(NAME verify_archive_test COMMAND verify_archive_test)

add_executable(securitykey_test securitykey_test.cpp)
target_link_libraries(securitykey_test PRIVATE filepan_core)
add_test(NAME securitykey_test COMMAND securitykey_test)

add_executable(openpgp_card_test openpgp_card_test.cpp)
target_link_libraries(openpgp_card_test PRIVATE filepan_core)
add_test(NAME openpgp_card_test COMMAND openpgp_card_test)

add_executable(ssh_agent_test ssh_agent_test.cpp)
target_link_libraries(ssh_agent_test PRIVATE filepan_core)
add_test(NAME ssh_agent_test COMMAND ssh_agent_test)

add_executable(checksum_test checksum_test.cpp)
target_link_libraries(checksum_test PRIVATE filepan_core)
add_test(NAME checksum_test COMMAND checksum_test)

# Needs OpenSSH's sshd and ssh-keygen, which the dev shell provides.
find_program(SSHD_EXECUTABLE sshd)
find_program(SSH_KEYGEN_EXECUTABLE ssh-keygen)
if (SSHD_EXECUTABLE AND SSH_KEYGEN_EXECUTABLE)
    add_executable(sftp_client_test sftp_client_test.cpp)
    target_link_libraries(sftp_client_test PRIVATE filepan_core)
    target_compile_definitions(sftp_client_test PRIVATE
            FILEPAN_SSHD="${SSHD_EXECUTABLE}" FILEPAN_SSH_KEYGEN="${SSH_KEYGEN_EXECUTABLE}")
    add_test(NAME sftp_client_test COMMAND sftp_client_test)

    add_executable(private_key_test private_key_test.cpp)
    target_link_libraries(private_key_test PRIVATE filepan_core)
    target_compile_definitions(private_key_test PRIVATE
            FILEPAN_SSHD="${SSHD_EXECUTABLE}" FILEPAN_SSH_KEYGEN="${SSH_KEYGEN_EXECUTABLE}")
    add_test(NAME private_key_test COMMAND private_key_test)
else ()
    message(WARNING "sshd not found; skipping sftp_client_test")
endif ()

add_subdirectory(text)
add_subdirectory(runtime)
add_subdirectory(media)
add_subdirectory(ui_components)
add_subdirectory(app)
add_subdirectory(adirstat)
