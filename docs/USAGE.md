<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Use

This page tells users how to start PlasmaZones after the install, which shortcuts it has by default, and where it
keeps its settings. For the full first-run tour, see
**[Getting started](https://phosphor-works.org/plasmazones/getting-started/)**.

## Start

1. Enable the daemon: `systemctl --user enable --now plasmazones.service`
2. Open the settings app: `plasmazones-settings`. On KDE, you can also open **System Settings → Apps → PlasmaZones**.
3. Make sure that **System Settings → Window Management → Desktop Effects → PlasmaZones** is selected.
4. Hold **Alt** and drag a window.
5. Drop the window into a zone.
6. To use the other modes, open **Overview** in the settings app and assign Tiling or Scrolling to a screen.

In step 3, the effect is enabled by default, so usually you only make sure. PlasmaZones runs as a KWin effect and
needs KWin with OpenGL compositing. With QPainter compositing, the effect entry is not available.

In step 4, the zones show. In step 5, the window snaps into the zone.

In step 6, you can also press `Meta+Shift+T`. It cycles the focused screen through the enabled modes.

## Shortcuts

| Action | Default |
|---|---|
| Open editor | `Meta+Shift+E` |
| Open settings | `Meta+Shift+P` |
| Cycle placement mode (snapping / tiling / scrolling) | `Meta+Shift+T` |
| Toggle floating | `Meta+F` |
| Restore window size | `Meta+Alt+Escape` |
| Snapping: snap window to zone 1–9 | `Meta+Ctrl+1` … `Meta+Ctrl+9` |
| Previous / next layout | `Meta+Alt+[` / `Meta+Alt+]` |
| Open layout picker | `Meta+Alt+Space` |
| Scrolling: consume / expel window | `Meta+Alt+I` / `Meta+Alt+Shift+I` |
| Scrolling: cycle column width forward / back | `Meta+Alt+PgUp` / `Meta+Alt+PgDown` |
| Scrolling: windowed fullscreen | `Meta+Alt+Shift+F` |
| Open shortcut cheatsheet | `Meta+Alt+/` |

The full reference has the core actions, zone movement, snap, layouts, autotile, scrolling, virtual screens and the
editor. See **[Keyboard shortcuts](https://phosphor-works.org/plasmazones/shortcuts/)**.

On KDE, you can change all bindings in **System Settings → Shortcuts → PlasmaZones**. The PlasmaZones settings app
changes only the shortcuts of the layout editor.

## Configuration

PlasmaZones keeps its data in these locations:

| Data | Location |
| --- | --- |
| Settings | `~/.config/phosphor-works/plasmazones/config.json` |
| Layouts | `~/.local/share/phosphor-works/layouts/` |

You change all settings in the settings app:

```bash
plasmazones-settings                           # overview
plasmazones-settings -p snapping-layouts       # jump straight to the snapping layout library
plasmazones-settings --page tiling-behavior
plasmazones-settings --page scrolling-templates
```

The app has a single instance. If you start it again while it runs, the open window comes to the front and shows
the page that you asked for.

## See also

- [Install](INSTALL.md)
- [Troubleshooting](TROUBLESHOOTING.md)
- [D-Bus API](DBUS.md)
