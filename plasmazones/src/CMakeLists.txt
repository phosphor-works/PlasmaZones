# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PlasmaZones Core Library and Daemon

include(GenerateExportHeader)

# QTP0004: All QML files are explicitly listed in qt_add_qml_module.
# Guarded — the policy exists only from Qt 6.8, so a toolchain older than that
# fatal-errors on the unknown policy if it is set unconditionally.
# QT_KNOWN_POLICY_*, not if(POLICY ...): CMake's POLICY test only knows
# CMP* policies and is always false for QTP*.
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

# Wayland layer-shell protocol code generation lives in the PhosphorWayland
# library; nothing in this tier runs wayland-scanner.

# Core library sources
set(plasmazones_core_SRCS
    phosphor_qml_i18n.h
    phosphor_qml_i18n.cpp
    core/utils/translationloader.h
    core/utils/translationloader.cpp
    # wallpaperprovider moved to PhosphorShaders
    # ilayoutmanager / izonedetector shims removed. Consumers include
    # <PhosphorZones/IZoneLayoutRegistry.h> and
    # <PhosphorZones/IZoneDetector.h> directly. (There is no
    # PhosphorZones/ILayoutManager.h; that name survives only in prose.)
    # virtualscreenswapper.{h,cpp} lifted to phosphor-libs/libs/phosphor-screens/Swapper.h -
    # callers include <PhosphorScreens/Swapper.h> directly.
    core/utils/dbusvariantutils.cpp
    core/utils/dbusvariantutils.h
    # zone, layout, layoututils, zonedetector, zonehighlighter, LayoutRegistry,
    # AssignmentEntry, LayoutAssignmentKey live in phosphor-libs/libs/phosphor-zones/ -
    # consumers include <PhosphorZones/...> directly and construct the registry
    # with a borrowed PhosphorRules::RuleStore* + the
    # ConfigKeys::layoutsSubdir() subdir.
    common/engineruntimenamespace.h
    common/hostsettings.cpp
    common/plasmazoneslogging.cpp
    common/plasmazoneslogging.h
    common/hostsettings.h
    common/kwinperoutputdesktops.cpp
    common/kwinperoutputdesktops.h
    core/platform/supportreport.cpp
    core/platform/supportreport.h
    # layersurface moved to PhosphorWayland; shaderregistry/params moved to PhosphorShaders
    core/platform/singleinstanceservice.cpp
    core/platform/singleinstanceservice.h
    # Header-only: the environment the daemon's spawn sites hand the editor and
    # the settings app.
    # screen_resolver.{h,cpp} and screenmanager.{h,cpp} + screenmanager/
    # lifted to phosphor-libs/libs/phosphor-screens. Daemon consumers take
    # PhosphorScreens::ScreenManager* via constructor injection -
    # no service-locator intermediary.
    # shaderregistry.cpp + shaderincluderesolver moved to PhosphorShaders
    core/utils/modifierutils.cpp
    core/utils/modifierutils.h
    config/settingsvaluelabels.cpp
    config/settingsvaluelabels.h
    # snapnavigationtargets moved to phosphor-libs/libs/phosphor-snap-engine/
    # TilingAlgorithm, TilingState, SplitTree, AlgorithmRegistry, scripted-algo
    # machinery live in phosphor-libs/libs/phosphor-tiles/. Consumers include canonical
    # <PhosphorTiles/...> headers directly; no forwarding shims remain.
    # AutotileEngine + supporting files moved to phosphor-libs/libs/phosphor-tile-engine/.
    # SnapEngine + SnapState + supporting files moved to phosphor-libs/libs/phosphor-snap-engine/.
)

# Create core library
add_library(plasmazones_core SHARED ${plasmazones_core_SRCS})

# Generate export header
generate_export_header(plasmazones_core
    EXPORT_FILE_NAME plasmazones_export.h
    EXPORT_MACRO_NAME PLASMAZONES_EXPORT
)

# Layer-shell: implementation moved to PhosphorWayland library.
# The compatibility wrappers were removed with the move; nothing remains here.
# PhosphorWayland provides the QPA plugin, protocol code, and LayerSurface implementation.

# PhosphorShaders + PhosphorWayland headers are available via PUBLIC link dependencies

