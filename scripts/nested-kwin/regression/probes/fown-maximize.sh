#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fown-maximize: a window the user maximizes gets a workspace of its own directly after the one it
# was on, stays maximized there, and goes back to its place when it is restored (#1201).
# Runner: 1 output, no flags. PZ_FOWN_MODE picks the placement mode of Virtual-0: snap (COLS3, the
#   default), tile (autotile:master-stack) or scroll (scrolling).
# Staging: KWin's per-output virtual desktops on, dynamic workspaces and the
#   Workspaces.Behavior MaximizedOwnWorkspace option on (config file and reloadSettings), three windows
#   a, b and c on desktop D0, b in the middle of the order, so a return to the end would show. Staging gate: both are on D0, Virtual-0 shows D0, and the daemon runs
#   dynamic workspaces.
# Asserts: b's maximize moves b to a new desktop that Virtual-0 then shows, b stays maximized there,
#   and a and c stay on D0. The restore moves b back to D0 at the frame it had before, shows D0, and the
#   emptied desktop goes.
#   KWin reports no un-maximize of b between its maximize and its restore.
# A FAIL means: the maximize edge did not open an own workspace, the move dropped the maximize, or
#   the restore did not bring b back to its place.
set -u
NEST="$1"
LOG="$2"
. "$(dirname "$0")/../lib.sh"
kwjs_ok
cur() { jsread CUR 'workspace.currentDesktopForScreen(workspace.screens[0]).x11DesktopNumber'; }
count() { jsread CNT 'workspace.desktops.length'; }
per_output_desktops
MODE="${PZ_FOWN_MODE:-snap}"
case "$MODE" in
    snap) LAYOUT="$COLS3" ;;
    tile) LAYOUT="autotile:master-stack" ;;
    scroll) LAYOUT="scrolling:" ;;
    *) stage_fail "unknown PZ_FOWN_MODE $MODE" ;;
esac
CFG="$XDG_CONFIG_HOME/phosphor-works/plasmazones/config.json"
python3 -I - "$CFG" <<'PYEOF'
import json, os, sys
path = sys.argv[1]
data = json.load(open(path)) if os.path.exists(path) else {}
behavior = data.setdefault("Workspaces", {}).setdefault("Behavior", {})
behavior["Enabled"] = True
behavior["MaximizedOwnWorkspace"] = True
os.makedirs(os.path.dirname(path), exist_ok=True)
json.dump(data, open(path, "w"), indent=4)
PYEOF
dmark
PZ org.phosphor_works.Placement.Settings.reloadSettings >/dev/null
sleep 3
dlog "dynamic workspaces active" | grep -q . || stage_fail "the daemon did not start dynamic workspaces"
setting snapAssistEnabled false
assign Virtual-0 "$LAYOUT"
sleep 1.5
open a
open b
open c
sleep 2
D0=$(deskof "$(geo b)")
B0=$(geo b)
C0=$(geo c)
# A strip's frames move with its view, so on scrolling the place is the column order.
place0() { if [ "$MODE" = scroll ]; then striporder Virtual-0; else echo "$(rect "$B0") | $(rect "$C0")"; fi; }
placenow() { if [ "$MODE" = scroll ]; then striporder Virtual-0; else echo "$(rect "$(geo b)") | $(rect "$(geo c)")"; fi; }
ids
P0=$(place0)
echo "places before: $P0"
echo "mode $MODE | a $(geo a) | b $B0 | Virtual-0 shows $(cur) | desktops $(count)"
[ "$(deskof "$(geo a)")" = "$D0" ] && [ "$(deskof "$(geo c)")" = "$D0" ] || stage_fail "a, b and c are not on one desktop"
[ "$(cur)" = "$D0" ] || stage_fail "Virtual-0 does not show the desktop of a and b"
N0=$(count)

# Every maximize change of b, so a drop during the move shows even when the end state is right.
HOOK="$NEST/fown-hook.js"
on_exit 'qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$HOOK" >/dev/null 2>&1; rm -f "$HOOK"'
printf '%s\n' 'const ws = workspace.windowList(); for (var i = 0; i < ws.length; i++) { if (ws[i].caption === "b") { const w = ws[i]; w.maximizedChanged.connect(function () { print("FOWNMAX " + w.maximizeMode); }); } }' > "$HOOK"
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$HOOK" >/dev/null
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
sleep 0.7

echo "== user maximize of b"
emark
dmark
on b 'w[i].setMaximize(true, true);'
sleep 4
M=$(geo b)
dlog "opening own workspace.*|closing own workspace.*" | sed 's/^/    /'
echo "  b $M | a $(geo a) | Virtual-0 shows $(cur) | desktops $(count)"
check "own workspace opened" "$(dlog 'opening own workspace' | wc -l)" -ge 1
check "b on another desktop" "$(deskof "$M")" != "$D0"
check "Virtual-0 shows b's desktop" "$(cur)" = "$(deskof "$M")"
check "b still maximized there" "$(maxof "$M")" = 3
check "a stays on D0" "$(deskof "$(geo a)")" = "$D0"
check "c stays on D0" "$(deskof "$(geo c)")" = "$D0"

echo "  maximize changes of b: $(elog 'FOWNMAX [0-9]+' | tr '\n' ' ')"
check "b never unmaximized during the move" "$(elog 'FOWNMAX 0' | wc -l)" -eq 0

echo "== user restore of b"
dmark
on b 'w[i].setMaximize(false, false);'
sleep 4
R=$(geo b)
dlog "closing own workspace.*" | sed 's/^/    /'
echo "  b $R | Virtual-0 shows $(cur) | desktops $(count)"
check "own workspace closed" "$(dlog 'closing own workspace' | wc -l)" -ge 1
check "b back on D0" "$(deskof "$R")" = "$D0"
check "Virtual-0 shows D0 again" "$(cur)" = "$D0"
check "b and c back at their places" "$(placenow)" = "$P0"
check "the emptied desktop is gone" "$(count)" -eq "$N0"
finish
