# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# What PlasmaZones builds from that is not in this repository, each pinned to
# one archive by version, URL and SHA-256: the phosphor-libs commit it builds
# against, the suite's branding release, the surface decoration packs commit,
# and, for the shell tier only
# (BUILD_PHOSPHOR_SHELL), the phosphor-shell-libs commit. This file is the only place these
# pins live. The phosphor-libs tree pins its own third-party sources (Luau,
# valijson) in its cmake/PhosphorDependencies.cmake, at the commit pinned here.
# The packaging recipes repeat the URL and hash of every archive they fetch,
# both these and the library tree's, and scripts/check-conventions.py (the
# dep-pins rule) fails when one of them no longer matches its pin.
#
# How phosphor-libs is obtained, in order:
#
#   1. PHOSPHOR_DEPS_ARCHIVES names a directory holding the archives under their
#      FILE names: these and the library tree's. Each is checked against its
#      SHA-256 and extracted into the build tree. Nothing is downloaded and CPM
#      is never loaded, so this is the path every offline packaging build takes
#      (makepkg, rpmbuild, OBS, dpkg-buildpackage, Nix).
#   2. PHOSPHOR_LIBS_GIT_REF names a branch, tag or commit. CPM clones
#      phosphor-libs at that ref instead of the pinned archive, for building
#      against a library change before it is pinned (-DPHOSPHOR_LIBS_GIT_REF=main).
#   3. Otherwise CPM.cmake (https://github.com/cpm-cmake/CPM.cmake, itself pinned
#      by hash) downloads the pinned archive once into CPM_SOURCE_CACHE, which is
#      .cache/cpm at the repository root unless the cache variable or the
#      environment variable says otherwise. Every build directory shares it, and
#      a configure works offline once it is filled.
#
# CPM's own override applies to 2 and 3: -DCPM_phosphor-libs_SOURCE=<dir> builds
# a local checkout, such as the sibling ../phosphor-libs, in place of either.
# That is how a change spanning both repositories is developed.
#
# Script mode, for packagers and CI:
#
#   cmake -DPHOSPHOR_DEPS_FETCH=<dir> -P cmake/PlasmaZonesDependencies.cmake
#
# downloads every archive the build needs into <dir> under its FILE name and
# checks it: the ones pinned here, then the ones the pinned phosphor-libs tree
# pins, so <dir> can be handed to PHOSPHOR_DEPS_ARCHIVES. An archive already
# there with the right hash is kept.
#
# Bumping a pin: change its VERSION, URL, FILE and SHA256 together, with the
# hash taken from a download you checked, never from whatever the next configure
# fetched. Then run check-conventions.py, which names every recipe still
# carrying the old pin. A phosphor-libs bump can also move the library tree's
# own pins, which the same rule then reports.

include_guard(GLOBAL)

# ─── Pins ────────────────────────────────────────────────────────────────────
# PLASMAZONES_DEPS lists the ones every build needs, for the fetch script and
# for the dep-pins rule. PLASMAZONES_SHELL_DEPS lists the ones only the shell
# tier needs. No packaging recipe builds the shell, so those are neither in the
# recipes nor fetched by the script mode, and an offline build of the shell
# fetches them into its PHOSPHOR_DEPS_ARCHIVES directory by hand.
set(PLASMAZONES_DEPS phosphor-libs branding phosphor-surface-packs)
set(PLASMAZONES_SHELL_DEPS phosphor-shell-libs)

# The phosphor-libs commit PlasmaZones builds against (LGPL-2.1-or-later,
# phosphor-works/phosphor-libs): its placement engines, rendering, layer-shell
# and config libraries. A commit, not a release, so VERSION is the commit hash.
set(PLASMAZONES_DEP_phosphor-libs_VERSION "793344a136af82d067e4a8b0e5abe0d57404f74a")
set(PLASMAZONES_DEP_phosphor-libs_URL "https://github.com/phosphor-works/phosphor-libs/archive/793344a136af82d067e4a8b0e5abe0d57404f74a.tar.gz")
set(PLASMAZONES_DEP_phosphor-libs_FILE "phosphor-libs-793344a136af82d067e4a8b0e5abe0d57404f74a.tar.gz")
set(PLASMAZONES_DEP_phosphor-libs_SHA256 "a35b8fbdec56b8ed17368d5d4df149c9cd473f7a974aa815e7db78a0c19281fd")

