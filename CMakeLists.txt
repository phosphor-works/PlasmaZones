# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PlasmaZones - Window snapping, tiling and scrolling for KDE Plasma

# 3.23: the phosphor-libs tree this builds (cmake/PlasmaZonesDependencies.cmake)
# requires it, for phosphor-identity's target_sources(FILE_SET).
cmake_minimum_required(VERSION 3.23)

project(PlasmaZones VERSION 3.5.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ═══════════════════════════════════════════════════════════════════════════════
# Build Performance Optimizations
# ═══════════════════════════════════════════════════════════════════════════════

# Unity builds - combine multiple source files into larger translation units
# This significantly reduces compilation time by reducing header parsing overhead
#
# As the top-level project the switch is the CMAKE_UNITY_BUILD cache entry
# (-DCMAKE_UNITY_BUILD=OFF for a non-unity build). Inside a superbuild a cache
# entry would reach every project in the tree, including third-party code that
# does not build as unity, so there the switch is PLASMAZONES_UNITY_BUILD and the
# setting is a directory variable: this tree and the libraries it adds only.
if(PROJECT_IS_TOP_LEVEL)
    option(CMAKE_UNITY_BUILD "Enable unity builds for faster compilation" ON)
    set(CMAKE_UNITY_BUILD_BATCH_SIZE 16 CACHE STRING "Number of sources per unity batch")
else()
    option(PLASMAZONES_UNITY_BUILD "Build PlasmaZones and the libraries it adds as unity builds" ON)
    set(CMAKE_UNITY_BUILD ${PLASMAZONES_UNITY_BUILD})
    set(CMAKE_UNITY_BUILD_BATCH_SIZE 16)
endif()

# Precompiled headers - reuse expensive header compilation
option(ENABLE_PCH "Enable precompiled headers" ON)

# ccache support - cache compilation results for faster rebuilds
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    message(STATUS "Found ccache: ${CCACHE_PROGRAM} - enabling compiler cache")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# Parallel AUTOMOC for faster meta-object compilation
set(CMAKE_AUTOMOC_PARALLEL_ENABLED ON)

# ═══════════════════════════════════════════════════════════════════════════════
# Release Build Optimizations (LTO and aggressive optimization)
# ═══════════════════════════════════════════════════════════════════════════════

# Link-Time Optimization (LTO) for Release builds
# Benefits: 5-15% runtime performance improvement, smaller binary size
# Trade-off: Significantly increased link times (2-5x longer) and memory usage
# Recommendation: Keep enabled for release builds, disable for development
# To disable: cmake -DENABLE_LTO=OFF ..
option(ENABLE_LTO "Enable Link-Time Optimization for Release builds" ON)
if(ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_OUTPUT)
    if(IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        message(STATUS "Link-Time Optimization (LTO) enabled for Release builds")
        message(STATUS "  Note: LTO increases link times significantly. Use -DENABLE_LTO=OFF for faster dev builds.")
    else()
        message(WARNING "LTO not supported by compiler: ${IPO_OUTPUT}")
    endif()
endif()

# QML compilation to C++ (qmlcachegen) for faster QML execution
# Qt6 enables this by default when using qt_add_qml_module
set(QT_QML_GENERATE_QMLLS_INI ON CACHE BOOL "Generate QML language server config")

# AUTOMOC's `<target>_autogen/include` path is cleaned by `--clean-first`
# and then recreated mid-build by the autogen step, so `-Wmissing-include-dirs`
# intermittently fires against these directories. Instead of racing the
# clean/autogen ordering from here at configure time (which `--clean-first`
# would wipe again), the suppression is applied per target rather than
# globally here. plasmazones/src/settings/CMakeLists.txt carries the
# rationale. Dozens of CMakeLists across the tiers apply it, so do not read
# this as "one file opts out". For the current set, grep for the full
# `set_property(DIRECTORY APPEND PROPERTY COMPILE_OPTIONS "-Wno-missing-include-dirs")`
# line rather than the bare flag, which also matches this comment; and
# rather than a count written here, which drifts whenever a directory that
# builds a QML module is added.

# KDE/Qt requirements (Plasma 6.7 target: KF 6.26, Qt 6.10, KWin 6.7)
# Plasma 6.8 is supported by the same source: the KWin effect adapts to either
# version's paint API at compile time (plasmazones/kwin-effect/kwincompat.h), so these floors
# stay at the oldest supported Plasma rather than the newest.
set(QT_MIN_VERSION "6.10.0")
set(KF_MIN_VERSION "6.26.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
# Prepend our own cmake/ so project modules (PhosphorTestIsolation) resolve
# alongside ECM's.
set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake ${ECM_MODULE_PATH})

# The pinned phosphor-libs commit, the branding release, the test-time glslang
# and the CPM.cmake that downloads them. Included here, at the top scope: its
# pins and CPM's state are ordinary variables, and a module first included in
# one tier's directory would leave them out of every sibling. Pass
# -DPHOSPHOR_DEPS_ARCHIVES=<dir> for an offline build, or
# -DCPM_phosphor-libs_SOURCE=<dir> to build a local phosphor-libs checkout. The
# module's header explains every path.
include(PlasmaZonesDependencies)

# Test/tool toggles MUST be set before KDECMakeSettings, which calls
# include(CTest) and locks BUILD_TESTING into the cache as ON. Declaring
# them after KDECMakeSettings runs makes the option() call a no-op.
option(BUILD_TESTING "Build the test suite" OFF)
option(BUILD_TOOLS "Build developer tools (shader-render, etc.)" OFF)

include(KDEInstallDirs)
set(KDE_SKIP_UNINSTALL_TARGET ON)  # We provide our own uninstall target
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(FeatureSummary)

# Find Qt6
find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS
    Concurrent
    Core
    Qml
    Quick
    QuickControls2
    Gui
    DBus
    Network
    Widgets
    ShaderTools
    Svg
)
# Silence Qt 6.6+ deprecation warnings on qt_add_qml_module: opt into
# the new default for resource-prefix derivation (QTP0001) and the new
# default for QML module dependency resolution (QTP0004). These are
# the NEW behaviours we already rely on across phosphor-* and src/
# qt_add_qml_module calls.
# NOTE: CMake's if(POLICY) only understands CMP* CMake policies — for QTP*
# Qt policies it is ALWAYS false. Qt's documented existence test is the
# QT_KNOWN_POLICY_<policy> variable (set by the Qt CMake machinery from 6.5).
if(QT_KNOWN_POLICY_QTP0001)
    qt_policy(SET QTP0001 NEW)
endif()
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()
# GuiPrivate (QRhi) and ShaderToolsPrivate (QShaderBaker) required for daemon zone overlay.
# Some distros (Fedora) create these targets inside Qt6GuiConfig.cmake rather than providing
# standalone config files, so only call find_package if the target doesn't already exist.
if(NOT TARGET Qt6::GuiPrivate)
    find_package(Qt6GuiPrivate CONFIG REQUIRED)
endif()
if(NOT TARGET Qt6::ShaderToolsPrivate)
    find_package(Qt6ShaderToolsPrivate CONFIG REQUIRED)
endif()
# QuickPrivate is required by phosphor-animation's QML module - must be
# discovered at top-level so the IMPORTED target is global and visible to
# every consumer of `PhosphorAnimation::PhosphorAnimation`. Without this,
# Qt's static-QML-plugin dependency walker emits "QuickPrivate target is
# mentioned as a dependency ... but not declared" when collecting
# transitive deps for the executables that link static QML plugins.
if(NOT TARGET Qt6::QuickPrivate)
    find_package(Qt6QuickPrivate CONFIG REQUIRED)
endif()


# Make the Qt6 string-literal rule real. Without this, `const char*` converts
# implicitly to QString, so the CLAUDE.md rule ("QLatin1String for JSON keys
# and comparisons, QStringLiteral for constants, never a raw literal") was
# enforced only by reviewer attention and a raw literal compiled happily.
# With it, the conversion is gone and a violation is a compile error naming
# the exact line, which no pattern-matching linter can do as precisely.
#
# Verified clean before enabling: every TU in the compile database (with
# BUILD_TOOLS=ON) was recompiled with the define
# and none reported a conversion error, so this fixed no existing code and can
# only catch new violations.
add_compile_definitions(QT_NO_CAST_FROM_ASCII)

# KDE Frameworks integration.
# ON (default): full KDE integration - KWin effect, KCM, KGlobalAccel shortcuts,
#               and dynamic per-monitor workspaces (KConfig reads kwinrc and
#               makes the consented PerOutputVirtualDesktops write).
# OFF: portable build without the KWin effect, the KCM, KGlobalAccel or
#      dynamic workspaces, which compile out under the USE_KDE_FRAMEWORKS
#      define below. It is not free of KDE dependencies: ECM is required
#      in both modes (find_package(ECM) above), the settings app is built in
#      both and requires KF6 Kirigami (plasmazones/src/settings/CMakeLists.txt),
#      and the editor's and settings app's QML import org.kde.kirigami at run
#      time. Shortcuts use XDG Portal or D-Bus trigger fallback.
# Activity support is not part of this switch: PhosphorWorkspaces looks for
# PlasmaActivities itself (libs/phosphor-workspaces/CMakeLists.txt in
# phosphor-libs) and links it in either mode when it is installed.
option(USE_KDE_FRAMEWORKS "Build with KDE Frameworks integration" ON)

if(USE_KDE_FRAMEWORKS)
    add_compile_definitions(USE_KDE_FRAMEWORKS)

    find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS
        KCMUtils
        GlobalAccel   # KGlobalAccel shortcut backend + KCM
        Config        # KConfig for the kwinrc read and the consented kwinrc write (QSettings would rewrite the whole file)
    )
