#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build-tree phosphorctl aimed at the NESTED shell's socket. The pinned
# --socket is the point: a bare phosphorctl resolves the host session's
# $XDG_RUNTIME_DIR/phosphor.sock, silently driving the wrong shell.
#
#   scripts/nested-shell/ctl.sh call power.toggle
#   scripts/nested-shell/ctl.sh list
set -eu

# This tier's scripts live three levels below the repository root, and the
# nested-kwin harness, the build tree and the shell QML all hang off that root.
REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
# The same state-dir resolution run-nested.sh uses (PZ_NESTED_SESSION included).
. "$REPO/scripts/nested-kwin/nest-dir.sh"

if [ ! -f "$NEST/env.sh" ]; then
    echo "no nested session in $NEST (run scripts/nested-shell/run-shell.sh first)" >&2
    exit 1
fi
# shellcheck disable=SC1091
. "$NEST/env.sh"
BUILD="${PZ_NESTED_BUILD:-build}"

if [ ! -S "$NEST/phosphor.sock" ]; then
    echo "the nested shell's socket $NEST/phosphor.sock is not bound (start it with scripts/nested-shell/shell.sh)" >&2
    exit 1
fi
exec "$REPO/$BUILD/bin/phosphorctl" --socket "$NEST/phosphor.sock" "$@"
