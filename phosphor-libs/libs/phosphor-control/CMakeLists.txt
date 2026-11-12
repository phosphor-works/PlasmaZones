# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# PhosphorControl — reusable Qt6/QML/Kirigami settings-app framework
#
# Provides the chrome (window, sidebar, breadcrumbs, footer, page host)
# and orchestration (PageController base, PageRegistry, StagingDomain,
# ApplicationController) that any Qt6/Kirigami settings app can build on,
# so each consumer only writes its own page controllers + QML pages. It
# also hosts Facet modules: plugins that add their pages lazily
# (FacetHost, loaded through phosphor-registry's PluginCatalog), with
# PhosphorControlTest (FacetTestHost) for a module's own CI.
#
# Boundary:
#   - phosphor-config owns the IBackend/IGroup configuration store. This
#     library does NOT duplicate that contract. It links phosphor-config
#     for one thing: FacetHost keeps the IBackend of each config file Facet
#     modules share and hands it out (FacetContext::configBackend).
#     Consumers wire their own ISettings implementation against
#     phosphor-config.
#   - phosphor-shortcuts owns global-shortcut backends. Not included here.
#
# Built standalone or as a subdirectory of Phosphor (or any consumer).

# 3.21 floor: this file uses ${PROJECT_IS_TOP_LEVEL} (>= 3.21) for the
# examples gate. A lower floor would fail mid-configure with an
# empty-variable error instead of a clear minimum-version message.
cmake_minimum_required(VERSION 3.21)

set(PHOSPHORCONTROL_VERSION "0.1.0")

# The Qt floor this library requires AND advertises in its generated
# PhosphorControlConfig.cmake. Taken from the top-level QT_MIN_VERSION when built in-tree so
# the two cannot drift, with its own value for a STANDALONE build, where that
# variable does not exist. The fallback is not optional: the Config template
# substitutes this, and an undefined variable expands to nothing and ships a config
# whose find_dependency(Qt6 ...) carries no version floor at all.
if(DEFINED QT_MIN_VERSION)
    set(PHOSPHORCONTROL_QT_MIN ${QT_MIN_VERSION})
else()
    set(PHOSPHORCONTROL_QT_MIN "6.10.0")
endif()

if(NOT PROJECT_VERSION)
    project(PhosphorControl VERSION ${PHOSPHORCONTROL_VERSION} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_AUTOMOC ON)
endif()

include(GenerateExportHeader)

# Hoist GNUInstallDirs + the KDE_INSTALL_* fallbacks here so every
# install(...) / target_include_directories(... INSTALL_INTERFACE) block
# below has the variables defined unconditionally. Previously this was
# included lazily in three places (around the lib target, around the
# QML install block, and at the start of the install section), each
# guarded by a separate `if(NOT DEFINED ...)` — easy to reorder a way
# that left INSTALL_INTERFACE with an empty path until the third
# include fired.
include(GNUInstallDirs)
if(NOT DEFINED KDE_INSTALL_INCLUDEDIR)
    set(KDE_INSTALL_INCLUDEDIR ${CMAKE_INSTALL_INCLUDEDIR})
endif()
if(NOT DEFINED KDE_INSTALL_LIBDIR)
    set(KDE_INSTALL_LIBDIR ${CMAKE_INSTALL_LIBDIR})
endif()
if(NOT DEFINED KDE_INSTALL_BINDIR)
    set(KDE_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR})
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Dependencies
# ═══════════════════════════════════════════════════════════════════════════════

find_package(Qt6 ${PHOSPHORCONTROL_QT_MIN} REQUIRED COMPONENTS Core DBus Gui Qml Quick)

