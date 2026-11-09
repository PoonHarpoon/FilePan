# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

# Builds the pinned native dependencies in third_party/ (git submodules) with ExternalProject,
# installing them into a staging prefix where libarchive's own find_package() logic finds them.
# They are always optimized, even in debug builds, so that performance testing is meaningful.
#
# Defines imported static library targets such as filepan::archive and filepan::ssh.

include(ExternalProject)

get_filename_component(FILEPAN_THIRD_PARTY_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../third_party"
        ABSOLUTE)
if (NOT EXISTS "${FILEPAN_THIRD_PARTY_DIR}/libarchive/CMakeLists.txt")
    message(FATAL_ERROR "Native dependencies are missing; run: git submodule update --init --recursive")
endif ()

if (ANDROID)
    # Nixpkgs' CMake doesn't search /usr/include, so it wouldn't look in the NDK sysroot's either.
    list(APPEND CMAKE_SYSTEM_INCLUDE_PATH /usr/include)
endif ()

set(FILEPAN_DEPS_PREFIX "${CMAKE_BINARY_DIR}/filepan-deps")
set(FILEPAN_DEPS_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX=${FILEPAN_DEPS_PREFIX}
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_PREFIX_PATH=${FILEPAN_DEPS_PREFIX}
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_TESTING=OFF
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        "-DCMAKE_C_FLAGS=-fvisibility=hidden"
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
if (ANDROID)
    list(APPEND FILEPAN_DEPS_ARGS
            -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
            -DANDROID_ABI=${ANDROID_ABI}
            -DANDROID_PLATFORM=${ANDROID_PLATFORM}
            -DANDROID_STL=${ANDROID_STL}
            -DCMAKE_SYSTEM_INCLUDE_PATH=/usr/include
            # The NDK toolchain only searches the sysroot unless told otherwise.
            -DCMAKE_FIND_ROOT_PATH=${FILEPAN_DEPS_PREFIX})
endif ()
if (CMAKE_MAKE_PROGRAM)
    list(APPEND FILEPAN_DEPS_ARGS -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})
endif ()

ExternalProject_Add(filepan_zstd
        SOURCE_DIR ${FILEPAN_THIRD_PARTY_DIR}/zstd/build/cmake
        CMAKE_ARGS ${FILEPAN_DEPS_ARGS}
            -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_CONTRIB=OFF
            -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_MULTITHREAD_SUPPORT=ON
        BUILD_BYPRODUCTS ${FILEPAN_DEPS_PREFIX}/lib/libzstd.a)

ExternalProject_Add(filepan_lz4
        SOURCE_DIR ${FILEPAN_THIRD_PARTY_DIR}/lz4/build/cmake
        CMAKE_ARGS ${FILEPAN_DEPS_ARGS}
            -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF -DBUILD_STATIC_LIBS=ON
        BUILD_BYPRODUCTS ${FILEPAN_DEPS_PREFIX}/lib/liblz4.a)

ExternalProject_Add(filepan_xz
        SOURCE_DIR ${FILEPAN_THIRD_PARTY_DIR}/xz
        CMAKE_ARGS ${FILEPAN_DEPS_ARGS}
            -DXZ_NLS=OFF -DXZ_DOC=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF
            -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF
        BUILD_BYPRODUCTS ${FILEPAN_DEPS_PREFIX}/lib/liblzma.a)

ExternalProject_Add(filepan_bzip2
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/bzip2
        CMAKE_ARGS ${FILEPAN_DEPS_ARGS} -DBZIP2_SOURCE_DIR=${FILEPAN_THIRD_PARTY_DIR}/bzip2
        BUILD_BYPRODUCTS ${FILEPAN_DEPS_PREFIX}/lib/libbz2.a)

ExternalProject_Add(filepan_mbedtls
        SOURCE_DIR ${FILEPAN_THIRD_PARTY_DIR}/mbedtls
        CMAKE_ARGS ${FILEPAN_DEPS_ARGS}
            -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
            -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
            -DMBEDTLS_USER_CONFIG_FILE=${CMAKE_CURRENT_LIST_DIR}/mbedtls_user_config.h
        BUILD_BYPRODUCTS
            ${FILEPAN_DEPS_PREFIX}/lib/libmbedtls.a
            ${FILEPAN_DEPS_PREFIX}/lib/libmbedx509.a
            ${FILEPAN_DEPS_PREFIX}/lib/libmbedcrypto.a
            ${FILEPAN_DEPS_PREFIX}/lib/libeverest.a
            ${FILEPAN_DEPS_PREFIX}/lib/libp256m.a)
