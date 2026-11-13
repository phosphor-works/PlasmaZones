<!-- SPDX-FileCopyrightText: 2026 fuddlesworth
     SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-workspaces

> Virtual desktop and activity tracking, plus the per-monitor
> dynamic-workspace model. KWin virtual desktops via D-Bus; KDE Activities
> via KActivities / PlasmaActivities.

## Responsibility

Tracks the active virtual desktop and current Activity, with change
signals on switch. It also owns the per-monitor dynamic-workspace model:
which KWin desktops each monitor owns, in what order, and the state
machine that creates, destroys and moves desktops to keep that model true.
It is independent of the Phosphor daemon, so consumers that need workspace
awareness link this library directly.

## Key types

| Type | Purpose |
|------|---------|
| `VirtualDesktopManager` | Tracks KWin virtual desktops via D-Bus — current desktop, count, names, UUID mapping |
| `ActivityManager` | Tracks KDE Activities via KActivities/PlasmaActivities (optional compile-time dep) |
| `WorkspaceMap` | Per-monitor workspace ownership: screen id to an ordered slice of KWin desktop ids, with a JSON form the daemon persists as its state file |
| `WorkspaceReconciler` | Dynamic-workspace lifecycle state machine over a `WorkspaceMap`: an echo-safe ledger of the requests it sent KWin, create-on-occupy, destroy-on-empty, hotplug migration and named workspaces |

## Typical use

```cpp
#include <PhosphorWorkspaces/VirtualDesktopManager.h>
#include <PhosphorWorkspaces/ActivityManager.h>

PhosphorWorkspaces::VirtualDesktopManager vdm;
vdm.init();
vdm.start();
connect(&vdm, &VirtualDesktopManager::currentDesktopChanged, [](int desktop) {
    // React to desktop switch
});

PhosphorWorkspaces::ActivityManager activities;
activities.init();
activities.start();
// activities.currentActivity() — empty string if KActivities unavailable
```

## Dependencies

- `QtCore`, `QtDBus`
- PhosphorEngine (`DesktopRemap`, which the reconciler uses to follow KWin
  renumbering a desktop removal causes)
- PhosphorIdentity (private, virtual-screen id parsing)
- KActivities / PlasmaActivities (optional, detected at CMake time;
  `ActivityManager::isAvailable()` returns false when absent and
  activity queries return empty results, while virtual desktop tracking
  works regardless).
