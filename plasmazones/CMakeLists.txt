# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PlasmaZones: the daemon, layout editor, settings app, KCM, KWin effect,
# developer tools, bundled data assets and the unit tests. Builds on the
# phosphor tier; nothing here links a shell-tier library.

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
    DESTINATION ${KDE_INSTALL_DATADIR}/plasmazones/layouts
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
    DESTINATION ${KDE_INSTALL_DATADIR}/phosphor-works/placement-hosts
    FILES_MATCHING PATTERN "*.json"
)

# Install bundled scrolling templates
install(DIRECTORY data/scrolling-templates/
    DESTINATION ${KDE_INSTALL_DATADIR}/plasmazones/scrolling-templates
    FILES_MATCHING PATTERN "*.json"
)

# Settings embeds whatsnew.json as a Qt resource at build time, so nothing in
# this repo reads the installed copy. It ships anyway as a packaging
# convenience: the release notes a distro or a downstream tool wants to quote
# are here in one file, rather than inside the settings binary's resource
# bundle.
install(FILES data/whatsnew.json
    DESTINATION ${KDE_INSTALL_DATADIR}/plasmazones
)

# Install shared named curves. Profiles reference these by name
# (e.g. `widget-out`) so a single edit to a curve file
# retunes every profile that uses it, and users can drop their own
# JSON under ~/.local/share/plasmazones/curves/ to override a shipped
# curve system-wide. The daemon's CurveLoader scans this path first, so
# every later consumer resolves curve names against the merged set.
install(DIRECTORY data/curves/
    DESTINATION ${KDE_INSTALL_DATADIR}/plasmazones/curves
    FILES_MATCHING PATTERN "*.json"
)

# The bundled overlay packs no longer ship preview.png card screenshots (the
# browser previews live shaders), but install(DIRECTORY) never removes a file
# dropped from the tree, and ShaderRegistry still adopts a preview.png it
# finds beside a pack's metadata.json into that pack's previewPath. A prefix
# upgraded in place would keep the old screenshots.
#
# This removes preview.png from the installed directories of the BUNDLED packs
# and from nowhere else. The pack list is taken at configure time: the source
# directories under data/overlays/ that hold a metadata.json. That leaves out
# shared/, and it leaves alone any pack a user or another package put under
# the installed overlays directory.
#
# The installed path is put together when the install runs, from the prefix
# and the DESTDIR in force then, so `cmake --install --prefix <other>` and a
# staged install each clean the tree they write and no other. An absolute
# KDE_INSTALL_DATADIR is used as it stands, the way install(DIRECTORY) treats
# an absolute DESTINATION.
#
# It is declared ABOVE the install(DIRECTORY) rule for the overlays on
# purpose. The install rules of one directory run in the order they are
# declared, so a preview.png that a bundled pack does ship is copied after
# this has run and survives it.
file(GLOB _pz_overlay_pack_metadata CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/data/overlays"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/overlays/*/metadata.json")
set(_pz_bundled_overlay_packs "")
foreach(_pz_overlay_metadata IN LISTS _pz_overlay_pack_metadata)
    get_filename_component(_pz_overlay_pack "${_pz_overlay_metadata}" DIRECTORY)
    # A dot-name is not bundled: the install(DIRECTORY) rule below excludes it.
    if(NOT _pz_overlay_pack MATCHES "^\\.")
        list(APPEND _pz_bundled_overlay_packs "${_pz_overlay_pack}")
    endif()
endforeach()
if(IS_ABSOLUTE "${KDE_INSTALL_DATADIR}")
    set(_pz_overlays_install_dir "${KDE_INSTALL_DATADIR}/plasmazones/overlays")
else()
    set(_pz_overlays_install_dir "\${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_DATADIR}/plasmazones/overlays")