else()
    message(STATUS "Building WITHOUT KDE Frameworks (portable Qt-only mode)")
endif()

# Wayland layer-shell support (direct zwlr_layer_shell_v1 protocol, no KDE
# dependency). PlasmaZones is Wayland-only, Plasma 6 dropped X11 as a
# session target, and the layer-shell overlays we use have no X equivalent.
find_package(Wayland 1.21 REQUIRED COMPONENTS Client)
find_package(Qt6 6.10 REQUIRED COMPONENTS WaylandClient)
if(NOT TARGET Qt6::WaylandClientPrivate)
    find_package(Qt6WaylandClientPrivate REQUIRED)
endif()
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# Systemd user unit directory (fallback if not provided by ECM). Defer to
# GNUInstallDirs' libdir so multilib distros land on the right path
# (e.g. lib64/systemd/user on Fedora, lib/x86_64-linux-gnu/systemd/user
# on Debian multiarch).
if(NOT DEFINED KDE_INSTALL_SYSTEMDUSERUNITDIR)
    include(GNUInstallDirs)
    set(KDE_INSTALL_SYSTEMDUSERUNITDIR "${CMAKE_INSTALL_LIBDIR}/systemd/user")
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# phosphor-libs
# ═══════════════════════════════════════════════════════════════════════════════
# The Phosphor libraries live in their own repository, phosphor-works/
# phosphor-libs, and are built here from the commit
# cmake/PlasmaZonesDependencies.cmake pins, added with CPM as a subproject. Every
# setting above that a library must share (ECM, the KDE settings, Qt and its
# private modules, QT_NO_CAST_FROM_ASCII) is in force when it is added, so it is
# added here, before anything reads its targets. The translation module below
# reads its phosphor-control sources, so this also stays above that include.
#
# Its library tests that check a library constant against the packs PlasmaZones
# ships read them from here. FORCE, so the tree's own empty default, which it
# declares as a cache entry, never wins.
set(PHOSPHOR_LIBS_BUNDLED_PACKS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plasmazones/data" CACHE PATH
    "Directory holding bundled animations/ and pointer/ packs, for the phosphor-libs tests that check them" FORCE)
