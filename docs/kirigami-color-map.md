<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Kirigami color rules

This page tells developers which Kirigami theme role to use when they paint QML in
`plasmazones/src/settings/qml`, `plasmazones/src/editor/qml` and `libs/phosphor-control` of phosphor-libs. The
shell libraries (`phosphor-shell*`, `phosphor-popout`, `phosphor-theme`) have their own token system, and these
rules do not apply to them. The rules apply to `phosphor-control`, because it paints the sidebar and footer of the
settings app with `Kirigami.Theme`.

## Why the rules exist

The app made most surfaces by putting `Kirigami.Theme.textColor` with a low alpha over the window background
(`Qt.rgba(textColor, 0.03–0.4)`). On a dark scheme, textColor is almost white. Thus every card, tile and inactive
zone showed as flat gray, and the theme colors made from the wallpaper could not reach them.

Two more errors occurred. The focus and hover ring code assumed that highlight, focus and hover have the same hue.
In the desktop scheme, they have different hues on purpose. The code also used the status colors (active, positive,
neutral) as decoration.

## Rules

| If the code paints… | Use | Never |
|---|---|---|
| a card/tile/list surface | `Kirigami.Theme.backgroundColor` / `alternateBackgroundColor` under `Kirigami.Theme.colorSet: Kirigami.Theme.View` | `Qt.rgba(textColor, α)` |
| a subtle border / separator | `Kirigami.ColorUtils.linearInterpolation(backgroundColor, textColor, frameContrast)`. `Kirigami.Theme.separatorColor` does not exist (see "Known errors"). | `Qt.rgba(textColor, 0.08–0.3)` |
| a strong border / stroke | `alternateBackgroundColor` | `Qt.rgba(textColor, 0.9)` |
| a selection tint / selected fill | `highlightColor` (full or @α ≤ 0.25) | — |
| text on a highlight fill | `highlightedTextColor` | plain `textColor` |
| a keyboard-focus ring | `Kirigami.Theme.focusColor` | `highlightColor` |
| a hover affordance | `Kirigami.Theme.hoverColor` | `highlightColor` |
| de-emphasized text | `disabledTextColor` | `textColor` @ α |
| success / warning / error / attention **status** | `positive` / `neutral` / `negative` / `activeTextColor` | using these as badge/category decoration |
| a decorative badge/category tint | `textColor` or `highlightColor` @ α | status colors |
| brighten/darken a theme color | `Qt.lighter()` / `Qt.darker()` / `Qt.tint()` | per-channel `Qt.rgba(c.r*k, …)` |
| toolbars / page headers / sidebar chrome | declare `colorSet: Kirigami.Theme.Header` | inheriting Window silently |
| tooltips / toasts | `colorSet: Kirigami.Theme.Tooltip` | hand-inverted colors |
| an overlay scrim over arbitrary desktop content (compositor OSDs) | self-contained `Qt.rgba(0,0,0,α)` is sanctioned | textColor-derived scrims (invert on light themes) |

## KDE color sources

| Kirigami role | KDE source |
| --- | --- |
| `highlightColor` | `[Colors:Selection] BackgroundNormal` |
| `focusColor`/`hoverColor` | `DecorationFocus`/`DecorationHover` |
| `activeTextColor` | `ForegroundActive`. This means **attention**, not "active item". |
| Header colorSet | `[Colors:Header]` |

In the scheme made from the wallpaper, Selection and Header have the second hue of the wallpaper. Decoration* has
the primary hue, and active and positive have the tertiary accent. This is why the incorrect uses in the "Never"
column show in colors that look wrong.

## Examples in the tree

Copy these files:

| File | Correct use |
| --- | --- |
| `EasingPreview.qml` / `SpringPreview.qml` | `View` colorSet |
| `ShaderBrowserCard.qml`, `ParameterRow.qml` | `focusColor` |
| `DimensionTooltip.qml` | `Tooltip` colorSet |
| `UnsavedChangesFooter.qml` | `Window` pin with `inherit: false` |

`ParameterRow.qml` is in phosphor-components, and `UnsavedChangesFooter.qml` is in `libs/phosphor-control` of
phosphor-libs. The other files are in this repository.

## Known errors

**`Kirigami.Theme.separatorColor` is not a real property.** At runtime, each use of it evaluates to `undefined`. The
code once had 84 uses of it. A runtime probe against the installed Kirigami showed that it was the only property that
does not exist, of the 21 properties that the color fixes used. Use the replacement that Kirigami itself uses:
`Kirigami.ColorUtils.linearInterpolation(Kirigami.Theme.backgroundColor, Kirigami.Theme.textColor, Kirigami.Theme.frameContrast)`.
Before you add a theme API to these rules, make sure at runtime that it exists.

## Zone colors

**The zone-color pipeline overrides the QML theme.** Zone fills and borders are settings that the user can change.
The settings keep them as strings with a theme fallback:

- An EMPTY value (the default) means "follow the system palette".
- `Settings::resolvedSystemColor(SystemColorRole)` resolves the value when it is read
  (`libs/phosphor-engine-runtime/src/config/settings/systemcolors.cpp` in phosphor-engine-libs).
- Nothing writes a value back into the configuration when the palette changes.

The daemon sends the resolved colors to every overlay and popup slot in this order: zone-custom, then rule override,
then global (`overlay_data.cpp`, also in `libs/phosphor-engine-runtime` of phosphor-engine-libs). The QML defaults
(`ZoneColorDefaults`) apply only where the daemon does not send a color. Start all zone-color work at
`resolvedSystemColor`, not in QML.

The `applySystemColorScheme()` write path that came before the resolver is removed. It set the inactive fill and
border to `QPalette::Text` @ alpha, which is the same textColor error in C++. The resolver uses `AlternateBase` and
`Mid` for these roles.

**ZoneColorDefaults flavors:** zone cards in a panel (PopupFrame contents, settings previews) use the opaque
`preview*` flavor. Only zones drawn directly over the desktop use the alpha overlay flavor.

## Shared QML plugins in the settings app

The settings app links the shared QML plugins. Before, it included a copy of the shared module with a hand-written
file list and qmldir. The copy changed differently from the source and broke LayoutThumbnail→LayoutCard. The error
"X is not a type" occurred only in the settings app, and the daemon and the tests worked. Now the settings app links
the generated static plugins (`PhosphorComponentsQmlplugin`, `PhosphorZoneRenderingQmlplugin`) with
`Q_IMPORT_QML_PLUGIN`, the same as the daemon.

| Build rule | Reason |
| --- | --- |
| Put `Q_IMPORT_QML_PLUGIN` at FILE scope. | In an anonymous namespace, the extern symbol binds internally, and the link gives an undefined reference. |
| Exclude the importing .cpp from unity builds (`SKIP_UNITY_BUILD_INCLUSION`). | — |

## Troubleshooting

To debug the settings app past its single-instance guard, start it like this:
`dbus-run-session -- env QT_QPA_PLATFORM=offscreen ./build/bin/plasmazones-settings`.
