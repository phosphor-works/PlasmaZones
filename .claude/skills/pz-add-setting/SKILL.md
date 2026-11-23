---
name: "PZ Add Setting"
description: "Add, rename or remove a PlasmaZones setting across every file the value has to touch, in the store-backed shape the config actually uses. Use when: add a setting, new config option, new preference, expose this in settings, make this configurable, add a toggle."
---

<!--
SPDX-FileCopyrightText: 2026 fuddlesworth
SPDX-License-Identifier: GPL-3.0-or-later
-->

# PZ Add Setting

A setting is spread over six files. Getting five of six right produces a value
that reads back as its type-default with no error anywhere, so work down the
list and verify each one.

The six files are phosphor-engine-libs' since the settings model moved to its engine
runtime (`libs/phosphor-engine-runtime`, below `<lib>`), so a setting is a phosphor-engine-libs PR followed by a
pin bump in PlasmaZones. A setting only PlasmaZones means anything by (KWin's,
or plasmazonesd's own) is not one of these: it goes in PlasmaZones'
`plasmazones/src/common/hostsettings.cpp`, through the host settings extension.

CLAUDE.md summarises this; the worked example lives here. The pattern is
store-backed: the getter reads through `m_store` on every call, there is **no
member variable**, and there is **no load/save/reset arm** to write. Defaults
and clamping come from the schema.

## The six files

### 1. `<lib>/include/PhosphorEngineRuntime/ConfigDefaults<Area>.h` — the default value

`ConfigDefaults.h` is split by area: `Appearance`, `Gaps`, `Limits`,
`Screens`, `Scrolling`, `ScrollingBehavior`, `ScrollingShortcuts`,
`Shaders`, `Workspaces` (`ConfigDefaults<Area>.h`). Pick the matching one.

```cpp
static bool enableAudioVisualizer() { return false; }
static int  audioSpectrumBarCount() { return 64; }
// A clamped numeric setting also declares its bounds here, constexpr:
static constexpr int audioSpectrumBarCountMin() { return 16; }
static constexpr int audioSpectrumBarCountMax() { return 256; }
```

### 2. `<lib>/include/PhosphorEngineRuntime/ConfigKeys.h` — the group and key accessors

`ConfigKeys.h`, NOT `ConfigDefaults.h`. Every accessor lives there, or in
`ConfigKeysScrolling.h` for a scrolling group and `ConfigKeysWorkspaces.h`
for a workspaces group; ConfigDefaults.h declares none of them. The
`P_CONFIG_GROUP` / `P_CONFIG_KEY` macros come from `configkeymacro.h`, which
each of those headers includes at its top and undoes with
`configkeymacro_undef.h` at its end, so a new per-domain header follows the
same pair. They are reachable as `ConfigDefaults::fooGroup()` only because
ConfigDefaults inherits through that chain, so the CALL is always spelled
`ConfigDefaults::` even though the DECLARATION is not in that file.

Only if the group or key is new. Group names are v2 dot-paths mirroring the UI
hierarchy (`"Snapping.Behavior.ZoneSpan"`). Key accessors are generic
(`enabledKey()`, `triggersKey()`) because the group disambiguates them.

Never inline a config path as a `QStringLiteral`. `scripts/check-conventions.py`
fails the build on that.

### 3. `<lib>/src/config/settingsschema*.cpp` — register the key

This is the step that is easy to miss and the reason a setting silently reads
back as `false` or `0`: **the store gets its default and its type from the
schema**, not from the getter. Split across `settingsschema.cpp`,
`settingsschema_tiling.cpp`, `settingsschema_scrolling.cpp`,
`settingsschema_overlayshaders.cpp`, `settingsschema_shaderbounds.cpp`,
`settingsschema_shadertrees.cpp` and `settingsschema_workspaces.cpp`.

```cpp
schema.groups[CD::shadersAudioGroup()] = {
    {CD::enabledKey(), CD::enableAudioVisualizer(), QMetaType::Bool,
     QStringLiteral("Capture system audio so the audio-reactive shader packs "
                    "have a signal to follow. Off, those shaders render but stay still.")},
    {CD::barsKey(), CD::audioSpectrumBarCount(), QMetaType::Int,
     QStringLiteral("Number of frequency bands in the audio visualization."),
     clampInt(CD::audioSpectrumBarCountMin(), CD::audioSpectrumBarCountMax())},
};
```

The 4th field is a **user-facing description** that settings UIs and generated
docs surface, so it is held to the plain-prose rules: no em-dash splice, no
clause-splicing semicolon, no spaced hyphen. The conventions checker enforces
that. The 5th field is the coercion applied on **every read and every write**,
which is what makes the clamped-setter idiom below necessary.

### 4. `<lib>/include/PhosphorEngineRuntime/ISettings.h` — the signal

Add it under `Q_SIGNALS:`, past tense, named `<property>Changed`.

### 5. `<lib>/include/PhosphorEngineRuntime/Settings.h` — the property

```cpp
Q_PROPERTY(bool enableAudioVisualizer READ enableAudioVisualizer
           WRITE setEnableAudioVisualizer NOTIFY enableAudioVisualizerChanged)
...
bool enableAudioVisualizer() const override;
void setEnableAudioVisualizer(bool enable) override;
```

No member.

### 6. `<lib>/src/config/settings/<concern>.cpp` — getter and setter

Never `<lib>/src/config/settings.cpp`. Pick the file matching the concern (not exhaustive):
`setters.cpp`, `storescalars.cpp`, `shortcuts.cpp`, `scrolling.cpp`,
`triggers.cpp`, `perscreen.cpp`, `disable.cpp`, `uienums.cpp`,
`profiletrees.cpp`, `animationprofile.cpp`, `systemcolors.cpp`.

Note there are three files named `settings.cpp` across the two trees (`<lib>/src/config/`,
PlasmaZones' `plasmazones/src/runtime/overlay/` and `plasmazones/src/editor/controller/`). Always use full paths.

**Unclamped setting** — compare, early-return, write, emit:

```cpp
bool Settings::enableAudioVisualizer() const
{
    return m_store->read<bool>(ConfigDefaults::shadersAudioGroup(), ConfigDefaults::enabledKey());
}

void Settings::setEnableAudioVisualizer(bool enable)
{
    if (enableAudioVisualizer() == enable) {
        return;
    }
    m_store->write(ConfigDefaults::shadersAudioGroup(), ConfigDefaults::enabledKey(), enable);
    Q_EMIT enableAudioVisualizerChanged();
    Q_EMIT settingsChanged();
}
```

**Clamped setting** — write first, then compare, because the schema's coercion
runs on the write and the stored value may not be the value passed in. Comparing
before writing would suppress the signal on a value that actually changed, and
emit on one that did not:

```cpp
void Settings::setAudioSpectrumBarCount(int count)
{
    const int before = audioSpectrumBarCount();
    m_store->write(ConfigDefaults::shadersAudioGroup(), ConfigDefaults::barsKey(), count);
    if (audioSpectrumBarCount() == before) {
        return;
    }
    Q_EMIT audioSpectrumBarCountChanged();
    Q_EMIT settingsChanged();
}
```

Both `<property>Changed` and `settingsChanged` are emitted, and only when the
value actually changed.

## Editor.* settings (the layout editor's own)

These have no `Q_PROPERTY` on `Settings` (`EditorPageController` is their
property surface), so three more places carry each one:

- the `editorSettingEntries()` table in `<lib>/src/config/settings.cpp`,
  which is how a reload announces it (`test_settings_core`'s schema sweep
  fails for a key missing there);
- `EditorOwnedConfig` in `plasmazones/src/editor/helpers/`, the editor's own
  read and write (`test_editor_owned_config`'s sweep fails for a key missing
  there), plus the member and change gate in `EditorController`;
- the `"editor"` entry in `pageOwnedConfigKeys()`, as for any page.

## Renaming or removing a setting

No ad-hoc migration. Within a schema version, just use the new key; old values
are silently dropped and users get the default. Do not add a fallback read, do
not write an empty string to clear the old key, do not special-case the rename.

Only a `ConfigSchemaVersion` bump is different: exactly one migration function
in `configmigration.cpp` plus one `MigrationStep` registry entry, transforming
the whole JSON root and stamping the new `_version`.

## Persistence is sparse

A write whose value equals the default **deletes the key** rather than storing
it. A test that asserts "the key is present after setting it to the default
value" is asserting the opposite of the intended behaviour.

## Then

- Settings that must reach the daemon or the effect need the D-Bus side too.
- `cmake --build build --target update-ts` if you added translatable strings.
- `python3 scripts/check-conventions.py`
- Build and test via the `pz-build` skill.