# The surface decoration packs, from their own repository at the commit
# cmake/PlasmaZonesDependencies.cmake pins. Fetched BEFORE the libraries:
# phosphor-libs fetches its own pinned copy for its tests unless an app has
# already set PHOSPHOR_SURFACE_PACKS_DIR, and CPM keeps the first copy
# declared, so fetching them after the libraries let phosphor-libs' pin decide
# the packs this build compiled, validated and installed (#1282).
plasmazones_fetch_dependency(phosphor-surface-packs)
include(${phosphor-surface-packs_SOURCE_DIR}/cmake/PhosphorSurfacePacks.cmake)
plasmazones_add_phosphor_libs()
# The placement engines, their models and the engine runtime, on the phosphor-libs
# tree just added (cmake/PlasmaZonesDependencies.cmake pins the commit).
plasmazones_add_phosphor_engine_libs()
# Where PlasmaZones' files live: its own folder inside the suite folder every
# Phosphor app shares, in each XDG root. phosphor-libs names the suite folder
# (PHOSPHOR_SUITE_DIRECTORY_NAME); PlasmaZones' own name is set here once and
# reaches the code through the generated version.h (AppDirectoryName).
include("${PHOSPHOR_LIBS_CMAKE_DIR}/PhosphorSuite.cmake")
set(PLASMAZONES_APP_DIRECTORY_NAME "plasmazones")
# Phosphor.Components, on the phosphor-libs tree just added.
plasmazones_add_phosphor_components()

