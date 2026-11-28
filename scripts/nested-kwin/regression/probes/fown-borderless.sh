#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fown-borderless: a borderless window that covers its whole screen (a borderless game) gets a
# workspace of its own, and goes back when it stops covering the screen (#1201). Runner: 1 output,
# no flags.
# Staging: KWin's per-output virtual desktops on, dynamic workspaces and the Workspaces.Behavior
#   BorderlessOwnWorkspace option on, Virtual-0 COLS3, two free windows a and b (a Qt client) on
#   desktop D0.
#   Staging gate: both are on D0 and Virtual-0 shows D0.
# Asserts: b without its title bar and at the output's frame moves to a new desktop that Virtual-0
#   then shows. Its title bar back and its old frame move it back to D0 and show D0, and the emptied
#   desktop goes.
# A FAIL means: the borderless reading missed the window, or its end did not come back.
OPTION=BorderlessOwnWorkspace
set -u
NEST="$1"
LOG="$2"
. "$(dirname "$0")/../lib.sh"
kwjs_ok
cur() { jsread CUR 'workspace.currentDesktopForScreen(workspace.screens[0]).x11DesktopNumber'; }
count() { jsread CNT 'workspace.desktops.length'; }
per_output_desktops
CFG="$XDG_CONFIG_HOME/phosphor-works/plasmazones/config.json"
python3 -I - "$CFG" "$OPTION" <<'PYEOF'
import json, os, sys
path, option = sys.argv[1], sys.argv[2]
data = json.load(open(path)) if os.path.exists(path) else {}
behavior = data.setdefault("Workspaces", {}).setdefault("Behavior", {})
behavior["Enabled"] = True
behavior[option] = True
os.makedirs(os.path.dirname(path), exist_ok=True)
json.dump(data, open(path, "w"), indent=4)
PYEOF
dmark
PZ org.phosphor_works.Placement.Settings.reloadSettings >/dev/null
sleep 3
dlog "dynamic workspaces active" | grep -q . || stage_fail "the daemon did not start dynamic workspaces"
setting snapAssistEnabled false
assign Virtual-0 "$COLS3"
sleep 1.5
open a
# A Qt client: the GTK one keeps its content height when its title bar goes.
qtclient minsize_qt.py b 400 300 100 100
sleep 2
ids
D0=$(deskof "$(geo b)")
B0=$(geo b)
echo "a $(geo a) | b $B0 | Virtual-0 shows $(cur) | desktops $(count)"
[ "$(deskof "$(geo a)")" = "$D0" ] || stage_fail "a and b are not on one desktop"
[ "$(cur)" = "$D0" ] || stage_fail "Virtual-0 does not show the desktop of a and b"
N0=$(count)

echo "== b goes borderless over the whole output"
dmark
# The frame is set once the title bar is gone: the first configure still carries the old
# decoration, and the client answers it at its old content height.
on b 'w[i].noBorder = true;'
sleep 1
on b 'w[i].frameGeometry = {x: 0, y: 0, width: 1024, height: 768};'
sleep 4
F=$(geo b)
[ "$(rect "$F")" = "0 0 1024 768" ] || stage_fail "b does not cover the output without its title bar ($F)"
dlog "opening own workspace.*" | sed 's/^/    /'
echo "  b $F | Virtual-0 shows $(cur) | desktops $(count)"
check "own workspace opened" "$(dlog 'opening own workspace' | wc -l)" -ge 1
check "b on another desktop" "$(deskof "$F")" != "$D0"
check "Virtual-0 shows b's desktop" "$(cur)" = "$(deskof "$F")"

echo "== b gets its title bar and its frame back"
dmark
on b "w[i].noBorder = false; w[i].frameGeometry = {x: $(echo "$B0" | awk '{printf "%d", $1}'), y: $(echo "$B0" | awk '{printf "%d", $2}'), width: $(echo "$B0" | awk '{printf "%d", $3}'), height: $(echo "$B0" | awk '{printf "%d", $4}')};"
sleep 4
R=$(geo b)
echo "  b $R | Virtual-0 shows $(cur) | desktops $(count)"
check "own workspace closed" "$(dlog 'closing own workspace' | wc -l)" -ge 1
check "b back on D0" "$(deskof "$R")" = "$D0"
check "Virtual-0 shows D0 again" "$(cur)" = "$D0"
check "the emptied desktop is gone" "$(count)" -eq "$N0"
finish
