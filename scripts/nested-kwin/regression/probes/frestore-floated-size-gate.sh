#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-FileCopyrightText: 2026 MaslovKK
# SPDX-License-Identifier: GPL-3.0-or-later
#
# frestore-floated-size-gate: with "Restore unsnapped windows to their previous position and size"
# off, a reopened window keeps the size the app asked for: the remembered SIZE follows the same
# setting as the move back to the old position. Runner: 1 output, no flags.
# Staging: Virtual-0 COLS3, so a plain window opens floating and nothing re-snaps it. A floats at
#   800x600 and closes; its float record is what every later reopen reads. The setting is on (the
#   default) for the first reopen: B (same app) asks for 400x300 and is restored to A's 800x600.
#   B closes, the setting goes off, and C asks for 400x300 too.
# Asserts: B is 800x600 (the control, the restore chain is live), C keeps 400x300.
# A FAIL on C means: the size arm of the reopen restore ran ungated, resizing the reopened window
#   to its remembered size while the setting the user turned off looks like it does nothing.
set -u
NEST="$1"
LOG="$2"
. "$(dirname "$0")/../lib.sh"
kwjs_ok
assign Virtual-0 "$COLS3"
sleep 2
openapp a org.pz.sizerestore 800 600
sleep 2
AG=$(geo a)
echo "a: $AG"
[ "$(size "$AG")" = "800x600" ] || stage_fail "a never sat at the size it asked for ($AG)"
on a 'w[i].closeWindow();'
sleep 2
dmark
openapp b org.pz.sizerestore 400 300
sleep 3
BG=$(geo b)
echo "b (setting on): $BG"
dlog "resolveWindowRestore: placement\(floated\)|geometryRestoreRequested|restoreFreeSizeWhereItStands" | sed 's/^/    /'
check "the reopen with the setting on takes the remembered size" "$(size "$BG")" = "800x600"
on b 'w[i].closeWindow();'
sleep 2
gsetting_ok snappingRestoreFloatedWindowsOnLogin "<false>"
sleep 2
dmark
openapp c org.pz.sizerestore 400 300
sleep 3
CG=$(geo c)
echo "c (setting off): $CG"
dlog "resolveWindowRestore: placement\(floated\)|geometryRestoreRequested|restoreFreeSizeWhereItStands" | sed 's/^/    /'
check "the reopen with the setting off keeps the size it asked for" "$(size "$CG")" = "400x300"
finish
