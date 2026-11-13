<!-- SPDX-FileCopyrightText: 2026 fuddlesworth
     SPDX-License-Identifier: GPL-3.0-or-later -->

# Dynamic per-monitor workspaces

Feature notes for the dynamic-workspaces layer from PR #990, which is not in a
release yet. The original design is in `docs/dynamic-workspaces-plan.md`, a
historical record with a list of where the shipped code drifted from it. This
file records the user-visible policy decisions a support answer needs.

## Model

Each monitor owns an ordered list of virtual desktops ("its workspaces") layered
over KWin's single shared desktop pool. The daemon's ownership map
(`phosphor-libs/libs/phosphor-workspaces`) is the authority on which monitor
owns which desktop, and KWin remains the authority on which desktops exist.
Occupying a monitor's last empty workspace appends a new one. A workspace
emptied in the middle of the list is removed once the monitor leaves it, unless
it is named or is waiting for its monitor to come back.

Named workspaces (Settings → Workspaces → Named Workspaces) persist while empty
and can be pinned to an output. Each one carries a persistent id, so a rename,
made on that page or in KWin itself, keeps the same desktop, and the quick
slots and "Open on workspace" rules that point at it follow the rename. A
desktop PlasmaZones treats as unnamed gets a positional label ("Desktop 3"),
which is refreshed when a desktop is created or removed earlier in the list.

On unplug, a monitor's workspaces foster onto a surviving screen and migrate
home on replug. A fostered workspace keeps its home stamp while it is away and
is exempt from empty-workspace cleanup, so it comes home intact even when it
was empty.

A desktop created outside PlasmaZones (System Settings' desktop count, the
Pager's add button, a script) joins the focused monitor's list first. Once KWin
settles, a desktop that exactly one other monitor is showing moves to that
monitor's list, and one a monitor is showing stays. An empty one that no
monitor is showing is removed again, the way niri drops a stray empty
workspace, so such a desktop lasts only once a window is on it or a monitor
shows it.

The daemon keeps its runtime state under `~/.local/state/plasmazones/plasmazonesd/`:

- `workspaces.json`, the ownership map, so monitors get their workspaces back
  after a restart.
- `kwin-shortcut-backup.ini`, the stock KWin chords taken over while the
  feature is on, restored from here on disable.

A named workspace renamed or deleted while the daemon was down does not leave
its old chord registered. At start the daemon drops every PlasmaZones shortcut
record that is not one of its fixed shortcuts, then binds the chords of the
named workspaces as they are now.

## Desktop limit

KWin caps the number of virtual desktops (25 in KWin 6.8), and the cap covers
every monitor's list together. At the cap, occupying a monitor's last empty
workspace no longer appends another, an OSD says "Workspace limit reached" once
per episode, and Settings → Workspaces → Behavior shows a warning while the
limit holds. A named workspace
that cannot be created at the cap waits until there is room. Emptying any
workspace makes room again. When KWin refuses creates below 25, PlasmaZones
learns the lower ceiling at runtime and forgets it once a create succeeds.

## Requirements and consent

The feature requires KWin's per-output virtual desktops
(`PerOutputVirtualDesktops` in kwinrc, Plasma 6.7). Enabling it with that off
asks for consent, then writes the key and reconfigures KWin, which applies it
immediately. Consent is asked once. If the key is turned off again later, from
System Settings or by hand, PlasmaZones takes that as the user's choice. The
feature goes dormant and the Behavior page offers the consent banner again
instead of rewriting the key. PlasmaZones never writes the key silently and
never reverts it when the feature is disabled.

The consent is machine-scoped. A settings profile or a full settings import
does not carry it, so a machine never gets kwinrc written on the strength of
consent given on another one. The takeover toggle below does ride profiles.

The feature needs KDE Frameworks (KConfig reads and writes kwinrc). The
portable `-DUSE_KDE_FRAMEWORKS=OFF` build compiles it out.

The Behavior and Named Workspaces pages show in the settings app's simple mode,
so the enable toggle is reachable there. Quick Shortcuts is advanced-only.

## Window rules

"Open on workspace" sends a window to a named workspace wherever that
workspace currently lives. The rule stores the workspace's id and resolves it by
id, falling back to the stored name when the id is gone. When it realizes, it
wins over "Open on monitor" in the same rule set, because the workspace already
decides the monitor. "Open on desktop" still takes a desktop number, and with
dynamic workspaces on those numbers shift as workspaces come and go, so the
rule editor points at the named-workspace action instead.

## The Pager and stock desktop navigation are unsupported-by-policy

Plasma's Pager and the stock Overview render KWin's whole desktop pool and know
nothing about per-monitor ownership. They will show every monitor's workspaces
as one flat list. The Task Manager's current-desktop filter already follows the
desktop each monitor shows. Switching to another
monitor's workspace through them triggers the owner-wins snap-back: the
switching monitor returns to a workspace of its own, with an OSD hint that
Settings → Workspaces → Behavior can turn off. Hide the Pager widget when using
dynamic workspaces. A desktop added from the Pager follows the external-desktop
rule in the Model section above: it survives only once a window is on it or a
monitor shows it, and an empty one nobody is looking at is removed again.

KWin's stock desktop-stepping shortcuts walk the same global pool: the four
"Switch One Desktop" actions (Up, Down, to the Left, to the Right) and the two
"Walk Through Desktops" actions (forward and Reverse). While the feature is on
they are taken over by default (backed up and restored on
disable, toggleable in Settings → Workspaces → Behavior). With the takeover on,
the PlasmaZones workspace shortcuts (`Meta+Ctrl+Up/Down` and friends) walk only
the current monitor's own workspace list. With the takeover off, the stock
chords stay KWin's, and PlasmaZones parks its own focus pair while it still
sits on those stock chords, so KWin's global switch keeps working and the
cheat sheet does not list a binding that would never fire.

Two more stock paths walk the global pool and are not taken over:

- KWin's "Switch to Desktop N" (`Meta+F1` to `Meta+F4` and `Ctrl+F1` to
  `Ctrl+F4` by default) switches the active monitor to global desktop N, which
  usually belongs to another monitor, so it snaps back. The per-monitor
  replacement is the focus slots on Settings → Workspaces → Quick Shortcuts,
  where slot N focuses the Nth workspace of the acting monitor's own list.
  They ship unbound.
- Touchpad and touchscreen desktop-switch gestures step KWin's global grid.
  They are gestures rather than global shortcuts, so the takeover cannot reach
  them, and a step onto another monitor's workspace snaps back.
