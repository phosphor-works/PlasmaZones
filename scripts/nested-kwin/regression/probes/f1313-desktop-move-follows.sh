#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# f1313-desktop-move-follows: a move shortcut that takes a window to the next desktop takes the
# view there too, and the window keeps the focus (#1313). Runner: 1 output, no flags.
# Staging: two desktops, Virtual-0 Columns (2) on every desktop, Snap Assist off, p1 snapped into
#   zone 2 (the right half) on desktop 1 and active. move_window_right then has no zone to the
#   right, so the snap engine moves p1 to desktop 2.
# Asserts: p1 is on desktop 2 only, KWin shows desktop 2, and p1 is the active window.
# A FAIL means: the window went to desktop 2 alone, so the view stayed on desktop 1 and the
#   window lost the focus.
set -u
NEST="$1"
LOG="$2"
. "$(dirname "$0")/../lib.sh"
kwjs_ok
setting snapAssistEnabled false
newdesk 1 D2
ZONE2="{05b9766b-d2a1-49ab-94fa-35d3e31f33f9}"
assign Virtual-0 "$COLS2"
sleep 1.5
case "$(curdesk)" in "1 of 2") ;; *) stage_fail "the session is not on desktop 1 of 2 ($(curdesk))" ;; esac
open p1
ids
P1U=$(uuid p1)
[ -n "$P1U" ] || stage_fail "p1 never mapped"
P1="org.pz.plain|$P1U"
PZ org.phosphor_works.Placement.Snap.moveWindowToZone "$P1" "$ZONE2" >/dev/null
sleep 2
[ "$(zoneof "$P1")" = "$ZONE2" ] || stage_fail "p1 did not snap into Columns (2) zone 2"
activate p1
[ "$(activecap)" = p1 ] || stage_fail "p1 is not active before the move"
echo "p1 staged: $(geo p1)"

echo "== move_window_right past the last zone"
shortcut move_window_right
sleep 2.5
G=$(geo p1)
echo "  after: p1 $G | current $(curdesk) | active $(activecap)"
[ "$(deskof "$G")" = 2 ] || stage_fail "move_window_right did not move p1 to desktop 2 ($G)"
check "the desktop KWin shows" "$(curdesk)" = "2 of 2"
check "the active window" "$(activecap)" = p1
finish
