#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fclaim-release: each release path of the maximize-to-edges claim leaves KWin's maximize bit where
# the engine has the window, and the next maximize press still toggles both (workspace#28, item 5).
# Runner: 1 output (2 for PZ_CLAIM_PATH=output), no flags. PZ_FOWN_MODE picks tile
# (autotile:master-stack) or scroll (scrolling), PZ_CLAIM_PATH the path:
#   float      the window floats with the float shortcut, then unfloats
#   snap       its screen switches to a snapping layout
#   flip       its screen switches to the other engine (tiling to scrolling, or back)
#   desktop    it moves to another desktop that the monitor then shows
#   fullscreen it goes fullscreen and back
#   output     it moves to the other output, which runs the same engine
# Staging: Virtual-0 (and Virtual-1 for output) in the mode, windows a and b tiled, b maximized
#   through a user maximize. Staging gate: the engine reports b maximized and KWin agrees.
# Asserts: after the path, KWin's maximize bit of b matches the engine's state of b (not maximized
#   where no engine holds it), and a user maximize then a restore each move both together.
# A FAIL means: a release path left a claim, or a bit, that the engine does not hold, so KWin and
#   the engine disagree or the next press is swallowed as one that already agrees.
set -u
NEST="$1"
LOG="$2"
. "$(dirname "$0")/../lib.sh"
kwjs_ok
MODE="${PZ_FOWN_MODE:-tile}"
CPATH="${PZ_CLAIM_PATH:-float}"
case "$MODE" in
    tile) LAYOUT="autotile:master-stack" OTHER="scrolling:" ;;
    scroll) LAYOUT="scrolling:" OTHER="autotile:master-stack" ;;
    *) stage_fail "unknown PZ_FOWN_MODE $MODE" ;;
esac
[ "$CPATH" = output ] && need_outputs 2
# engmax CAPTION -> true/false: the engine's maximize to edges of the window, on the output it is on
# (false where no engine runs or the window is not tiled)
engmax() {
    local u screen
    u=$(uuid "$1")
    screen=$(outof "$(geo "$1")")
    {
        PZ org.phosphor_works.Placement.Tiling.currentTilesJson "$screen"
        echo
        PZ org.phosphor_works.Placement.Scrolling.stripModelJson "$screen"
    } | python3 -c '
import json, sys
u = sys.argv[1]
lines = sys.stdin.read().split("\n", 1)
found = False
try:
    for e in json.loads(lines[0] or "[]"):
        if e.get("windowId", "").endswith(u) and not e.get("floating"):
            found = found or bool(e.get("maximizedToEdges"))
except Exception:
    pass
try:
    m = json.loads(lines[1] if len(lines) > 1 else "{}")
    for c in m.get("columns", []):
        if any(t.get("windowId", "").endswith(u) for t in c.get("tiles", [])):
            found = found or bool(c.get("maximized"))
except Exception:
    pass
print("true" if found else "false")' "$u"
}
kmax() { [ "$(maxof "$(geo "$1")")" = 3 ] && echo true || echo false; }

setting snapAssistEnabled false
assign Virtual-0 "$LAYOUT"
[ "$CPATH" = output ] && assign Virtual-1 "$LAYOUT"
sleep 2
open a
open b
sleep 2
ids
activate b
on b 'w[i].setMaximize(true, true);'
sleep 3
echo "b $(geo b) | engine $(engmax b) | kwin $(kmax b)"
[ "$(engmax b)" = true ] && [ "$(kmax b)" = true ] || stage_fail "b is not maximized by the engine ($(geo b))"

echo "== path: $CPATH"
case "$CPATH" in
    float)
        activate b
        shortcut toggle_window_float
        sleep 2.5
        echo "  floated: b $(geo b) | engine $(engmax b) | kwin $(kmax b)"
        check "a float ends the maximize" "$(kmax b)" = false
        activate b
        shortcut toggle_window_float
        sleep 2.5
        ;;
    snap)
        assign Virtual-0 "$COLS3"
        sleep 3
        ;;
    flip)
        assign Virtual-0 "$OTHER"
        sleep 3
        ;;
    desktop)
        newdesk 2 Two
        todesk b 2
        sleep 1
        js 'workspace.currentDesktop = workspace.desktops[1];'
        sleep 3
        ;;
    fullscreen)
        on b 'w[i].fullScreen = true;'
        sleep 2.5
        on b 'w[i].fullScreen = false;'
        sleep 3
        ;;
    output)
        sendto b Virtual-1
        sleep 3
        ;;
    *) stage_fail "unknown PZ_CLAIM_PATH $CPATH" ;;
esac
E=$(engmax b)
K=$(kmax b)
echo "  after: b $(geo b) | engine $E | kwin $K"
check "KWin's maximize matches the engine after the path" "$K" = "$E"

echo "== a maximize press, then a restore"
activate b
on b 'w[i].setMaximize(true, true);'
sleep 3
E1=$(engmax b)
K1=$(kmax b)
echo "  maximized: engine $E1 | kwin $K1"
check "KWin maximized after the press" "$K1" = true
# A snapping screen keeps KWin's own maximize, with no engine state behind it.
if [ "$CPATH" != snap ]; then
    check "the engine maximized after the press" "$E1" = true
fi
on b 'w[i].setMaximize(false, false);'
sleep 3
echo "  restored: engine $(engmax b) | kwin $(kmax b)"
check "KWin restored after the restore" "$(kmax b)" = false
check "the engine restored after the restore" "$(engmax b)" = false
finish
