#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fown-visitor: a window that opens while the monitor shows an own workspace goes to the workspace
# the owner came from, and its close brings the monitor back to the own workspace and the owner
# (#1201, kde-fullscreen-spaces#3). Runner: 1 output, no flags.
# Staging: KWin's per-output virtual desktops on, dynamic workspaces and the Workspaces.Behavior
#   FullscreenOwnWorkspace option on, Virtual-0 COLS3, two windows a and b on desktop D0, and b
#   fullscreen on its own workspace T. Staging gate: Virtual-0 shows T.
# Asserts: a new window d opens on D0, not T, and Virtual-0 shows D0 with d active. Closing d shows T
#   again with b active, still fullscreen.
# A FAIL means: d stayed on the own workspace, or its close left the monitor on D0.
OPTION=FullscreenOwnWorkspace
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
open b
sleep 2
ids
D0=$(deskof "$(geo b)")
B0=$(geo b)
echo "a $(geo a) | b $B0 | Virtual-0 shows $(cur) | desktops $(count)"
[ "$(deskof "$(geo a)")" = "$D0" ] || stage_fail "a and b are not on one desktop"
[ "$(cur)" = "$D0" ] || stage_fail "Virtual-0 does not show the desktop of a and b"
N0=$(count)

on b 'w[i].fullScreen = true;'
sleep 4
T=$(deskof "$(geo b)")
[ "$T" != "$D0" ] && [ "$(cur)" = "$T" ] || stage_fail "b did not get its own workspace ($(geo b), Virtual-0 shows $(cur))"

echo "== d opens while Virtual-0 shows b's own workspace"
dmark
open d
sleep 4
G=$(geo d)
dlog "opened on the own workspace.*" | sed 's/^/    /'
echo "  d $G | Virtual-0 shows $(cur) | active $(activecap)"
check "d on D0" "$(deskof "$G")" = "$D0"
check "Virtual-0 shows D0" "$(cur)" = "$D0"
check "d active" "$(activecap)" = d

echo "== d closes"
dmark
on d 'w[i].closeWindow();'
sleep 3
echo "  Virtual-0 shows $(cur) | active $(activecap) | b $(geo b)"
check "Virtual-0 shows T again" "$(cur)" = "$T"
check "b active again" "$(activecap)" = b
check "b still fullscreen" "$(wprop b 'w.fullScreen')" = true
finish
