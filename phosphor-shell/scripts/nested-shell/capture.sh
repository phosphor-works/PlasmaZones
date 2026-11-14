#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Screenshot one nested output as a PNG — the harness's eyes for
# headless / AI-driven runs.
#
#   phosphor-shell/scripts/nested-shell/capture.sh [OutputName] <out.png>
#
# OutputName defaults to Virtual-0 (kwin's first --virtual output; in
# PZ_NESTED_VISIBLE mode list the real names with
# `qdbus6 org.kde.KWin /KWin org.kde.KWin.supportInformation | grep -A2 Screens`
# after sourcing env.sh). Delegates to nested-kwin's capture-output.py
# with the session env loaded.
#
# The shell is an ordinary layer-shell client on the normal composite path:
# bars, popouts and toasts appear in these captures, so they are genuine
# rendering evidence here. capture-output.py uses CaptureScreen, which on KWin
# 6.7 includes compositor effects as well, so effect transforms can show in
# the pixels; committed geometry still comes from dump-windows.sh.
set -eu

# This tier's scripts live three levels below the repository root, and the
# nested-kwin harness, the build tree and the shell QML all hang off that root.
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
# The same state-dir resolution run-nested.sh uses (PZ_NESTED_SESSION included).
. "$REPO/scripts/nested-kwin/nest-dir.sh"

case $# in
    1) OUTPUT="Virtual-0"; OUT="$1" ;;
    2) OUTPUT="$1"; OUT="$2" ;;
    *) echo "usage: capture.sh [OutputName] <out.png>" >&2; exit 1 ;;
esac

if [ ! -f "$NEST/env.sh" ]; then
    echo "no nested session in $NEST (run phosphor-shell/scripts/nested-shell/run-shell.sh first)" >&2
    exit 1
fi
# shellcheck disable=SC1091
. "$NEST/env.sh"

exec python3 "$REPO/scripts/nested-kwin/capture-output.py" "$OUTPUT" "$OUT"