# The phosphor-shell-libs commit the shell tier builds against
# (LGPL-2.1-or-later, phosphor-works/phosphor-shell-libs): the theme, popout and
# Phosphor.* surface libraries and the system service libraries. Added after
# phosphor-libs, so this file's phosphor-libs pin is the one in use and that
# tree's own pin is never fetched. A commit, so VERSION is the commit hash.
set(PLASMAZONES_DEP_phosphor-shell-libs_VERSION "18386019f57cd9d225c1bfe5d3bd4668cba07ddb")
set(PLASMAZONES_DEP_phosphor-shell-libs_URL "https://github.com/phosphor-works/phosphor-shell-libs/archive/18386019f57cd9d225c1bfe5d3bd4668cba07ddb.tar.gz")
set(PLASMAZONES_DEP_phosphor-shell-libs_FILE "phosphor-shell-libs-18386019f57cd9d225c1bfe5d3bd4668cba07ddb.tar.gz")
set(PLASMAZONES_DEP_phosphor-shell-libs_SHA256 "4f93d39a3d793787007c9cb21ca7b5803837d395eb64e194b84a3ad3db5aab5e")

# The suite's branding release (CC-BY-SA-4.0, phosphor-works/branding): the app
# icons, installed from it with its cmake/PhosphorBranding.cmake. No app keeps
# icon files of its own.
set(PLASMAZONES_DEP_branding_VERSION "0.3.1")
set(PLASMAZONES_DEP_branding_URL "https://github.com/phosphor-works/branding/releases/download/v0.3.1/phosphor-branding-0.3.1.tar.gz")
set(PLASMAZONES_DEP_branding_FILE "phosphor-branding-0.3.1.tar.gz")
set(PLASMAZONES_DEP_branding_SHA256 "d6ec9261d4391148f5c21dfb02be410214d42ea675f1558a826042d63f495094")

# The surface decoration packs commit (LGPL-2.1-or-later with a few GPL-3.0 and
# BSD-3-Clause files, phosphor-works/phosphor-surface-packs): the border, glow,
# shadow, blur and glass packs PlasmaZones and Lambent draw, installed from it
# with its cmake/PhosphorSurfacePacks.cmake. A commit, so VERSION is the commit
# hash.
set(PLASMAZONES_DEP_phosphor-surface-packs_VERSION "e89d9f7bb6320eab8b6a0b3325858d2ca5356ce3")
set(PLASMAZONES_DEP_phosphor-surface-packs_URL "https://github.com/phosphor-works/phosphor-surface-packs/archive/e89d9f7bb6320eab8b6a0b3325858d2ca5356ce3.tar.gz")
set(PLASMAZONES_DEP_phosphor-surface-packs_FILE "phosphor-surface-packs-e89d9f7bb6320eab8b6a0b3325858d2ca5356ce3.tar.gz")
set(PLASMAZONES_DEP_phosphor-surface-packs_SHA256 "fbbf0b9849d83396029bc4e310c3dcac49c08758e73d560538888e685f0a5e30")

# glslang's prebuilt tools, a test-time program and not a source dependency, so
# not in PLASMAZONES_DEPS: no packaging build runs the tests. Khronos publishes
# a Linux build for x86-64 only. See phosphor_put_glslang_on_test_path().
set(PHOSPHOR_GLSLANG_VERSION "16.6.0")
set(PHOSPHOR_GLSLANG_URL
    "https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-linux-x86_64-release.tar.gz")
