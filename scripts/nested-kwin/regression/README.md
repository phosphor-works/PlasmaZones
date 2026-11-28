<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Nested-KWin regression probes

The probes check placement, tiling, scrolling and effect fixes live, against a real compositor. Each probe sets up
a scene in a new nested KWin session (`../run-nested.sh`, `../daemon.sh`) and asserts the current behavior. Each
probe showed the defect on the old build and not on the build with the fix. The exceptions are the rows that the
table calls a control, a smoke check or a regression row. They guard a fix but do not tell the two builds apart.

## Use

```bash
scripts/nested-kwin/regression/run.sh --list                  # the probe table
scripts/nested-kwin/regression/run.sh f1151-touch-forgets-claim f1022-new-desktop-move
scripts/nested-kwin/regression/run.sh all                     # every probe, a few hours
scripts/nested-kwin/regression/run.sh --out DIR --build build-nounity all
```

- Each probe gets a new session, on socket `pzregress` and state dir `$XDG_RUNTIME_DIR/pz-regress-nest`. Thus an
  interactive nested session is not touched. `PZR_SETTLE` (default 15 s) is the pause between sessions.
- The logs go to `--out DIR` (default `$XDG_RUNTIME_DIR/pz-regression/<date>-<time>`): `NAME.out`, `NAME.kwin.log`,
  `NAME.daemon.log`, `NAME.session.log` and `summary.txt`.

The exit status of run.sh:

| Status | Meaning |
| --- | --- |
| 0 | All probes passed. |
| 1 | A probe failed. |
| 2 | No probe failed, but one or more could not set up their scene. |

## Requirements

- The build tree (`build/bin`), `qdbus6`, `gdbus`, `dbus-monitor` and `kscreen-doctor`.
- python3 with the GTK4 bindings (PyGObject).
- PySide6 for `f345-caption-push`, `f1154`, `f1156`, `f1157` (with its xcb platform plugin for `f1157`) and `f1182`.
- Xwayland for flag X.
- Flag F probes use `fakeinput/`. run.sh builds it into `<build>/regression-fakeinput` (Qt6 Gui and KWayland).

## Probe names and flags