# phosphor-animation -- PhosphorControlQml links the QML plugin so the
# chrome's org.phosphor.animation imports resolve for consumers. In the
# superbuild the target already exists. A standalone configure additionally
# requires phosphor-animation vendored via add_subdirectory: the installed
# PhosphorAnimation package exports only the C++ target (the STATIC
# PhosphorAnimationQml target is never installed/exported), so find_package
# alone cannot provide it.
if(NOT TARGET PhosphorAnimation::PhosphorAnimationQml)
    find_package(PhosphorAnimation CONFIG REQUIRED)
    # find_package succeeding is not enough — surface the missing QML
    # target here with a clear message instead of an opaque "Target not
    # found" later in the configure.
    if(NOT TARGET PhosphorAnimation::PhosphorAnimationQml)
        message(FATAL_ERROR "phosphor-control: the installed PhosphorAnimation package exports only the C++ target; vendor phosphor-animation via add_subdirectory for a standalone build")
    endif()
endif()

# The shared plugin catalog Facet modules load through. In-tree the target
# already exists; a standalone build finds the installed package.
if(NOT TARGET PhosphorRegistry::PhosphorRegistry)
    find_package(PhosphorRegistry CONFIG REQUIRED)
endif()

# The IBackend that FacetContext::configBackend hands Facet modules. In-tree
# the target already exists; a standalone build finds the installed package.
if(NOT TARGET PhosphorConfig::PhosphorConfig)
    find_package(PhosphorConfig CONFIG REQUIRED)
endif()

# Shared-vs-static lib type. SHARED is the LGPL-friendly default: an
# end-user can relink against a modified copy of this library without
# also having to relink the consuming application, satisfying the
# LGPL-2.1's dynamic-linking exemption. Override to OFF only for
# embedded / single-binary builds where the LGPL relinking obligation
# is handled out-of-band (e.g. shipping the .o files alongside the
# binary). A STATIC build cannot host Facet modules: each module would
# link its own copy of the library.
option(PHOSPHOR_CONTROL_BUILD_SHARED
    "Build phosphor-control as a shared library (LGPL relinking-friendly)"
    ON)
if(PHOSPHOR_CONTROL_BUILD_SHARED)
    set(_phosphor_control_lib_type SHARED)
else()
    set(_phosphor_control_lib_type STATIC)
endif()

