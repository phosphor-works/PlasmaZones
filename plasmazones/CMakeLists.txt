# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PlasmaZones: the daemon, layout editor, settings app, KCM, KWin effect,
# developer tools, bundled data assets and the unit tests. Builds on the
# phosphor-libs tree.

# The surface decoration packs, fetched in the root CMakeLists ahead of the
# libraries: PHOSPHOR_SURFACE_PACKS_DIR is the packs tree the tests and the
# validator gate read, and every target here gets the install subdir as
# PHOSPHOR_SURFACE_PACKS_DATA_SUBDIR, which each process hands to
# PhosphorSurfaceShaders::setSurfacePackDataSubdir() at startup and uses to
# build its own pack search list.
add_compile_definitions(PHOSPHOR_SURFACE_PACKS_DATA_SUBDIR="${PHOSPHOR_SURFACE_PACKS_DATA_SUBDIR}")

# Source directories
add_subdirectory(src)

# Developer tools - opt-in via -DBUILD_TOOLS=ON.  Each tool is a
# subdirectory that tools/CMakeLists.txt adds.  Skipped by default so
# packagers don't pick up CLI tools they didn't ask for.
if(BUILD_TOOLS)
    add_subdirectory(tools)
endif()

option(BUILD_KWIN_EFFECT "Build KWin C++ effect plugin (needs KWin 6.7+; 6.7 and 6.8 both supported)" ON)
if(USE_KDE_FRAMEWORKS)
    if(BUILD_KWIN_EFFECT)
        add_subdirectory(kwin-effect)  # KWin C++ Effect (KDE-only)
    endif()
    add_subdirectory(kcm)          # KDE System Settings modules (KDE-only)
endif()

# Tests - enable with -DBUILD_TESTING=ON. The option is declared in the ROOT
# CMakeLists before KDECMakeSettings, which is what makes the OFF default
# win; declaring it here would be a no-op, because include(CTest) has
# already locked BUILD_TESTING into the cache by the time this file runs.
# This block just gates the test subdirectory.
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# Install D-Bus interface XMLs (one per interface). Keep in sync with
# dbus/*.xml on disk, third-party clients (and qdbus-qt6 introspection)
# resolve interfaces against these files post-install.
install(FILES
    dbus/org.phosphor_works.Placement.Autotile.xml
    dbus/org.phosphor_works.Placement.CompositorBridge.xml
    dbus/org.phosphor_works.Placement.Control.xml
    dbus/org.phosphor_works.PlasmaZones.Editor.xml
    dbus/org.phosphor_works.Placement.LayoutRegistry.xml
    dbus/org.phosphor_works.Placement.Overlay.xml
    dbus/org.phosphor_works.Placement.Rules.xml
    dbus/org.phosphor_works.Placement.Screen.xml
    dbus/org.phosphor_works.Placement.Scrolling.xml
    dbus/org.phosphor_works.Placement.Settings.xml
    dbus/org.phosphor_works.Facet.xml
    dbus/org.phosphor_works.Placement.Shader.xml
    dbus/org.phosphor_works.Placement.Snap.xml
    dbus/org.phosphor_works.Placement.Tiling.xml
    dbus/org.phosphor_works.Placement.WindowDrag.xml
    dbus/org.phosphor_works.Placement.WindowTracking.xml
    dbus/org.phosphor_works.Placement.ZoneDetection.xml
    DESTINATION ${KDE_INSTALL_DBUSINTERFACEDIR}
)

# Install default layouts
install(DIRECTORY data/layouts/
    DESTINATION ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SUITE_DIRECTORY_NAME}/layouts
    FILES_MATCHING PATTERN "*.json"
)

# PlasmaZones' host descriptor for the placement settings API. It declares
# that on KDE the placement settings live in PlasmaZones' config file, for a
# settings module that looks the host up through PhosphorConfig::HostDirectory,
# which reads this directory whether or not plasmazonesd is running. Installed
# as a directory with the *.json filter, like the data rules around it, so the
# descriptors at its top level that ship are the ones
# scripts/validate-json-schemas.py checks and HostDirectory reads.
install(DIRECTORY data/placement-hosts/
    DESTINATION ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SUITE_DIRECTORY_NAME}/placement-hosts
    FILES_MATCHING PATTERN "*.json"
)

# Install bundled scrolling templates
install(DIRECTORY data/scrolling-templates/
    DESTINATION ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SUITE_DIRECTORY_NAME}/scrolling-templates
    FILES_MATCHING PATTERN "*.json"
)

