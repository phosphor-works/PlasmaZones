#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# fmaximize-daemon-restart: an engine's maximize stays while the daemon is down and after it
# returns. Runner: 1 output, no flags. PZ_FOWN_MODE picks tile (autotile:master-stack, b maximized
# to the edges by a user maximize), scroll (scrolling, the same) or monocle (autotile:monocle, a and
# b both maximized by the stack).
# Staging: Virtual-0 in the mode, windows a and b, b maximized as above, and a KWin script that
#   records every maximize change of a and b. Staging gate: b is max=3.
# Asserts: with the daemon stopped, a and b keep their maximize modes. After the restart they still
#   have them, and KWin reported no un-maximize of either. A user restore of b after the restart
#   still restores it (in tile and scroll).
# A FAIL means: the daemon loss or the bring-up gave the maximize back.
set -u
NEST="$1"
LOG="$2"
. "$(dirname "$0")/../lib.sh"
kwjs_ok
MODE="${PZ_FOWN_MODE:-tile}"
case "$MODE" in
    tile) LAYOUT="autotile:master-stack" ;;
    scroll) LAYOUT="scrolling:" ;;
    monocle) LAYOUT="autotile:monocle" ;;
    *) stage_fail "unknown PZ_FOWN_MODE $MODE" ;;
esac
setting snapAssistEnabled false
assign Virtual-0 "$LAYOUT"
sleep 2
open a
open b
sleep 2
ids
if [ "$MODE" != monocle ]; then
    activate b
    on b 'w[i].setMaximize(true, true);'
    sleep 3
fi
A0=$(maxof "$(geo a)")
echo "a $(geo a) | b $(geo b)"
[ "$(maxof "$(geo b)")" = 3 ] || stage_fail "b is not maximized ($(geo b))"
HOOK="$NEST/fmaxrestart-hook.js"
on_exit 'qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$HOOK" >/dev/null 2>&1; rm -f "$HOOK"'
printf '%s\n' 'const ws = workspace.windowList(); for (var i = 0; i < ws.length; i++) { const w = ws[i]; if (w.caption === "a" || w.caption === "b") { w.maximizedChanged.connect(function () { print("FMAXRESTART " + w.caption + " " + w.maximizeMode); }); } }' > "$HOOK"
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$HOOK" >/dev/null
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
sleep 0.7
emark

echo "== the daemon stops"
dkill
sleep 3
echo "  a $(geo a) | b $(geo b)"
check "b maximized while the daemon is down" "$(maxof "$(geo b)")" = 3
check "a's maximize mode while the daemon is down" "$(maxof "$(geo a)")" = "$A0"

echo "== the daemon comes back"
drestart
sleep 8
echo "  a $(geo a) | b $(geo b)"
echo "  maximize changes: $(elog 'FMAXRESTART [ab] [0-9]+' | tr '\n' ' ')"
check "b maximized after the restart" "$(maxof "$(geo b)")" = 3
check "a's maximize mode after the restart" "$(maxof "$(geo a)")" = "$A0"
check "no un-maximize of b" "$(elog 'FMAXRESTART b 0' | wc -l)" -eq 0
check "no un-maximize of a" "$(elog "FMAXRESTART a 0" | wc -l)" -eq 0

if [ "$MODE" != monocle ]; then
    echo "== a user restore of b after the restart"
    on b 'w[i].setMaximize(false, false);'
    sleep 3
    echo "  b $(geo b)"
    check "b restored" "$(maxof "$(geo b)")" = 0
fi
finish