set(PHOSPHOR_GLSLANG_SHA256 "2c34071f56ecf39d16233294d3739cf43f981fd39080f2fd9dccb782101191ce")

# CPM.cmake itself. Loaded here first, so the phosphor-libs tree, which pins the
# same file, uses this copy.
set(PHOSPHOR_CPM_VERSION "0.43.2")
set(PHOSPHOR_CPM_SHA256 "49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa")

# The repository root: this file sits in its cmake/ directory. Captured here,
# at include time, because CMAKE_CURRENT_LIST_DIR names the caller inside a
# function.
get_filename_component(_PLASMAZONES_DEPS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# _plasmazones_download(<name> <dir>): the archive pinned as <name> into <dir>
# under its FILE name, kept when one with the right hash is already there.
function(_plasmazones_download name dir)
    set(_file "${dir}/${PLASMAZONES_DEP_${name}_FILE}")
    set(_have "")
    if(EXISTS "${_file}")
        file(SHA256 "${_file}" _have)
    endif()
    if(NOT _have STREQUAL PLASMAZONES_DEP_${name}_SHA256)
        message(STATUS "Downloading ${PLASMAZONES_DEP_${name}_URL}")
        # To a .part file, renamed only once its hash checks, so an interrupted
        # download never sits under the real name.
        file(DOWNLOAD "${PLASMAZONES_DEP_${name}_URL}" "${_file}.part"
             EXPECTED_HASH SHA256=${PLASMAZONES_DEP_${name}_SHA256} STATUS _status)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            file(REMOVE "${_file}.part")
            list(GET _status 1 _why)
            message(FATAL_ERROR "Could not download ${PLASMAZONES_DEP_${name}_URL}: ${_why}")
        endif()
        file(RENAME "${_file}.part" "${_file}")
    endif()
    message(STATUS "${PLASMAZONES_DEP_${name}_FILE}: ok")
endfunction()

# _plasmazones_extract(<archive> <dest>): <archive> unpacked into <dest> with its
# single top-level directory removed. The stamp is written after the extraction
# finishes, so one cut short (disk full, ^C) is redone next time instead of
# passing as complete.
function(_plasmazones_extract archive dest stamp)
    if(EXISTS "${stamp}")
        return()
    endif()
    file(REMOVE_RECURSE "${dest}" "${dest}.tmp")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dest}.tmp")
    file(GLOB _top LIST_DIRECTORIES true "${dest}.tmp/*")
    list(LENGTH _top _count)
    if(_count EQUAL 1 AND IS_DIRECTORY "${_top}")
        file(RENAME "${_top}" "${dest}")
        file(REMOVE_RECURSE "${dest}.tmp")
    else()
        file(RENAME "${dest}.tmp" "${dest}")
    endif()
    file(TOUCH "${stamp}")
endfunction()

# ─── Script mode: download every archive ─────────────────────────────────────
if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT PHOSPHOR_DEPS_FETCH)
        message(FATAL_ERROR "usage: cmake -DPHOSPHOR_DEPS_FETCH=<dir> -P ${CMAKE_CURRENT_LIST_FILE}")
    endif()
    get_filename_component(_dir "${PHOSPHOR_DEPS_FETCH}" ABSOLUTE)
    file(MAKE_DIRECTORY "${_dir}")
    foreach(_name IN LISTS PLASMAZONES_DEPS)
        _plasmazones_download(${_name} "${_dir}")
    endforeach()
    # The library tree's own pins, read from the pinned tree itself, so the
    # list here can never fall behind a phosphor-libs bump.
    set(_libs_tmp "${_dir}/.phosphor-libs-${PLASMAZONES_DEP_phosphor-libs_VERSION}")
    _plasmazones_extract("${_dir}/${PLASMAZONES_DEP_phosphor-libs_FILE}" "${_libs_tmp}" "${_libs_tmp}.stamp")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DPHOSPHOR_DEPS_FETCH=${_dir}"
                -P "${_libs_tmp}/cmake/PhosphorDependencies.cmake"
        RESULT_VARIABLE _rc)
    file(REMOVE_RECURSE "${_libs_tmp}" "${_libs_tmp}.stamp")
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "Fetching the archives phosphor-libs ${PLASMAZONES_DEP_phosphor-libs_VERSION} pins failed")
    endif()
    return()
