---
name: "PZ Verify Live"
description: "Verify a placement, tiling, scrolling or effect change against a real running compositor using the nested-KWin harness, instead of inferring it from a green test suite. Use when: does this actually work, verify the fix, test it live, reproduce the bug, check on a real desktop, multi-monitor repro, before claiming a fix works."
---

<!--
SPDX-FileCopyrightText: 2026 fuddlesworth
SPDX-License-Identifier: GPL-3.0-or-later
-->

# PZ Verify Live

A green suite is not evidence a placement fix works. The unit suite has never
caught a real engine bug on its own; what catches them is a live run plus
checking state invariants. Anything touching the snap, tile or scroll engines,
window tracking, or the KWin effect needs a live check before it is called done.

The nested harness exists because **the effect `.so` never reloads in a live
session**. Rebuilding it and expecting the running desktop to pick it up gives a
false negative every time. A nested compositor starts fresh on every launch, and
needs no logout.

## Start a session

```bash
scripts/nested-kwin/run-nested.sh [output-count] [width height] [scale]
```

- `width/height` apply to **every** virtual output; kwin_wayland's flags are
  global. `scale` is passed only to the visible window: the headless
  `--virtual` backend multiplies the MODE by `--scale` and keeps scale 1
  (1600x900 at 1.25 comes up 2000x1125), so the script leaves it off there and
  warns. Scale a headless output once the session is up, from a shell that
  sourced env.sh: `kscreen-doctor output.Virtual-0.scale.1.25`. The same
  command per output gives mixed scales, as does a seeded
  `kwinoutputconfig.json` in the nested config home.
- `PZ_NESTED_VISIBLE=1` runs it as a window on the host desktop instead of
  headless, so you can watch and drive it with pointer and keyboard. Defaults to
  1600x900 when no size is given.
- `PZ_NESTED_XWAYLAND=1` for X11 test clients.
- `PZ_NESTED_BUILD` selects a configure dir other than `build/`. `build/` is the
  `debug` preset, which turns the shell tier ON, so it is fine for shell work;
  reach for `build-nounity` when you also want the non-unity gate.
- `PZ_NESTED_KEEP_STATE=1` keeps the previous run's XDG homes instead of
  wiping them, including the nested `kwinrc` that carries the desktop count.
  The only way to probe session restore: `daemon.sh` restarts the daemon but
  not the effect, so a login-shaped restore needs a full nested restart with
  the `session.json` the daemon reads still on disk. No `PZ_NESTED_FORCE` is
  needed after the compositor stops: both live-session guards probe for a
  bound listener, so a socket file left behind does not block the next launch.
  After a terminal hangup the session's `dbus-daemon` can survive on its own;
  kill it by pid (`pgrep -a dbus-daemon`) rather than reaching for FORCE. The
  previous daemon log is rotated to `daemon.log.prev` for comparison.
- `PZ_NESTED_SESSION=<name>` runs a second, independent session: it suffixes
  both the default state dir (`pz-nested-<name>`) and the default socket
  (`pznested-<name>`), and every sibling script resolves the same dir from it.
- The repository's bundled data (layouts, algorithms, scrolling templates and
  every shader pack family) is served from `$NEST/repo-data`, a system-tier
  data dir ahead of the installed one, rebuilt from the checkout at every
  launch. A pack added on the branch is present without copying anything, and
  an edit to one is live at the next registry rescan. It overrides an installed
  asset of the same id but cannot hide one the branch deleted or renamed: that
  still loads under its old id from the install, so check a removal on a host
  without the install or by id in the logs.

State lives in `$PZ_NESTED_DIR` (default `$XDG_RUNTIME_DIR/pz-nested`, or
`$XDG_RUNTIME_DIR/pz-nested-<name>` with `PZ_NESTED_SESSION`). In every
follow-up shell:

```bash
. "$XDG_RUNTIME_DIR/pz-nested/env.sh"
```

**One run at a time per state directory.** Two sessions sharing a
`PZ_NESTED_DIR` evict each other's daemon and overwrite each other's pid file.
Concurrent sessions need a distinct `PZ_NESTED_SOCKET` *and* a distinct
`PZ_NESTED_DIR`; `PZ_NESTED_SESSION` gives both at once.

## Start the daemon

```bash
scripts/nested-kwin/daemon.sh
```

Runs the build-tree `plasmazonesd` inside the nested session with a known pid
and log. It never touches the host session's daemon.

Never `pkill -f plasmazonesd` — that kills the user's real desktop daemon.

## Inspect

```bash
scripts/nested-kwin/dump-windows.sh
```

Committed geometry and KWin output assignment for every normal window, read
from inside the compositor via a throwaway KWin script. **Committed geometry is
the ground truth for boundary bugs; screenshots are not.**

```bash
scripts/nested-kwin/capture-output.py <OutputName> <out.png>
```

It uses ScreenShot2's `CaptureScreen`, which on KWin 6.7 includes compositor
effects (verified with a shader-only border), so decorations and effect
transforms can show in the pixels. Committed geometry still comes from
`dump-windows.sh`, because those pixels can include effect transforms. The PNG
is in device pixels; the script prints the output scale to convert against
logical geometry. Needs `dbus-python` and Pillow.

## Reading the logs

The daemon log is the only log file; the compositor's output stays on the
terminal that launched it. For the host session:

```bash
journalctl --user -f -u plasma-kwin_wayland.service
```

Set `QT_FORCE_STDERR_LOGGING=1` when a log looks empty. **An empty log section
is evidence, not an absence of it**: a silent `lcEffect` where a code path
should have logged means that path never ran, which is usually the actual
finding.

## What counts as verified

- The invariant you claimed to fix, checked in `dump-windows.sh` output, before
  and after the change.
- A proxy check that cannot fail is not verification. If the assertion would
  pass on the unfixed build too, it proves nothing.
- Confirm the change is in the binary you are running. A stale
  `/usr/share/plasmazones/*.qml` shadows the worktree copy, and a stale installed
  effect shadows the build tree.

## Host session, not nested

Only the host session can show real multi-monitor hardware, fractional scale on
real outputs, and real application behaviour. The cost is that effect changes
need a full logout to load. For effect work, iterate in the nested session and
confirm once on the host.