# Settings embeds whatsnew.json as a Qt resource at build time, so nothing in
# this repo reads the installed copy. It ships anyway as a packaging
# convenience: the release notes a distro or a downstream tool wants to quote
# are here in one file, rather than inside the settings binary's resource
# bundle.
install(FILES data/whatsnew.json
    DESTINATION ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SUITE_DIRECTORY_NAME}/${PLASMAZONES_APP_DIRECTORY_NAME}
)

# Install shared named curves. Profiles reference these by name
# (e.g. `widget-out`) so a single edit to a curve file
# retunes every profile that uses it, and users can drop their own
# JSON under ~/.local/share/phosphor-works/curves/ to override a shipped
# curve system-wide. The daemon's CurveLoader scans this path first, so
# every later consumer resolves curve names against the merged set.
install(DIRECTORY data/curves/
    DESTINATION ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SUITE_DIRECTORY_NAME}/curves
    FILES_MATCHING PATTERN "*.json"
)

# Install bundled RenderNode shaders (raw GLSL, no compilation needed).
# Ship the whole pack tree so every asset a pack's metadata references
# travels with it, and exclude only hidden editor/OS/dev files (.DS_Store,
# .luaurc-style linter configs, VCS dotfiles). An extension allowlist here
# silently dropped any new asset type, or any JSON not named metadata.json.
install(DIRECTORY data/overlays/
    DESTINATION ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SUITE_DIRECTORY_NAME}/shaders/overlays
    PATTERN ".*" EXCLUDE
    # plasmazones-shader-validate --emit-preamble writes this
    # gitignored editor aid for ALL FOUR pack families, not just the
    # two that already excluded it. It carries no SPDX header, so
    # installing one after a local emit ships an unheadered file.
    PATTERN "p_generated.glsl" EXCLUDE
)

# Install bundled animation transition shader packs. The daemon's
# AnimationShaderRegistry scans ${XDG_DATA_DIRS}/plasmazones/animations
# at startup; user overrides at ~/.local/share/plasmazones/animations win.
# Ship the whole pack tree (metadata, shaders, the shared/ GLSL includes,
# and any texture a pack references) and exclude only hidden dev/OS files.
install(DIRECTORY data/animations/
    DESTINATION ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SUITE_DIRECTORY_NAME}/shaders/animations
    PATTERN ".*" EXCLUDE
    # plasmazones-shader-validate --emit-preamble writes this
    # gitignored editor aid for ALL FOUR pack families, not just the
    # two that already excluded it. It carries no SPDX header, so
    # installing one after a local emit ships an unheadered file.
    PATTERN "p_generated.glsl" EXCLUDE
)

# Install the surface decoration packs (window and shell surface decoration;
# the third shader-pack category alongside shaders + animations) from the
# pinned phosphor-surface-packs tree, to
# ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SURFACE_PACKS_DATA_SUBDIR}. Every process
# that draws them discovers that subdir under ${XDG_DATA_DIRS}; user packs in
# ~/.local/share/${PHOSPHOR_SURFACE_PACKS_DATA_SUBDIR} win.
phosphor_install_surface_packs()

# Install bundled pointer shader packs (pointer trails, halos, click ripples;
# the fourth shader-pack family). The kwin-effect and the settings preview
# discover ${XDG_DATA_DIRS}/plasmazones/pointer via the PointerShaderRegistry;
# user overrides at ~/.local/share/plasmazones/pointer win.
# Ship the whole pack tree (metadata, shaders, the shared/ GLSL includes,
# and any texture a pack references) and exclude only hidden dev/OS files.
install(DIRECTORY data/pointer/
    DESTINATION ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SUITE_DIRECTORY_NAME}/shaders/pointer
    PATTERN ".*" EXCLUDE
    PATTERN "p_generated.glsl" EXCLUDE
)

# Install bundled scripted tiling algorithms
install(DIRECTORY data/algorithms/
    DESTINATION ${KDE_INSTALL_DATADIR}/${PHOSPHOR_SUITE_DIRECTORY_NAME}/algorithms
    FILES_MATCHING
    PATTERN "*.luau"
)

# Install support report script
install(PROGRAMS scripts/plasmazones-report.sh
    DESTINATION ${KDE_INSTALL_BINDIR}
    RENAME plasmazones-report
)

# Icons come from the pinned branding release (cmake/PlasmaZonesDependencies.cmake):
# scalable and 16 to 512 px, in hicolor for dark themes and hicolor-light for
# light ones. Each installs under its app's ID, which is also its .desktop and
# AppStream id.
plasmazones_fetch_dependency(branding)
include(${branding_SOURCE_DIR}/cmake/PhosphorBranding.cmake)
phosphor_install_brand_icons(
    org.phosphor_works.PlasmaZones plasmazones
    org.phosphor_works.PlasmaZones.Editor plasmazones-editor
    org.phosphor_works.Facet plasmazones-settings
)

