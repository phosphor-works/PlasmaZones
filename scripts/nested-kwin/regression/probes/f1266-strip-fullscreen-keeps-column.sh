#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# SPDX-License-Identifier: GPL-3.0-or-later
#
# f1266-strip-fullscreen-keeps-column: a strip tile that goes fullscreen keeps its column, so the
# other columns do not move (#1266). Runner: 1 output, no flags.
# Staging: Virtual-0 scrolling with three tiles s1, s2, s3, s2 active. A KWin script sets
#   s2.fullScreen = true, then back to false.
# Asserts: while s2 is fullscreen it covers the output and the committed frames of s1 and s3 are
#   the ones they had before. After s2 leaves fullscreen, s2 is back at its column rect and s1 and
#   s3 are still where they were.
# A FAIL means: the strip closed up behind the fullscreen window, so a neighbour moved into the
#   slot s2 left, and moved back when s2 left fullscreen.
set -u
NEST="$1"
LOG="$2"
. "$(dirname "$0")/../lib.sh"
kwjs_ok
assign Virtual-0 "scrolling:"
sleep 2
for c in s1 s2 s3; do
    open "$c"
done
sleep 0.5
ids
[ -n "$(uuid s2)" ] || stage_fail "s2 never mapped"
activate s2 1.5
[ "$(activecap)" = s2 ] || stage_fail "s2 is not active"
read -r OX OY OW OH <<< "$(scr Virtual-0)"
S1=$(rect "$(geo s1)")
S2=$(rect "$(geo s2)")
S3=$(rect "$(geo s3)")
echo "strip: s1 $S1 | s2 $S2 | s3 $S3"
[ "$S2" != "$OX $OY $OW $OH" ] || stage_fail "s2 already covers the output"

echo "== s2 fullscreen"
on s2 'w[i].fullScreen = true;'
sleep 2.5
check "s2 while fullscreen" "$(rect "$(geo s2)")" = "$OX $OY $OW $OH"
check "s1 while s2 is fullscreen" "$(rect "$(geo s1)")" = "$S1"
check "s3 while s2 is fullscreen" "$(rect "$(geo s3)")" = "$S3"

echo "== s2 leaves fullscreen"
on s2 'w[i].fullScreen = false;'
sleep 2.5
check "s2 after leaving fullscreen" "$(rect "$(geo s2)")" = "$S2"
check "s1 after s2 left fullscreen" "$(rect "$(geo s1)")" = "$S1"
check "s3 after s2 left fullscreen" "$(rect "$(geo s3)")" = "$S3"
finish