# ---------------------------------------------------------------------------
# Code formatting (clang-format for C++, qmlformat for QML)
#   make clang-format  (or ninja clang-format)
#   make format-qml   (or ninja format-qml)
# ---------------------------------------------------------------------------
include(KDEClangFormat)
# `**` is not a CMake glob primitive; it expands to the same regex as
# `*` (anything except `/`). GLOB_RECURSE with a plain `*.cpp` pattern
# already recurses into subdirectories, so the recursive form is what
# we want; using `**/*.cpp` would require an intermediate path segment
# and silently drop files directly under each root (e.g. plasmazones/src/phosphor_qml_i18n.cpp,
# any top-level kwin-effect/.cpp).
file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES
    ${PROJECT_SOURCE_DIR}/plasmazones/src/*.cpp
    ${PROJECT_SOURCE_DIR}/plasmazones/src/*.h
    ${PROJECT_SOURCE_DIR}/plasmazones/src/*.hpp
    ${PROJECT_SOURCE_DIR}/plasmazones/kcm/*.cpp
    ${PROJECT_SOURCE_DIR}/plasmazones/kcm/*.h
    ${PROJECT_SOURCE_DIR}/plasmazones/kcm/*.hpp
    ${PROJECT_SOURCE_DIR}/plasmazones/kwin-effect/*.cpp
    ${PROJECT_SOURCE_DIR}/plasmazones/kwin-effect/*.h
    ${PROJECT_SOURCE_DIR}/plasmazones/kwin-effect/*.hpp
    ${PROJECT_SOURCE_DIR}/plasmazones/tests/*.cpp
    ${PROJECT_SOURCE_DIR}/plasmazones/tests/*.h
    # Built only with -DBUILD_TOOLS=ON, but this glob is filesystem-based
    # rather than target-based, so the entry is unconditional. Without it
    # the clang-format target skipped shader-render and
    # settings-schema-dump entirely; only lefthook reached them.
    ${PROJECT_SOURCE_DIR}/plasmazones/tools/*.cpp
    ${PROJECT_SOURCE_DIR}/plasmazones/tools/*.h
    ${PROJECT_SOURCE_DIR}/plasmazones/tools/*.hpp
)
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})

# QML: qmlformat (from qt6-tools, optional)
# Same `**`-vs-recursion rationale as the clang-format glob above.
# Repository tooling: only for a build of this repository on its own. A tree
# that adds PlasmaZones as a subproject (the workspace superbuild) has its own,
# and two targets of one name stop its configure.
find_program(QMLFORMAT qmlformat)
file(GLOB_RECURSE ALL_QML_FILES
    ${PROJECT_SOURCE_DIR}/plasmazones/src/*.qml
    ${PROJECT_SOURCE_DIR}/plasmazones/kcm/*.qml
)
if(PROJECT_IS_TOP_LEVEL AND QMLFORMAT AND ALL_QML_FILES)
    # Run qmlformat via helper script; per-file failures are ignored (one bad QML won't fail target)
    add_custom_target(format-qml
        COMMAND ${CMAKE_COMMAND}
            -DQMLFORMAT=${QMLFORMAT}
            -DFILES="${ALL_QML_FILES}"
            -P ${PROJECT_SOURCE_DIR}/cmake/format-qml.cmake
        COMMENT "Formatting QML files with qmlformat"
    )
elseif(PROJECT_IS_TOP_LEVEL)
    add_custom_target(format-qml
        COMMAND ${CMAKE_COMMAND} -E echo "qmlformat not found or no QML files; install qt6-tools for QML formatting"
    )
endif()

# ---------------------------------------------------------------------------
# Translations (Qt Linguist). Extracted to cmake/PhosphorTranslations.cmake
# and included in this scope. Other self-contained blocks can be extracted the
# same way. See that file for the update-ts / lrelease detail.
# ---------------------------------------------------------------------------
# Must stay ABOVE add_subdirectory(plasmazones): that tier's CMakeLists reads
# the plasmazones_translations and update-ts targets and the
# Qt6LinguistTools_FOUND and TRANSLATION_TS_FILES variables this module leaves
# behind, to attach the .qm catalogs to plasmazones-tier. Moved below it, none
# of them exists there yet, and plasmazones/CMakeLists.txt stops the configure
# on the missing `update-ts`, which this module declares in every branch.
include(cmake/PhosphorTranslations.cmake)

# Generate version header with compile-time version info
configure_file(
    "${PROJECT_SOURCE_DIR}/plasmazones/src/core/version.h.in"
    "${PROJECT_BINARY_DIR}/generated/version.h"
    @ONLY
)
include_directories("${PROJECT_BINARY_DIR}/generated")

# ═══════════════════════════════════════════════════════════════════════════════
# Tiers
# ═══════════════════════════════════════════════════════════════════════════════
# The tree is one product tier, plasmazones/: the PlasmaZones daemon, editor,
# settings, KCM, KWin effect, bundled data and tests. It builds on
# phosphor-libs and phosphor-engine-libs, added above. The engine QML firewall
# runs inside phosphor-engine-libs, next to the engines it guards.

add_subdirectory(plasmazones)

# One precompiled header shared by every test target in this tree and the
# library trees it adds, attached after all of them are declared because it
# walks this directory for them (cmake/PhosphorTestPch.cmake). Also inside a
# superbuild, where each app makes its own call so the PCH has its flags.
if(BUILD_TESTING AND ENABLE_PCH)
    include(PhosphorTestPch)
    phosphor_share_test_pch()
endif()

# Custom uninstall target that cleans up directories properly. Top level only,
# like the other repository tooling above: it reads this build's install manifest.
if(PROJECT_IS_TOP_LEVEL)
    # QT6_INSTALL_PLUGINS repeats phosphor-wayland's QPA install-path derivation
    # (a subdirectory-scope variable there, so re-derived here for the configure
    # below). The fallback there spells the libdir relative and here absolute,
    # which resolves to the same place: install() roots a relative DESTINATION at
    # the prefix, and the absolute branch below prepends the prefix itself.
    if(NOT DEFINED QT6_INSTALL_PLUGINS)
        get_target_property(QT6_INSTALL_PLUGINS Qt6::Core QT_INSTALL_PLUGINS)
        if(NOT QT6_INSTALL_PLUGINS)
            set(QT6_INSTALL_PLUGINS "${KDE_INSTALL_FULL_LIBDIR}/qt6/plugins")
        endif()
    endif()
    # The Qt property can be prefix-relative; the uninstall script needs an
    # absolute path (matching how install() resolves relative DESTINATIONs).
    if(IS_ABSOLUTE "${QT6_INSTALL_PLUGINS}")
        set(PZ_UNINSTALL_QT_PLUGINS "${QT6_INSTALL_PLUGINS}")
    else()
        set(PZ_UNINSTALL_QT_PLUGINS "${CMAKE_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}")
    endif()
    configure_file(
        "${PROJECT_SOURCE_DIR}/cmake/uninstall.cmake.in"
        "${PROJECT_BINARY_DIR}/cmake_uninstall.cmake"
        @ONLY
    )

    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${PROJECT_BINARY_DIR}/cmake_uninstall.cmake
        COMMENT "Uninstalling PlasmaZones..."
    )
endif()

# Build/install only installs files. Enabling the daemon, refreshing sycoca/KWin,
# and user-facing messages are handled by packaging (Arch .install, Debian postinst, RPM %post).

# Qt's WrapVulkanHeaders backs the QVulkanInstance Vulkan-backend support, which
# is guarded throughout with #if QT_CONFIG(vulkan); it stays OPTIONAL here so that
# backend degrades gracefully when absent.
# NOTE: this does NOT make the whole build Vulkan-optional. The dma-buf thumbnail
# daemon path hard-requires the Vulkan SDK via find_package(Vulkan REQUIRED) in
# plasmazones/src/CMakeLists.txt (dmabuftextureprovider.cpp includes <vulkan/vulkan.h>
# unconditionally), so a daemon build without the Vulkan SDK fails at configure time.
# Packaging specs (PKGBUILD, debian/control, rpm .spec, nix) already list Vulkan as a
# hard BuildRequires, so distribution packages are unaffected.
set_package_properties(WrapVulkanHeaders PROPERTIES TYPE OPTIONAL)

# Feature summary (REQUIRED + OPTIONAL only; RUNTIME hidden to reduce noise)
feature_summary(WHAT REQUIRED_PACKAGES_FOUND REQUIRED_PACKAGES_NOT_FOUND
                     OPTIONAL_PACKAGES_FOUND OPTIONAL_PACKAGES_NOT_FOUND
                FATAL_ON_MISSING_REQUIRED_PACKAGES)

include(cmake/PhosphorCPack.cmake)