endif()

# ─── Configure mode ──────────────────────────────────────────────────────────
set(PHOSPHOR_DEPS_ARCHIVES "" CACHE PATH
    "Directory holding the pinned dependency archives (cmake/PlasmaZonesDependencies.cmake), for an offline build. Empty: download them with CPM")
set(PHOSPHOR_LIBS_GIT_REF "" CACHE STRING
    "Build phosphor-libs from this git branch, tag or commit instead of the pinned archive. Empty: the pin")

# CPM.cmake, loaded here at the including directory's scope rather than from
# inside a function: CPM keeps part of its state in ordinary variables of the
# scope that includes it, which a function scope would drop. Not loaded at all
# for an offline build (PHOSPHOR_DEPS_ARCHIVES), which must not reach the
# network even for CPM itself. The cached copy is checked against its hash on
# every configure, not only after a download, so a broken-off download left in
# the cache is replaced rather than included.
if(NOT PHOSPHOR_DEPS_ARCHIVES AND NOT COMMAND CPMAddPackage)
    if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
        set(CPM_SOURCE_CACHE "${_PLASMAZONES_DEPS_ROOT}/.cache/cpm" CACHE PATH
            "Where CPM.cmake keeps downloaded packages, shared by every build directory")
    endif()
    if(DEFINED CPM_SOURCE_CACHE)
        set(_phosphor_cpm_file "${CPM_SOURCE_CACHE}/cpm/CPM_${PHOSPHOR_CPM_VERSION}.cmake")
    else()
        set(_phosphor_cpm_file "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${PHOSPHOR_CPM_VERSION}.cmake")
    endif()
    set(_phosphor_cpm_have "")
    if(EXISTS "${_phosphor_cpm_file}")
        file(SHA256 "${_phosphor_cpm_file}" _phosphor_cpm_have)
    endif()
    if(NOT _phosphor_cpm_have STREQUAL PHOSPHOR_CPM_SHA256)
        file(DOWNLOAD
            "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${PHOSPHOR_CPM_VERSION}/CPM.cmake"
            "${_phosphor_cpm_file}.part" EXPECTED_HASH SHA256=${PHOSPHOR_CPM_SHA256} STATUS _phosphor_cpm_status)
        list(GET _phosphor_cpm_status 0 _phosphor_cpm_code)
        if(NOT _phosphor_cpm_code EQUAL 0)
            file(REMOVE "${_phosphor_cpm_file}.part")
            message(FATAL_ERROR
                "Could not download CPM.cmake (${_phosphor_cpm_status}). Configure with network access once, "
                "or pass -DPHOSPHOR_DEPS_ARCHIVES=<dir> with the archives cmake/PlasmaZonesDependencies.cmake pins.")
        endif()
        file(RENAME "${_phosphor_cpm_file}.part" "${_phosphor_cpm_file}")
    endif()
    include("${_phosphor_cpm_file}")
endif()

