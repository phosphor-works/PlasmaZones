<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- BEGIN shared: tools/sync writes this block from workspace/sync/claude-header.md. Change it there. -->
# PlasmaZones

This file has two parts. The rules that every phosphor-works repository follows come first, from the conventions of
the workspace repository, imported below. The sections after them belong to this repository only: what it is, its
layout, how to build and test it, and its own rules, code, skills and agents, and links to the other repositories.
A section here does not repeat a convention. When a section and the conventions disagree, change one of them so
that they agree.

The Claude Code tooling comes from the plugins that `.claude/settings.json` enables and `workspace/tools/setup`
installs. The `phosphor-works` plugin has the command guard, the shared skills (`phosphor-works:build`, `debug`,
`code-audit`, `record-lesson`, `new-repo`), the shared reviewers, and the lesson index, which adds the lessons that
fit each prompt.

@../workspace/claude/conventions.md
<!-- END shared -->

## Overview

PlasmaZones: window snapping, tiling and scrolling for KDE Plasma. Qt6, KF6, Kirigami, C++20, Wayland-only.

PlasmaZones is the window placement app of the phosphor-works suite. It builds on phosphor-libs and
phosphor-engine-libs (see Other repositories). Its app IDs are:

| App ID | Program |
| --- | --- |
| `org.phosphor_works.PlasmaZones` | `plasmazonesd`, the daemon |
| `org.phosphor_works.PlasmaZones.Editor` | `plasmazones-editor`, the layout editor |
| `org.phosphor_works.Facet` | `plasmazones-settings`, the settings app |

Its D-Bus interfaces are under `org.phosphor_works.*` (for example `org.phosphor_works.Placement.Snap`).

### Placement modes
Three mutually exclusive modes. Each screen runs exactly one, resolved per (screen, desktop, activity):
- **Snapping** — drag a window with a modifier held, drop it into a user-drawn zone. Engine: `libs/phosphor-snap-engine` in phosphor-engine-libs. Artifacts: layouts (`data/layouts` in phosphor-engine-libs, user copies in `~/.local/share/phosphor-works/layouts/`).
- **Tiling** — windows place themselves via a scripted algorithm. Engine: `libs/phosphor-tile-engine` in phosphor-engine-libs, running Luau through `phosphor-tiles` / `phosphor-scripting`. Artifacts: algorithms (`data/algorithms/*.luau` in phosphor-engine-libs).
- **Scrolling** — windows form columns on an endless strip, modeled on niri. Engine: `libs/phosphor-scroll-engine` in phosphor-engine-libs. Artifacts: templates (`data/scrolling-templates` in phosphor-engine-libs).

The bundled layouts, algorithms and templates are phosphor-engine-libs' data, which that tree installs when PlasmaZones adds it. A change to one is a phosphor-engine-libs PR followed by a pin bump here. The tests read them through `P_ENGINE_DATA_DIR`, the schema gate and the `engine-data-prose` rule from the pinned tree (`PHOSPHOR_ENGINE_LIBS_DIR=<checkout>` for a local one), and CI's `luau-analyze` job checks the pinned algorithms.

Shared placement policy lives in `libs/phosphor-engine` in phosphor-engine-libs. A verdict from one mode never gates another — see the float-is-per-mode invariant, written up at `libs/phosphor-engine/include/PhosphorEngine/WindowPlacement.h` there (each engine keeps its own float slot and state, independent of the others). When adding a cross-cutting feature, check whether all three modes need an arm before calling it done.

### Links
- Documentation: https://phosphor-works.org/plasmazones/
- Repository: https://github.com/phosphor-works/PlasmaZones
- Issues: https://github.com/phosphor-works/PlasmaZones/issues

## Layout

- Source, tests and data live INSIDE a tier, not at the root. There is no
  top-level `/src`, `/tests`, `/config` or `/examples`:
  - `plasmazones/{src,tests,data}` for the app tier
- Use `/docs` for documentation and markdown files
- Use `/scripts` for repo-level checks and dev harnesses
- Do not put working files, text files or tests in the root folder.