endif()
install(CODE "
    set(_pz_bundled_overlay_packs \"${_pz_bundled_overlay_packs}\")
    foreach(_pz_overlay_pack IN LISTS _pz_bundled_overlay_packs)
        set(_pz_stale_overlay_preview \"\$ENV{DESTDIR}${_pz_overlays_install_dir}/\${_pz_overlay_pack}/preview.png\")
        if(EXISTS \"\${_pz_stale_overlay_preview}\")
            message(STATUS \"Removing stale overlay preview: \${_pz_stale_overlay_preview}\")
            file(REMOVE \"\${_pz_stale_overlay_preview}\")
        endif()
    endforeach()
")

# Install bundled RenderNode shaders (raw GLSL, no compilation needed).
# Ship the whole pack tree so every asset a pack's metadata references
# travels with it, and exclude only hidden editor/OS/dev files (.DS_Store,
# .luaurc-style linter configs, VCS dotfiles). An extension allowlist here
# silently dropped any new asset type, or any JSON not named metadata.json.
install(DIRECTORY data/overlays/
    DESTINATION ${KDE_INSTALL_DATADIR}/plasmazones/overlays
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
    DESTINATION ${KDE_INSTALL_DATADIR}/plasmazones/animations
    PATTERN ".*" EXCLUDE
    # plasmazones-shader-validate --emit-preamble writes this
    # gitignored editor aid for ALL FOUR pack families, not just the
    # two that already excluded it. It carries no SPDX header, so
    # installing one after a local emit ships an unheadered file.
    PATTERN "p_generated.glsl" EXCLUDE
)

# Install bundled surface shader packs (window and shell surface decoration;
# the third shader-pack category alongside shaders + animations). The kwin-effect
# discovers ${XDG_DATA_DIRS}/plasmazones/surface via the SurfaceShaderRegistry;
# user overrides at ~/.local/share/plasmazones/surface win.
# Ship the whole pack tree (metadata, shaders, the shared/ GLSL includes,
# and any texture a pack references) and exclude only hidden dev/OS files.
install(DIRECTORY data/surface/
    DESTINATION ${KDE_INSTALL_DATADIR}/plasmazones/surface
    PATTERN ".*" EXCLUDE
    PATTERN "p_generated.glsl" EXCLUDE
)

# Install bundled pointer shader packs (pointer trails, halos, click ripples;
# the fourth shader-pack family). The kwin-effect and the settings preview
# discover ${XDG_DATA_DIRS}/plasmazones/pointer via the PointerShaderRegistry;
# user overrides at ~/.local/share/plasmazones/pointer win.
# Ship the whole pack tree (metadata, shaders, the shared/ GLSL includes,
# and any texture a pack references) and exclude only hidden dev/OS files.
install(DIRECTORY data/pointer/
    DESTINATION ${KDE_INSTALL_DATADIR}/plasmazones/pointer
    PATTERN ".*" EXCLUDE
    PATTERN "p_generated.glsl" EXCLUDE
)

# Install bundled scripted tiling algorithms
install(DIRECTORY data/algorithms/
    DESTINATION ${KDE_INSTALL_DATADIR}/plasmazones/algorithms
    FILES_MATCHING
    PATTERN "*.luau"
)

# Install support report script
install(PROGRAMS scripts/plasmazones-report.sh
    DESTINATION ${KDE_INSTALL_BINDIR}
    RENAME plasmazones-report
)

# Icons come from the pinned branding release (cmake/PhosphorDependencies.cmake):
# scalable and 16 to 512 px, in hicolor for dark themes and hicolor-light for
# light ones. Each installs under its app's ID, which is also its .desktop and
# AppStream id. The shell's icon is installed by phosphor-shell/CMakeLists.txt.
phosphor_fetch_dependency(branding)
include(${branding_SOURCE_DIR}/cmake/PhosphorBranding.cmake)
phosphor_install_brand_icons(
    org.phosphor_works.PlasmaZones plasmazones
    org.phosphor_works.PlasmaZones.Editor plasmazones-editor
    org.phosphor_works.Facet plasmazones-settings
)

# Aggregate target for the moon workspace: `cmake --build build --target plasmazones-tier`.
include(PhosphorTier)
phosphor_tier_target(plasmazones-tier)

# The .qm catalogs are this tier's artifact, but the lrelease target cannot be
# collected automatically: cmake/PhosphorTranslations.cmake is include()d from
# the ROOT CMakeLists, so its targets belong to the root directory scope, and
# phosphor_tier_target() only walks from this directory down. Even relocating
# the include would not be enough — qt_add_lrelease() produces custom targets,
# which the collector skips by TYPE UTILITY. So attach it explicitly, or
# `moon run plasmazones:build` produces no catalogs and `moon run repo:install`
# then fails on install(FILES ${QM_FILES}) with nothing to install.
#
# Attached by the name cmake/PhosphorTranslations.cmake gives it, not by Qt's
# global `release_translations`. That global is created by qt_add_lrelease as
# an internal convenience and its name is not part of any contract we control,
# so keying the tier's artifact on it would silently stop working if Qt renamed
# it or the call ever passed NO_GLOBAL_TARGET.
#
# A missing target is a FATAL_ERROR rather than a silent skip, whichever way
# it went missing. The previous spelling keyed on Qt's internal
# `release_translations` inside a bare `if(TARGET ...)`, and when the name was
# wrong the guard simply did not fire: the catalogs dropped out of the tier,
# `moon run repo:install` was left to fail on install(FILES ${QM_FILES}), and
# nobody noticed, because a full `cmake --build` had already produced the .qm
# on this machine. A guard protecting an install rule must not fail open.
#
# The first elseif() is for cmake/PhosphorTranslations.cmake not having run by
# the time this file does, which is what moving its include() below
# add_subdirectory(plasmazones) in the root CMakeLists would cause. Neither the
# target nor the variables the second elseif() reads would exist here, so
# without this arm every condition would be false and the attach would be
# skipped without a word. `update-ts` is the witness, because that module
# declares it in every branch, whether or not lrelease is available.
#
# The second elseif() is for the module having called qt_add_lrelease (the
# condition is the one that call sits under) without producing the target name
# attached above.
#
# What falls through is the module having run and compiled no catalogs, for
# want of Qt6LinguistTools or of .ts files. There is nothing to attach then.
if(TARGET plasmazones_translations)
    add_dependencies(plasmazones-tier plasmazones_translations)
elseif(NOT TARGET update-ts)
    message(FATAL_ERROR
        "cmake/PhosphorTranslations.cmake has not run yet: its update-ts target "
        "does not exist. The root CMakeLists must include() it above "
        "add_subdirectory(plasmazones), or the .qm catalogs never join "
        "plasmazones-tier.")
elseif(Qt6LinguistTools_FOUND AND TRANSLATION_TS_FILES)
    message(FATAL_ERROR
        "plasmazones_translations target missing although lrelease ran. "
        "cmake/PhosphorTranslations.cmake must pass LRELEASE_TARGET; without it "
        "qt_add_lrelease names the target ${PROJECT_NAME}_lrelease and the .qm "
        "catalogs never join plasmazones-tier.")
endif()
