#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fmaximize-snap: a user maximize of a snapped window on a snapping screen is KWin's maximize, and
# the window keeps its zone under it (phosphor-works/workspace#28). Runner: 1 output, no flags.
# Staging: Virtual-0 COLS3, Snap Assist off, x (appid_gtk.py, app org.pz.snapmax) snapped into
#   zone 1. Staging gate: x is in a zone.
# Asserts: the effect declines the request, so KWin maximizes x (max=3 at the maximize area) and
#   the daemon still reports x's zone. The user restore puts x back at the zone rect, and its zone
#   is the same one.
# A FAIL means: the maximize unsnapped x, or the restore did not land in the zone.
set -u
NEST="$1"
LOG="$2"
. "$(dirname "$0")/../lib.sh"
kwjs_ok
area() { wprop "$1" '(function () { var a = workspace.clientArea(KWin.MaximizeArea, w); return a.x + " " + a.y + " " + a.width + " " + a.height; })()'; }
setting snapAssistEnabled false
assign Virtual-0 "$COLS3"
sleep 1.5
openapp x org.pz.snapmax 400 300
sleep 1.5
ids
XU=$(uuid x)
[ -n "$XU" ] || stage_fail "x never mapped"
X="org.pz.snapmax|$XU"
PZ org.phosphor_works.Placement.Control.snapWindowToZone "$X" 1 Virtual-0 >/dev/null
sleep 2
Z=$(geo x)
ZONE=$(zoneof "$X")
echo "x snapped: $Z zone=$ZONE"
case "$ZONE" in "{"*) ;; *) stage_fail "x did not snap" ;; esac

echo "== user maximize of x"
MARK=$(($(wc -l < "$LOG") + 1))
on x 'w[i].setMaximize(true, true);'
sleep 3
M=$(geo x)
MA=$(area x)
echo "  x $M | maximize area $MA | zone $(zoneof "$X")"
check "effect declines x's maximize" "$(tail -n +"$MARK" "$LOG" | grep -c 'Maximize interception: claiming')" -eq 0
check "x's maximize mode after the user maximize" "$(maxof "$M")" = 3
check "x at the maximize area" "$(rect "$M")" = "$(rect "$MA")"
check "x keeps its zone while maximized" "$(zoneof "$X")" = "$ZONE"

echo "== user restore of x"
on x 'w[i].setMaximize(false, false);'
sleep 3
R=$(geo x)
echo "  x $R | zone $(zoneof "$X")"
check "x's maximize mode after the restore" "$(maxof "$R")" = 0
check "x back at the zone rect" "$(rect "$R")" = "$(rect "$Z")"
check "x's zone after the restore" "$(zoneof "$X")" = "$ZONE"
finish
