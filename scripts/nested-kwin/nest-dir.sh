# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced, never run: the one place the nested session's runtime dir and state
# dir are resolved. run-nested.sh, daemon.sh, dump-windows.sh and the
# phosphor-shell nested-shell scripts all source it, so a follow-up script
# always joins the session run-nested.sh started. Each of them used to carry
# its own copy, and the copies drifted (a new PZ_NESTED_SESSION suffix reached
# the launcher and none of the others).
#
# Sets RUNTIME_DIR and NEST. Exits the sourcing script when XDG_RUNTIME_DIR is
# unset or PZ_NESTED_SESSION is malformed.

# No fallback for a missing XDG_RUNTIME_DIR. kwin_wayland creates its wayland
# socket there and refuses to start without it, so a fallback only moved the
# state dir somewhere the compositor never used, and the old /tmp fallback was
# a predictable path another local user could create first.
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
    echo "XDG_RUNTIME_DIR is unset; the nested session needs it (kwin creates its wayland socket there)" >&2
    exit 1
fi
RUNTIME_DIR="$XDG_RUNTIME_DIR"

# PZ_NESTED_SESSION names a second, independent session: it suffixes both the
# default state dir and the default socket name. It is a NAME, not a path, so
# it may not carry a slash or start with a dot ('..' would walk the default
# state dir up into the runtime dir itself).
case "${PZ_NESTED_SESSION:-}" in
    '') ;;
    .* | *[!A-Za-z0-9._-]*)
        echo "PZ_NESTED_SESSION must match [A-Za-z0-9._-]+ and not start with '.', got '$PZ_NESTED_SESSION'" >&2
        exit 1
        ;;
esac
NEST="${PZ_NESTED_DIR:-$RUNTIME_DIR/pz-nested${PZ_NESTED_SESSION:+-$PZ_NESTED_SESSION}}"
