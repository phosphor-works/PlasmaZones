#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Start the BUILD-TREE plasmazonesd inside the nested session. run-nested.sh
# shadows the D-Bus service file, so activation on the nested bus already
# starts a build-tree daemon in-session; this evicts whatever holds the name
# so the run has one daemon with a known pid and log, and reaps daemons left
# behind by earlier sessions whose bus is gone. Never touches the host
# session's own daemon.
#
# ONE run at a time per nested session. Two concurrent runs against the same
# PZ_NESTED_DIR each evict the other's daemon through the bus-owner kill and
# each overwrite the other's daemon.pid, so both end up reporting a pid that
# is either dead or not theirs. That is not guarded here because the harness
# is already documented as one-session-per-directory (run-nested.sh pairs a
# distinct PZ_NESTED_SOCKET with a distinct PZ_NESTED_DIR); two sessions
# sharing a directory are broken well before this script runs.
set -euo pipefail
# Resolved by the same file run-nested.sh sources — a divergence here points
# this script at a different state dir than the session it is meant to join.
. "$(dirname "$0")/nest-dir.sh"
if [ ! -r "$NEST/env.sh" ]; then
    echo "no nested session state at $NEST/env.sh; start run-nested.sh first" >&2
    exit 1
fi
. "$NEST/env.sh"

# env.sh is written just before kwin_wayland starts, so on a fast follow-up
# the compositor may not be listening yet. Wait for the socket, bounded.
SOCK="$RUNTIME_DIR/$WAYLAND_DISPLAY"
for _ in $(seq 1 50); do
    [ -S "$SOCK" ] && break
    sleep 0.2
done
if [ ! -S "$SOCK" ]; then
    echo "nested compositor socket $SOCK never appeared; is run-nested.sh up?" >&2
    exit 1
fi

# Hold D-Bus activation off from here until this script's daemon owns the
# name. Between evicting the owner and the new daemon's registration the name
# is unowned (the daemon waits for KWin before it registers), and any effect
# call in that gap would activate the session's activation shim, whose daemon
# can take the name first; this one would then exit "Already running" while
# daemon.pid and daemon.log describe it. The shim refuses while this exists.
touch "$NEST/activation-blocked"
trap 'rm -f "$NEST/activation-blocked"' EXIT

# A leftover daemon from a previous nested run is invisible to the bus
# check below (its bus died with the old session); kill it by pid first,
# identity-gated so a recycled pid cannot hit an unrelated process.
if [ -f "$NEST/daemon.pid" ]; then
    OLDPID=$(cat "$NEST/daemon.pid")
    # Bus address, not comm alone: comm only proves the pid is A
    # plasmazonesd, and after a pid recycle that can be the user's real
    # host-session daemon. A nested daemon always runs on a dbus-run-session
    # bus under /tmp/dbus-, which the login bus never does; older dbus binds
    # that as an abstract socket, so both address forms are accepted.
    if [ "$(cat "/proc/$OLDPID/comm" 2>/dev/null)" = "plasmazonesd" ] \
        && grep -qszE '^DBUS_SESSION_BUS_ADDRESS=unix:(path|abstract)=/tmp/dbus-' "/proc/$OLDPID/environ"; then
        kill "$OLDPID" 2>/dev/null || true
    fi
    rm -f "$NEST/daemon.pid"
fi
BUSPID=$(dbus-send --session --print-reply --dest=org.freedesktop.DBus / \
    org.freedesktop.DBus.GetConnectionUnixProcessID string:org.phosphor_works.Placement 2>/dev/null \
    | awk '/uint32/{print $2}') || true
if [ -n "${BUSPID:-}" ]; then
    kill "$BUSPID" 2>/dev/null || true
    # Bounded poll rather than a fixed sleep: a fixed wait is simultaneously
    # too long for the common case (the daemon is gone in well under a second)
    # and too short for a loaded machine, where the name is still owned when
    # the new daemon starts and the run silently answers from the old one.
    for _ in $(seq 1 25); do
        [ -d "/proc/$BUSPID" ] || break
        sleep 0.2
    done
    if [ -d "/proc/$BUSPID" ]; then
        kill -9 "$BUSPID" 2>/dev/null || true
        # SIGKILL is not synchronous either; give the bus time to notice the
        # peer drop and release the name before we claim it.
        for _ in $(seq 1 10); do
            [ -d "/proc/$BUSPID" ] || break
            sleep 0.2
        done
    fi