# Qt 6.5+ QML policies:
#   QTP0001 — use `:/qt/qml/` as the QML module resource prefix (NEW).
#             Matches the RESOURCE_PREFIX we set on qt_add_qml_module below.
#   QTP0004 — auto-generate qmldir files for QML subdirectories (`qml/`)
#             so module type resolution finds them. Without NEW, Qt warns
#             "You need qmldir files for each extra directory".
if(COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
    # QTP0004 exists only from Qt 6.8 — qt_policy(SET) on an unknown policy
    # fatal-errors, and the COMMAND guard alone doesn't prove the policy
    # exists (qt_policy ships from 6.5). QT_KNOWN_POLICY_*, not
    # if(POLICY ...), which only knows CMP* CMake policies.
    if(QT_KNOWN_POLICY_QTP0004)
        qt_policy(SET QTP0004 NEW)
    endif()
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Library
# ═══════════════════════════════════════════════════════════════════════════════

set(phosphorcontrol_public_HDRS
    include/PhosphorControl/PhosphorControl.h
    include/PhosphorControl/StagingDomain.h
    include/PhosphorControl/PageController.h
    include/PhosphorControl/PageRegistry.h
    include/PhosphorControl/ApplicationController.h
    include/PhosphorControl/LocalizedContext.h
    include/PhosphorControl/DBusBridge.h
    include/PhosphorControl/SearchEntry.h
    include/PhosphorControl/SearchRanker.h
    include/PhosphorControl/ISearchProvider.h
    include/PhosphorControl/SearchController.h
    include/PhosphorControl/SidebarRows.h
    include/PhosphorControl/FacetModule.h
    include/PhosphorControl/FacetModuleMetadata.h
    include/PhosphorControl/FacetContext.h
    include/PhosphorControl/FacetHost.h
    include/PhosphorControl/FacetSlotModel.h
)

set(phosphorcontrol_SRCS
    src/phosphorcontrol.cpp
    src/stagingdomain.cpp
    src/pagecontroller.cpp
    src/pageregistry.cpp
    src/applicationcontroller.cpp
    # Async state-machine half of ApplicationController. Same class,
    # separate TU, no API change.
    src/applicationcontroller_async.cpp
    src/localizedcontext.cpp
    src/dbusbridge.cpp
    src/searchranker.cpp
    src/searchcontroller.cpp
    src/sidebarrows.cpp
    src/facetmodulemetadata.cpp
    src/facetcontext.cpp
    src/facethost.cpp
    src/facetslotmodel.cpp
)

# NOTE: three near-identical targets (PhosphorControl C++ lib +
# PhosphorControlQml STATIC QML module + optional
# PhosphorControlQmlShared install-only plugin) compile the same
# source files three times. This mirrors phosphor-animation and is
# intentional defence-in-depth — unifying them risks Qt's
# qmltyperegistrar emitting clashing type registrations into multiple
# targets that the linker would then resolve in a generator-dependent
# order. WATCH FOR: a "Type already registered" warning at QML import
# time if anyone ever adds a `qmlRegisterUncreatableType()` (or any
# side-effecting registration) to one of the shared sources — that
# would surface the triple-registration footgun and need plumbing
# (split source between targets, or a single-target install path).

# Suppress -Wmissing-include-dirs for Qt-managed AUTOMOC scratch dirs.
# KDECompilerSettings (in the Phosphor parent build) enables the
# warning globally, and qt_add_qml_module adds `<target>_autogen/include`
# to every generated sub-target's -I list (qmltyperegistration, resource
# init, qmlplugin). Those sub-targets carry no Q_OBJECT, so AUTOMOC
# produces nothing and the dir ends up empty or — after `--clean-first`
# — missing entirely, so cc1plus fires before AUTOMOC recreates it.
# Set at DIRECTORY level so every implicitly-generated sub-target picks
# it up without enumerating the ever-changing list by name; placed before
# the first target so it also propagates to the examples/ and tests/
# subdirectories added below. Mirrors phosphor-libs/libs/phosphor-animation and
# plasmazones/src/shared — see those for the full rationale.
set_property(DIRECTORY APPEND PROPERTY COMPILE_OPTIONS "-Wno-missing-include-dirs")

add_library(PhosphorControl ${_phosphor_control_lib_type}
    ${phosphorcontrol_public_HDRS}
    ${phosphorcontrol_SRCS}
)

add_library(PhosphorControl::PhosphorControl ALIAS PhosphorControl)

generate_export_header(PhosphorControl
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/phosphorcontrol_export.h
    EXPORT_MACRO_NAME PHOSPHORCONTROL_EXPORT
)

target_include_directories(PhosphorControl
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
)

target_compile_features(PhosphorControl PUBLIC cxx_std_20)

target_link_libraries(PhosphorControl
    PUBLIC
        Qt6::Core
        Qt6::DBus
        # Qt6::Qml is PUBLIC because the public headers include
        # `<qqmlregistration.h>` (QML_NAMED_ELEMENT etc.) — consumers
        # need the Qml include path at compile time even if they don't
        # use the registration macros themselves.
        Qt6::Qml
        # PUBLIC: FacetHost.h exposes PluginSeam, PluginSearchPath and
        # RefusedPlugin, the shared plugin catalog's types.
        PhosphorRegistry::PhosphorRegistry
        # PUBLIC: FacetContext::configBackend hands modules a
        # PhosphorConfig::IBackend.
        PhosphorConfig::PhosphorConfig
    PRIVATE
        # Qt6::Gui is PRIVATE here: moc-generated code on the
        # implementation side may reference QColor/QPalette/etc. through
        # Qt5 compat headers, but the public API in include/PhosphorControl/
        # exposes no Gui types (QObject + QString + QUrl + QList only).
        # Consumers link Qt6::Gui all the same: PhosphorConfig above
        # links it PUBLIC, and passes it on to everything that links this
        # library. The QML targets below also add Gui PUBLIC themselves.
        Qt6::Gui
)

set_target_properties(PhosphorControl PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    UNITY_BUILD OFF
)

# VERSION/SOVERSION apply to SHARED libraries only and are silently
# ignored on STATIC — set them only on the SHARED build so the property
# surface matches what the target actually carries (same principle as
# the PhosphorControlQml STATIC note below).
if(PHOSPHOR_CONTROL_BUILD_SHARED)
    set_target_properties(PhosphorControl PROPERTIES
        VERSION ${PHOSPHORCONTROL_VERSION}
        SOVERSION 0
    )
endif()

# Surface the CMake version to the C++ version() accessor so the two
# stay in sync (avoid drift between the project metadata + a hardcoded
# string literal in src/phosphorcontrol.cpp).
target_compile_definitions(PhosphorControl PRIVATE
    PHOSPHORCONTROL_VERSION_STR="${PHOSPHORCONTROL_VERSION}"
)

# ═══════════════════════════════════════════════════════════════════════════════
# PhosphorControlTest — FacetTestHost, for a Facet module's own CI
#
# Test harness code (it stages a copy of the module and waits on event
# loops), so it is its own library rather than part of the runtime one,
# and it is not compiled into the QML module targets below.
# ═══════════════════════════════════════════════════════════════════════════════

add_library(PhosphorControlTest ${_phosphor_control_lib_type}
    include/PhosphorControl/FacetTestHost.h
    src/facettesthost.cpp
)
add_library(PhosphorControl::PhosphorControlTest ALIAS PhosphorControlTest)

generate_export_header(PhosphorControlTest
    EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/phosphorcontroltest_export.h
    EXPORT_MACRO_NAME PHOSPHORCONTROLTEST_EXPORT
)

target_link_libraries(PhosphorControlTest
    PUBLIC
        PhosphorControl::PhosphorControl
        Qt6::Core
)

set_target_properties(PhosphorControlTest PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
if(PHOSPHOR_CONTROL_BUILD_SHARED)
    set_target_properties(PhosphorControlTest PROPERTIES
        VERSION ${PHOSPHORCONTROL_VERSION}
        SOVERSION 0
    )
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# QML Module — org.phosphor.control (STATIC, IN-TREE LINKABLE ONLY)
#
# The C++ library above links Qt6::Qml only for the registration macros
# its headers carry, and avoids Qt6::Quick, so non-QML consumers (e.g.
# headless tests) stay light. The QML module target
# below re-compiles the same public C++ sources so qmltyperegistrar
# sees the QML_NAMED_ELEMENT markers and emits the type registrations
# into the org.phosphor.control module — no separate foreigntypes
# .cpp is needed because every exposed type lives in this lib's own
# headers.
#
# IMPORTANT CONSUMPTION NOTE:
#
# This QML module is a STATIC archive. The `PhosphorControl::PhosphorControlQml`
# ALIAS works inside this source tree (when consumed via add_subdirectory).
# It is NOT installed and NOT added to the `PhosphorControlTargets`
# export set, so an out-of-tree consumer doing `find_package(PhosphorControl)`
# + `target_link_libraries(... PhosphorControl::PhosphorControlQml)`
# will fail at configure time with "Target ... not found".
#
# Out-of-tree consumers must either:
#   * Add this directory as a git submodule and consume via add_subdirectory,
#     OR
#   * Configure with -DPHOSPHOR_CONTROL_QML_INSTALL=ON, which builds the
#     parallel SHARED `PhosphorControlQmlShared` target and installs its
#     plugin .so + qmldir + qmltypes under
#     `${KDE_INSTALL_QMLDIR}/org/phosphor/control/` (the same pattern as
#     phosphor-animation's PHOSPHOR_ANIMATION_QML_INSTALL).
#
# The SHARED variant (PhosphorControlQmlShared, further down) is opt-in
# through PHOSPHOR_CONTROL_QML_INSTALL, off by default because every
# in-tree consumer links the STATIC module. Turn it on for an out-of-tree
# consumer that imports the module from the installed QML path.
# ═══════════════════════════════════════════════════════════════════════════════

# The QML module re-compiles the same public headers + sources as the C++
# lib so qmltyperegistrar sees the QML_NAMED_ELEMENT markers and emits
# registrations into org.phosphor.control. This mirrors phosphor-animation.

set(phosphorcontrol_qml_FILES
    qml/SettingsAppWindow.qml
    qml/Sidebar.qml
    qml/SidebarRow.qml
    qml/SidebarBackButton.qml
    qml/Breadcrumbs.qml
    qml/UnsavedChangesFooter.qml
    qml/PageHost.qml
    qml/PageLoadingIndicator.qml
    qml/DiscardChangesDialog.qml
    qml/AboutPageShell.qml
    qml/FacetSlot.qml
    qml/FacetExtensionFrame.qml
    qml/FacetModulePage.qml
)

# JS helpers shared by the QML chrome, which imports them by relative
# path. Listed apart from QML_FILES and shipped as plain resources of the
# module.
set(phosphorcontrol_qml_RESOURCES
    qml/LoaderHelpers.js
)

qt_add_qml_module(PhosphorControlQml
    URI org.phosphor.control
    VERSION 1.0
    STATIC
    RESOURCE_PREFIX /qt/qml
    SOURCES
        ${phosphorcontrol_public_HDRS}
        ${phosphorcontrol_SRCS}
    QML_FILES ${phosphorcontrol_qml_FILES}
    RESOURCES ${phosphorcontrol_qml_RESOURCES}
    OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml/org/phosphor/control
)

# The QML target compiles the same .cpp files as the C++ SHARED library;
# set the export define so PHOSPHORCONTROL_EXPORT resolves to
# visibility("default") on both builds. Mirrors phosphor-animation.
# Also surface PHOSPHORCONTROL_VERSION_STR so the shared
# phosphorcontrol.cpp's version() accessor compiles in this target too.
target_compile_definitions(PhosphorControlQml PRIVATE
    PhosphorControl_EXPORTS
    PHOSPHORCONTROL_VERSION_STR="${PHOSPHORCONTROL_VERSION}"
)

add_library(PhosphorControl::PhosphorControlQml ALIAS PhosphorControlQml)

target_include_directories(PhosphorControlQml
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
    PRIVATE
        # The generated <module>_qmltyperegistrations.cpp guards each registered
        # type with `#if __has_include(<Header.h>)` using the BARE header name,
        # so the header's own directory must be on the include path for a
        # non-unity build (which compiles that TU alone) to resolve it. PRIVATE —
        # consumers still use the <PhosphorControl/…> spelling above.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/PhosphorControl>
)

target_compile_features(PhosphorControlQml PUBLIC cxx_std_20)

target_link_libraries(PhosphorControlQml
    PUBLIC
        PhosphorControl::PhosphorControl
        # The chrome QML files import org.phosphor.animation for the
        # motion profiles they bind (panel.fadeIn/Out, panel.slideIn/Out,
        # widget.accordionExpand/Collapse, widget.fadeIn, widget.hover,
        # widget.tint, widget.tint.fast).
        # Linking the QML plugin here pulls phosphor-animation into
        # the consumer's QML import path automatically.
        PhosphorAnimation::PhosphorAnimationQml
        Qt6::Core
        Qt6::DBus
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
)

# Static archive: VERSION/SOVERSION apply to SHARED libraries only and are
# silently ignored on STATIC. Don't set them on PhosphorControlQml so the
# property surface matches what the target actually carries (mirrors
# phosphor-popout / phosphor-theme). The SHARED variant below keeps its own.

# ═══════════════════════════════════════════════════════════════════════════════
# QML Module — SHARED (install-only runtime plugin)
# ═══════════════════════════════════════════════════════════════════════════════
#
# The STATIC PhosphorControlQml target above is fine for in-tree
# consumers — they link it directly
# and Qt's plugin registration runs at link time. Out-of-tree
# consumers using `find_package(PhosphorControl)` get the C++
# target via the export set, but cannot consume STATIC QML modules:
# Qt only loads QML modules from the QML module search path
# (KDE_INSTALL_QMLDIR/org/phosphor/control/), which requires a
# SHARED plugin .so. Mirror phosphor-animation's pattern — build a
# parallel SHARED target gated behind PHOSPHOR_CONTROL_QML_INSTALL
# so packagers can opt in to shipping the QML module separately from
# in-tree consumers that don't need it.

option(PHOSPHOR_CONTROL_QML_INSTALL
    "Also build a SHARED variant of org.phosphor.control and install it"
    OFF)

if(PHOSPHOR_CONTROL_QML_INSTALL)
    qt_add_qml_module(PhosphorControlQmlShared
        URI org.phosphor.control
        VERSION 1.0
        SHARED
        SOURCES
            ${phosphorcontrol_public_HDRS}
            ${phosphorcontrol_SRCS}
        QML_FILES ${phosphorcontrol_qml_FILES}
        RESOURCES ${phosphorcontrol_qml_RESOURCES}
        OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml_install/org/phosphor/control
    )

    target_include_directories(PhosphorControlQmlShared
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
            $<INSTALL_INTERFACE:${KDE_INSTALL_INCLUDEDIR}>
        PRIVATE
            # Same bare-`__has_include(<Header.h>)` resolution as the
            # PhosphorControlQml target above.
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/PhosphorControl>
    )

    # The SHARED variant compiles the same sources under a different
    # target name; set the export define so PHOSPHORCONTROL_EXPORT
    # resolves to visibility("default") on the SHARED build too.
    target_compile_definitions(PhosphorControlQmlShared PRIVATE
        PhosphorControl_EXPORTS
        PHOSPHORCONTROL_VERSION_STR="${PHOSPHORCONTROL_VERSION}"
    )

    target_compile_features(PhosphorControlQmlShared PUBLIC cxx_std_20)

    target_link_libraries(PhosphorControlQmlShared
        PUBLIC
            PhosphorControl::PhosphorControl
            PhosphorAnimation::PhosphorAnimationQml
            Qt6::Core
            Qt6::DBus
            Qt6::Gui
            Qt6::Qml
            Qt6::Quick
    )

    set_target_properties(PhosphorControlQmlShared PROPERTIES
        VERSION ${PHOSPHORCONTROL_VERSION}
        SOVERSION 0
    )

    set(_phosphor_control_qml_shared_plugin_target PhosphorControlQmlSharedplugin)

    # The QML plugin and its backing PhosphorControlQmlShared library
    # install side-by-side in the QML module directory, which is not on
    # the linker's default search path. $ORIGIN on the plugin's
    # INSTALL_RPATH resolves the sibling library at runtime and during
    # dpkg-shlibdeps packaging scans. qt_add_qml_module creates the
    # plugin target before it returns on every Qt this library supports,
    # so it can be set here directly.
    set_target_properties(${_phosphor_control_qml_shared_plugin_target} PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Examples — opt-in via -DBUILD_PHOSPHOR_CONTROL_EXAMPLES=ON
# ═══════════════════════════════════════════════════════════════════════════════

# Default follows PROJECT_IS_TOP_LEVEL: ON when this lib is the outermost
# project (standalone build), and also ON in-tree (PlasmaZones is top-level,
# so the value propagates TRUE here) where examples/minimal is built and
# expected. CAVEAT: a third party that vendors this lib via add_subdirectory
# inside their OWN top-level project also gets the propagated TRUE and builds
# these examples unintentionally — they should pass -DBUILD_PHOSPHOR_CONTROL_EXAMPLES=OFF.
# (Requires CMake >= 3.21 for PROJECT_IS_TOP_LEVEL — see the minimum above.)
option(BUILD_PHOSPHOR_CONTROL_EXAMPLES
    "Build the phosphor-control example apps"
    ${PROJECT_IS_TOP_LEVEL})

if(BUILD_PHOSPHOR_CONTROL_EXAMPLES AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Install
# ═══════════════════════════════════════════════════════════════════════════════
#
# KDE_INSTALL_* variables are resolved at the top of this file via the
# hoisted include(GNUInstallDirs) block — no per-section fallback
# include needed here.

install(TARGETS PhosphorControl PhosphorControlTest
    EXPORT PhosphorControlTargets
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${KDE_INSTALL_BINDIR}
)

install(EXPORT PhosphorControlTargets
    FILE PhosphorControlTargets.cmake
    NAMESPACE PhosphorControl::
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorControl
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PhosphorControlConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorControlConfig.cmake"
    INSTALL_DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorControl
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorControlConfigVersion.cmake"
    VERSION ${PHOSPHORCONTROL_VERSION}
    # SameMinorVersion is the pre-1.0 compatibility contract: a
    # find_package(PhosphorControl 0.1) call accepts 0.1.x patch
    # releases only — bumping to 0.2 is treated as breaking because
    # we still expect API/ABI to churn freely under 1.0. Switch this
    # back to SameMajorVersion once we hit 1.0 (when the API is
    # stabilised and we promise within-major back-compat).
    COMPATIBILITY SameMinorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorControlConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PhosphorControlConfigVersion.cmake"
    DESTINATION ${KDE_INSTALL_LIBDIR}/cmake/PhosphorControl
)

install(DIRECTORY include/PhosphorControl/
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorControl
    FILES_MATCHING PATTERN "*.h"
)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/phosphorcontrol_export.h
        ${CMAKE_CURRENT_BINARY_DIR}/phosphorcontroltest_export.h
    DESTINATION ${KDE_INSTALL_INCLUDEDIR}/PhosphorControl
)

# QML module install (SHARED variant) — gated behind the option above.
if(PHOSPHOR_CONTROL_QML_INSTALL)
    if(NOT DEFINED KDE_INSTALL_QMLDIR)
        # Without ECM the QML dir falls back to a Qt-side install
        # location. Match the freedesktop convention so the consumer
        # can drop the lib under any standard prefix.
        set(KDE_INSTALL_QMLDIR ${KDE_INSTALL_LIBDIR}/qt6/qml)
    endif()
    set(_phosphor_control_qml_install_dir "${KDE_INSTALL_QMLDIR}/org/phosphor/control")

    # NOTE: no EXPORT clause here on purpose — QML modules are consumed
    # via the QML import path at runtime, not via `find_package` +
    # `target_link_libraries`. Earlier revisions registered these to
    # an export set that was never installed via `install(EXPORT ...)`,
    # which left a dangling reference for any consumer that tried to
    # use the QML targets through CMake.
    install(TARGETS PhosphorControlQmlShared
        RUNTIME DESTINATION ${_phosphor_control_qml_install_dir}
        LIBRARY DESTINATION ${_phosphor_control_qml_install_dir}
        ARCHIVE DESTINATION ${_phosphor_control_qml_install_dir}
    )
    install(TARGETS ${_phosphor_control_qml_shared_plugin_target}
        RUNTIME DESTINATION ${_phosphor_control_qml_install_dir}
        LIBRARY DESTINATION ${_phosphor_control_qml_install_dir}
        ARCHIVE DESTINATION ${_phosphor_control_qml_install_dir}
    )

    install(FILES
            ${CMAKE_BINARY_DIR}/qml_install/org/phosphor/control/qmldir
            ${CMAKE_BINARY_DIR}/qml_install/org/phosphor/control/PhosphorControlQmlShared.qmltypes
        DESTINATION ${_phosphor_control_qml_install_dir}
    )
endif()

# ═══════════════════════════════════════════════════════════════════════════════
# Tests
# ═══════════════════════════════════════════════════════════════════════════════

# enable_testing() here too, like the sibling libraries: a parent project
# that vendors this one with BUILD_TESTING on but never calls it would
# otherwise build the tests and register none of them.
if(BUILD_TESTING)
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
        add_subdirectory(tests)
    endif()
endif()
