# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorChannel — ordered byte frames between two processes, with
# backpressure: a shared-memory ring on one computer, and a stream of several
# channels over any connected byte stream (a Unix socket, or ssh's standard
# input and output).
#
# No Qt and no other Phosphor library, in the library and in its build: a
# headless daemon on a host without Qt builds it alone. The tests use Qt
# Test, as the other libraries do, and are left out when Qt is not found.
# Linux only: memfd, SCM_RIGHTS and MSG_NOSIGNAL have no portable stand-in.

cmake_minimum_required(VERSION 3.16)

set(PHOSPHORCHANNEL_VERSION "0.1.0")

# The top of the build when built alone. A project that adds it with
# add_subdirectory keeps its own settings and its own BUILD_TESTING.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(PhosphorChannel VERSION ${PHOSPHORCHANNEL_VERSION} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    option(BUILD_TESTING "Build the PhosphorChannel tests (needs Qt6 Test)" ON)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "PhosphorChannel needs Linux (memfd, SCM_RIGHTS, MSG_NOSIGNAL)")
endif()

# While the version is 0.x, every minor version may change the ABI: the
# soname carries major.minor, and a consumer of 0.1 does not load 0.2. From
# 1.0 on, only a major version may, and the soname carries the major alone.
if(PHOSPHORCHANNEL_VERSION VERSION_LESS 1)
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" PHOSPHORCHANNEL_SOVERSION "${PHOSPHORCHANNEL_VERSION}")
    set(PHOSPHORCHANNEL_COMPATIBILITY SameMinorVersion)
else()
    string(REGEX MATCH "^[0-9]+" PHOSPHORCHANNEL_SOVERSION "${PHOSPHORCHANNEL_VERSION}")
    set(PHOSPHORCHANNEL_COMPATIBILITY SameMajorVersion)
endif()

include(GenerateExportHeader)

add_library(PhosphorChannel SHARED
    src/Channel.cpp
    src/Handshake.cpp
    src/Ring.cpp
    src/Stream.cpp
)
add_library(PhosphorChannel::PhosphorChannel ALIAS PhosphorChannel)

generate_export_header(PhosphorChannel
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/PhosphorChannel/phosphorchannel_export.h
    EXPORT_MACRO_NAME PHOSPHORCHANNEL_EXPORT
)

if(NOT DEFINED KDE_INSTALL_INCLUDEDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR})
    set(KDE_INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR})
    set(KDE_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR})
endif()

target_include_directories(PhosphorChannel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
)

target_compile_features(PhosphorChannel PUBLIC cxx_std_20)

# pthread_sigmask, which keeps a write to a closed pipe from raising SIGPIPE.
find_package(Threads REQUIRED)
target_link_libraries(PhosphorChannel PRIVATE Threads::Threads)

# The callbacks of a Stream are the consumer's code, which may throw. The
# tree's KDECompilerSettings turn exceptions off; built so, the library's
# frames would unwind without their cleanups, and differ from the standalone
# build. On for the whole library.
target_compile_options(PhosphorChannel PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-fexceptions>)

# Its sources have no Qt: the top level's AUTOMOC must not scan them.
set_target_properties(PhosphorChannel PROPERTIES
    VERSION ${PHOSPHORCHANNEL_VERSION}
    SOVERSION ${PHOSPHORCHANNEL_SOVERSION}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

# ═══════════════════════════════════════════════════════════════════════════════
# Install
# ═══════════════════════════════════════════════════════════════════════════════

install(TARGETS PhosphorChannel
    EXPORT PhosphorChannelTargets
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
)

install(DIRECTORY include/PhosphorChannel
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/PhosphorChannel/phosphorchannel_export.h"
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorChannel
)

install(EXPORT PhosphorChannelTargets
    FILE PhosphorChannelTargets.cmake
    NAMESPACE PhosphorChannel::
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorChannel
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PhosphorChannelConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorChannelConfig.cmake"
    INSTALL_DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorChannel
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorChannelConfigVersion.cmake"
    VERSION ${PHOSPHORCHANNEL_VERSION}
    COMPATIBILITY ${PHOSPHORCHANNEL_COMPATIBILITY}
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorChannelConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorChannelConfigVersion.cmake"
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorChannel
)

# ═══════════════════════════════════════════════════════════════════════════════
# Tests
# ═══════════════════════════════════════════════════════════════════════════════

if(BUILD_TESTING)
    find_package(Qt6 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "PhosphorChannel: Qt6 Test not found, so its tests are not built")
    endif()
endif()
