#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# f1301-resume-strip-returns: the windows of a scrolling strip go back into the strip when its
# monitor comes back, the shape of a sleep that drops the monitor's connection (#1301). KWin does
# not let a configuration change disable its last output, so the probe drops the strip's output
# beside a second one, where the windows float in between, which is the harder case. A sleep
# that drops every output moves the windows to KWin's placeholder output instead, which the
# effect does not report, so they wait there without floating.
# Runner: 2 outputs, no flags.
# Staging: Virtual-1 scrolling with three tiles c1, c2, c3, Virtual-0 COLS3, Snap Assist off.
#   Virtual-1 is disabled, so KWin moves its windows to Virtual-0, nothing is touched, and
#   Virtual-1 is enabled again.
# Asserts: after the return no window floats, the strip is "c1 | c2 | c3" as before, and each
#   window is back at its column rect.
# A FAIL means: a window came back floating or out of its place, which the user then has to drag
#   back into the strip.
set -u
NEST="$1"
LOG="$2"
. "$(dirname "$0")/../lib.sh"
kwjs_ok
need_outputs 2
on_exit 'ksd output.Virtual-1.enable >/dev/null'
setting snapAssistEnabled false
assign Virtual-0 "$COLS3"
assign Virtual-1 "scrolling:"
sleep 2
for c in c1 c2 c3; do
    open "$c"
    sendto "$c" Virtual-1 || stage_fail "$c not on Virtual-1 ($(geo "$c"))"
done
sleep 1.5
ids
declare -A ID BEFORE
for c in c1 c2 c3; do
    U=$(uuid "$c")
    [ -n "$U" ] || stage_fail "$c never mapped"
    ID[$c]="org.pz.plain|$U"
    BEFORE[$c]=$(rect "$(geo "$c")")
done
ORDER=$(striporder Virtual-1)
ACTIVE=$(activecap)
echo "staged: active $ACTIVE | strip $ORDER | c1 ${BEFORE[c1]} | c2 ${BEFORE[c2]} | c3 ${BEFORE[c3]}"
[ "$ORDER" = "c1 | c2 | c3" ] || stage_fail "the strip is not c1 | c2 | c3 before the monitor goes"

echo "== Virtual-1 disabled"
dmark
ksd output.Virtual-1.disable
sleep 4
echo "  away: c1 $(geo c1) | c2 $(geo c2) | c3 $(geo c3)"
[ -n "$(dlog "ScrollEngine::parkOutput: .*")" ] || stage_fail "the scroll engine parked nothing for Virtual-1"

echo "== Virtual-1 enabled again"
dmark
ksd output.Virtual-1.enable
sleep 6
dlog "reportOutputSettle: .*|readoptEvacuee: .*|ScrollEngine::readoptParked: .*" | sed 's/org.pz.plain|//g; s/^/    /'
echo "  returned: active $(activecap) | c1 $(geo c1) | c2 $(geo c2) | c3 $(geo c3)"
for c in c1 c2 c3; do
    check "$c floating after the return" "$(floating "${ID[$c]}")" = false
done
check "the strip after the return" "$(striporder Virtual-1)" = "c1 | c2 | c3"
for c in c1 c2 c3; do
    check "$c's frame after the return" "$(rect "$(geo "$c")")" = "${BEFORE[$c]}"
done
finish
