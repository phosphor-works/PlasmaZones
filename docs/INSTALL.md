<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Install

This page tells users how to install PlasmaZones from a package on each distribution, and what the system must have.
For openSUSE Tumbleweed, the release tarball and a build from source, see the
**[Install page](https://phosphor-works.org/plasmazones/#install)**. That page also has the portable build
(`-DUSE_KDE_FRAMEWORKS=OFF`).

## Requirements

| Item | Requirement |
| --- | --- |
| Desktop | KDE Plasma 6.7+ on Wayland |
| Compositor | KWin with OpenGL compositing. The integration runs as a KWin effect. |
| Qt | 6.10+ |
| CMake | 3.23+ |
| Compiler | C++20 |
| Kirigami | Required for the settings app in every build |
| KDE Frameworks 6.26+ (the rest) | Optional. Gives the settings KCM and the KGlobalAccel shortcuts. |
| PlasmaActivities | Optional. Gives layouts for each activity. |

The portable build (`-DUSE_KDE_FRAMEWORKS=OFF`) does not use the optional framework dependencies.

## Arch Linux

Install the prebuilt package from the AUR:

```bash
yay -S plasmazones-bin
```

## Fedora

Install the package from COPR:

```bash
sudo dnf copr enable fuddlesworth/PlasmaZones && sudo dnf install plasmazones
```

## Fedora Atomic (Kinoite)

Layer the same COPR package. In the commands, replace 44 with your Fedora release.

```bash
sudo curl -fsSL -o /etc/yum.repos.d/plasmazones.repo https://copr.fedorainfracloud.org/coprs/fuddlesworth/PlasmaZones/repo/fedora-44/fuddlesworth-PlasmaZones-fedora-44.repo
rpm-ostree install plasmazones && systemctl reboot
```

## Debian testing and unstable

Add the apt repository and install the package:

```bash
sudo curl -fsSL https://download.opensuse.org/repositories/home:fuddlesworth/Debian_Unstable/Release.key -o /etc/apt/keyrings/plasmazones.asc
echo 'deb [signed-by=/etc/apt/keyrings/plasmazones.asc] https://download.opensuse.org/repositories/home:/fuddlesworth/Debian_Unstable/ /' | sudo tee /etc/apt/sources.list.d/plasmazones.list
sudo apt update && sudo apt install plasmazones
```

On Debian testing, replace `Debian_Unstable` with `Debian_Testing` in the two lines.

- `signed-by` limits the key to this repository. Thus the key cannot sign packages from the Debian archive.
- apt reads the armored key as it is. You do not need a `gpg --dearmor` step, and you do not install a tool first.
- The two URLs have different spellings, and both are correct. The repository line uses `home:/fuddlesworth/`
  and the key uses `home:fuddlesworth/`. This is how OBS arranges them.
- The packages are for x86_64 only.
- The same repository works on rolling distributions that come from Debian, for example PikaOS.

## NixOS

The NixOS module is the recommended method.

Add this line to your flake inputs:

```nix
plasmazones.url = "github:phosphor-works/PlasmaZones";
```

Import the module and enable it in your NixOS configuration:

```nix
imports = [ inputs.plasmazones.nixosModules.default ];
programs.plasmazones.enable = true;
```

Rebuild the system. Then enable the daemon and refresh the KDE service cache:

```bash
systemctl --user enable --now plasmazones.service
kbuildsycoca6 --noincremental
```

Log out and log in again.

## Nix profile (without NixOS)

```bash
nix profile install github:phosphor-works/PlasmaZones
```

> **Note:** This method pins the package to the nixpkgs of the flake. If the KWin of your system updates, the
> effect plugin can stop loading until you install the package again. The NixOS module does not have this problem,
> because it always builds against the KWin of your system.

## Enable the daemon

After you install a package, enable the daemon:

```bash
systemctl --user enable --now plasmazones.service
kbuildsycoca6 --noincremental    # KDE only — refresh the service cache
```

Then follow [Use](USAGE.md) to start.

## Limits

The release tarball is built on Arch. It runs only where KWin is the same as the KWin of Arch at the time of the
release, and where Qt is the same version or newer. Thus the tarball is not a method for Fedora, Debian or SteamOS.

## See also

- [Use](USAGE.md)
- [Troubleshooting](TROUBLESHOOTING.md)
