<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Dynamic per-monitor workspaces

This page tells how dynamic per-monitor workspaces behave, and which user-visible policy decisions apply. It is for
developers and for people who answer support questions. The feature came in PR #990.

## Model

Each monitor has an ordered list of virtual desktops, its "workspaces", on top of the one shared desktop pool of
KWin. The ownership map of the daemon (`phosphor-engine-libs/libs/phosphor-workspaces`) decides which monitor owns
which desktop. KWin decides which desktops exist.

- When a window goes onto the last empty workspace of a monitor, the monitor gets a new workspace at the end.
- When a workspace in the middle of the list becomes empty, the daemon removes it after the monitor leaves it. This
  does not apply to a named workspace or to a workspace that waits for its monitor to come back.

Named workspaces (Settings → Workspaces → Named Workspaces) stay while they are empty, and you can pin each one to
an output. Each named workspace has a persistent id. Thus a rename on that page or in KWin keeps the same desktop,
and the quick slots and "Open on workspace" rules that point at it follow the rename. A desktop that PlasmaZones
treats as unnamed gets a label from its position ("Desktop 3"). The label changes when a desktop is created or
removed earlier in the list.

When you disconnect a monitor, its workspaces go to a monitor that stays connected. When you connect the monitor
again, they go back. A workspace that is away keeps the mark of its home monitor, and the empty-workspace cleanup
does not remove it. Thus it comes back complete, also when it was empty.

A desktop created outside PlasmaZones (the desktop count in System Settings, the add button of the Pager, a script)
first goes into the list of the focused monitor. After KWin settles, these rules apply:

| Desktop | Result |
| --- | --- |
| Exactly one other monitor shows it | It moves to the list of that monitor. |
| A monitor shows it | It stays. |
| It is empty and no monitor shows it | The daemon removes it again, the same as niri removes a stray empty workspace. |

Thus such a desktop stays only when a window is on it or a monitor shows it.

The daemon keeps its runtime state in `~/.local/state/phosphor-works/plasmazones/`:

| File | Contents |
| --- | --- |
| `workspaces.json` | The ownership map. Monitors get their workspaces back after a restart. |
| `kwin-shortcut-backup.ini` | The stock KWin key combinations that the feature takes while it is on. The daemon restores them from this file when you disable the feature. |

If you rename or delete a named workspace while the daemon is stopped, its old key combination does not stay
registered. At start, the daemon removes each PlasmaZones shortcut record that is not one of its fixed shortcuts.
Then it binds the key combinations of the named workspaces as they are now.

## Requirements and consent

The feature needs the per-output virtual desktops of KWin (`PerOutputVirtualDesktops` in kwinrc, Plasma 6.7).

1. If that key is off when you enable the feature, PlasmaZones asks for consent.
2. After you give consent, PlasmaZones writes the key and reconfigures KWin.

KWin applies the key immediately. PlasmaZones asks for consent once. If the key is turned off again later, from
System Settings or by hand, PlasmaZones takes that as your choice. The feature becomes dormant, and the Behavior page
shows the consent banner again. PlasmaZones does not write the key again. It never writes the key without consent,
and it never reverts the key when you disable the feature.

