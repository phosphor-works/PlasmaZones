#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fmaximize-tile: a user maximize of a tile on a tiling screen is the tile engine's maximize to
# edges (phosphor-works/workspace#28). Runner: 1 output, no flags.
# Staging: Virtual-0 autotile:master-stack, three windows a, b and c. Staging gate: all three are
#   tiled.
# Asserts: after a user maximize of b, the effect claims the request and b is max=3 at the
#   maximize area while a and c keep their tiles. A fourth window d opens and b stays maximized
#   through that retile. The user restore puts b back at a tile, unmaximized, with four tiles.
# A FAIL means: the request went to KWin instead of the tile engine (b loses the maximize on the
#   next retile, or the restore does not land on a tile), or the override moved the other tiles.
set -u
NEST="$1"
LOG="$2"
. "$(dirname "$0")/../lib.sh"
kwjs_ok
area() { wprop "$1" '(function () { var a = workspace.clientArea(KWin.MaximizeArea, w); return a.x + " " + a.y + " " + a.width + " " + a.height; })()'; }
assign Virtual-0 "autotile:master-stack"
sleep 2
for c in a b c; do
    open "$c"
done
sleep 2
ids
A=$(geo a)
C=$(geo c)
echo "a $A | b $(geo b) | c $C | tiles $(tiles Virtual-0)"
[ "$(tiles Virtual-0 | cut -d' ' -f1)" = 3 ] || stage_fail "a, b and c are not all tiled on Virtual-0"

echo "== user maximize of b"
MARK=$(($(wc -l < "$LOG") + 1))
on b 'w[i].setMaximize(true, true);'
sleep 3
M=$(geo b)
MA=$(area b)
echo "  b $M | maximize area $MA"
check "effect claims b's maximize" "$(tail -n +"$MARK" "$LOG" | grep -c 'Maximize interception: claiming')" -ge 1
check "b's maximize mode after the user maximize" "$(maxof "$M")" = 3
check "b at the maximize area" "$(rect "$M")" = "$(rect "$MA")"
check "a keeps its tile" "$(rect "$(geo a)")" = "$(rect "$A")"
check "c keeps its tile" "$(rect "$(geo c)")" = "$(rect "$C")"

echo "== d opens while b is maximized"
open d
sleep 3
D=$(geo b)
echo "  b $D | tiles $(tiles Virtual-0)"
check "tiled windows on Virtual-0 with d open" "$(tiles Virtual-0 | cut -d' ' -f1)" -eq 4
check "b's maximize mode after the retile" "$(maxof "$D")" = 3
check "b still at the maximize area" "$(rect "$D")" = "$(rect "$MA")"

echo "== user restore of b"
on b 'w[i].setMaximize(false, false);'
sleep 3
R=$(geo b)
echo "  b $R | tiles $(tiles Virtual-0)"
check "b's maximize mode after the restore" "$(maxof "$R")" = 0
check "b not at the maximize area after the restore" "$(rect "$R")" != "$(rect "$MA")"
check "tiled windows on Virtual-0 after the restore" "$(tiles Virtual-0 | cut -d' ' -f1)" -eq 4
finish