The tree is one product tier plus repo-level support directories. The tier has its own `CMakeLists.txt` (entered from the root one). The libraries it builds on are the phosphor-libs repository (see Other repositories). This block is the authoritative layout.
```
plasmazones/             — PlasmaZones
  src/core/              — PlasmaZones' app-side helpers (the engine core is phosphor-engine-libs' engine runtime)
  src/daemon/            — plasmazonesd: the KWin host of phosphor-engine-libs' engine runtime, its host interfaces, bound to the session bus beside KWin and Plasma
  src/editor/            — Layout editor (zone layouts + scrolling templates)
  src/settings/          — Standalone settings app
  src/dbus/              — D-Bus adaptors (thin bindings of the runtime's services), its own plasmazones_dbus library
  src/config/            — PlasmaZones' settings labels (the settings model itself is phosphor-engine-libs' engine runtime)
  src/common/            — Cross-target helpers
  src/shaderpreview/     — Shader preview host
  src/shadervalidate/    — plasmazones-shader-validate pack validator
  kcm/                   — System Settings module
  kwin-effect/           — KWin effect (C++)
  tests/                 — Unit tests (Qt Test)
  tools/                 — Developer tools; built with -DBUILD_TOOLS=ON
  data/animations/       — Window animation shader packs
  data/overlays/         — Zone overlay shader packs
  data/pointer/          — Pointer shader packs
  data/curves/           — Animation easing curves
  data/placement-hosts/  — Host descriptor for the placement settings API
  data/schemas/          — Schemas for plasmazones-only documents (whatsnew)
cmake/                   — Shared CMake modules, including the pins (PlasmaZonesDependencies.cmake)
scripts/                 — Repo-level checks and dev harnesses
```
Not exhaustive: `scripts/`, `nix/` (the Nix recipe and modules that `flake.nix` imports) and `docs/` sit at the root; `plasmazones/translations/`, `plasmazones/dbus/` and `plasmazones/scripts/` (the installed support script) live inside their tiers. App icons are not in the tree: they install from the pinned branding release (`cmake/PlasmaZonesDependencies.cmake`). Neither are the surface decoration packs, which install from the pinned phosphor-surface-packs commit.