target_link_libraries(plasmazones_core
    PUBLIC
        Qt6::Core
        Qt6::Quick
        Qt6::Gui
        Qt6::DBus
        # PhosphorAnimation PUBLIC because Settings::animationProfile()
        # exposes PhosphorAnimation::Profile in the header - consumers
        # that #include <settings.h> need the core library for the
        # return type (Phase 4 sub-commit 6).
        PhosphorAnimation::PhosphorAnimation
        PhosphorConfig::PhosphorConfig
        PhosphorEngine::PhosphorEngine
        PhosphorGeometry::PhosphorGeometry
        PhosphorWorkspaces::PhosphorWorkspaces
        PhosphorPlacement::PhosphorPlacement
        PhosphorTileEngine::PhosphorTileEngine
        PhosphorSnapEngine::PhosphorSnapEngine
        PhosphorScrollEngine::PhosphorScrollEngine
        PhosphorIdentity::PhosphorIdentity
        PhosphorLayoutApi::PhosphorLayoutApi
        PhosphorProtocol::PhosphorProtocol
        PhosphorScreens::PhosphorScreens
        PhosphorShaders::PhosphorShaders
        # The engine runtime's settings model (Settings, ISettings, ConfigDefaults,
        # the schema and migrations), from phosphor-libs. PUBLIC: every consumer of
        # plasmazones_core reads settings through it.
        PhosphorEngineRuntime::PhosphorEngineRuntime
        PhosphorWayland::PhosphorWayland
        PhosphorTiles::PhosphorTiles
        PhosphorZones::PhosphorZones
        # PhosphorCompositor: DecorationDefaults — the shared window-decoration
        # default constants ConfigDefaults delegates to (PUBLIC because
        # configdefaults.h includes the header).
        PhosphorCompositor::PhosphorCompositor
        # PhosphorSurface: DecorationProfileTree / DecorationProfile — the
        # per-surface decoration tree type. PUBLIC because the engine runtime's
        # ConfigDefaults.h, Settings.h and ISettings.h expose the type by value in
        # their public API (the decorationProfileTree accessors + the
        # ConfigDefaults baseline), so consumers linking plasmazones_core need
        # the include path + link edge.
        PhosphorSurface::PhosphorSurface
        # PhosphorRegistry: PathKeyedOverrides — the shared baseline + keyed
        # overrides container. PUBLIC because PhosphorEngineRuntime/OverlayShaderTree.h
        # includes the header and holds one BY VALUE as a member, so anything that
        # #includes that header needs the include path and the link edge.
        PhosphorRegistry::PhosphorRegistry
        # PhosphorRules: the unified LGPL rule engine. PUBLIC since
        # Phase 3 — the engine runtime's Settings owns a RuleStore for the
        # per-mode disable lists, the RuleService serves the store, and its
        # migrations build rules during the v3→v4 conversion,
        # so consumers of plasmazones_core need the include path + link edge.
        PhosphorRules::PhosphorRules
        # Frozen-snapshot per-screen mode + disable/lock resolver. PUBLIC
        # because runtime classes compiled into plasmazones_core
        # (DragResolver, WindowTracker, SnapService) hold
        # `PhosphorContext::IContextResolver*` members; consumers linking
        # plasmazones_core need the include path + link edge to read those.
        PhosphorContextResolver::PhosphorContextResolver
    PRIVATE
        # PhosphorZoneRendering: setOverlayPackDataSubdir, which
        # initPlasmaZonesHost() (common/hostsettings.cpp) calls for every
        # PlasmaZones process.
        PhosphorZoneRendering::PhosphorZoneRendering
        Wayland::Client
        Qt6::Concurrent
        Qt6::WaylandClientPrivate
)

# Activities are not linked here: Plasma's activity backend lives in
# PhosphorWorkspaces::Plasma, which finds PlasmaActivities itself and keeps the
# link PRIVATE, and nothing in this target includes an activities header.

# kwinrc read and the consented write of PerOutputVirtualDesktops
# (common/kwinperoutputdesktops.cpp), the only KConfig user under plasmazones/:
# the daemon and the settings app reach it through this library and need no
# link of their own. Dynamic workspaces compile out of the
# USE_KDE_FRAMEWORKS=OFF build, so the link exists only where the feature
# does. PRIVATE: no plasmazones_core header exposes a KConfig type.
if(USE_KDE_FRAMEWORKS)
    target_link_libraries(plasmazones_core PRIVATE KF6::ConfigCore)
endif()

target_include_directories(plasmazones_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include/plasmazones>
)

set_target_properties(plasmazones_core PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    EXPORT_NAME Core
)

# Precompiled headers for core library - dramatically reduces compile time
if(ENABLE_PCH)
    target_precompile_headers(plasmazones_core
        PRIVATE
            <QObject>
            <QString>
            <QList>
            <QVariant>
            <QRect>
            <QPoint>
            <QSize>
            <QDebug>
            <QJsonObject>
            <QJsonArray>
            <memory>
            <vector>
            <functional>
    )
endif()

install(TARGETS plasmazones_core
    # NOTE: PlasmaZonesTargets is assigned here but never installed -- there
    # is no install(EXPORT PlasmaZonesTargets) and no PlasmaZonesConfig.cmake
    # anywhere in the tree, so find_package(PlasmaZones) cannot work and the
    # INSTALL_INTERFACE include dirs above are inert. Kept because dropping
    # the keyword changes install() semantics; wire up the config-file pair
    # if the app tier is ever meant to be consumable.
    EXPORT PlasmaZonesTargets
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
)

# ═══════════════════════════════════════════════════════════════════════════════
# D-Bus adaptors (plasmazonesd's bus surface over the runtime's services)
# ═══════════════════════════════════════════════════════════════════════════════
# The QDBusAbstractAdaptor bindings in dbus/, apart from plasmazones_core so
# that the core, which the engine runtime will be carved from, holds no bus
# surface of its own. They depend on the runtime and never the reverse (the
# runtime-firewall conventions rule refuses a dbus/ include under runtime/).
# Only the daemon links this, and the unit tests that drive an adaptor.
set(plasmazones_dbus_SRCS
    dbus/dbushelpers.cpp
    dbus/layoutadaptor.cpp
    dbus/layoutadaptor.h
    dbus/settingsadaptor.cpp
    dbus/settingsadaptor.h
    dbus/shaderadaptor.cpp
    dbus/shaderadaptor.h
    dbus/compositorbridgeadaptor.cpp
    dbus/compositorbridgeadaptor.h
    dbus/controladaptor.cpp
    dbus/controladaptor.h
    dbus/overlayadaptor.cpp
    dbus/overlayadaptor.h
    dbus/zonedetectionadaptor.cpp
    dbus/zonedetectionadaptor.h
    dbus/ruleadaptor.cpp
    dbus/ruleadaptor.h
    dbus/windowtrackingadaptor.cpp
    dbus/windowtrackingadaptor.h
    dbus/windowdragadaptor.cpp
    dbus/windowdragadaptor.h
    dbus/scrollingadaptor.cpp
    dbus/scrollingadaptor.h
    dbus/autotileadaptor.cpp
    dbus/autotileadaptor.h
    dbus/tilingadaptor.cpp
    dbus/tilingadaptor.h
    dbus/snapadaptor.cpp
    dbus/snapadaptor.h
    dbus/dbushelpers.h
)

add_library(plasmazones_dbus SHARED ${plasmazones_dbus_SRCS})