fi

# Reap daemons stranded on a DEAD nested bus. run-nested.sh now shadows the
# D-Bus service file so activation starts the build-tree daemon in-session
# rather than the installed one on the host, but a session started before that
# fix — or killed hard enough to orphan its activation — leaves one behind.
# They are invisible to the bus check above (their own bus is gone), they
# accumulate across runs, and once several are competing the eviction here
# starts losing the race and a run silently answers with the HOST's screens.
#
# Matched on the bus address in the process environment. The pattern is any
# dbus-run-session bus under /tmp/dbus-, not specifically a pz-nested one, so
# a daemon stranded on some other private bus is fair game too — which is the
# intent, since the kill fires only once that bus socket is already gone and
# the daemon is genuinely orphaned. Narrowing this to pz-nested buses would
# miss exactly the daemons it exists to collect, because a session killed hard
# enough to orphan its activation leaves no pz marker on the bus path. The
# user's real session daemon carries the login bus address and is never
# matched.
for pid in $(pgrep -x plasmazonesd 2>/dev/null || true); do
    [ "$pid" = "${BUSPID:-}" ] && continue
    peer_bus=$(tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null \
        | sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p') || true
    case "$peer_bus" in
        "unix:path=/tmp/dbus-"*)
            # A pz-nested bus socket that no longer exists means the session it
            # belonged to is gone and this daemon is stranded.
            #
            # unix:path= specifically, not any address mentioning /tmp/dbus-:
            # a dbus built for ABSTRACT sockets reads unix:abstract=/tmp/dbus-…,
            # which the strip below cannot turn into a filesystem path, so the
            # -S test would answer false for a perfectly LIVE bus and kill its
            # daemon. An address whose liveness cannot be checked is left alone.
            sock=${peer_bus#unix:path=}
            sock=${sock%%,*}
            [ -S "$sock" ] || kill "$pid" 2>/dev/null || true
            ;;
    esac
done
BUILD="${PZ_NESTED_BUILD:-build}"
# Truncated first, then appended to, the way the activation shim writes it. A
# daemon the bus activated just before the block above can still hold this
# file open, and with one writer in append mode and one writing from offset 0
# the second overwrites the head of the first one's log.
: > "$NEST/daemon.log"
nohup "$PZ_NESTED_REPO/$BUILD/bin/plasmazonesd" >> "$NEST/daemon.log" 2>&1 &
echo $! > "$NEST/daemon.pid"

# "Up" means it OWNS the bus name, not merely that the pid exists — a
# daemon that started but failed to claim org.phosphor_works.Placement (or died into a
# zombie) would otherwise print success while every later qdbus6 call
# answers from nothing.
up=""
for _ in $(seq 1 30); do
    OWNER=$(dbus-send --session --print-reply --dest=org.freedesktop.DBus / \
        org.freedesktop.DBus.GetNameOwner string:org.phosphor_works.Placement 2>/dev/null | awk '/string/{print $2}') || true
    if [ -n "${OWNER:-}" ]; then
        up=1
        break
    fi
    sleep 0.2
done
if [ -n "$up" ]; then
    echo "daemon up (pid $(cat "$NEST/daemon.pid"), owns org.phosphor_works.Placement); screens:"
    # The screen dump is a convenience, so a missing qdbus6 must not fail a
    # run that is otherwise healthy: the bus-name check above has ALREADY
    # established the daemon is up, and exiting non-zero here would report a
    # working session as a failure. Distros ship this binary under three
    # different names.
    if command -v qdbus6 >/dev/null 2>&1; then
        qdbus6 org.phosphor_works.Placement /org/phosphor_works/Placement org.phosphor_works.Placement.Screen.getPhysicalScreens
    elif command -v qdbus-qt6 >/dev/null 2>&1; then
        qdbus-qt6 org.phosphor_works.Placement /org/phosphor_works/Placement org.phosphor_works.Placement.Screen.getPhysicalScreens
    elif command -v qdbus >/dev/null 2>&1; then
        qdbus org.phosphor_works.Placement /org/phosphor_works/Placement org.phosphor_works.Placement.Screen.getPhysicalScreens
    else
        echo "  (no qdbus6/qdbus-qt6/qdbus on PATH; skipping the screen dump)"
    fi
else
    echo "daemon FAILED to claim org.phosphor_works.Placement; see $NEST/daemon.log" >&2
    exit 1
fi