### Targets and build options
- CMake with `CMAKE_AUTOMOC/AUTORCC/AUTOUIC ON`
- `qt_add_qml_module()`: a QML file that is not listed gives a runtime "not a type" error
- `cmake -DUSE_KDE_FRAMEWORKS=ON` (default) or `OFF` for a portable build without the KWin effect, the KCM and dynamic workspaces (compiled out under the `USE_KDE_FRAMEWORKS` define). `OFF` is not Qt-only: the settings app requires KF6 `Kirigami` in both modes
- KF6 deps: `Kirigami` always (`plasmazones/src/settings/CMakeLists.txt`); when ON also `KCMUtils`, `GlobalAccel`, `ColorScheme` (the KWin effect's KColorScheme), `Config` (KConfig reads kwinrc and makes the consented `PerOutputVirtualDesktops` write for dynamic workspaces); optional in either mode: `Activities`, which only phosphor-engine-libs' `phosphor-workspaces` looks up and links
- Pluggable backends: `PhosphorConfig::IBackend`, `PhosphorShortcuts::IBackend`, `IWallpaperProvider`
- Standalone settings app (`plasmazones-settings`) + minimal KCM launcher

## Build and test

On macOS, use Docker (KDE/Qt6 deps are Linux-only):

```bash
# First build the image (once)
docker build -t plasmazones-build .

# Build + test (default runs ctest)
docker run --rm -v "$PWD":/src plasmazones-build

# Build, then run a command of your own in place of the default (shown here: the default itself)
docker run --rm -v "$PWD":/src plasmazones-build ctest --output-on-failure --no-tests=error
```

On Linux (native):

```bash
# Configure. BUILD_TESTING defaults to OFF, so a build dir configured without
# it produces NO tests and ctest then reports "No tests were found" — which
# reads like success. Pass it explicitly, or the suite silently runs nothing.
# BUILD_TOOLS=ON adds shader-render and friends.
#
# TEST-TIME DEPENDENCY: the shader_validate_animations, shader_validate_surface
# and shader_validate_pointer gates shell out to `glslangValidator` (or the newer
# `glslang`; either name works) to compile every animation, surface and pointer
# pack for the compositor's classic-GL branch, and HARD-FAIL when neither is on
# PATH rather than skipping. On x86-64 Linux the configure fetches the glslang
# release cmake/PlasmaZonesDependencies.cmake pins and puts it on those tests' PATH,
# so nothing needs installing. Anywhere else install your distro's glslang
# package before running ctest. Not needed with BUILD_TESTING=OFF.
#
# PINNED SOURCES: phosphor-libs, phosphor-engine-libs, the branding release and the surface packs
# are pinned archives in cmake/PlasmaZonesDependencies.cmake (version, URL,
# SHA-256), and the phosphor-libs tree pins its own Luau and jsoncons the same
# way. None of them
# is committed here. The first configure downloads them with CPM into
# .cache/cpm, which every build dir shares. An offline build passes
# -DPHOSPHOR_DEPS_ARCHIVES=<dir> holding the archives instead, and
# `cmake -DPHOSPHOR_DEPS_FETCH=<dir> -P cmake/PlasmaZonesDependencies.cmake`
# fills such a dir with all of them. The packaging recipes repeat each pin, and
# the dep-pins conventions rule fails when one drifts. To build against a local
# phosphor-libs checkout, add -DCPM_phosphor-libs_SOURCE=<dir>.
#
# SECOND TEST-TIME DEPENDENCY: `appstreamcli` (distro package `appstream`).
# ECM registers the `appstreamtest` test only when it finds appstreamcli AT
# CONFIGURE TIME, and otherwise just prints a STATUS line. On a machine
# without it, `ctest --test-dir build -R ^appstreamtest$ --no-tests=error`
# fails with "no tests were found" rather than skipping. Install appstream
# before you configure, or reconfigure after installing it.
cmake -B build -DBUILD_TESTING=ON

# Build
cmake --build build -j3   # 3 jobs, inside the memory cap: see phosphor-works:build

# Test
ctest --test-dir build --output-on-failure
```

### Presets
Configurations come from `CMakePresets.json`: `dev` (Debug) configures into `build/`, `release` into `build-release/`, `relwithdebinfo` into `build-relwithdebinfo/` and `asan` (ASan and UBSan at `-O1`) into `build-asan/`. Every preset turns the tests and the tools on, and each has a workflow preset: `cmake --workflow --preset dev` configures, builds and tests. CI configures from the `dev` preset in its Presets job, so a broken preset turns CI red. The `Makefile` keeps only what the presets do not cover: install, post-install, uninstall, run-editor, run-daemon and format.

Install is whole-tree: none of the install rules declares a `COMPONENT`. `DESTDIR=/tmp/stage cmake --install build` stages it without root. After a real install (the user runs it), `make post-install` refreshes the KDE service cache.

### Conventions checker

```bash
# Machine-checks the rules in this file that are decidable by
# inspection: SPDX headers, the GPL-3 app / LGPL-2.1 libs split,
# PhosphorI18n::tr() over i18n() in C++, ConfigDefaults:: accessors over inline
# config paths, `.pragma library` inside Qt's 128-byte window in QML .js
# libraries (see Code), the plain-prose rules on the user-facing strings it
# can reach, that debian/copyright of the recipes still declares every file's
# real license and copyright holders, that a chain-resolved shader param's
# DESCRIPTION reads identically on every pack that offers it (the twenty
# roundBottomCorners copies in the pinned surface packs; the check and its data
# live in scripts/conventions_shared_text.py), and that every packaging recipe repeats
# the archive pins of cmake/PlasmaZonesDependencies.cmake and of the pinned
# phosphor-libs tree exactly (the dep-pins
# rule, in scripts/conventions_dep_pins.py). Stdlib only, spread over eight
# files: the entry point plus conventions_dep5.py, conventions_dep_pins.py,
# conventions_packaging.py, conventions_prose.py, conventions_shared_text.py,
# conventions_selftest.py and conventions_selftest_rules.py.
#
# The release recipes are in plasmazones/ of the packaging repository. The
# dep5, dep-pins and recipe-prose rules read them from the checkout at
# PLASMAZONES_PACKAGING_DIR, or else from ../packaging. A whole-tree run without
# one reports a finding. The pre-commit run prints a note and skips them, and CI
# checks out phosphor-works/packaging (the branch of the same name when there
# is one) and runs them.
#
# It also enforces one PRECONDITION that is not a rule and that --rules cannot
# switch off: every path handed to a rule must be readable as text. An unreadable
# one gets a single `[unreadable]` finding naming the errno instead of being fed
# to the rules as an empty string, which used to make them report a false cause
# ("malformed JSON" on well-formed JSON, "missing SPDX" on a file that has one).
#
# The prose rule reaches data JSON, tr()/i18n(), settings-schema descriptions,
# .desktop, AppStream, the Nix recipe, .github/workflows/*.yml (through the
# pkgdesc arm), the recipes of the packaging repository (as recipe-prose), algorithm .luau, and CHANGELOG.md entry BODIES (the text after the
# `**Term**:` lead-in; the bold lead-in itself and the trailing reference link
# are skipped, and an indented sub-bullet is reached like any other entry). Four
# things stay review-only even where it does reach: an entry's bold title; the
# dramatic "Label: payload" colon; the rule-of-three triad and "not just X, but
# Y", none of which it tests; and a clause-splicing semicolon built from
# PAST-tense prose, because the verb list its semicolon arm matches on carries no
# past-tense lexical verbs.
# Also runs on pre-commit (staged files) and in CI (whole tree).
python3 scripts/check-conventions.py
python3 scripts/check-conventions.py --list-rules
```

### Tests
- Qt Test: `QTEST_MAIN`, `QCOMPARE`, `QVERIFY`
- Test behavior, not implementation; mock D-Bus for daemon tests
- Edge cases: empty zones, overlapping zones, invalid coordinates

## Rules

### General
- Atomic commits; don't commit build artifacts

### Licenses
- JSON has no comment syntax, so no `.json` file needs a header wherever it sits. The usual cases are `plasmazones/data/**/*.json`, and every `metadata.json` (the ones Qt and KWin plugins embed, such as `plasmazones/kwin-effect/metadata.json` and the plugin demo's `plugins/cpu-meter/metadata.json`, and test fixtures such as wallpaper package metadata) — never add a header to those, it makes the file invalid.
- License identifier depends on the tree:
  - **App tiers** (`plasmazones/**` except its `data/` trees, `scripts/**`): `GPL-3.0-or-later`
  - **Reusable libraries, including their own tests** (everything in phosphor-libs and phosphor-engine-libs): `LGPL-2.1-or-later`
  - A library's own `tests/` follow the library (LGPL), NOT the app-tier GPL rule: test code that links and ships inside an LGPL lib must not taint that lib's build tree with GPL. The GPL `plasmazones/tests/**` rule means only the top-level app test tree.
  - **Bundled animation and pointer shader packs** (`plasmazones/data/animations/**` and `plasmazones/data/pointer/**` shader source: `.frag`, `.vert`, `.glsl`): `LGPL-2.1-or-later` for PlasmaZones-original shaders, so a third-party pack or tool can build on them. The exception is incorporated upstream copyleft. A shader that copies verbatim or ports GPL-3.0 upstream code (the Burn-My-Windows ports and their `shared/bmw_compat.glsl` shim, the niri `honeycomb` port) MUST stay `GPL-3.0-or-later` and carries a second `SPDX-FileCopyrightText` crediting the upstream author, because PlasmaZones is not the copyright holder of those bodies and cannot relicense them. The license follows the incorporated content and is never a per-directory blanket, so a pack's `.frag` and `.vert` may legitimately differ (a GPL-derived `.frag` beside a PlasmaZones-original LGPL `.vert`). Ports of permissively-licensed upstreams such as the MIT gl-transitions `desktop-*` frags may be LGPL. Generated editor aids like `p_generated.glsl` are gitignored and carry no SPDX header. (`plasmazones/data/overlays/**` is currently GPL and has not been normalized, so follow the existing header in that tree. The surface packs carry their own REUSE licensing in phosphor-surface-packs.)
  - Rationale: the shell is GPL; libraries are LGPL so third-party plugins / tools can link them without inheriting GPL. Never "fix" a lib header to GPL-3 without understanding the split.

### User-facing text
The writing rules of the conventions apply to every surface a user reads: `description`/`name` fields in `plasmazones/data/**/*.json` (animation, shader and curve metadata) and in the bundled layouts and scrolling templates of phosphor-engine-libs, `plasmazones/data/whatsnew.json` highlights, the `description` fields of the bundled `.luau` algorithms (in phosphor-engine-libs, checked from here by `engine-data-prose`), `CHANGELOG.md` entries, the `.desktop` `Name`/`GenericName`/`Comment` fields, AppStream `.metainfo.xml` summaries and descriptions, packaging descriptions (pkgdesc / Summary / %description / Debian Description in `plasmazones/` of the packaging repository, through `recipe-prose`, and Nix meta in `nix/`), and every translatable string (`PhosphorI18n::tr()`, QML `i18n()`/`i18nc()`). App icons live in the branding repo, which checks their `<desc>` prose itself (`scripts/check.py`).

The surfaces above are the ones `check-conventions.py` reaches. The rest of the text goes through review. `README.md` and older documents still use em dashes as structure.

- A literal typographic separator between two nouns is acceptable (e.g. the `%1 — %2` Layout/Zone display format) and so are settings-path breadcrumbs (e.g. `Settings → Snapping`).

### No ad-hoc backwards compatibility
- NEVER add migration code for individual renamed keys or deprecated settings within the same schema version
- If a setting is renamed or restructured within a version, just use the new key — old values are silently dropped
- Users get the default value if their config doesn't have the current key; this is acceptable
- NEVER write empty strings to "clear" obsolete keys on save
- NEVER read from a fallback/legacy group when the primary group is empty
- Rationale: ad-hoc migration code is write-once, test-forever complexity that rots and never gets removed

### Schema version migrations (ConfigSchemaVersion bumps)
- Schema version migrations (`migrateV1ToV2`, etc.) are the ONE exception — each lives in its own `configmigration_v<N>.cpp`, named for the version it produces; the `MigrationStep` registry and `ensureJsonConfig()` are in `configmigration.cpp`
- Each version bump gets exactly one migration function + one `MigrationStep` registry entry
- Migration functions transform the entire JSON root in-place and stamp the new `_version`
- v1 group/key accessors in `configkeys.h` (prefixed `v1*`) exist ONLY for migration code readability
- The migration chain runs automatically via `ensureJsonConfig()` on startup
- NEVER add per-key fallback reads outside of migration functions — that's ad-hoc migration

### Security
- Validate user input at system boundaries
- Sanitize file paths to prevent directory traversal

### D-Bus
- The daemon adaptors are hand-written `QDBusAbstractAdaptor` subclasses in `plasmazones/src/dbus`, kept in sync with the installed `plasmazones/dbus/*.xml`; only the shell service libraries generate theirs with `qt6_add_dbus_adaptor()`
- `QDBusConnection::sessionBus()`; keep methods simple; `QVariantMap` for complex data

## Code

### C++ naming
- Struct POD fields: `camelCase` (no prefix) — Constants: `PascalCase` (class) / `UPPER_SNAKE` (global)
- Signals: past tense (`layoutChanged`) — Slots: action verb (`saveLayout`)

### C++ core rules
- `namespace PlasmaZones`. The engine runtime is `namespace PhosphorEngineRuntime` and lives in phosphor-engine-libs (`libs/phosphor-engine-runtime`, included as `<PhosphorEngineRuntime/X.h>`): the settings model, the core, the tracker and drag resolver, the services, the overlays and controllers, and the `EngineRuntime` hub plasmazonesd hosts. What stays in `core/` and `config/` is PlasmaZones' own (`supportreport`, `translationloader`, `singleinstanceservice`, `modifierutils`, `dbusvariantutils`, `settingsvaluelabels`, `version.h`), `namespace PlasmaZones`. PlasmaZones code sees the runtime unqualified through `common/engineruntimenamespace.h`; forward-declare a runtime type in `namespace PhosphorEngineRuntime`, never in `PlasmaZones` (that declares a different, incomplete type)
- `Q_OBJECT`, `Q_PROPERTY` with READ/WRITE/NOTIFY
- Forward declare in headers; group includes: own header → project → KDE → Qt
- `PLASMAZONES_EXPORT` on public API classes in `plasmazones/src/**`, where `plasmazones_shaderpreview` and `plasmazones_dbus` (the adaptors in `src/dbus`) each carry their own. A phosphor library uses its OWN `PHOSPHOR<LIB>_EXPORT`. Writing `PLASMAZONES_EXPORT` into a phosphor lib header names an undefined macro.
- Never copy QObjects.

### Qt6 string literals
- Use `QStringLiteral()` for MIME types too.
- NEVER use raw `"string"` with QString/QJsonObject. The top-level CMakeLists defines `QT_NO_CAST_FROM_ASCII`, so this is a compile error, not a convention

### QUuid convention
- `toString()` (with braces) everywhere — EXCEPT filesystem paths use `WithoutBraces`

### QML style
- Kirigami, QtQuick.Controls/Layouts
- Components/files: `PascalCase.qml` — IDs/props/functions: `camelCase`
- Use `Kirigami.Theme` for colors, `Kirigami.Units` for spacing — never hardcode
- Zone IDs (QUuid), never indices — `Accessible.name` on interactive elements
- A `PascalCase.js` library must declare `.pragma library` within the first 128
  BYTES. `qt_add_qml_module` scans only that far, and past it Qt emits an author
  warning claiming the file is re-evaluated per importing document. Machine-checked
  by the `js-pragma` rule.

### Architecture
- Service-oriented with DI via constructor (the editor's `ILayoutService`, `ZoneManager`, `SnappingService` are the reference shape)
- Placement runs in phosphor-engine-libs' engine runtime (`EngineRuntime`) behind the three engines above, hosted by the daemon; the KWin effect draws overlays, decorations, and tab indicators
- Business logic in C++, UI in QML; controllers bridge via `Q_PROPERTY`
- Zone IDs everywhere, never indices
- JSON persistence in `~/.local/share/phosphor-works/layouts/` with relative geometry (0.0–1.0)
- Wayland only (custom layer-shell QPA plugin for overlays); XWayland windows handled within Wayland session

### i18n
- C++: `PhosphorI18n::tr()` — NEVER `KLocalizedString`/`i18n()`/`i18nc()` in C++
- QML: `i18n()` / `i18nc()` (via `PhosphorLocalizedContext`)
- Extract: `cmake --build build --target update-ts`

### Settings architecture
- `ISettings` interface → `Settings` class → `PhosphorConfig::Store` over a pluggable `PhosphorConfig::IBackend` (default `JsonBackend`: `~/.config/phosphor-works/plasmazones/config.json`)
- `ConfigDefaults` for all default values; the old `.kcfg` schema files were removed from the repo
- PlasmaZones' own choices about KWin and its own process (the per-output desktops consent, the stock-shortcut takeover, the Rendering group's graphics API and GPU pin) are not engine settings: `plasmazones/src/common/hostsettings.{h,cpp}` declares them through the host settings extension (`HostSettingsExtension`, installed by `initPlasmaZonesHost()` at the top of every main), and `HostSettings` serves them (the daemon adds the ones the D-Bus settings registry serves with `SettingsService::registerHostSetting`). A new KWin-only or plasmazonesd-only setting goes there, not through the steps below
- Editor settings: separate, in `EditorController` (separate process)

### Adding a setting
Use the `plasmazones:add-setting` skill, which carries the full worked example. An engine setting lives in phosphor-engine-libs' engine runtime (`libs/phosphor-engine-runtime`, the settings model PlasmaZones links), so adding one is a phosphor-engine-libs PR followed by a pin bump here (see Other repositories). Summary, with paths in that library:

1. `include/PhosphorEngineRuntime/ConfigDefaults<Area>.h` — static default accessor (plus `constexpr` Min/Max for a clamped numeric). `ConfigDefaults.h` is split by area (`Appearance`, `Gaps`, `Limits`, `Screens`, `Scrolling`, `ScrollingBehavior`, `ScrollingShortcuts`, `Shaders`, `Workspaces`).
2. `include/PhosphorEngineRuntime/ConfigKeys.h` — group and `xxxKey()` accessors, if new (`ConfigKeysScrolling.h` for a scrolling group, `ConfigKeysWorkspaces.h` for a workspaces group). NOT `ConfigDefaults.h`, which declares none of them; the call is still spelled `ConfigDefaults::` because ConfigDefaults inherits the chain.
3. `src/config/settingsschema*.cpp` — register the `{key, default, QMetaType, description, coercion}` KeyDef in its group. **The store takes its default, type and clamping from the schema, not from the getter.** Skip this and the setting silently reads back as the type-default. The description field is user-facing prose and is held to the plain-prose rules (see User-facing text).
4. `include/PhosphorEngineRuntime/ISettings.h` — signal in ISettings.
5. `include/PhosphorEngineRuntime/Settings.h` — Q_PROPERTY + getter + setter declarations (`override`). **No member variable.**
6. The matching `src/config/settings/*.cpp` — store-backed getter (`m_store->read<T>(group, key)`) and setter. There is **no** load/save/reset arm to write; persistence goes through the store. Setters live in that directory split by concern (`setters.cpp`, `shortcuts.cpp`, `storescalars.cpp`, `scrolling.cpp`, `triggers.cpp`, `perscreen.cpp`, `disable.cpp`, `uienums.cpp`, and so on), NOT in `src/config/settings.cpp`.

A setting only PlasmaZones means anything by (KWin's, or plasmazonesd's own) is not an engine setting: it goes in `plasmazones/src/common/hostsettings.cpp` (see Settings architecture above), in this repository.

An unclamped setter compares, early-returns, writes, then emits. A **clamped** setter must write first and compare after, because the schema's coercion runs on the write and the stored value may differ from the value passed in.

### Config key strings
- ALL config group names and key strings MUST use `ConfigDefaults::` accessors — never inline `QStringLiteral("...")`
- Group accessors: `ConfigDefaults::snappingBehaviorGroup()`, key accessors: `ConfigDefaults::enabledKey()`
- v2 groups use dot-paths mirroring the UI hierarchy (e.g. `"Snapping.Behavior.ZoneSpan"`)
- Key accessors are generic (e.g. `enabledKey()`, `triggersKey()`) — the group context disambiguates

### Shortcuts
- `PhosphorShortcuts::IBackend` (KGlobalAccel / XDG Portal / D-Bus fallback) — never use KGlobalAccel directly
- Register via `ShortcutManager`; dynamic updates via settings signals

## Skills and agents

The skills and agents come from two plugins: `plasmazones` for PlasmaZones' own, `phosphor-works` for the shared ones. Invoke them rather than reconstructing the procedure:
- `phosphor-works:build`, then `plasmazones:build` for PlasmaZones' extras — configure, build and test. Carries the two `OFF`-by-default flags that make a suite silently run nothing, the warning-vs-error grep, the ctest D-Bus isolation, and the non-unity gate.
- `plasmazones:add-setting` — the six files a setting touches, in the store-backed shape.
- `plasmazones:verify-live` — nested-KWin harness for verifying placement and effect changes against a real compositor.
- `plasmazones:algorithm-api` — extend the Luau tiling-algorithm API (ctx inputs, outputs, pluau helpers, metadata and capabilities), including the keys `LuauTileAlgorithm` silently drops.
- `plasmazones:shader-pack` — write, change or preview one shader pack, or change the uniform contract packs read.
- `phosphor-works:code-audit` — audit-then-remediate loop: read-only Phase A passes, then one Phase B fix with confirmation rounds.
- `plasmazones:shader-theme` — build a cohesive shader theme.
- `phosphor-works:record-lesson` — put what a session learned where the next session will read it.

The `plasmazones` plugin's agents: `plasmazones:kwin-compositor-reviewer` and the shader-theme art and critique agents (`plasmazones:shader-art-director`, `plasmazones:shader-material-reviewer`, `plasmazones:shader-motion-reviewer`). The shared `phosphor-works` agents cover the rest and read PlasmaZones' notes for their domain (`plasmazones:review-notes`): the `cpp-qt`, `qml`, `wayland`, `build`, `config`, `shader` and `luau` reviewers the audit dispatches, `phosphor-works:crash-triage` (core or hang to mechanism), `phosphor-works:source-scout` (what a KWin or Qt API actually does, from the installed version's source) and `phosphor-works:perf-expert`. `phosphor-works:debug` is the debugging procedure.

The `phosphor-works` plugin's PreToolUse guard (`workspace/plugins/phosphor-works/hooks/guard/`, with PlasmaZones' rules in `repos/plasmazones.py`) blocks the commands these rules forbid: `sudo`, installs outside a `/tmp` stage, `pkill`/`killall`, D-Bus calls to PlasmaZones' names (`org.phosphor_works.*`) on the user's own session bus, restarting the user's KWin, plasmashell or daemon, force pushes, pushes to `main`, broad `git add`, and edits inside build trees. Run `python3 -I test_guard.py` there after changing it.

## Other repositories

The Phosphor libraries (rendering, layer-shell, animation, config, screens, rules, ipc, ...) live in their own repository, [phosphor-works/phosphor-libs](https://github.com/phosphor-works/phosphor-libs), checked out beside this one as `../phosphor-libs`. The placement engines, their models (zones, tiles, workspaces, placement) and the engine runtime live in [phosphor-works/phosphor-engine-libs](https://github.com/phosphor-works/phosphor-engine-libs), checked out as `../phosphor-engine-libs`, which builds on phosphor-libs. PlasmaZones builds both from source, each from the one commit `cmake/PlasmaZonesDependencies.cmake` pins, added with CPM as subprojects (`plasmazones_add_phosphor_libs()`, then `plasmazones_add_phosphor_engine_libs()`), and installs them with the package as before. Everything below about phosphor-libs applies to phosphor-engine-libs the same way: `-DCPM_phosphor-engine-libs_SOURCE=<dir>` for a local checkout, `PHOSPHOR_ENGINE_LIBS_DIR=<checkout>` for the scripts, `${phosphor-engine-libs_SOURCE_DIR}` in CMake.
- A change to a library is a phosphor-libs PR. Build PlasmaZones against your checkout of it with `-DCPM_phosphor-libs_SOURCE=../phosphor-libs` (an absolute path is safest), or against a branch with `-DPHOSPHOR_LIBS_GIT_REF=<ref>`.
- Once the library change is merged, bump the pin: the commit, URL, FILE and SHA-256 together, then the same archive in every recipe the dep-pins rule names. A bump can move the library tree's own Luau and jsoncons pins too, and the rule reports those. It also reports a recipe that still fetches an archive no pin names, which is what a bump that replaces a dependency leaves behind.
- The scripts that read library files without a build (the JSON schema gate, the dep-pins rule) find the pinned tree with `scripts/pinned_phosphor_libs.py`, which unpacks the pinned archive into `.cache/phosphor-libs/`. `PHOSPHOR_LIBS_DIR=<checkout>` points them at a local tree instead.
- In CMake the library tree is `${phosphor-libs_SOURCE_DIR}` and its build tree `${phosphor-libs_BINARY_DIR}`. Its shared helper modules (`PhosphorTestIsolation`, `PhosphorLibTesting`, `PhosphorTestPch`) are in `${PHOSPHOR_LIBS_CMAKE_DIR}`, which is also on `CMAKE_MODULE_PATH`.
- The surface decoration packs (borders, glows, shadows, blur and glass) are [phosphor-works/phosphor-surface-packs](https://github.com/phosphor-works/phosphor-surface-packs), shared with the other apps that draw them and checked out as `../phosphor-surface-packs`. PlasmaZones pins one commit of it in `cmake/PlasmaZonesDependencies.cmake`, fetches it like the branding release and installs its packs to `share/phosphor-works/shaders/surface` with its `cmake/PhosphorSurfacePacks.cmake`. User packs go in `~/.local/share/phosphor-works/shaders/surface`. In CMake the packs are `${PHOSPHOR_SURFACE_PACKS_DIR}` and the install subdir `${PHOSPHOR_SURFACE_PACKS_DATA_SUBDIR}`, which each process hands to `PhosphorSurfaceShaders::setSurfacePackDataSubdir()` at startup. A pack change is a phosphor-surface-packs PR followed by a pin bump here, and `scripts/pinned_surface_packs.py` finds the pinned packs for the scripts that read them without a build (`PHOSPHOR_SURFACE_PACKS_DIR=<packs dir>` points them at a local checkout).
- The release recipes (Arch, Debian, RPM and OBS), the release workflow (`plasmazones-release.yml`) and the KWin rebuild watch (`plasmazones-rebuild-watch.yml`) live in [phosphor-works/packaging](https://github.com/phosphor-works/packaging), in `plasmazones/` and `.github/workflows/`, checked out as `../packaging`. A push of a `v*` tag here starts the release there through `.github/workflows/release-dispatch.yml`. The release builds this tree at the tag with the recipes of the packaging repository, and makes the GitHub release here. The Nix recipe and modules stay in `nix/` here, beside the flake. A change that touches a recipe and this tree is a change set: the same branch name in both repositories. CI here reads the packaging branch of the same name when there is one, and the packaging repository's CI reads the PlasmaZones branch of the same name.