# plasmazones_fetch_dependency(<name>)
#
# Makes the pinned archive <name> (one of PLASMAZONES_DEPS) available and sets
# <name>_SOURCE_DIR in the caller's scope to its unpacked tree. Only fetches the
# source. What to build from it is the caller's business.
function(plasmazones_fetch_dependency name)
    if(NOT name IN_LIST PLASMAZONES_DEPS AND NOT name IN_LIST PLASMAZONES_SHELL_DEPS)
        message(FATAL_ERROR "plasmazones_fetch_dependency: '${name}' is not pinned in ${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
    endif()
    set(_version "${PLASMAZONES_DEP_${name}_VERSION}")
    set(_sha "${PLASMAZONES_DEP_${name}_SHA256}")

    if(PHOSPHOR_DEPS_ARCHIVES)
        set(_archive "${PHOSPHOR_DEPS_ARCHIVES}/${PLASMAZONES_DEP_${name}_FILE}")
        if(NOT EXISTS "${_archive}")
            message(FATAL_ERROR
                "PHOSPHOR_DEPS_ARCHIVES is ${PHOSPHOR_DEPS_ARCHIVES}, which has no ${PLASMAZONES_DEP_${name}_FILE}. "
                "Fetch it from ${PLASMAZONES_DEP_${name}_URL}, or fill the directory with "
                "cmake -DPHOSPHOR_DEPS_FETCH=<dir> -P cmake/PlasmaZonesDependencies.cmake")
        endif()
        file(SHA256 "${_archive}" _have)
        if(NOT _have STREQUAL _sha)
            message(FATAL_ERROR
                "${_archive} does not match its pin:\n  expected ${_sha}\n  got      ${_have}")
        endif()
        # Extracted once per pinned version. The hash is part of the stamp, so
        # a re-pinned archive under the same version is extracted again.
        set(_root "${CMAKE_BINARY_DIR}/_phosphor_deps")
        set(_dest "${_root}/${name}-${_version}")
        _plasmazones_extract("${_archive}" "${_dest}" "${_root}/.${name}-${_version}-${_sha}")
        set(${name}_SOURCE_DIR "${_dest}" PARENT_SCOPE)
        return()
    endif()

    CPMAddPackage(NAME ${name}
                  URL "${PLASMAZONES_DEP_${name}_URL}"
                  URL_HASH SHA256=${_sha}
                  DOWNLOAD_ONLY YES)
    set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

# plasmazones_add_phosphor_libs()
#
# Adds the phosphor-libs tree as a subproject, so every find_package and
# target_link_libraries call for a Phosphor library resolves to its targets, and
# leaves phosphor-libs_SOURCE_DIR and phosphor-libs_BINARY_DIR set in the
# caller's scope for the tests that reach into the library tree. A macro, so CPM
# sets those two in the caller's scope directly. Call it from the top-level
# CMakeLists, once ECM, Qt and the compile definitions every library must share
# are set up.
macro(plasmazones_add_phosphor_libs)
    if(PHOSPHOR_DEPS_ARCHIVES)
        plasmazones_fetch_dependency(phosphor-libs)
        set(phosphor-libs_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/phosphor-libs-build")
        add_subdirectory("${phosphor-libs_SOURCE_DIR}" "${phosphor-libs_BINARY_DIR}")
    elseif(PHOSPHOR_LIBS_GIT_REF)
        CPMAddPackage(NAME phosphor-libs
                      GIT_REPOSITORY "https://github.com/phosphor-works/phosphor-libs.git"
                      GIT_TAG "${PHOSPHOR_LIBS_GIT_REF}"
                      GIT_SHALLOW NO)
    else()
        CPMAddPackage(NAME phosphor-libs
                      URL "${PLASMAZONES_DEP_phosphor-libs_URL}"
                      URL_HASH SHA256=${PLASMAZONES_DEP_phosphor-libs_SHA256})
    endif()
    if(NOT PHOSPHOR_LIBS_CMAKE_DIR)
        message(FATAL_ERROR
            "phosphor-libs at ${phosphor-libs_SOURCE_DIR} did not set PHOSPHOR_LIBS_CMAKE_DIR. "
            "It is older than the commit cmake/PlasmaZonesDependencies.cmake pins.")
    endif()
    # The library tree's helper modules (PhosphorTestIsolation, PhosphorTestPch),
    # reachable by name from here on.
    list(APPEND CMAKE_MODULE_PATH "${PHOSPHOR_LIBS_CMAKE_DIR}")
endmacro()

# plasmazones_add_phosphor_shell_libs()
#
# Adds the phosphor-shell-libs tree as a subproject for the shell tier, the way
# plasmazones_add_phosphor_libs() adds phosphor-libs, and after it: that tree
# then finds phosphor-libs already added and uses it. Leaves
# phosphor-shell-libs_SOURCE_DIR and phosphor-shell-libs_BINARY_DIR set in the
# caller's scope. -DCPM_phosphor-shell-libs_SOURCE=<dir> builds a local
# checkout, and -DPHOSPHOR_SHELL_LIBS_GIT_REF=<ref> a branch or commit.
set(PHOSPHOR_SHELL_LIBS_GIT_REF "" CACHE STRING
    "Build phosphor-shell-libs from this git branch, tag or commit instead of the pinned archive. Empty: the pin")
macro(plasmazones_add_phosphor_shell_libs)
    if(NOT PHOSPHOR_LIBS_CMAKE_DIR OR NOT TARGET PhosphorIpc)
        message(FATAL_ERROR "plasmazones_add_phosphor_shell_libs() runs after plasmazones_add_phosphor_libs()")
    endif()
    if(PHOSPHOR_DEPS_ARCHIVES)
        plasmazones_fetch_dependency(phosphor-shell-libs)
        set(phosphor-shell-libs_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/phosphor-shell-libs-build")
        add_subdirectory("${phosphor-shell-libs_SOURCE_DIR}" "${phosphor-shell-libs_BINARY_DIR}")
    elseif(PHOSPHOR_SHELL_LIBS_GIT_REF)
        CPMAddPackage(NAME phosphor-shell-libs
                      GIT_REPOSITORY "https://github.com/phosphor-works/phosphor-shell-libs.git"
                      GIT_TAG "${PHOSPHOR_SHELL_LIBS_GIT_REF}"
                      GIT_SHALLOW NO)
    else()
        CPMAddPackage(NAME phosphor-shell-libs
                      URL "${PLASMAZONES_DEP_phosphor-shell-libs_URL}"
                      URL_HASH SHA256=${PLASMAZONES_DEP_phosphor-shell-libs_SHA256})
    endif()
endmacro()

# phosphor_put_glslang_on_test_path(<test>...)
#
# Puts a directory holding glslangValidator at the front of PATH for each named
# test, for the shader tests that shell out to it (plasmazones-shader-validate
# looks it up on PATH). The directory is PHOSPHOR_GLSLANG_DIR when that is set.
# Otherwise, on an x86-64 Linux host and outside an offline build (which must
# not download), it is Khronos's release build pinned above, fetched with CPM.
# Anywhere else PATH is left alone: the system's glslang package then has to be
# installed, and the tests that need it fail and say so when it is not.
function(phosphor_put_glslang_on_test_path)
    set(PHOSPHOR_GLSLANG_DIR "" CACHE PATH
        "Directory with glslangValidator for the shader tests. Empty: the pinned release, or PATH")
    get_property(_dir GLOBAL PROPERTY _phosphor_glslang_dir)
    get_property(_resolved GLOBAL PROPERTY _phosphor_glslang_resolved)
    if(NOT _resolved)
        set(_dir "${PHOSPHOR_GLSLANG_DIR}")
        if(NOT _dir AND NOT PHOSPHOR_DEPS_ARCHIVES AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
           AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
            CPMAddPackage(NAME glslang-tools VERSION ${PHOSPHOR_GLSLANG_VERSION}
                          URL "${PHOSPHOR_GLSLANG_URL}"
                          URL_HASH SHA256=${PHOSPHOR_GLSLANG_SHA256}
                          DOWNLOAD_ONLY YES)
            set(_dir "${glslang-tools_SOURCE_DIR}/bin")
        endif()
        set_property(GLOBAL PROPERTY _phosphor_glslang_dir "${_dir}")
        set_property(GLOBAL PROPERTY _phosphor_glslang_resolved TRUE)
    endif()
    if(NOT _dir)
        return()
    endif()
    foreach(_test IN LISTS ARGN)
        set_property(TEST ${_test} APPEND PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_dir}")
    endforeach()
endfunction()