# Make the installed configuration include ours, so that libarchive, libssh and FilePan compile
# against the same struct layouts as the library.
ExternalProject_Add_Step(filepan_mbedtls append_user_config
        COMMAND ${CMAKE_COMMAND}
            -DCONFIG=${FILEPAN_DEPS_PREFIX}/include/mbedtls/mbedtls_config.h
            -DUSER_CONFIG=${CMAKE_CURRENT_LIST_DIR}/mbedtls_user_config.h
            -P ${CMAKE_CURRENT_LIST_DIR}/AppendMbedtlsUserConfig.cmake
        DEPENDEES install
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/mbedtls_user_config.h)

ExternalProject_Add(filepan_libssh
        SOURCE_DIR ${FILEPAN_THIRD_PARTY_DIR}/libssh
        DEPENDS filepan_mbedtls
        CMAKE_ARGS ${FILEPAN_DEPS_ARGS}
            -DWITH_MBEDTLS=ON -DWITH_GCRYPT=OFF -DMBEDTLS_ROOT_DIR=${FILEPAN_DEPS_PREFIX}
            -DWITH_SFTP=ON -DWITH_ZLIB=ON -DWITH_SERVER=OFF -DWITH_GSSAPI=OFF -DWITH_PCAP=OFF
            -DWITH_EXAMPLES=OFF -DWITH_NACL=OFF -DWITH_PKCS11_URI=OFF
            # Security keys, through our callbacks rather than libfido2.
            -DWITH_FIDO2=ON
            -DWITH_SYMBOL_VERSIONING=OFF -DUNIT_TESTING=OFF -DCLIENT_TESTING=OFF
            -DSERVER_TESTING=OFF -DGLOBAL_BIND_CONFIG=/nonexistent -DGLOBAL_CLIENT_CONFIG=/nonexistent
            # Bionic lacks GLOB_TILDE (only used for ssh_config Include, which FilePan doesn't
            # process) and the obsolete S_IWRITE alias.
            "-DCMAKE_C_FLAGS=-fvisibility=hidden -DGLOB_TILDE=0 -DS_IWRITE=S_IWUSR"
        BUILD_BYPRODUCTS ${FILEPAN_DEPS_PREFIX}/lib/libssh.a)

ExternalProject_Add(filepan_libarchive
        SOURCE_DIR ${FILEPAN_THIRD_PARTY_DIR}/libarchive
        DEPENDS filepan_zstd filepan_lz4 filepan_xz filepan_bzip2 filepan_mbedtls
        CMAKE_ARGS ${FILEPAN_DEPS_ARGS}
            -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF
            -DENABLE_TEST=OFF -DENABLE_INSTALL=ON -DENABLE_WERROR=OFF
            -DENABLE_MBEDTLS=ON -DENABLE_OPENSSL=OFF -DENABLE_NETTLE=OFF -DENABLE_CNG=OFF
            -DENABLE_ZLIB=ON -DENABLE_BZip2=ON -DENABLE_LZMA=ON -DENABLE_ZSTD=ON -DENABLE_LZ4=ON
            -DENABLE_LZO=OFF -DENABLE_LIBB2=OFF -DENABLE_LIBXML2=OFF -DENABLE_BSDXML=OFF
            -DENABLE_EXPAT=OFF -DENABLE_PCREPOSIX=OFF -DENABLE_PCRE2POSIX=OFF -DENABLE_LIBGCC=OFF
            -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_ICONV=OFF
            # libarchive's find modules look for these by name.
            -DBZIP2_INCLUDE_DIR=${FILEPAN_DEPS_PREFIX}/include
            -DBZIP2_LIBRARIES=${FILEPAN_DEPS_PREFIX}/lib/libbz2.a
        BUILD_BYPRODUCTS ${FILEPAN_DEPS_PREFIX}/lib/libarchive.a)

# Text: FreeType rasterizes glyphs, HarfBuzz shapes with its own OpenType font functions (so it
# doesn't need FreeType) and draws color glyphs (COLRv0/v1) with harfbuzz-raster. Neither uses
# compressed or PNG fonts, glib, ICU or cairo. FreeType gets FilePan's module list
# (freetype/filepan_ftmodule.h).
ExternalProject_Add(filepan_freetype
        SOURCE_DIR ${FILEPAN_THIRD_PARTY_DIR}/freetype
        CMAKE_ARGS ${FILEPAN_DEPS_ARGS}
            -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON
            -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_HVF=ON
            "-DCMAKE_C_FLAGS=-fvisibility=hidden -I${CMAKE_CURRENT_LIST_DIR}/freetype -include ${CMAKE_CURRENT_LIST_DIR}/freetype/filepan_ftconfig.h"
        BUILD_BYPRODUCTS ${FILEPAN_DEPS_PREFIX}/lib/libfreetype.a)

