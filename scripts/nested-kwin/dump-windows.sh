#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Print every normal window's committed geometry and KWin output assignment
# from inside the nested compositor, via a throwaway KWin script. Committed
# geometry is the ground truth for boundary bugs; screenshots are not.
#
# The script's print() lines are read back from the COMPOSITOR'S LOG, not the
# journal: run-nested.sh sets QT_FORCE_STDERR_LOGGING, so they go to the
# compositor's stderr as "js: ...". That is the file the compositor's stderr
# points at when it is a regular file (run-shell.sh, the regression runner),
# and $PZ_NESTED_DIR/compositor.log otherwise (run-nested.sh copies the output
# there when it runs on a terminal or a pipe). Each run tags its
# lines with a fresh nonce and ends with a sentinel, so an earlier run's lines
# are never reported and "no windows" is told apart from "no output".
set -eu

# Distros ship the Qt6 D-Bus CLI under three names; daemon.sh resolves all
# three. Hardcoding qdbus6 made this script fail at loadScript under set -eu
# on a distro shipping only qdbus-qt6, with the cleanup trap's unloadScript
# then a silent no-op.
QDBUS=""
for _c in qdbus6 qdbus-qt6 qdbus; do
    if command -v "$_c" >/dev/null 2>&1; then QDBUS="$_c"; break; fi
done
if [ -z "$QDBUS" ]; then
    echo "no qdbus6/qdbus-qt6/qdbus on PATH; cannot drive the KWin scripting API" >&2
    exit 1
fi
# Resolved by the same file run-nested.sh and daemon.sh source, so this joins
# the session they do (PZ_NESTED_SESSION included).
. "$(dirname "$0")/nest-dir.sh"
if [ ! -r "$NEST/env.sh" ]; then
    echo "no nested session state at $NEST/env.sh; start run-nested.sh first" >&2
    exit 1
fi
# The bus address has to come FROM env.sh. Every qdbus call below goes to
# whatever DBUS_SESSION_BUS_ADDRESS holds, and one inherited from the caller is
# the HOST bus, where org.kde.KWin also answers: the throwaway script would load
# into the user's real compositor. Cleared before sourcing so an env.sh without
# the line leaves it empty, and refused then (capture-output.py checks the same
# thing from Python).
unset DBUS_SESSION_BUS_ADDRESS
. "$NEST/env.sh"
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    echo "$NEST/env.sh sets no DBUS_SESSION_BUS_ADDRESS; refusing to reach the host bus instead (restart run-nested.sh)" >&2
    exit 1
fi

# The nested compositor, by the socket env.sh names. Anchored on the command
# name, so the shell that launched it (whose command line carries the same
# text) never matches. The socket name may hold dots, which are escaped.
KWIN_PID=$(pgrep -u "$(id -u)" -f "^kwin_wayland .*--socket ${WAYLAND_DISPLAY//./\\.}( |\$)" | head -n1) || true
if [ -z "$KWIN_PID" ]; then
    echo "no nested kwin_wayland is running on socket $WAYLAND_DISPLAY" >&2
    exit 1
fi
# The compositor's stderr, read through its PARENT's fd 2. kwin_wayland itself
# carries file capabilities (cap_sys_nice on Arch), which makes it non-dumpable,
# so its own /proc/<pid>/fd is root-only. Its parent is run-nested.sh's session
# shell, which kwin inherited that stderr from. Opening the fd link reaches the
# live file even after run-shell.sh renamed it, and it is opened only when it
# is a regular file: anything else (the session shell's own terminal or pipe)
# must never be read, since reading a pipe would steal the session's output.
KWIN_PARENT=$(awk '/^PPid:/ { print $2 }' "/proc/$KWIN_PID/status" 2>/dev/null) || true
if [ -n "$KWIN_PARENT" ] && [ -f "/proc/$KWIN_PARENT/fd/2" ]; then
    LOGSRC="/proc/$KWIN_PARENT/fd/2"
elif [ -f "$NEST/compositor.log" ]; then
    LOGSRC="$NEST/compositor.log"
else
    echo "cannot find the nested compositor's log (its stderr is not a file and $NEST/compositor.log does not exist)" >&2
    exit 1
fi

NONCE="$$-$RANDOM$RANDOM"
JS="$NEST/dump-$$.js"
SCRIPT_ID=""
cleanup() {
    if [ -n "$SCRIPT_ID" ]; then
        "$QDBUS" org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "$JS" >/dev/null 2>&1 || true
    fi
    rm -f "$JS"
}
trap cleanup EXIT INT TERM
cat > "$JS" <<DUMPJS
const wins = workspace.windowList();
for (var i = 0; i < wins.length; i++) {
    var w = wins[i];
    if (!w.normalWindow) continue;
    print("WIN ${NONCE} " + w.resourceClass + " geo=" + w.frameGeometry.x + "," + w.frameGeometry.y
          + " " + w.frameGeometry.width + "x" + w.frameGeometry.height
          + " output=" + (w.output ? w.output.name : "?"));
}
print("DUMPEND ${NONCE}");
DUMPJS
SCRIPT_ID=$("$QDBUS" org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$JS")
"$QDBUS" org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
# Bounded wait for the sentinel: the script runs at start, but the log write
# reaches the file asynchronously.
seen=""
for _ in $(seq 1 25); do
    if grep -qF "DUMPEND $NONCE" "$LOGSRC" 2>/dev/null; then
        seen=1
        break
    fi
    sleep 0.2
done
if [ -z "$seen" ]; then
    echo "the dump script printed nothing into $LOGSRC within 5 s (is the js logging category disabled?)" >&2
    exit 1
fi
# No normal windows is an answer, not an error: the sentinel proves the script
# ran, so the grep's no-match exit must not fail the run.
grep -F "WIN $NONCE " "$LOGSRC" | sed "s/.*WIN $NONCE /WIN /" || true
