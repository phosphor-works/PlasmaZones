<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# PlasmaZones

PlasmaZones puts windows in place on KDE Plasma 6 on Wayland. One daemon gives three placement modes. You snap windows into
zones that you draw, an algorithm tiles them, or they scroll along an endless strip. Each monitor uses its own mode.

<p align="center">
  <img src="https://raw.githubusercontent.com/phosphor-works/branding/v0.1.0/svg/square/dark/plasmazones.svg" alt="PlasmaZones" width="96">
</p>

[![CI](https://github.com/phosphor-works/PlasmaZones/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/phosphor-works/PlasmaZones/actions/workflows/ci.yml)
[![GitHub release](https://img.shields.io/github/v/release/phosphor-works/PlasmaZones)](https://github.com/phosphor-works/PlasmaZones/releases/latest)
[![AUR](https://img.shields.io/aur/version/plasmazones-bin)](https://aur.archlinux.org/packages/plasmazones-bin)
[![COPR](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fcopr.fedorainfracloud.org%2Fapi_3%2Fpackage%3Fownername%3Dfuddlesworth%26projectname%3DPlasmaZones%26packagename%3Dplasmazones%26with_latest_succeeded_build%3Dtrue&query=%24.builds.latest_succeeded.source_package.version&label=COPR&color=blue)](https://copr.fedorainfracloud.org/coprs/fuddlesworth/PlasmaZones/package/plasmazones/)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPL%203.0-blue.svg)](LICENSE)
[![Wayland](https://img.shields.io/badge/Wayland-native-blue.svg)](https://wayland.freedesktop.org/)

## Features

Each monitor runs one mode. You choose the mode for each virtual desktop and each activity, so a widescreen
monitor can scroll while the laptop panel tiles. `Meta+Shift+T` cycles the focused screen through the enabled
modes. You can turn off each mode completely.

### Snapping

Hold **Alt** (or the modifier that you set) while you drag a window. The zones show. Drop the window into a zone,
and the window resizes to fill that zone.

<p align="center">
  <img src="docs/media/videos/snapping.gif" alt="Drag and Snap" />
</p>

Zones are named regions that you draw in a visual editor. The editor has drag-to-resize, snap-to-grid and mirroring.
PlasmaZones includes a library of layouts: grids, BSP, master and stack, fibonacci, and portrait, ultrawide and
super-ultrawide variants. After a snap, the remaining zones show as thumbnails, so you can put the next window with
one click. A window can span zones. From the keyboard, `Meta+Ctrl+1` through `Meta+Ctrl+9` snap a window with no
drag. The snapping mode is inspired by [FancyZones](https://learn.microsoft.com/en-us/windows/powertoys/fancyzones)
from PowerToys.

[Layouts gallery](https://phosphor-works.org/plasmazones/layouts/) ·
[Coming from FancyZones](https://phosphor-works.org/plasmazones/from-fancyzones/)

### Tiling

<p align="center">
  <img src="docs/media/videos/tiling.gif" alt="Automatic tiling" />
</p>

Windows put themselves in place when they open. You do not drag and you do not draw zones. The included algorithms
are written in Luau and run in a sandbox. They cover master and stack, BSP, columns, grid, spiral, monocle and
other families. You can write your own algorithm in Luau. The daemon reloads it from disk while you edit it.

[Autotile gallery](https://phosphor-works.org/plasmazones/autotile/) ·
[Authoring guide](https://phosphor-works.org/guides/tiling/)

### Scrolling

<p align="center">
  <img src="docs/media/videos/scrolling.gif" alt="Scrolling strip" />
</p>

The scrolling mode is modeled on the [niri](https://github.com/YaLTeR/niri) compositor. Windows make columns on an
endless strip, and the screen shows one part of the strip. When a window opens, the other windows keep their size.
The strip moves them to the side and scrolls the view.

A column cycles through width presets or takes a width that you give it. The windows in a column share it, or show
one at a time as tabs with an indicator that you can configure. A column can take a window from the column next to
it, or move a window out into its own column. A column can move to the center when you ask. The strip goes from
side to side or from top to bottom, to agree with the shape of the monitor. To move along the strip, hold **Meta**
and turn the mouse wheel. Templates set the first columns and the width presets of each screen.

A shortcut family on `Meta+Alt` controls the strip. See
[Keyboard shortcuts](https://phosphor-works.org/plasmazones/shortcuts/).

### More

- **Appearance**: GLSL shader overlays for zones (audio-reactive, procedural and distribution themes) with up to 4
  image textures each, window decoration packs, and animation packs for open, close, minimize and desktop
  switching. See the [shader gallery](https://phosphor-works.org/plasmazones/shaders/) and the
  [authoring guide](https://phosphor-works.org/guides/shaders/).
- **Scopes for each monitor, desktop and activity**: layouts, algorithms, templates and mode assignments have a
  scope. Virtual screens divide a physical monitor into independent logical workspaces with their own settings.
- **Window rules**: a rule matches on class, title, active layout, screen orientation and more. It then sets
  placement, floating, opacity, borders, animations, scroll speed and the behavior of each app in every mode.
- **Scripting**: a D-Bus API on `org.phosphor_works.Placement` covers every mode. See [D-Bus API](docs/DBUS.md).

## Install

On Arch Linux, install the prebuilt package from the AUR:

```bash
yay -S plasmazones-bin
```

On Fedora, install from COPR:

```bash
sudo dnf copr enable fuddlesworth/PlasmaZones && sudo dnf install plasmazones
```

After the install, enable the daemon:

```bash
systemctl --user enable --now plasmazones.service
kbuildsycoca6 --noincremental    # KDE only — refresh the service cache
```

For Fedora Atomic, Debian, Nix and NixOS, and for the requirements, see [Install](docs/INSTALL.md).

## Build

```bash
cmake -B build
cmake --build build -j3
sudo cmake --install build
```

The first configure downloads the pinned phosphor-libs and phosphor-engine-libs sources. For the tests and the
other build options, see [CONTRIBUTING.md](CONTRIBUTING.md) and the
[Install page](https://phosphor-works.org/plasmazones/#install).

## Documentation

- [phosphor-works.org/plasmazones](https://phosphor-works.org/plasmazones/) has the user documentation and screenshots.
- [docs/README.md](docs/README.md) lists the pages in this repository: install, use, troubleshooting and the D-Bus API.
- [Project Structure](https://github.com/phosphor-works/PlasmaZones/wiki/Project-Structure) on the wiki shows the
  directory tree and the data locations.
- For questions, ideas or bug reports, [join the Discord](https://discord.gg/9CQzAptdJ5) or open a
  [GitHub issue](https://github.com/phosphor-works/PlasmaZones/issues).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the code style, the license headers, the tests and the translations.

To support the development, use [Ko-fi](https://ko-fi.com/fuddlesworth) or
[GitHub Sponsors](https://github.com/sponsors/fuddlesworth).

## License

PlasmaZones is GPL-3.0-or-later. The license text is in [LICENSE](LICENSE).