generate_export_header(plasmazones_dbus
    EXPORT_FILE_NAME plasmazones_dbus_export.h
    EXPORT_MACRO_NAME PLASMAZONES_DBUS_EXPORT
)

target_link_libraries(plasmazones_dbus
    PUBLIC
        plasmazones_core
        Qt6::DBus
        PhosphorProtocol::PhosphorProtocol
    PRIVATE
        # ControlAdaptor::generateSupportReport builds its report off the GUI thread.
        Qt6::Concurrent
        # OverlayAdaptor::compositor reads the Wayland connection's fd to name
        # the compositor process (wl_display_get_fd).
        Wayland::Client
)

target_include_directories(plasmazones_dbus
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include/plasmazones>
)

# Versioned SONAME, as plasmazones_core.
set_target_properties(plasmazones_dbus PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

install(TARGETS plasmazones_dbus
    # EXPORT PlasmaZonesTargets is inert, as on plasmazones_core above.
    EXPORT PlasmaZonesTargets
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
)

# ═══════════════════════════════════════════════════════════════════════════════
# Layer Shell Qt Plugin - now provided by PhosphorWayland library
# ═══════════════════════════════════════════════════════════════════════════════
# The QPA plugin (phosphorwayland-qpa) is built and installed by phosphor-libs/libs/phosphor-wayland/CMakeLists.txt.
# PhosphorWayland::registerLayerShellPlugin() sets
# QT_WAYLAND_SHELL_INTEGRATION=phosphorwayland. There is no PlasmaZones::
# forwarder; callers use the PhosphorWayland symbol directly.

# plasmazones_shaderpreview: shared zone-shader live-preview feed. Used by the
# settings app (which injects its own IShaderPreviewBackend). The daemon does NOT
# link it because it hosts no preview, not to keep dependencies out: every one of
# this target's PUBLIC deps (plasmazones_core, PhosphorZoneRendering,
# PhosphorAudio, Qt6::Gui) already reaches plasmazonesd below, CAVA included,
# since the daemon runs its own CavaSpectrumProvider. The first three are named
# on it directly. Qt6::Gui is not named, and arrives two ways: through Qt6::Quick,
# and one hop from the Qt6::GuiPrivate the daemon links further down.
set(plasmazones_shaderpreview_SRCS
    shaderpreview/ishaderpreviewbackend.h
    shaderpreview/shaderpreviewcontroller.h
    shaderpreview/shaderpreviewcontroller.cpp
)

add_library(plasmazones_shaderpreview SHARED ${plasmazones_shaderpreview_SRCS})

generate_export_header(plasmazones_shaderpreview
    EXPORT_FILE_NAME plasmazones_shaderpreview_export.h
    EXPORT_MACRO_NAME PLASMAZONES_SHADERPREVIEW_EXPORT
)

target_link_libraries(plasmazones_shaderpreview
    PUBLIC
        plasmazones_core  # brings PhosphorShaders + PhosphorZones PUBLIC
        PhosphorZoneRendering::PhosphorZoneRendering  # ZoneLabelTextureBuilder + ZoneShaderItem (phosphor-libs)
        PhosphorAudio::PhosphorAudio  # CAVA spectrum provider
        Qt6::Gui
)

target_include_directories(plasmazones_shaderpreview
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include/plasmazones>
)

set_target_properties(plasmazones_shaderpreview PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

install(TARGETS plasmazones_shaderpreview
    # NOTE: PlasmaZonesTargets is assigned here but never installed -- there
    # is no install(EXPORT PlasmaZonesTargets) and no PlasmaZonesConfig.cmake
    # anywhere in the tree, so find_package(PlasmaZones) cannot work and the
    # INSTALL_INTERFACE include dirs above are inert. Kept because dropping
    # the keyword changes install() semantics; wire up the config-file pair
    # if the app tier is ever meant to be consumable.
    EXPORT PlasmaZonesTargets
    LIBRARY DESTINATION ${KDE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${KDE_INSTALL_LIBDIR}
)

# Daemon executable
set(plasmazones_daemon_SRCS
    daemon/main.cpp
    daemon/rendering/vulkansupport.cpp
    daemon/rendering/vulkansupport.h
    daemon/daemon.cpp
    daemon/daemon.h
    daemon/bridgewatchdog.cpp
    daemon/plasmaworkspace.cpp
    runtime/engine/engineruntime.cpp
    runtime/engine/start.cpp
    runtime/engine/desktop_renumber.cpp
    runtime/engine/evacuees.cpp
    runtime/engine/shortcuts_wiring.cpp
    runtime/engine/autotile_init.cpp
    runtime/engine/scrolling_init.cpp
    runtime/engine/scrolling.cpp
    runtime/engine/workspaces.cpp
    runtime/engine/signals.cpp
    runtime/engine/shader_warmup.cpp
    runtime/engine/animation_profiles.cpp
    runtime/engine/init_services.cpp
    runtime/engine/init_adaptors.cpp
    runtime/engine/init_engines.cpp
    runtime/engine/init_assignment_apply.cpp
    runtime/engine/init_service_wiring.cpp
    runtime/engine/lifecycle.cpp
    runtime/engine/idle.cpp
    runtime/engine/navigation.cpp
    runtime/engine/autotile.cpp
    runtime/engine/engine_release.cpp
    runtime/engine/seedorderfilter.cpp
    runtime/engine/seedorderfilter.h
    runtime/engine/enginerouting.cpp
    runtime/engine/enginerouting.h
    runtime/engine/screenreshapetracker.cpp
    runtime/engine/screenreshapetracker.h
    runtime/engine/snapgaptracker.cpp
    runtime/engine/snapgaptracker.h
    runtime/engine/osd.cpp
    runtime/engine/cheatsheet.cpp
    runtime/engine/stripzones.h
    runtime/engine/engineruntime.h
    runtime/engine/engineruntime_fwd.h
    runtime/engine/helpers.h
    runtime/engine/macros.h
    # Zero-copy GPU thumbnail path (PHOSPHOR_DMABUF_THUMBNAILS): imports
    # dma-bufs handed from the kwin-effect as Vulkan-backed QSGTextures. The
    # overlays reach it through the presentation host below.
    daemon/host/kwinsurfacepresentation.cpp
    daemon/host/kwinsurfacepresentation.h
    daemon/rendering/dmabuftextureprovider.cpp
    daemon/rendering/dmabuftextureprovider.h
    daemon/rendering/dmabuffencewaiter.cpp
    daemon/rendering/dmabuffencewaiter.h
    # OpenGL-RHI fallback import (EGL dma-buf -> GL texture). Isolated TU so its
    # epoxy GL/EGL headers don't mix with the Vulkan + Qt RHI headers.
    daemon/rendering/dmabufglimport.cpp
    daemon/rendering/dmabufglimport.h
    runtime/controllers/shellcheatsheetbridge.cpp
    runtime/controllers/shellcheatsheetbridge.h
    # Dynamic per-monitor workspaces: KWin's side
    daemon/host/kwinworkspacehost.cpp
    daemon/host/kwinworkspacehost.h
    daemon/host/plasmasystemosd.cpp
    daemon/host/plasmasystemosd.h
    # The host's own apps (IHostApps): settings and the layout editor
    daemon/host/childprocessenvironment.h
    daemon/host/plasmazonesapps.cpp
    daemon/host/plasmazonesapps.h
)

add_executable(plasmazonesd ${plasmazones_daemon_SRCS})

target_link_libraries(plasmazonesd
    PRIVATE
        plasmazones_core
        plasmazones_dbus  # the bus surface: every adaptor the daemon registers
        PhosphorZoneRendering::PhosphorZoneRendering
        # org.phosphor.zonerendering: ZoneShaderItem and ZoneShaderRenderer,
        # which the zone overlay draws with.
        PhosphorZoneRendering::PhosphorZoneRenderingQml
        PhosphorZoneRenderingQmlplugin  # Generated by qt_add_qml_module (STATIC)
        PhosphorComponents::PhosphorComponentsQml
        PhosphorComponentsQmlplugin  # Generated by qt_add_qml_module (STATIC)
        # org.phosphor.surface: the SurfaceDecoration host the overlay QML
        # mounts and the SurfaceShaderItem stages it runs, plus the item's
        # C++ API for the warm bake (shader_warmup.cpp).
        PhosphorSurfaceQuick::PhosphorSurfaceQuickQml
        PhosphorSurfaceQuickQmlplugin  # Generated by qt_add_qml_module (STATIC)
        PhosphorAnimation::PhosphorAnimation  # motion runtime + QML module + shaders + layer
        PhosphorAnimationQmlplugin  # Generated by qt_add_qml_module (STATIC)
        PhosphorLayer::PhosphorLayer  # layer-surface lifecycle
        # The KWin session's platform (daemon/daemon.cpp hands both to the
        # engine runtime): the wlr-layer-shell transport, and KWin's virtual
        # desktops and Plasma's activities.
        PhosphorLayer::Wayland  # PhosphorWaylandTransport
        PhosphorWorkspaces::Plasma  # KWin / Plasma workspace backends
        PhosphorShellPatterns::PhosphorShellPatterns  # axis-2 UI-pattern recipes
        PhosphorShortcuts::PhosphorShortcuts  # ShortcutManager / Registry / backends
        PhosphorAudio::PhosphorAudio  # audio spectrum provider (CAVA)
        PhosphorSurfaces::PhosphorSurfaces  # managed surface lifecycle (engine, keep-alive)
        # Named directly, not left to plasmazones_core's PUBLIC edge: daemon TUs
        # include <PhosphorSurface/...> and the engine runtime's overlay
        # decoration.cpp
        # calls its exported chain-compose helpers, so a future demotion of that
        # edge to PRIVATE would break them. The other chain hosts (kwin-effect
        # and the settings app's decoration preview) both name it explicitly
        # already.
        PhosphorSurface::PhosphorSurface  # SurfaceChainCompose / registry / DecorationSupportedPaths
        # Named directly for the same reason: daemon/main.cpp includes
        # <PhosphorConfig/HostDirectory.h> and calls HostDirectory::writeRecord.
        PhosphorConfig::PhosphorConfig  # HostDirectory (the placement host record)
        PhosphorServiceIdle::PhosphorServiceIdle  # ext-idle-notify-v1; the daemon owns session-idle detection and pushes it to the KWin effect (a compositor SERVES that protocol, it cannot consume it)
        PhosphorOverlay::PhosphorOverlay  # per-screen shell-host + slot mechanism (Phase 2 onward)
        # Frozen-snapshot per-screen mode + disable/lock cascade façade.
        # The daemon owns a `ContextResolver` over three local adapters
        # (DaemonWorkspaceStateAdapter / DaemonScreenModeAdapter /
        # DaemonSettingsGateAdapter in
        # the engine runtime's ContextResolverWiring.h) and lets D-Bus adaptors
        # + OverlayService borrow it via `EngineRuntime::contextResolver()`.
        PhosphorContextResolver::PhosphorContextResolver
        # Daemon is headless: runs under QGuiApplication. No Qt6::Widgets
        # link: it was unused and pulled QtWidgets + xcb deps into a binary
        # that never instantiates a QWidget.
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Concurrent  # Pre-load shader bake cache on registry refresh
        Qt6::Network  # Optional shell shortcut IPC, also available in shell-OFF builds
        Qt6::ShaderTools  # QShaderBaker for ZoneShaderNodeRhi (runtime GLSL bake)
)

# Qt6::GuiPrivate (QRhi) for daemon's include of zoneshadernoderhi.h (warmShaderBakeCacheForPaths)
target_link_libraries(plasmazonesd PRIVATE Qt6::GuiPrivate Qt6::ShaderToolsPrivate)

# Vulkan headers for the dma-buf thumbnail import (dmabuftextureprovider.cpp).
# Only the headers + function pointers are used (entry points are resolved via
# QVulkanInstance / vkGetDeviceProcAddr), so this is headers-only — but linking
# the imported target is the clean way to pick up the include dirs. REQUIRED:
# dmabuftextureprovider.cpp #includes <vulkan/vulkan.h> unconditionally and is
# always compiled into the daemon, so a missing Vulkan SDK must fail at configure
# time with a clear message rather than as an opaque header-not-found compile error.
find_package(Vulkan REQUIRED)
target_link_libraries(plasmazonesd PRIVATE Vulkan::Vulkan)

# libepoxy: EGL/GL entry points for the OpenGL-RHI dma-buf import fallback
# (daemon/rendering/dmabufglimport.cpp). Only that TU includes epoxy headers.
find_package(PkgConfig REQUIRED)
pkg_check_modules(Epoxy REQUIRED IMPORTED_TARGET epoxy)
target_link_libraries(plasmazonesd PRIVATE PkgConfig::Epoxy)

# Keep the epoxy TU out of the Unity (jumbo) build: epoxy/gl.h must be the
# first GL include and refuses to coexist with GL/gl.h, which jumbo-grouping
# with the Vulkan + Qt RHI sources would pull in. Standalone TU keeps its
# EGL/GL headers fully isolated.
set_source_files_properties(daemon/rendering/dmabufglimport.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

install(TARGETS plasmazonesd DESTINATION ${KDE_INSTALL_BINDIR})

# Separate precompiled headers for executables (different PIC/PIE settings than shared lib)
if(ENABLE_PCH)
    target_precompile_headers(plasmazonesd
        PRIVATE
            <QObject>
            <QString>
            <QList>
            <QVariant>
            <QDebug>
    )
endif()

# Systemd user service (preferred over autostart for proper session ordering)
# This ensures PlasmaZones starts AFTER plasma-workspace.target, avoiding
# race conditions with KDE's session restore.
# Uses configure_file to resolve the correct binary path for the install prefix.
configure_file(
    daemon/plasmazones.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/plasmazones.service
    @ONLY
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/plasmazones.service
    DESTINATION ${KDE_INSTALL_SYSTEMDUSERUNITDIR}
)

# Daemon desktop file: use full Exec path so KWin's fetchRestrictedDBusInterfacesFromPid
# can match the process (it compares executable path to desktop Exec).
configure_file(
    daemon/org.phosphor_works.PlasmaZones.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/org.phosphor_works.PlasmaZones.desktop
    @ONLY
)

# D-Bus service activation: lets a client method-call wake the daemon on
# first contact without requiring the systemd user unit to be already
# running. Installed to ${KDE_INSTALL_DBUSSERVICEDIR}; install() rule is
# in the install block lower in this file.
configure_file(
    daemon/org.phosphor_works.Placement.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/org.phosphor_works.Placement.service
    @ONLY
)

# ═══════════════════════════════════════════════════════════════════════════════
# Layout Editor
# ═══════════════════════════════════════════════════════════════════════════════

set(plasmazones_editor_SRCS
    editor/main.cpp
    editor/EditorController.cpp
    editor/EditorGapsModel.cpp
    editor/EditorGapsModel.h
    editor/EditorTemplateModel.cpp
    editor/EditorTemplateModel.h
    editor/EditorLaunchController.cpp
    editor/EditorLaunchController.h
    editor/EditorAppAdaptor.cpp
    editor/EditorAppAdaptor.h
    editor/controller/gaps.cpp
    editor/controller/layout.cpp
    editor/controller/selection.cpp
    editor/controller/multiselect.cpp
    editor/controller/visibility.cpp
    editor/controller/clipboard.cpp
    editor/controller/zones.cpp
    editor/controller/zoneops.cpp
    editor/controller/settings.cpp
    editor/controller/scrollingtemplate.cpp
    editor/EditorController.h
    editor/helpers/BatchOperationScope.h
    editor/helpers/EditorOwnedConfig.cpp
    editor/helpers/EditorOwnedConfig.h
    editor/helpers/ZoneSerialization.cpp
    editor/helpers/ZoneSerialization.h
    editor/helpers/SettingsDbusQueries.cpp
    editor/helpers/SettingsDbusQueries.h
    editor/services/ILayoutService.h
    editor/services/DBusLayoutService.cpp
    editor/services/DBusLayoutService.h
    editor/services/ZoneManager.cpp
    editor/services/zonemanager/divider.cpp
    editor/services/zonemanager/zorder.cpp
    editor/services/zonemanager/serialization.cpp
    editor/services/ZoneManager.h
    editor/services/ZoneAutoFiller.cpp
    editor/services/ZoneAutoFiller.h
    editor/services/SnappingService.cpp
    editor/services/SnappingService.h
    editor/services/TemplateService.cpp
    editor/services/TemplateService.h
    editor/services/TemplateStrategy.h
    editor/undo/UndoController.cpp
    editor/undo/UndoController.h
    editor/undo/commands/BaseZoneCommand.cpp
    editor/undo/commands/BaseZoneCommand.h
    editor/undo/commands/CommandId.h
    editor/undo/commands/AddZoneCommand.cpp
    editor/undo/commands/AddZoneCommand.h
    editor/undo/commands/DeleteZoneCommand.cpp
    editor/undo/commands/DeleteZoneCommand.h
    editor/undo/commands/UpdateZoneGeometryCommand.cpp
    editor/undo/commands/UpdateZoneGeometryCommand.h
    editor/undo/commands/UpdateZoneNameCommand.cpp
    editor/undo/commands/UpdateZoneNameCommand.h
    editor/undo/commands/UpdateZoneNumberCommand.cpp
    editor/undo/commands/UpdateZoneNumberCommand.h
    editor/undo/commands/UpdateZoneAppearanceCommand.cpp
    editor/undo/commands/UpdateZoneAppearanceCommand.h
    editor/undo/commands/DuplicateZoneCommand.cpp
    editor/undo/commands/DuplicateZoneCommand.h
    editor/undo/commands/SplitZoneCommand.cpp
    editor/undo/commands/SplitZoneCommand.h
    editor/undo/commands/FillZoneCommand.cpp
    editor/undo/commands/FillZoneCommand.h
    editor/undo/commands/DeleteZoneWithFillCommand.cpp
    editor/undo/commands/DeleteZoneWithFillCommand.h
    editor/undo/commands/DividerResizeCommand.cpp
    editor/undo/commands/DividerResizeCommand.h
    editor/undo/commands/ChangeZOrderCommand.cpp
    editor/undo/commands/ChangeZOrderCommand.h
    editor/undo/commands/ApplyTemplateCommand.cpp
    editor/undo/commands/ApplyTemplateCommand.h
    editor/undo/commands/ClearAllZonesCommand.cpp
    editor/undo/commands/ClearAllZonesCommand.h
    editor/undo/commands/UpdateLayoutNameCommand.cpp
    editor/undo/commands/UpdateLayoutNameCommand.h
    editor/undo/commands/ChangeSelectionCommand.cpp
    editor/undo/commands/ChangeSelectionCommand.h
    editor/undo/commands/BatchUpdateAppearanceCommand.cpp
    editor/undo/commands/BatchUpdateAppearanceCommand.h
    editor/undo/commands/PasteZonesCommand.cpp
    editor/undo/commands/PasteZonesCommand.h
    editor/undo/commands/UpdateGapOverrideCommand.cpp
    editor/undo/commands/UpdateGapOverrideCommand.h
    editor/undo/commands/UpdateFullScreenGeometryCommand.cpp
    editor/undo/commands/UpdateFullScreenGeometryCommand.h
    editor/undo/commands/UpdateFixedGeometryUnitCommand.cpp
    editor/undo/commands/UpdateFixedGeometryUnitCommand.h
    editor/undo/commands/ToggleGeometryModeCommand.cpp
    editor/undo/commands/ToggleGeometryModeCommand.h
    editor/undo/commands/UpdateFixedGeometryCommand.cpp
    editor/undo/commands/UpdateFixedGeometryCommand.h
    editor/undo/commands/UpdateVisibilityCommand.cpp
    editor/undo/commands/UpdateVisibilityCommand.h
    editor/undo/commands/UpdateTemplateCommand.cpp
    editor/undo/commands/UpdateTemplateCommand.h
)

# Create the executable first
add_executable(plasmazones-editor ${plasmazones_editor_SRCS})

# ColorUtils.js is imported by relative path from each QML file that uses it, so it needs
# no qmldir entry. NOT about re-evaluation: the file's own `.pragma library` is what makes
# it shared, and it declares one inside Qt's 128-byte scan window. ThemeHelpers.js is
# imported the same way and declares its pragma at the same byte offset, but carries no
# such CMAKE SOURCE PROPERTY, so the two files are inconsistent. The inconsistency stays,
# deliberately and in this direction. The property's whole effect is visible in the
# generated qmldir, which lists ThemeHelpers.js and not ColorUtils.js, and nothing imports
# either through the module URI. Levelling it either way would change what qmllint and
# qmlls read, and no gate in this repo runs them, so there is nothing that would catch a
# regression from the change.
set_source_files_properties(editor/qml/ColorUtils.js PROPERTIES
    QT_QML_SKIP_QMLDIR_ENTRY TRUE
)

# Suppress -Wmissing-include-dirs for the QML module's Qt-managed AUTOMOC
# scratch dirs. KDECompilerSettings enables the warning globally, and
# qt_add_qml_module adds `<target>_autogen/include` to each generated
# sub-target's -I list; the no-Q_OBJECT sub-targets (qmltyperegistration,
# resource init) leave it empty or — after `--clean-first` — missing, so
# cc1plus fires before AUTOMOC recreates it. Set here (after the core /
# rendering / shaderpreview / daemon targets above, which are unaffected). NOTE: directory
# COMPILE_OPTIONS are read at TARGET-CREATION time, so they apply only to targets
# created AFTER this point in the scope. plasmazones-editor is declared ABOVE it and
# therefore does NOT carry the flag (verified in the generated flags.make); what does
# is plasmazones-shader-validate, plus the add_subdirectory(settings) tree further
# down. The suppression is harmless for those (it only silences the
# missing-AUTOMOC-include-dir warning class). Mirrors
# plasmazones/src/settings/CMakeLists.txt.
set_property(DIRECTORY APPEND PROPERTY COMPILE_OPTIONS "-Wno-missing-include-dirs")

# Add QML module to the executable
qt_add_qml_module(plasmazones-editor
    URI org.plasmazones.editor
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES
        editor/qml/EditorWindow.qml
        editor/qml/EditorZone.qml
        editor/qml/TopBar.qml
        editor/qml/PropertyPanel.qml
        editor/qml/ControlBar.qml
        editor/qml/DividerManager.qml
        editor/qml/DividerHandle.qml
        editor/qml/ResizeHandles.qml
        editor/qml/ZoneContent.qml
        editor/qml/ActionButtons.qml
        editor/qml/ZoneActionButton.qml
        editor/qml/ThemeHelpers.js
        editor/qml/SnapIndicator.qml
        editor/qml/DimensionTooltip.qml
        editor/qml/EditorNotifications.qml
        editor/qml/NotificationBanner.qml
        editor/qml/ZoneContextMenu.qml
        editor/qml/EditorShortcuts.qml
        editor/qml/KeyboardNavigation.qml
        editor/qml/ZoneDragHandler.qml
        editor/qml/ZoneFillAnimation.qml
        editor/qml/ZoneGeometrySync.qml
        editor/qml/GridOverlay.qml
        editor/qml/CanvasMouseHandler.qml
        editor/qml/ZoneOperations.qml
        editor/qml/HelpDialogContent.qml
        editor/qml/TemplatePreview.qml
        editor/qml/TemplateStripCanvas.qml
        editor/qml/TemplatePropertyPanel.qml
        editor/qml/PresetChipEditor.qml
        editor/qml/VisibilitySettingsDialog.qml
        editor/qml/LayoutSettingsDialog.qml
        editor/qml/SectionHeader.qml
        # SRP-extracted components from PropertyPanel
        editor/qml/ColorUtils.js
        editor/qml/ColorPickerRow.qml
        editor/qml/OpacitySliderRow.qml
        editor/qml/AppearanceSpinBox.qml
        editor/qml/ZoneColorDialog.qml
    )

target_link_libraries(plasmazones-editor
    PRIVATE
        plasmazones_core
        PhosphorComponents::PhosphorComponentsQml
        PhosphorComponentsQmlplugin  # Generated by qt_add_qml_module (STATIC)
        PhosphorAnimation::PhosphorAnimation  # motion runtime + QML module
        PhosphorAnimationQmlplugin  # Generated by qt_add_qml_module (STATIC)
        # Qt6::Widgets is REQUIRED even though src/editor has no direct QWidget
        # usage: the editor runs under QApplication and uses the org.kde.desktop
        # QtQuick Controls style (qqc2-desktop-style), which renders controls
        # through a QtWidgets QStyle. Dropping Widgets here and constructing a
        # QGuiApplication is what crashed third-party styles like Darkly on first
        # paint (discussion #262). The daemon stays QGuiApplication-only — it
        # uses no Kirigami/Controls — so it deliberately does NOT link Widgets.
        Qt6::Widgets
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::DBus
)

install(TARGETS plasmazones-editor DESTINATION ${KDE_INSTALL_BINDIR})

# T1.2 — offline shader-pack validator. Parses metadata.json with the daemon's
# parser, reproduces the runtime GLSL assembly (entry scaffold + p_<id> preamble
# + includes) and bakes every stage through headless glslang (QShaderBaker — CPU
# only, no GPU/Wayland). The CI gate for the bundled sets; pre-commit-friendly for
# pack authors. Links PhosphorZoneRendering for the zone entry scaffold and
# PhosphorAnimation for the animation effect parser + entry scaffold.
add_executable(plasmazones-shader-validate
    shadervalidate/main.cpp
    shadervalidate/packvalidator_overlay.cpp
    shadervalidate/packvalidator_animation.cpp
    shadervalidate/packvalidator_surface.cpp
    shadervalidate/packvalidator_surface_lints.cpp
    shadervalidate/packvalidator_surface_buffer_lints.cpp
    shadervalidate/packvalidator_pointer.cpp
    shadervalidate/packvalidatorcommon.cpp
    shadervalidate/packvalidator_compositor.cpp
)
target_link_libraries(plasmazones-shader-validate
    PRIVATE
        # initPlasmaZonesHost() at the top of main(): the app directory name and
        # the surface packs' data subdirectory the pack lookups search.
        plasmazones_core
        PhosphorZoneRendering::PhosphorZoneRendering
        PhosphorRendering::PhosphorRendering
        PhosphorShaders::PhosphorShaders
        PhosphorAnimation::PhosphorAnimation
        PhosphorSurface::PhosphorSurface
        PhosphorPointer::PhosphorPointer
        Qt6::Core
        Qt6::GuiPrivate
        Qt6::ShaderToolsPrivate
)
install(TARGETS plasmazones-shader-validate DESTINATION ${KDE_INSTALL_BINDIR})

# CI gate: every bundled pack must parse + bake clean. Headless glslang, so this
# runs in CI with no GPU/Wayland — exactly like luau-analyze for algorithms. Zone
# packs (data/overlays), animation packs (--animation, data/animations),
# surface packs (--surface, the pinned phosphor-surface-packs tree at
# PHOSPHOR_SURFACE_PACKS_DIR) and pointer packs (--pointer, data/pointer) are distinct authoring models with different entry conventions,
# so each gets its own invocation. (The validator also detects the model per
# pack now, so the flags are belt-and-braces here;
# they pin each test to one model regardless of what a tree grows later.)
#
# REQUIRES `glslangValidator` (or the newer `glslang` spelling, either name is
# accepted) on PATH for shader_validate_animations, shader_validate_surface and
# shader_validate_pointer. On x86-64 Linux the pinned Khronos release is put
# there (phosphor_put_glslang_on_test_path in cmake/PlasmaZonesDependencies.cmake),
# so only other hosts need the system's glslang package. Every animation pack's fragment and declared vertex,
# every surface pack's fragment, buffer passes and own declared vertex, and
# every pointer pack's stages, are compiled for the compositor's classic-GL
# branch (default-block uniforms, which QShaderBaker's SPIR-V target rejects by
# design), so they go out of process through glslang's default GLSL-semantics
# mode. Each is compiled in both dialects KWin compiles it in, desktop GLSL 140
# and GLSL ES 300, and each fragment is linked against the vertex stage the
# compositor pairs it with. Those three invocations FAIL rather than skip when
# the binary is absent: the compositor branch is compiled nowhere else in a
# headless run (the GPU bake test QSKIPs without a desktop GL 4.5 context), so a
# silent skip would put the gate back to lints-only. The overlay invocation bakes through
# QShaderBaker alone and needs no such tool.
if(BUILD_TESTING)
    add_test(NAME shader_validate_bundled
             COMMAND plasmazones-shader-validate --overlay ${CMAKE_SOURCE_DIR}/plasmazones/data/overlays)
    add_test(NAME shader_validate_animations
             COMMAND plasmazones-shader-validate --animation ${CMAKE_SOURCE_DIR}/plasmazones/data/animations)
    add_test(NAME shader_validate_surface
             COMMAND plasmazones-shader-validate --surface ${PHOSPHOR_SURFACE_PACKS_DIR})
    add_test(NAME shader_validate_pointer
             COMMAND plasmazones-shader-validate --pointer ${CMAKE_SOURCE_DIR}/plasmazones/data/pointer)

    # The XDG half of the standard isolation, applied by hand — NOT a stray copy
    # that should have used the shared helper. These tests have no target of
    # their own (they all invoke the one `plasmazones-shader-validate` command), and
    # `phosphor_apply_test_isolation` keys its sandbox on a TARGET name, so it
    # cannot apply here. Giving the module a sandbox key independent of the
    # target name is the only way to fold this in.
    #
    # The validator's pack ENUMERATION never touches QStandardPaths — it walks
    # the explicit --overlay/--animation/--surface/--pointer directory arguments.
    # Include roots come from packSharedRoots: a bundled pack sits in the
    # self-contained `<family root>/<id>` layout and resolves against its
    # sibling shared/ only, so the XDG chain is never consulted for these runs
    # either; the widening to QStandardPaths::locateAll is reserved for the
    # installed layout under an XDG data dir. What this sandbox pins is
    # the residue: SurfaceShaderRegistry's `builtin:` buffer fallback resolves
    # through QStandardPaths when a pack names a builtin buffer whose sibling
    # `../shared/` copy is missing — never the case for the bundled packs
    # today, so this is a belt-and-braces pin rather than a live-failure fix.
    # The bus half is not needed: the validator never touches D-Bus.
    set(_pz_shader_validate_xdg "${CMAKE_BINARY_DIR}/test-xdg/shader_validate")
    file(MAKE_DIRECTORY "${_pz_shader_validate_xdg}/config" "${_pz_shader_validate_xdg}/data"
                        "${_pz_shader_validate_xdg}/state" "${_pz_shader_validate_xdg}/cache")
    foreach(_pz_sv shader_validate_bundled shader_validate_animations shader_validate_surface
                   shader_validate_pointer)
        set_property(TEST ${_pz_sv} APPEND PROPERTY ENVIRONMENT
            "XDG_CONFIG_HOME=${_pz_shader_validate_xdg}/config"
            "XDG_DATA_HOME=${_pz_shader_validate_xdg}/data"
            "XDG_STATE_HOME=${_pz_shader_validate_xdg}/state"
            "XDG_CACHE_HOME=${_pz_shader_validate_xdg}/cache"
            "XDG_DATA_DIRS=${_pz_shader_validate_xdg}/data")
        # These bypass phosphor_apply_test_isolation (target-keyed, can't apply
        # to a shared command), so they'd otherwise be the only isolated tests
        # with no default timeout. Pin the same 120s a hung glslang invocation
        # would need to wedge the run.
        set_tests_properties(${_pz_sv} PROPERTIES TIMEOUT 120)
    endforeach()
    phosphor_put_glslang_on_test_path(shader_validate_animations shader_validate_surface shader_validate_pointer)
endif()

# Standalone settings app: independent of KDE System Settings, so it runs
# outside a Plasma session. It is NOT framework-free — src/settings/CMakeLists.txt
# does find_package(KF6Kirigami REQUIRED) and links KF6::Kirigami, and this
# add_subdirectory is unconditional, so Kirigami is a hard configure-time
# dependency of the portable USE_KDE_FRAMEWORKS=OFF build too.
add_subdirectory(settings)

# Separate precompiled headers for executables (different PIC/PIE settings than shared lib)
if(ENABLE_PCH)
    target_precompile_headers(plasmazones-editor
        PRIVATE
            <QObject>
            <QString>
            <QList>
            <QVariant>
            <QRect>
            <QDebug>
            <memory>
    )
endif()

# Desktop file for the editor (org.kde naming for portal integration)
install(FILES
    editor/org.phosphor_works.PlasmaZones.Editor.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)

# Desktop file for the daemon (for XDG portal registration, NoDisplay=true).
# Uses configured .desktop with full Exec path for KWin screenshot auth.
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/org.phosphor_works.PlasmaZones.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)

# D-Bus session activation file. Wakes the daemon on first bus call.
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/org.phosphor_works.Placement.service
    DESTINATION ${KDE_INSTALL_DBUSSERVICEDIR}
)

# AppStream metainfo: so Discover / GNOME Software / KDE store list the
# editor + settings apps with summary, screenshots, and content rating.
# Validated by `appstreamcli validate <file>` if `appstream` is installed.
install(FILES
    editor/org.phosphor_works.PlasmaZones.Editor.metainfo.xml
    settings/org.phosphor_works.Facet.metainfo.xml
    DESTINATION ${KDE_INSTALL_METAINFODIR}
)

# KGlobalAccel component desktop file - provides display name + icon in
# System Settings > Shortcuts.  Filename must match applicationName ("plasmazonesd").
install(FILES
    daemon/plasmazonesd.desktop
    DESTINATION ${KDE_INSTALL_DATADIR}/kglobalaccel
)

# Snap Assist thumbnails are now produced inside the kwin-effect via
# OffscreenQuickScene + org.kde.kwin's WindowThumbnail, so the daemon no
# longer needs ScreenShot2 access. The X-KDE-DBUS-Restricted-Interfaces
# entry and the environment.d/99-plasmazones-screenshot.conf override
# that gated that path are gone with the C++ removal.