The consent applies to one machine. A settings profile or a full settings import does not carry it. Thus no machine
gets kwinrc written because of consent given on another machine. The takeover setting (see "The Pager and the stock
desktop navigation") does go with profiles.

The feature needs KDE Frameworks, because KConfig reads and writes kwinrc. The portable `-DUSE_KDE_FRAMEWORKS=OFF`
build does not include the feature.

The Behavior and Named Workspaces pages show in the simple mode of the settings app, so you can find the enable
switch there. Quick Shortcuts shows only in the advanced mode.

## Window rules

"Open on workspace" sends a window to a named workspace, on the monitor where that workspace is now. The rule keeps
the id of the workspace and finds the workspace by that id. When no workspace has that id, the rule uses the stored
name. When the rule applies, it has priority over "Open on monitor" in the same rule set, because the workspace
already sets the monitor.

"Open on desktop" still takes a desktop number. With dynamic workspaces on, those numbers change as workspaces come
and go. Thus the rule editor tells you to use the named-workspace action.

## The Pager and the stock desktop navigation

By policy, PlasmaZones does not support the Pager and the stock desktop navigation with this feature. The Pager and
the stock Overview of Plasma show the full desktop pool of KWin and do not know which monitor owns which desktop.
They show the workspaces of all monitors as one flat list. The current-desktop filter of the Task Manager already
follows the desktop that each monitor shows.

If you use them to switch to a workspace of another monitor, the owner-wins snap-back occurs. The monitor that
switched goes back to one of its own workspaces and shows an OSD hint. You can turn off the hint in Settings →
Workspaces → Behavior. Hide the Pager widget when you use dynamic workspaces. A desktop added from the Pager follows
the rule for desktops created outside PlasmaZones (see "Model"). It stays only when a window is on it or a monitor
shows it. The daemon removes an empty one that no monitor shows.

The stock desktop-step shortcuts of KWin also go through the global pool. These are the four "Switch One Desktop"
actions (Up, Down, to the Left, to the Right) and the two "Walk Through Desktops" actions (forward and Reverse).

- While the feature is on, PlasmaZones takes these shortcuts by default. It makes a backup of them and restores them
  when you disable the feature. You can turn the takeover on and off in Settings → Workspaces → Behavior.
- With the takeover on, the PlasmaZones workspace shortcuts (`Meta+Ctrl+Up/Down` and the related shortcuts) go only
  through the workspace list of the current monitor.
- With the takeover off, the stock key combinations stay with KWin. If the PlasmaZones focus pair is on those stock
  key combinations, PlasmaZones parks the pair. Thus the global switch of KWin continues to work, and the cheat sheet
  does not show a binding that never operates.

Two more stock paths go through the global pool, and PlasmaZones does not take them:

| Path | Behavior | Replacement |
| --- | --- | --- |
| "Switch to Desktop N" of KWin (`Meta+F1` to `Meta+F4` and `Ctrl+F1` to `Ctrl+F4` by default) | It switches the active monitor to global desktop N. That desktop usually belongs to another monitor, so a snap-back occurs. | The focus slots on Settings → Workspaces → Quick Shortcuts. Slot N focuses the Nth workspace in the list of the active monitor. The slots have no binding by default. |
| Touchpad and touchscreen desktop-switch gestures | They step through the global grid of KWin. They are gestures, not global shortcuts, so the takeover cannot change them. A step onto a workspace of another monitor snaps back. | None |

## Own workspaces

A window can get a workspace of its own while it is fullscreen, maximized or borderless. This is useful for games
and full-screen programs. Three switches on Settings → Workspaces → Behavior control this. All three are off by default.

| Switch | The window that gets its own workspace |
| --- | --- |
| Fullscreen windows get their own workspace | A window that goes fullscreen. A windowed fullscreen that the scrolling strip shows in its column does not count. |
| Maximized windows get their own workspace | A window that you maximize. A window in a monocle stack, which the tiling layout maximizes, does not count. |
| Borderless windows get their own workspace | A window with no title bar whose frame covers its whole screen, such as a borderless game. |

When a window enters one of these states:

1. PlasmaZones moves the window to a new workspace directly after the workspace that it was on.
2. The monitor shows the new workspace. There is no switch animation and no OSD.
3. The window keeps its state there. A maximized window stays maximized.

When the window leaves the state, it goes back to its place on the workspace that it came from. In snapping it goes
back to its zone, in tiling to its position in the layout, and in scrolling to its column. The monitor then shows that
workspace again, and the empty workspace is removed. When the window closes on its own workspace, the monitor shows
the workspace that it came from, and the window that you used last there gets the focus.

A new window that opens while the monitor shows an own workspace opens on the workspace that the owner came from. The
monitor follows it. When that window closes, the monitor goes back to the own workspace, and its window gets the focus.
Dialogs, notifications and windows that do not show in the task bar stay where they open.

PlasmaZones keeps the own workspaces when its service restarts. A window that leaves its state after a restart still
goes back to its place.

## Limits

KWin has a maximum number of virtual desktops (25 in KWin 6.8). The maximum applies to the lists of all monitors
together. At the maximum:

- When a window goes onto the last empty workspace of a monitor, the monitor gets no new workspace.
- An OSD shows "Workspace limit reached" one time for each occurrence.
- Settings → Workspaces → Behavior shows a warning while the limit applies.
- A named workspace that cannot be created waits until there is space.

- A window that goes fullscreen, or that you maximize, stays on its workspace. It does not get one of its own.

When any workspace becomes empty, there is space again. If KWin refuses to create a desktop below 25, PlasmaZones
learns the lower limit at runtime. It forgets that limit after a create succeeds.