ExternalProject_Add(filepan_harfbuzz
        SOURCE_DIR ${FILEPAN_THIRD_PARTY_DIR}/harfbuzz
        CMAKE_ARGS ${FILEPAN_DEPS_ARGS}
            -DHB_HAVE_FREETYPE=OFF -DHB_HAVE_GLIB=OFF -DHB_HAVE_ICU=OFF -DHB_HAVE_CAIRO=OFF
            -DHB_HAVE_GRAPHITE2=OFF -DHB_HAVE_GOBJECT=OFF -DHB_HAVE_INTROSPECTION=OFF
            -DHB_BUILD_UTILS=OFF -DHB_BUILD_SUBSET=OFF -DHB_BUILD_VECTOR=OFF -DHB_BUILD_GPU=OFF
            -DHB_BUILD_GPU_DEMO=OFF -DHB_BUILD_RASTER=ON -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON
            "-DCMAKE_CXX_FLAGS=-fvisibility=hidden"
        BUILD_BYPRODUCTS
            ${FILEPAN_DEPS_PREFIX}/lib/libharfbuzz.a
            ${FILEPAN_DEPS_PREFIX}/lib/libharfbuzz-raster.a)

file(MAKE_DIRECTORY ${FILEPAN_DEPS_PREFIX}/include)
file(MAKE_DIRECTORY ${FILEPAN_DEPS_PREFIX}/include/freetype2)
file(MAKE_DIRECTORY ${FILEPAN_DEPS_PREFIX}/include/harfbuzz)

function(filepan_import_library name file dependency)
    add_library(${name} STATIC IMPORTED GLOBAL)
    set_target_properties(${name} PROPERTIES
            IMPORTED_LOCATION ${FILEPAN_DEPS_PREFIX}/lib/${file}
            INTERFACE_INCLUDE_DIRECTORIES ${FILEPAN_DEPS_PREFIX}/include)
    add_dependencies(${name} ${dependency})
endfunction()

filepan_import_library(filepan::zstd libzstd.a filepan_zstd)
filepan_import_library(filepan::lz4 liblz4.a filepan_lz4)
filepan_import_library(filepan::lzma liblzma.a filepan_xz)
filepan_import_library(filepan::bz2 libbz2.a filepan_bzip2)
filepan_import_library(filepan::mbedtls libmbedtls.a filepan_mbedtls)
filepan_import_library(filepan::mbedx509 libmbedx509.a filepan_mbedtls)
filepan_import_library(filepan::mbedcrypto libmbedcrypto.a filepan_mbedtls)
filepan_import_library(filepan::archive libarchive.a filepan_libarchive)
filepan_import_library(filepan::ssh libssh.a filepan_libssh)
filepan_import_library(filepan::freetype libfreetype.a filepan_freetype)
filepan_import_library(filepan::harfbuzz libharfbuzz.a filepan_harfbuzz)
filepan_import_library(filepan::harfbuzz_raster libharfbuzz-raster.a filepan_harfbuzz)
set_property(TARGET filepan::freetype APPEND PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES ${FILEPAN_DEPS_PREFIX}/include/freetype2)
set_property(TARGET filepan::harfbuzz APPEND PROPERTY
        INTERFACE_INCLUDE_DIRECTORIES ${FILEPAN_DEPS_PREFIX}/include/harfbuzz)
set_target_properties(filepan::harfbuzz_raster PROPERTIES INTERFACE_LINK_LIBRARIES filepan::harfbuzz)

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
set_target_properties(filepan::archive PROPERTIES INTERFACE_LINK_LIBRARIES
        "filepan::zstd;filepan::lz4;filepan::lzma;filepan::bz2;filepan::mbedcrypto;ZLIB::ZLIB;Threads::Threads")
set_target_properties(filepan::mbedcrypto PROPERTIES
        INTERFACE_LINK_LIBRARIES
            "${FILEPAN_DEPS_PREFIX}/lib/libeverest.a;${FILEPAN_DEPS_PREFIX}/lib/libp256m.a")
set_target_properties(filepan::mbedx509 PROPERTIES INTERFACE_LINK_LIBRARIES filepan::mbedcrypto)
set_target_properties(filepan::ssh PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS LIBSSH_STATIC
        INTERFACE_LINK_LIBRARIES "filepan::mbedx509;filepan::mbedcrypto;ZLIB::ZLIB;Threads::Threads")
set_target_properties(filepan::mbedtls PROPERTIES INTERFACE_LINK_LIBRARIES filepan::mbedx509)
