#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fown-fullscreen: a window that goes fullscreen gets a workspace of its own directly after the one
# it was on, and goes back to its place when it leaves fullscreen (#1201). Runner: 1 output, no
# flags. PZ_FOWN_MODE picks the placement mode of Virtual-0: snap (COLS3, the default), tile
# (autotile:master-stack) or scroll (scrolling).
# Staging: KWin's per-output virtual desktops on, dynamic workspaces and the
#   Workspaces.Behavior FullscreenOwnWorkspace option on (config file and reloadSettings),
#   three windows a, b and c on desktop D0, b in the middle. Staging gate: a and b are on D0,
#   Virtual-0 shows D0, and the daemon runs dynamic workspaces.
# Asserts: b's fullscreen moves b to a new desktop that Virtual-0 then shows, while a stays on D0. The
#   end of the fullscreen moves b back to D0 at the frame it had, shows D0, and the emptied desktop
#   goes, with c where it was. With a focused, fullscreen again and then closing b shows D0 again
#   with a, the window last active there, active.
# A FAIL means: the fullscreen edge did not open an own workspace, or its end did not come back.
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
python3 -I - "$CFG" <<'EOF'
import json, os, sys
path = sys.argv[1]
data = json.load(open(path)) if os.path.exists(path) else {}
behavior = data.setdefault("Workspaces", {}).setdefault("Behavior", {})
behavior["Enabled"] = True
behavior["FullscreenOwnWorkspace"] = True
os.makedirs(os.path.dirname(path), exist_ok=True)
json.dump(data, open(path, "w"), indent=4)
EOF
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
echo "a $(geo a) | b $(geo b) | Virtual-0 shows $(cur) | desktops $(count)"
[ "$(deskof "$(geo a)")" = "$D0" ] || stage_fail "a and b are not on one desktop"
[ "$(cur)" = "$D0" ] || stage_fail "Virtual-0 does not show the desktop of a and b"
N0=$(count)

echo "== b goes fullscreen"
dmark
on b 'w[i].fullScreen = true;'
sleep 3
F=$(geo b)
dlog "opening own workspace.*" | sed 's/^/    /'
echo "  b $F | a $(geo a) | Virtual-0 shows $(cur) | desktops $(count)"
check "own workspace opened" "$(dlog 'opening own workspace' | wc -l)" -ge 1
check "b on another desktop" "$(deskof "$F")" != "$D0"
check "Virtual-0 shows b's desktop" "$(cur)" = "$(deskof "$F")"
check "a stays on D0" "$(deskof "$(geo a)")" = "$D0"
check "b still fullscreen" "$(wprop b 'w.fullScreen')" = true

echo "== b leaves fullscreen"
dmark
on b 'w[i].fullScreen = false;'
sleep 4
R=$(geo b)
dlog "closing own workspace.*" | sed 's/^/    /'
echo "  b $R | Virtual-0 shows $(cur) | desktops $(count)"
check "own workspace closed" "$(dlog 'closing own workspace' | wc -l)" -ge 1
check "b back on D0" "$(deskof "$R")" = "$D0"
check "Virtual-0 shows D0 again" "$(cur)" = "$D0"
check "the emptied desktop is gone" "$(count)" -eq "$N0"
check "b back at its place" "$(rect "$R")" = "$(rect "$B0")"
check "c back at its place" "$(rect "$(geo c)")" = "$(rect "$C0")"

echo "== a takes focus, b goes fullscreen again, then b closes"
activate a
sleep 0.5
on b 'w[i].fullScreen = true;'
sleep 3
check "b on another desktop again" "$(deskof "$(geo b)")" != "$D0"
activate b
sleep 0.5
dmark
on b 'w[i].closeWindow();'
sleep 3
echo "  Virtual-0 shows $(cur) | active $(activecap) | desktops $(count)"
check "Virtual-0 shows D0 after the close" "$(cur)" = "$D0"
check "a active after the close" "$(activecap)" = a
finish