The number in a probe name is the audit finding that the probe came from (F1151). For the later probes, it is the
issue that the probe covers (#1151, #1153, #1155). Thus two probes can have the same number.
f1151-touch-forgets-claim is audit finding F1151, and f1151-scroll-visit-back-to-zone is issue #1151. A probe named
with f and a word (fdrag, fresize, fscale, fsplash) has the name of the behavior that it covers.

The flags in the tables:

| Flag | Meaning |
| --- | --- |
| X | Xwayland |
| N | The permission checks of KWin are off. |
| F | The fakeinput tool (includes N) |
| S | Each output is 1600x900. |
| K | Keep the state home of the previous probe (`after=NAME` runs NAME first). |

A `kenv=KEY=VALUE` in a row goes into the environment of the nested compositor, where the effect reads it (for
example, the thumbnail trace switch). A plain `KEY=VALUE` goes into the environment of the probe.

## Results

A probe ends with `RESULT: PASS`, `RESULT: FAIL` or `RESULT: STAGING-FAILED`, and exits with 0, 1 or 2.

- STAGING-FAILED means that the scene did not hold (a window did not map, a race did not occur, the KWin scripting
  flake). The verdict does not count. Run the probe again.
- A dead nested daemon at the end is a FAIL.
- Some probes depend on timing (f1013, f1147, f1149, f625-rules-burst-refetch). By design, their scene setup fails
  from time to time.

## Write a probe

1. Copy `probes/f1022-new-desktop-move.sh`. It has the SPDX header and a comment with Runner, Staging and Asserts.
   It then has `NEST="$1"`, `LOG="$2"` and `. "$(dirname "$0")/../lib.sh"`.
2. Set up the scene with the lib helpers.
3. Gate the scene setup with `stage_fail`.
4. Assert with `check DESC ACTUAL OP WANT`.
5. End with `finish`.
6. Add a row to `TABLE` in run.sh.

`lib.sh` documents each helper. Signal the daemon only through its gated pid, never by name. The gated pid
(`dpid`) is the owner of org.phosphor_works.Placement on the nested bus, with comm plasmazonesd and a
dbus-run-session bus in its environment.

run.sh blocks D-Bus activation on the nested bus before it starts the daemon with `daemon.sh`, and keeps it blocked
until the session ends. If activation is not blocked, the effect can activate a second daemon. That daemon takes the
bus name first, writes its log into the output of the compositor and leaves `daemon.log` empty. lib.sh fails the
scene setup of a probe if the running daemon is not the one that `daemon.sh` started. `finish` fails a probe if its
daemon died or was replaced.

## Probes

| Probe | Outputs, flags | Asserts |
|---|---|---|
| f71-strip-counter-own-move | 2, XS | an X11 strip tile moved by `move_window_right` is never countered as an external move |
| f105-split-keeps-desktop-zones | 1 | splitting a monitor into virtual screens re-files memberships; a two-desktop window keeps its zone on both through split and un-split |
| f160-rule-after-daemon-move | 2, N | a monitor-scoped hide-title-bar rule applies right after a daemon move onto that monitor and lets go on the move back |
| f161-move-to-screen-redesk | 2, N | with per-output desktops, a daemon move onto another monitor re-desks the window onto the desktop that monitor shows |
| f310-overflow-float-half | 1, S | an overflow window floated back into vs:1 is announced there, counted on vs:1 only, keeps its free frame, and Meta+F tiles it on vs:1 |
| f345-caption-push | 1 | caption changes reach the registry (changed back while the daemon was down, first change to ""), and a Title rule follows retitles with no focus change |
| f379-tile-move-rule-verdict | 2, S | noBorder follows each landing of a tile moved between two bsp monitors |
| f488-tiled-not-snapped | 1, F | once the monitor goes autotile the isSnapped and zone rules stop matching a formerly snapped window, and FFM does not activate it |
| f552-copied-layout-zone-ids | 1, S | a copied or imported layout gets fresh zone ids, once, and a quick-layout round trip lands in the right zone |
| f573-no-dbus-errors-daemon-gone | 2, F | a drag across two monitors with the daemon gone logs no "D-Bus call failed" lines |
| f589-leg-ends-at-grab | 1, FS | a move grab, a resize grab or a maximize ends a running placement leg |
| f593-tiling-float-refloat | 1, F | float, drag-insert unfloat, float again: two float signals, the window ends unmaximized and active |
| f622-drag-redesk-held | 2, F | with per-output desktops, no desktop-membership carry during a drag onto another monitor; the free drop has no zone |
| f622-drag-desktop-switch | 1, F | control: a free drop after a mid-drag desktop switch lands on the new desktop with no zone |
| f625-restore-cache-refetch | 2, F | the effect refetches pending restores once per layout switch, desktop switch, setting change and output move, and reopened apps are not teleported |
| f625-rules-burst-refetch | 1 | ten rule edits in a burst refetch rules and pending restores once |
| f639-floating-evacuee-unplug | 2 | a floating evacuee never reads not-floating through the unplug, lands floating with no zone, and returns floating |
| f648-stash-forgets-departed | 2 | a strip window that left and came back gets the default width; sticky and multi-desktop windows keep theirs; a new window of the app is not given the departed one's |
| f653-cross-screen-leg-rule | 2 | a daemon move onto another monitor takes the placeIn timing from the rule scoped to the destination |
| f670-snap-float-evacuee | 2 | a snap float whose monitor goes away stays floating with no zone, returns floating, and the float shortcut still manages it |
| f677-plain-drop-floats-autotile | 2, F | a plain drag of a snapped window onto an autotile monitor floats there at its pre-snap size and leaves the tiles alone until Meta+F |
| f678-bus-float-engine | 2 | the bus setWindowFloating floats and unfloats through the owning engine (bsp and snap) and refuses a phantom id |
| f680-refused-maximize-counter | 1, X | a fixed-size X11 strip tile whose maximize KWin refuses stays unmaximized and is still countered back to its column |
| f685-evacuee-monocle-unmaximize | 2 | a returned Monocle evacuee un-maximized by the user floats and the next retile leaves it unmaximized |
| f699-tab-color-query | 1 | `Tiling.scrollTabColors` follows the window's keep-above and maximize state |
| f702-x11-alt-release-grab | 1, XF | releasing Alt mid-drag on X11 keeps the pointer inside the window's frame |
| f703-restore-cache-setting | 1 | with restore-on-login off the effect caches no pending restores and a reopened app is not put back in its zone |
| f713-drag-onto-tiling-floats | 2, F | dragging a snapped or free window onto a bsp monitor leaves its tiles alone mid-drag; the drop floats, Meta+F tiles |
| f718-arrival-move-back-x11 / -wayland | 2 XFS / 2 FS | a script move back off the autotile monitor after a daemon arrival is seen as a transfer and the remaining tile fills the monitor |
| f731-drag-between-tiling-monitors | 2, F | a tile dragged between bsp monitors is released from the old engine before it floats on the new one, with tiles untouched mid-drag |
| f736-virtual-screen-drag-tracked | 1, FS | after a drag onto vs:1, a script move back is a transfer and vs:1 drops the window |
| f738-strip-move-to-screen | 2, XF | KWin's Move to Screen of a Wayland and an X11 strip tile is a real crossing and transfer with no counter; scrolling causes none |
| f739-restart-keeps-records | 1 | window records survive a daemon restart and the bring-up sends sticky ahead of each window's metadata |
| f751-crossing-after-move-end | 2, F | with the daemon frozen through the drag's tail, the screen crossing is reported at the move's end, not before |
| f752-keyboard-move-leaves-zone | 1, F | a keyboard move accepted with Return leaves the zone at the free size; Escape puts the window back in its zone |
| f763-registry-follows-removal | 1 | removing a desktop below a snapped window renumbers its record; no release, its zone and frame kept on every switch |
| f763-snap-tile-follow-desktops | 1, S | snap zones and autotile tiles follow a desktop insert, swap and removal |
| f773-verbs-skip-dragged | 1, F | shortcuts and bus verbs leave a window being dragged alone |
| f780-hidden-float-back | 1, S | a float-back kept for a hidden window is re-stated once on first showing and a hand-moved window is left alone |
| f786-desktop-removal-renumber | 1 | removing a desktop renumbers per-desktop assignments, disabled-desktop settings and rules |
| f787-strips-follow-desktops | 1 | per-desktop strips follow a desktop insert, reorder and removal with no release |
| f787-stashed-strip-follows-insert | 1 | a stashed strip and its per-desktop assignment follow a front insert and rebuild when scrolling returns |
| f794-scroll-ffm | 1, FS | smoke check: scrolling focus-follows-mouse focuses the hovered column when on, and does nothing when off |
| f798-desktop-renumber-cache | 1 | after a desktop removal, switching to the renumbered desktop still reaches the daemon |
| f800-removed-desktop-retile | 1 | removing the desktop in view keeps its tiles tracked and tiled |
| f809-removal-while-down | 1 | desktops removed while the daemon is down are renumbered at its next start |
| f811-autotile-drag-return | 2, F | a drag off the autotile monitor and back is seen returning and floats there |
| f814-spots-follow-output | 2, S | floating spots follow an output that moves, and session.json stores them relative to the output |
| f814-login-relative-spot | 2, SFK | a login with the outputs rearranged restores the spot relative to its output (runs after f814-spots-follow-output) |
| f814-v1-migration-rescale | 2, S | the v1 to v2 session migration drops a gone monitor's spot; a rescaled output pulls the spot back onto it |
| f815-output-move-no-reassert | 2, XS | swapping output positions does not counter or reassert strip windows to the old origin |
| f884-drop-beats-late-restore | 1, F | a window dropped before its late restore reply stays where it was dropped |
| f884-reopen-restores-zone | 1, F | control: a reopen that nothing drags still restores into its zone |
| f911-min-size-rereported | 1, XS | an X11 client's grown minimum size is reported again after minimize and a float round trip, and it fits its column |
| f923-keep-above-frame | 1, FS | a kept-above window's moves reach the daemon and it floats back where it was moved |
| f963-minimized-reslot | 1, S | a layout switch re-slots minimized windows' pre-float zones; a window with no matching zone unminimizes floating |
| f973-strip-fullscreen-maximize | 1, X | strip fullscreen and maximize assert no fullscreen area, show no off-position frame and cause no counter |
| f989-strip-maximize-legs | 1, X | maximize and restore of a strip tile animate from the column (placeIn) and from the maximized frame (placeOut), Wayland and X11 |
| f991-late-tileable-adopt | 1 | clearing keep-above tiles the window without stealing focus; a minimized one is left alone |
| f1009-move-inside-settle | 3 | a Move to Screen inside the output settle is the user's move (verdict 4) and stays |
| f1011-monocle-arrival-unmaximize-wayland / -x11 | 2 / 2 X | a column-maximized arrival un-maximized on a Monocle monitor stays there, floating |
| f1012-disable-rule-renumber | 1 | minted disable-rule names follow a desktop removal; user-renamed rules keep their names |
| f1013-superseded-restore-ack | 2 | a stale ack of a superseded restore is not read as a user un-maximize; both Monocle members stay maximized |
| f1014-escape-monocle-drag | 1, F | an Escape-cancelled Monocle drag goes back into the stack, maximized and not floating |
| f1015-cancelled-drag-retile-monocle / -bsp / -scroll | 1, F | a cancelled engine drag retiles (or reapplies the strip rect) once and restores the tiled look; drops and pre-floated windows do not |
| f1017-parked-snap-gates-disabled / -control | 2 | a parked snap zone is not re-seated into a context disabled while away; the control re-seats it |
| f1018-carry-not-user-snap | 1 | a KWin desktop carry does not count as the user's last-used zone |
| f1022-new-desktop-move | 1 | a desktop created and entered in one KWin call is in range at once |
| f1028-parked-strip-float-inview / -hidden | 2 | a parked scrolling float returns floating at its spot, in and out of view, and the strip comes back whole |
| f1029-parked-overflow-float | 2 | a parked overflow float returns as overflow and tiles when the cap is raised |
| f1030-parked-ratio-returns | 2 | a master-stack context returned out of view keeps its tuned ratio |
| f1032-parked-script-bag | 2, F | aligned-grid tiles come back at their resized frames after the monitor returns |
| f1035-deleted-layout-minimized | 1 | a minimized window of a deleted layout unminimizes into the replacement layout's zone |
| f1041-cancelled-cross-drag | 2, F | a cross-monitor keyboard move KWin cancels keeps its zone and is not released |
| f1042-autotile-off-no-resnap | 1 | switching tiling off with nothing tiled resnaps nothing |
| f1046-minimized-evacuee-tile / -snap | 2 | a minimized tile readopted after its monitor returns unfloats into its tile; a minimized snap float evacuee is stated floating at the settle |
| f1048-bus-float-context | 1 | the bus setWindowFloating refuses a hidden-desktop window and a disabled context |
| f1056-stale-ack-move | 1 | a configure superseded by a same-size move is re-issued |
| f1057-vmax-split-move | 1, F | a vertically maximized window moved across a split monitor reports the half it is on |
| f1058-cancelled-suspended-tile | 2, F | a keyboard-cancelled drag of a tile suspended over a snapping monitor is retiled |
| f1060-restatement-keeps-maximize | 1 | a free-spot restatement keeps a maximize made at open |
| f1064-axis-fullscreen-restore-rect | 1 | axis maximize and fullscreen record the restore rect |
| f1071-grab-released-daemon-gone | 1, F | the drag grab is released when the daemon stops mid-drag |
| f1072-edge-maximize-not-cancel | 2, F | an edge maximize on another output is not a cancel |
| f1078-fullscreen-monocle-leaves | 1 | a fullscreen Monocle member moved to a snapping desktop gives its size back |
| f1092-desktop-rule-renumber | 1 | a desktop-only rule is switched off, not deleted, when its desktop goes; a later rule renumbers |
| f1093-context-layout-delete | 1 | deleting a context-assigned layout floats its windows back to their spots |
| f1097-hidden-strip-order | 2, F | an out-of-view strip comes back whole and in order after one evacuee is touched |
| f1099-hidden-return-seats-tiles | 2 | autotile windows returned out of view are seated and tile when shown; the parked float is kept |
| f1101-desktop-names-relay | 1 | desktop creates and renames are relayed as virtualDesktopNamesChanged |
| f1102-park-mode-switched-scroll / -tile | 2 | a parked context whose desktop was switched to snapping gets no re-seat back |
| f1105-hidden-evacuee-mode-tile / -snap | 2 | an evacuee on a hidden desktop floats in that desktop's own mode |
| f1108-bus-float-hidden-refused | 1 | a foreign setWindowFloatingForScreen on a hidden-desktop window is refused; the effect's own float is not |
| f1112-drag-off-removed-output-snap / -autotile | 2, F | regression rows: a drag held while its output goes away makes no screen-change call at the drop |
| f1114-fullscreen-no-float-back-plain / -fixed | 1 | a strip tile's fullscreen pushes no float-back |
| f1121-background-float-not-announced | 2 | a float on another desktop is not broadcast as floating while the window is a tile in view |
| f1122-tiling-off-snap-float | 2 | an evacuee on a hidden desktop assigned tiling while tiling is off is stated a snap float |
| f1123-bus-float-other-screen | 2 | a foreign setWindowFloatingForScreen naming another monitor is refused |
| f1129-held-drag-unanswered-drop | 3, F | a drag held across an output removal and dropped unanswered settles from where it was held |
| f1130-stale-unsnap-reaches-fallback | 1 | a closed window's unsnap notice reaches the same-app survivor its snap notice reached |
| f1134-hidden-float-lens | 2 | a snap float stated on a hidden tiling desktop with tiling off reads floating |
| f1137-parked-running-mode-snap / -tile | 2 | a parked snap zone is re-seated where the desktop runs snapping; parked tiles are not seated into disabled tiling |
| f1138-float-toggle-screen-hint | 2 | Snap.toggleFloatForWindow naming a monitor the window is not on is refused |
| f1139-readopt-keeps-claim | 2 | an evacuee's Monocle claim survives the park drop inside a Readopt and is taken back at the re-seat |
| f1140-stale-zone-notice-clear / -write | 1 | a stale id's zone notices neither clear nor overwrite the live window's own zone |
| f1141-parked-tab-offscreen | 1, S | on KWin 6.8 a hidden tab stays parked wholly below the output across tab switches, though each switch's acked configure clamps it back on screen |
| f1147-arrival-minimize-float | 2 | a minimized tile whose monitor leaves and returns gets its minimize-float back and tiles |
| f1149-overlapping-settle-reports | 4 | with two settle reports in flight, the second one's park drop waits for its own verdict |
| f1151-touch-forgets-claim | 2 | a touch after a Readopt that left a granted place forgets the Monocle claim, and the next batch does not take it back |
| f1151-scroll-visit-back-to-zone | 1 | a window on every desktop, snapped on a snapping desktop, goes back into its zone after a visit to a scrolling desktop |
| f1153-cropped-straddler-move | 2, S | regression row: KWin's Move to Screen of a scrolling column cropped at its output's edge still hands it to the neighbour's strip under the tracked-output test |
| f1154-straddle-peek | 1, S | a column parked on the memory of an earlier, wider answer while it straddles the screen edge peeks again once its window takes the park, after output scale 1.25 narrows the column below that memory |
| f1155-compositor-only-gate | 1 | the compositor-only D-Bus methods admit the KWin effect across a daemon restart, which keeps the strip as it was, and refuse another caller with AccessDenied |
| f1156-cross-floor | 1, S | a Wayland window that makes itself taller than its share of a two-tile column keeps that height and the tile under it gives way; one too tall to fit beside that tile floats and the tile takes the column back |
| f1157-x11-column-answer / -scaled | 1, XS | an X11 window that keeps resizing itself wider than its column is put back once, then learned, and its column widens to it; the -scaled row runs at output scale 1.25 and fails, saying so, when the frame never reads settled through rounding alone (F337) |
| f1170-shell-unmaps-after-drag | 1, F | the daemon's overlay shell, mapped while a snapping drag holds the trigger, is unmapped again 3 s after the drop |
| f1170-cheatsheet-focus-fsp | 1 | with focus-stealing prevention at Extreme, the cheatsheet's shell becomes the active window when its show maps the shell, and nothing stays mapped after it closes |
| f1182-user-maximize-borderless | 1 | with KWin's borderless maximized windows on, a tile the user maximizes stays maximized at the maximize area through the re-tile its decoration change causes, its restore lands back at the tile, and a monocle maximize still reports the minimum without the title bar |
| f1266-strip-fullscreen-keeps-column | 1 | a strip tile that goes fullscreen keeps its column, so the other columns keep their frames while it covers the output and after it leaves fullscreen (#1266) |
| f1301-resume-strip-returns | 2 | a scrolling strip whose monitor is dropped and returns, the shape of a sleep that drops the connection, comes back whole: no window floats, the order holds, and each window is back at its frame, the focused one in view (#1301) |
| f1313-desktop-move-follows | 1 | a move shortcut that takes a snapped window past the last zone to the next desktop also takes the view there, and the window keeps the focus (#1313) |
| fdrag-float-keeps-size | 1, FS | with column width and window height on "Window decides", a resized floating window drag-inserted into the strip with the trigger held is a column at its size on both axes (the drop indicator, the popup drop and the adopted arm are not staged) |
| fsplash-sibling-size-scroll / -tile / -snap | 1, S | a splash under the minimum window size that closes before its app's main window opens does not hand the main window its frame: on scrolling and tiling it is a column or a tile, on snapping it opens at its own size |
| fresize-centred-edge | 1, FS | under Center focused column Always, two Meta+right drags of a lone column's left edge each land that edge at the release and grow the column by twice the drag; with Center short columns on, a drag of its bottom edge up and one of its top edge down each land that edge at the release and shrink the column by twice the drag |
| fresize-then-width-verbs | 1, FS | after a Meta+right drag widens one of two columns and pushes the other part way past the edge, equalize puts the widened one back at the even split and brings the other back inside the output; the scrolling reset doing the same is the control |
| fmaximize-tile | 1 | a user maximize of a tile on a tiling screen is the tile engine's maximize to edges: the tile fills the maximize area, the other tiles stay, the maximize survives a retile, and the restore puts it back at a tile (workspace#28) |
| fmaximize-snap | 1 | regression row: a user maximize of a snapped window is KWin's maximize, the window keeps its zone while maximized, and the restore puts it back at the zone rect (workspace#28) |
| fown-fullscreen-snap / -tile / -scroll | 1 | with the FullscreenOwnWorkspace option on, a window that goes fullscreen moves to a new workspace that the monitor then shows, comes back to its frame when it leaves fullscreen while the emptied workspace goes, and a close there shows the origin with the window last active there focused (#1201) |
| fown-maximize-snap / -tile / -scroll | 1 | with the MaximizedOwnWorkspace option on, a window the user maximizes moves to a new workspace and stays maximized there with no un-maximize on the way, and its restore brings it back to its frame between the windows it was between (#1201) |
| fown-restart-snap / -tile / -scroll | 1 | an own workspace survives a daemon restart: the maximized window stays on it, maximized, and its restore after the restart brings it back to its place (#1201) |
| fown-borderless | 1 | with the BorderlessOwnWorkspace option on, a window with no title bar that covers the output moves to a new workspace, and comes back when it gets its title bar and frame back (#1201) |
| fown-visitor | 1 | a window that opens while the monitor shows an own workspace goes to the workspace the owner came from, and its close brings the monitor back to the owner (#1201, kde-fullscreen-spaces#3) |
| fclaim-release-PATH-MODE | 1 (2 for output) | each release path of the maximize-to-edges claim (PATH float, snap, flip, desktop, fullscreen, output; MODE tile or scroll) leaves KWin's maximize bit where the engine has the window, and the next maximize and restore move both together (workspace#28) |
| fmaximize-daemon-restart-tile / -scroll / -monocle | 1 | a window an engine maximized (a tile or a column maximized to the edges, or a monocle stack) keeps its maximize while the daemon is down and after it returns, with no un-maximize on the way, and a user restore after the restart still restores it |
| fscale-fixed-device-pixels | 1, S | at scale 1.25, a layout whose fixed zones count device pixels snaps a window to the zone divided by the scale; the same zones counted in logical pixels are the control |

## Follow-up probes

The header of each follow-up probe also tells what main (8ca183308) prints instead. Thus a run on the two builds
shows the change.

| Probe | Outputs, flags | Asserts |
|---|---|---|
| fu-optout-inview-evacuee | 2 | a snap float evacuated onto an in-view autotile:none desktop reads not floating, still not once that desktop switches to snapping, and floats again when its monitor returns |
| fu-optout-hidden-evacuee | 2 | an evacuee whose own desktop on the landing monitor is a hidden opt-out is not stated a float there, and reads not floating once shown and once that desktop switches to snapping |
| fu-optout-cross-screen-move | 2 | a floating window (snapped first, or never snapped) moved onto the opt-out by KWin lands not floating and is a free window back home |
| fu-optout-plain-drop | 2, F | a window dragged onto the opt-out with no trigger, from a snapping or a tiling monitor, lands not floating, and still reads not floating once that desktop switches to snapping |
| fu-optout-desktop-switched | 1 | switching a desktop to the opt-out drops its snap floats (in view at once, hidden when shown) and keeps zone snaps as memory |
| fu-optout-tiling-release-bsp / -scroll | 1 | a window a tiling engine releases into the opt-out does not get its old snap float back |
| fu-optout-float-verbs | 2 | the float and restore shortcuts, setWindowFloating and toggleFloatForWindow do nothing on the opt-out (a second output runs bsp, so the opt-out gate decides, not the engine-disabled one) |
| fu-optout-minimize | 1 | a minimize of a kept zone snap stores no float on the opt-out |
| fu-optout-restart | 1 | a float the opt-out dropped is not restored by a daemon restart |
| fu-optout2-drag-overlay | 2, F | regression row: a snap-trigger drag over the opt-out draws no zone overlay there, and the drop commits no zone |
| fu-optout2-zone-selector | 2, F | the drag zone selector stays hidden on the opt-out and still shows on a snapping screen |
| fu-optout2-snap-release | 1 | a snapping desktop switched to the opt-out announces its snapped windows unsnapped in place, and switching back resnaps them |
| fu-optout2-strip-release | 1 | a scrolling desktop switched to the opt-out leaves every strip window on screen, a free-spot window back at its spot |
| fu-dbusctx-thumbnail-admitted / -pixels | 1, F | the effect's snap-assist thumbnail pushes (dma-buf, or raw pixels) are admitted by the compositor trust gate |
| fu-dbusctx-spoof-refused | 1 | a thumbnail push from a shell process gets AccessDenied, with one refusal warning |
| fu-dbusctx-settings-errors | 1 | bad Settings requests get an InvalidArgs error at once |
| fu-dbusctx-drop-proxy-owner | 1, F | a drop proxy is retired when the process that registered it exits |

## Limits

- The probes do not set up step 3 of the optout outline. That step checks a screen_changed WindowStateEntry with
  floating false, and the struct dump of dbus-monitor cannot be parsed reliably for it. The probe asserts the log
  line and the float state instead.
- Part A of dbusctx (the kwin_wayland_wrapper of the Plasma session, and a KWin restart under it) needs the host
  session. It is not a nested probe.
