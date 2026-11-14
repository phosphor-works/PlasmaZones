#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Launch a nested kwin_wayland session with virtual outputs and the KWin
# effect loaded from the BUILD TREE. Reproduces multi-monitor compositor
# bugs (straddling, parking, mixed scale) in minutes with no logout: the
# effect .so never reloads in a live session, but a nested compositor
# starts fresh every time.
#
# Usage:
#   scripts/nested-kwin/run-nested.sh [output-count] [width height] [scale]
#
# width/height apply to EVERY output (kwin_wayland's flags are global).
# scale is passed only to the visible window (PZ_NESTED_VISIBLE). The headless
# --virtual backend does not take --scale as an output scale: it multiplies
# the mode by it and keeps scale 1 (1600x900 at 1.25 comes up 2000x1125 at
# scale 1), so the script does not pass it there and warns instead. To scale
# a headless output, change it once the session is up, from a shell that
# sourced env.sh:
#   kscreen-doctor output.Virtual-0.scale.1.25
# (the regression probe fscale-fixed-device-pixels does this). The same
# command per output gives mixed scales, and so does a seeded
# kwinoutputconfig.json in the nested config home.
#
# Set PZ_NESTED_XWAYLAND to any value to start Xwayland in the nested
# session (X11 test clients; see the flag site below for how to point a
# client at the nested display).
#
# Set PZ_NESTED_VISIBLE to any value to run the nested compositor as a
# WINDOW on the host desktop instead of headless --virtual: same isolated
# session, but you can watch it and interact with pointer/keyboard.
# Requires a live host WAYLAND_DISPLAY. Defaults to 1600x900 when no
# width/height are given (kwin_wayland's own windowed default, 1024x768 per
# its --help, is small for multi-output layouts). Everything else — bus,
# homes, env.sh, screenshots — behaves identically in both modes.
#
# State (isolated XDG home and env.sh) lives in $PZ_NESTED_DIR (default
# $XDG_RUNTIME_DIR/pz-nested, or $XDG_RUNTIME_DIR/pz-nested-<session> with
# PZ_NESTED_SESSION set; private to the user). nest-dir.sh is the one place
# that default is resolved, and every sibling script sources it. The daemon's
# log is $NEST/daemon.log, written by daemon.sh and by a daemon the nested bus
# activates. The compositor's output stays on this terminal and, unless stderr
# is already redirected to a regular file (run-shell.sh and the regression
# runner do that), is also written to $NEST/compositor.log. dump-windows.sh
# reads KWin script output from whichever file the compositor's stderr reaches.
# After the session is up, source env.sh from there in every follow-up shell:
#   . "$XDG_RUNTIME_DIR/pz-nested/env.sh"
# then start the daemon with daemon.sh, and inspect with dump-windows.sh
# and capture-output.py. Set PZ_NESTED_BUILD to use a configure dir other
# than build/ (a path relative to the repository; daemon.sh honours the same
# variable via env.sh).
#
# The session serves the repository's bundled data (layouts, algorithms,
# templates and the shader packs) from $NEST/repo-data, a SYSTEM-tier data dir
# ahead of the installed one on XDG_DATA_DIRS, so a pack added on the branch
# is present and an edit to one is live at the next rescan. It overrides an
# installed asset of the same id but cannot hide an installed one the branch
# deleted or renamed: that still loads under its old id from the install.
# Those dirs are symlinks into the checkout, so a bundled file's canonical path
# lies outside every data dir. A user copy of a bundled layout therefore loses
# its "restore system original" origin when it is reloaded here (the layout
# loader drops a systemSourcePath that does not resolve inside a system data
# dir); probe that restore on an installed build instead.
#
# Every run wipes the XDG homes for a clean slate. PZ_NESTED_KEEP_STATE
# (any value) keeps them, which is the only way to probe session restore:
# daemon.sh restarts the daemon but not the effect, so a login-shaped restore
# needs a full nested restart with the session.json the daemon reads still on
# disk. Stopping the compositor normally needs no PZ_NESTED_FORCE afterwards.
# Neither live-session guard below tests for a file: the wayland guard asks
# whether a process still holds the lock on the socket's ".lock" file, which
# a compositor releases however it exits, and the bus guard asks whether the
# previous bus address still has a BOUND listener, so a socket file a killed
# bus leaves behind (Ctrl+C takes dbus-daemon down without unlinking it) does
# not block the next launch either. One case does: a terminal hangup can leave
# the session's dbus-daemon running with no session around it (it treats
# SIGHUP as a config reload, while the launcher exits). The next launch then
# reports the session as live; kill that dbus-daemon by pid (pgrep -a
# dbus-daemon). FORCE skips the bus guard only, so use it only if that guard
# refuses a session you know has been stopped. The wayland guard applies even
# under FORCE, because a socket a live compositor still holds the lock on
# cannot be taken over. Services the nested bus activated are stopped when the
# compositor exits, and again at the next launch for a session that died
# before it could (see sweep-activated.sh below).
#
# PZ_NESTED_SOCKET (default pznested, or pznested-<session> with
# PZ_NESTED_SESSION set) names the wayland socket. Two nested sessions cannot
# share one — kwin locks on the name and the second dies with "could not add
# wayland socket". PZ_NESTED_SESSION is the one-variable way to run a second
# session: it gives both a distinct default state dir and a distinct default
# socket, and the sibling scripts resolve the same dir from it:
#   PZ_NESTED_SESSION=foo scripts/nested-kwin/run-nested.sh
# PZ_NESTED_SOCKET and PZ_NESTED_DIR still override either default.
#
# Gotchas learned the hard way:
#   - fish cannot source env.sh (POSIX `export VAR=...`); use bash -c.
#   - The effect log needs QT_FORCE_STDERR_LOGGING=1 (set below and carried
#     in env.sh). Without it Qt routes messages to journald whenever stderr
#     is not a tty, so the nested log is empty AND `journalctl --user` shows
#     the HOST daemon and effect, which are indistinguishable at a glance
#     from the nested ones. Check the compositor's own output, not the
#     journal.
#   - Do not `pkill -f` a pattern that appears in your own command line.
#   - A stock session bus D-Bus-activates the INSTALLED daemon the moment
#     the effect connects, which then answers with the HOST's screens. This
#     script shadows the service file so activation starts the build-tree
#     daemon in-session instead, logging to daemon.log and recording its pid
#     in daemon.pid. daemon.sh still evicts whoever holds the name, and holds
#     activation off while it does, so the run has one daemon with a known pid
#     and log.
#   - CaptureScreen includes effects on KWin 6.7; CaptureWindow uses a
#     direct item-rendering path. capture-output.py uses CaptureScreen.
#     Use dump-windows.sh for committed geometry because screenshot pixels
#     can include effect transforms.
set -eu
OUTPUTS="${1:-2}"
case "$OUTPUTS" in
    ''|*[!0-9]*)
        echo "output-count must be a positive integer, got '$OUTPUTS'" >&2
        exit 1
        ;;
esac
if [ "$OUTPUTS" -le 0 ]; then
    echo "output-count must be a positive integer, got '$OUTPUTS'" >&2
    exit 1
fi
WIDTH="${2:-}"
HEIGHT="${3:-}"
SCALE="${4:-}"
for n in "$WIDTH" "$HEIGHT"; do
    case "$n" in
        '') ;;
        *[!0-9]*) echo "width/height must be positive integers, got '$n'" >&2; exit 1 ;;
        *)
            # Zero is a well-formed integer and a useless output dimension:
            # kwin takes it and comes up with a degenerate screen, which reads
            # as the harness being broken. Rejected for the same reason
            # output-count rejects it above.
            [ "$n" -gt 0 ] || { echo "width/height must be positive integers, got '$n'" >&2; exit 1; }
            ;;
    esac
done
if { [ -n "$WIDTH" ] && [ -z "$HEIGHT" ]; } || { [ -z "$WIDTH" ] && [ -n "$HEIGHT" ]; }; then
    echo "width and height must be given together" >&2
    exit 1
fi
case "$SCALE" in
    '') ;;
    *[!0-9.]*|*.*.*|.|*..*|*.)
        echo "scale must be numeric, got '$SCALE'" >&2
        exit 1
        ;;
    *)
        # Well-formed but degenerate: 0, 00, 0.000, .0 and friends all parse
        # as numeric and scale every output to nothing. Compared as a number
        # rather than against a list of spellings, which always missed one.
        if ! awk -v s="$SCALE" 'BEGIN { exit !(s + 0 > 0) }'; then
            echo "scale must be greater than zero, got '$SCALE'" >&2
            exit 1
        fi
        ;;
esac
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
# RUNTIME_DIR and the default NEST, resolved the one way every sibling script
# resolves them.
. "$REPO/scripts/nested-kwin/nest-dir.sh"

# Validate and canonicalise PZ_NESTED_DIR before deriving HOME_N or touching
# any state: it is deleted under, and it is written into generated files.
#
# The character class matters as much as the location. This path is written
# unquoted into the generated D-Bus service file's Exec= line, which is parsed
# as a command line and cannot express a path containing whitespace, and it is
# interpolated into the activation heredoc and the `bash -c` text below, where a
# quote or a dollar sign would either break the shim or run as shell. A
# scratch directory has no business carrying any of that.
case "$NEST" in
    /*) ;;
    *)
        echo "refusing: PZ_NESTED_DIR must be an absolute path, got '$NEST'" >&2
        exit 1
        ;;
esac
case "$NEST" in
    *[!A-Za-z0-9._/-]*)
        echo "refusing: PZ_NESTED_DIR must match [A-Za-z0-9._/-]+ (no whitespace or shell metacharacters), got '$NEST'" >&2
        exit 1
        ;;
esac
# Lexical checks alone accept /tmp/.., and a symlinked ancestor can make an
# apparently deep path resolve somewhere else entirely, so the checks below
# run on the canonical path.
NEST=$(realpath -m -- "$NEST") || {
    echo "refusing: could not canonicalize PZ_NESTED_DIR '$NEST'" >&2
    exit 1
}
case "$NEST" in
    *[!A-Za-z0-9._/-]*)
        echo "refusing: canonical PZ_NESTED_DIR contains unsupported characters: '$NEST'" >&2
        exit 1
        ;;
    /*/*) ;;
    *)
        echo "refusing: PZ_NESTED_DIR must resolve at least two components below '/', got '$NEST'" >&2
        exit 1
        ;;
esac
# The directories a wrong value would actually take out are deeper than any
# fixed list could name: the home directory (/home/<user>, or /var/home/<user>
# on an ostree system), the runtime dir itself, and this repository. Refuse the
# state dir being any of them or an ancestor of any of them, compared on
# canonical paths.
for protected in "${HOME:-}" "$RUNTIME_DIR" "$REPO"; do
    [ -n "$protected" ] || continue
    protected=$(realpath -m -- "$protected") || continue
    case "$protected/" in
        "$NEST"/*)
            echo "refusing: PZ_NESTED_DIR '$NEST' is or contains '$protected'" >&2
            exit 1
            ;;
    esac
done

# The build tree, validated here with the rest of the inputs so a typo refuses
# BEFORE the previous session's state and evidence are reset below.
#
# PZ_NESTED_BUILD is a configure dir RELATIVE to the repository, and REPO is
# where this checkout lives. BUILD gets the same character class as
# PZ_NESTED_DIR. REPO is a path someone already has, so it is refused only for
# what actually breaks: it reaches the activation shim inside a double-quoted
# string (a quote, dollar sign, backtick, backslash or newline would end it or
# run as shell) and PATH, QT_PLUGIN_PATH and XDG_DATA_DIRS (a colon would split
# it). env.sh carries it single-quoted by shell_quote, and it never reaches the
# service file's Exec= line, so a space or a '+' is fine.
BUILD="${PZ_NESTED_BUILD:-build}"
case "$BUILD" in
    '' | /* | *[!A-Za-z0-9._/-]*)
        echo "refusing: PZ_NESTED_BUILD must be a relative path matching [A-Za-z0-9._/-]+, got '$BUILD'" >&2
        exit 1
        ;;
esac
case "$REPO" in
    *[\"\$\`\\:]* | *"
"*)
        echo "refusing: the repository path '$REPO' contains a quote, dollar sign, backtick, backslash, colon or newline, which the generated scripts cannot carry" >&2
        exit 1
        ;;
esac
# Refuse a missing build tree rather than starting a session that silently
# tests the INSTALLED effect or daemon. bin/ alone is not enough: a partial
# build, a failed effect link, or a -DUSE_KDE_FRAMEWORKS=OFF dir (which builds
# no effect) all have it, and kwin_wayland then loads the installed effect and
# the run looks entirely normal while exercising code that is not under test.
EFFECT_SO="$REPO/$BUILD/bin/kwin/effects/plugins/kwin_effect_plasmazones.so"
DAEMON_BIN="$REPO/$BUILD/bin/plasmazonesd"
if [ ! -d "$REPO/$BUILD/bin" ]; then
    echo "no build tree at $REPO/$BUILD/bin; configure and build first," >&2
    echo "or set PZ_NESTED_BUILD to the right configure dir" >&2
    exit 1
fi
if [ ! -f "$EFFECT_SO" ]; then
    echo "no build-tree effect at $EFFECT_SO; build the kwin effect (it needs -DUSE_KDE_FRAMEWORKS=ON)" >&2
    exit 1
fi
if [ ! -x "$DAEMON_BIN" ]; then
    echo "no build-tree daemon at $DAEMON_BIN; build the plasmazonesd target first" >&2
    exit 1
fi
# The rest of the environment the launch needs, refused here for the same
# reason: everything below resets the previous session's state and evidence
# (its homes, env.sh and daemon.log), and a run that then cannot start loses
# them for nothing. The visible backend needs the host compositor, so say why
# instead of letting kwin die with a bare backend error.
for tool in kwin_wayland dbus-run-session flock; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "$tool is not on PATH; the nested session needs it" >&2
        exit 1
    fi
done
if [ -n "${PZ_NESTED_VISIBLE:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    echo "PZ_NESTED_VISIBLE needs a live host Wayland session (WAYLAND_DISPLAY is unset)" >&2
    exit 1
fi

HOME_N="$NEST/home"

# Socket name, so two worktrees can each run a nested session at once (kwin
# takes a lockfile named after it, and a second session on the same name dies
# with "could not add wayland socket"). Pair a distinct PZ_NESTED_SOCKET with
# a distinct PZ_NESTED_DIR, or set PZ_NESTED_SESSION — the state dir holds the
# env.sh every follow-up script sources, so sharing one across two sessions
# points them both at whichever started last.
PZ_NESTED_SOCKET="${PZ_NESTED_SOCKET:-pznested${PZ_NESTED_SESSION:+-$PZ_NESTED_SESSION}}"
# Validated like the numeric arguments above, and for a sharper reason: this
# one is interpolated into the `bash -c` command text and into env.sh below, so a
# name carrying a space, a quote or a shell metacharacter either breaks every
# follow-up script or runs as shell. It is a wayland socket NAME, so the
# character class is not a restriction anyone will notice.
case "$PZ_NESTED_SOCKET" in
    ''|*[!A-Za-z0-9._-]*)
        echo "PZ_NESTED_SOCKET must be a plain socket name matching [A-Za-z0-9._-]+, got '$PZ_NESTED_SOCKET'" >&2
        exit 1
        ;;
esac
SOCK="$RUNTIME_DIR/$PZ_NESTED_SOCKET"
if [ -e "$SOCK" ] && [ ! -S "$SOCK" ]; then
    echo "refusing: $SOCK exists and is not a socket; remove it by hand" >&2
    exit 1
fi
# Refuse to wipe a LIVE session's state out from under it, and use the same
# test kwin_wayland itself applies: libwayland takes an exclusive flock on
# "<socket>.lock" before it binds the socket and holds it for the compositor's
# whole life. A compositor that still holds it cannot be displaced, so the new
# kwin would die with "could not add wayland socket" after this script had
# already reset the live session's state; that is refused even under
# PZ_NESTED_FORCE, because nothing useful can follow it. A crashed compositor
# releases the lock, so a stale socket file passes and is unlinked below.
if [ -e "$SOCK.lock" ] && ! flock -n "$SOCK.lock" true; then
    echo "a nested compositor is live on $SOCK (it holds $SOCK.lock); stop it first" >&2
    exit 1
fi
rm -f "$SOCK"

# The socket guard above keys on PZ_NESTED_SOCKET while everything below
# destroys PZ_NESTED_DIR, so two runs with distinct sockets and a shared or
# defaulted dir sail past it and then delete the first session's state out
# from under its running compositor. Probe the dir the same way: if the
# previous env.sh names a bus that still has a BOUND listener, a session is
# live in THIS tree.
#
# Bound, not merely present: dbus-daemon killed by SIGINT leaves its socket
# file behind, and a file test then refuses every later launch until FORCE.
# /proc/net/unix lists every bound unix socket by its bind path, with an
# abstract one shown as "@<name>", so both address families dbus may hand out
# are covered (older dbus binds unix:tmpdir as an abstract socket).
OLDADDR=""
if [ -f "$NEST/env.sh" ]; then
    OLDADDR=$(sed -n "s/^export DBUS_SESSION_BUS_ADDRESS='\(.*\)'\$/\1/p" "$NEST/env.sh" 2>/dev/null | head -n1)
fi
bus_is_live() {
    local addr="$1" sockname=""
    case "$addr" in
        unix:path=*) sockname=${addr#unix:path=} ;;
        unix:abstract=*) sockname="@${addr#unix:abstract=}" ;;
        *) return 1 ;;
    esac
    sockname=${sockname%%,*}
    [ -n "$sockname" ] && awk -v p="$sockname" '$NF == p { found = 1 } END { exit !found }' /proc/net/unix
}
if [ -n "$OLDADDR" ] && [ -z "${PZ_NESTED_FORCE:-}" ] && bus_is_live "$OLDADDR"; then
    echo "a nested session is live in $NEST (its bus $OLDADDR is still bound);" >&2
    echo "point PZ_NESTED_DIR somewhere else, kill that session, or re-run with PZ_NESTED_FORCE=1" >&2
    exit 1
fi

# The state dir has to be ours and private BEFORE anything is written into it,
# above all the sweep script below, which is written and then executed. mkdir
# -p accepts an existing directory whatever its owner, and `cat >` follows a
# planted symlink, so a directory another user created first (or a symlink in
# its place) would have this script overwrite and run a file it does not
# control.
mkdir -p "$NEST"
if [ -L "$NEST" ] || [ ! -O "$NEST" ]; then
    echo "refusing: $NEST is not a directory owned by you" >&2
    exit 1
fi
# env.sh carries the session bus address; keep the tree private even when
# PZ_NESTED_DIR points somewhere world-traversable.
chmod 700 "$NEST"

# Services the session bus activated outlive it. dbus-run-session stops the
# bus, but some activated services (xdg-desktop-portal-kde, ksecretd and
# kactivitymanagerd, observed) do not exit when their bus goes away. They keep
# writing into the nested XDG homes, so the next launch's `rm -rf` of $HOME_N
# fails with "Directory not empty": the home is renamed aside and the
# surviving service goes on writing into a stale-home.* that the quiet cleanup
# below cannot clear either.
#
# The sweep is identity-gated on DBUS_STARTER_ADDRESS, which dbus-daemon puts
# in the environment of every service IT activated and which those services'
# descendants inherit; nothing else carries it: not the harness, not the shell
# or daemon it starts, and never a terminal the user sourced env.sh into (a
# shell's `export` does not rewrite its /proc/<pid>/environ). The address
# embeds the bus's unique guid, so a match is this session's bus and no other.
# The session runs it when the compositor exits (see the bash -c below); the
# launch-time call here is the backstop for a session that never got to, such
# as one killed with SIGKILL.
cat > "$NEST/sweep-activated.sh" <<'SWEEP'
#!/usr/bin/env bash
# Generated by run-nested.sh. Stops the services one nested bus activated.
# Usage: bash sweep-activated.sh <full DBUS_SESSION_BUS_ADDRESS of that bus>
addr="${1:-}"
[ -n "$addr" ] || exit 0
# One process per pid: grep reads the NUL-separated environ records itself.
# A zombie's environ fails to open with ESRCH; -s keeps that quiet.
activated_by_bus() {
    [ -O "/proc/$1" ] && grep -qszFx -- "DBUS_STARTER_ADDRESS=$addr" "/proc/$1/environ"
}
targets=()
for d in /proc/[0-9]*; do
    p="${d#/proc/}"
    activated_by_bus "$p" || continue
    targets+=("$p")
done
[ "${#targets[@]}" -gt 0 ] || exit 0
kill -TERM "${targets[@]}" 2>/dev/null || true
alive=()
for _ in 1 2 3 4 5 6 7 8 9 10; do
    alive=()
    for p in "${targets[@]}"; do
        [ -d "/proc/$p" ] && alive+=("$p")
    done
    [ "${#alive[@]}" -gt 0 ] || exit 0
    sleep 0.2
done
# Re-check the identity before SIGKILL: a pid recycled during the wait is not
# the service it was.
for p in "${alive[@]}"; do
    activated_by_bus "$p" && kill -KILL "$p" 2>/dev/null || true
done
SWEEP
if [ -n "$OLDADDR" ]; then
    # Through bash rather than by its execute bit: a state dir on a noexec
    # mount must not abort the launch here.
    bash "$NEST/sweep-activated.sh" "$OLDADDR"
fi

# The previous session's daemon goes BEFORE its state home is reset, for the
# same reason as the sweep: a daemon still writing into the home makes the
# wipe below fail.
#
# The kill is identity-gated on the bus address, not on comm alone: comm only
# proves the pid is A plasmazonesd, and after a pid recycle that can be the
# user's real host-session daemon, or another worktree's live nested daemon.
# The previous session's exact address is in its env.sh; only without one does
# this fall back to "any dbus-run-session bus", which the login bus never is.
if [ -f "$NEST/daemon.pid" ]; then
    OLDPID=$(cat "$NEST/daemon.pid")
    if [ -n "$OLDADDR" ]; then
        bus_match=(-qszFx -- "DBUS_SESSION_BUS_ADDRESS=$OLDADDR")
    else
        bus_match=(-qszE -- '^DBUS_SESSION_BUS_ADDRESS=unix:(path|abstract)=/tmp/dbus-')
    fi
    if [ "$(cat "/proc/$OLDPID/comm" 2>/dev/null)" = "plasmazonesd" ] \
        && grep "${bus_match[@]}" "/proc/$OLDPID/environ"; then
        kill "$OLDPID" 2>/dev/null || true
        # Wait for it to actually go before dropping its only pid record,
        # otherwise a slow exit leaves an untracked daemon behind.
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            [ -d "/proc/$OLDPID" ] || break
            sleep 0.2
        done
        [ -d "/proc/$OLDPID" ] && kill -9 "$OLDPID" 2>/dev/null || true
    fi
fi

# PZ_NESTED_KEEP_STATE keeps the previous run's XDG homes. Any non-empty
# value, like every other toggle in this script (FORCE, XWAYLAND, VISIBLE):
# a probe that sets it to "yes" and has its state wiped reads exactly like a
# broken restore. The default wipe gives every session a clean slate, which
# is what nearly every probe wants — but it also destroys the one thing a
# session-restore probe needs, the session.json the daemon reads at start.
# Restarting only the daemon does not exercise the effect's own startup, so a
# login-shaped restore test has to go through a full nested restart, and that
# is only possible if the state home survives it. Control files under $NEST
# are still reset below either way.
KEEP_STATE=0
if [ -n "${PZ_NESTED_KEEP_STATE:-}" ] && [ -d "$HOME_N" ]; then
    KEEP_STATE=1
    echo "keeping previous state home $HOME_N (PZ_NESTED_KEEP_STATE set)" >&2
else
    # A home that cannot be deleted is renamed aside rather than ending the
    # run: a directory something else has mounted over (a sandbox masking a
    # credential-store path such as data/kwalletd) refuses rmdir, but its
    # parent can still be renamed, and the next session needs only a fresh
    # path. Earlier leftovers are retried quietly; one that still refuses
    # stays until the runtime dir is cleared at logout.
    if ! rm -rf "$HOME_N" 2>/dev/null; then
        mv "$HOME_N" "$NEST/stale-home.$(date +%s).$$"
        echo "state home could not be deleted; renamed it aside" >&2
    fi
    for stale in "$NEST"/stale-home.*; do
        if [ -e "$stale" ]; then
            rm -rf "$stale" 2>/dev/null || true
        fi
    done
fi
# Stale control files must not survive into the new run: a daemon.sh run
# against a previous session's env.sh would target a dead bus, and a leftover
# activation block would refuse every activation in the new session.
rm -f "$NEST/env.sh" "$NEST/env.sh.static" "$NEST/daemon.pid" "$NEST/activation-blocked"
# The previous daemon's log is the record of what it persisted, which is the
# evidence a restore probe compares the new session against; under keep-state
# it is rotated rather than deleted, and so is the compositor's log.
for log in daemon.log compositor.log; do
    if [ "$KEEP_STATE" = "1" ] && [ -f "$NEST/$log" ]; then
        mv -f "$NEST/$log" "$NEST/$log.prev"
    else
        rm -f "$NEST/$log" "$NEST/$log.prev"
    fi
done
mkdir -p "$HOME_N/config" "$HOME_N/data" "$HOME_N/cache" "$HOME_N/state"

# A home kept from a harness that still COPIED the bundled data into the user
# data home carries those copies, and a user-dir file wins over the repo-data
# system dir below: the session would run the stale copies as user files.
# Named rather than deleted, because under keep-state the user dir is the state
# under test and some of it may be the probe's own.
if [ "$KEEP_STATE" = "1" ] && [ -d "$HOME_N/data/plasmazones" ]; then
    shadowing=0
    for sub in layouts scrolling-templates curves overlays animations surface pointer algorithms; do
        [ -d "$HOME_N/data/plasmazones/$sub" ] || continue
        for entry in "$HOME_N/data/plasmazones/$sub"/*; do
            if [ -e "$entry" ] && [ -e "$REPO/plasmazones/data/$sub/${entry##*/}" ]; then
                shadowing=$((shadowing + 1))
            fi
        done
    done
    if [ "$shadowing" -gt 0 ]; then
        echo "warning: the kept home holds $shadowing user-dir copies named like bundled assets under $HOME_N/data/plasmazones;" >&2
        echo "they override the checkout's data as user files. Delete them unless the probe wrote them itself." >&2
    fi
fi

# The repository's bundled data, served as a SYSTEM-tier data dir (see the
# header). Symlinks laid out the way the install rules in
# plasmazones/CMakeLists.txt lay the files out, so every registry that looks
# them up through XDG_DATA_DIRS finds them where it finds the installed ones
# and classifies them as bundled, not as user files. Copying into the user
# data home instead made every bundled layout, template and algorithm a user
# file (user-dir algorithms run in their own sandboxed VM, and user layouts
# are deletable and saved under a new name) and re-copied over a kept state
# home. Rebuilt every launch, so it always follows the checkout.
REPO_DATA="$NEST/repo-data"
rm -rf "$REPO_DATA"
mkdir -p "$REPO_DATA/plasmazones" "$REPO_DATA/phosphor-works"
for sub in layouts scrolling-templates curves overlays animations surface pointer algorithms; do
    ln -s "$REPO/plasmazones/data/$sub" "$REPO_DATA/plasmazones/$sub"
done
ln -s "$REPO/plasmazones/data/placement-hosts" "$REPO_DATA/phosphor-works/placement-hosts"

# KWin keeps the virtual-desktop count and names in this same kwinrc
# ([Desktops] Number= / Id_N=), and it preserves the [Plugins] group across
# its own writes. Rewriting the file under keep-state would restart the
# session with one desktop, which is precisely what a per-desktop restore
# probe cannot survive: every membership on desktop 2 and up would land on a
# desktop that no longer exists. Only a fresh home gets the seed file.
if [ "$KEEP_STATE" = "1" ] && [ -f "$HOME_N/config/kwinrc" ]; then
    echo "keeping previous kwinrc (desktop count and names)" >&2
else
    cat > "$HOME_N/config/kwinrc" <<KWINRC
[Plugins]
kwin_effect_plasmazonesEnabled=true
KWINRC
fi

export XDG_CONFIG_HOME="$HOME_N/config"
export XDG_DATA_HOME="$HOME_N/data"
export XDG_CACHE_HOME="$HOME_N/cache"
export XDG_STATE_HOME="$HOME_N/state"
export QT_QPA_PLATFORM=wayland
# A nested client has to die with the nested compositor. With reconnection
# allowed (a host environment that exports this keeps it on), a client whose
# compositor went away waits for it instead, and a daemon or service from the
# last session keeps running into the next one's wipe.
unset QT_WAYLAND_RECONNECT
# Put the worktree's executable directory first. The daemon launches the
# settings app and editor by name (and D-Bus activation inherits this
# environment), so leaving PATH untouched makes those actions silently start
# the installed system binaries while the daemon and effect themselves run
# from the build tree. Keep the host PATH after it for non-PlasmaZones
# helpers.
export PATH="$REPO/$BUILD/bin:${PATH:-/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin}"
# build/bin carries the KWin effect (kwin/effects/plugins/...);
# build/plugins carries the layer-shell QPA integration
# (wayland-shell-integration/phosphorwayland-qpa.so). Without the second
# entry the nested daemon silently loads the INSTALLED layer-shell plugin,
# so layer-shell changes would not actually be under test.
export QT_PLUGIN_PATH="$REPO/$BUILD/bin:$REPO/$BUILD/plugins:${QT_PLUGIN_PATH:-}"
export KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1
# The same categories the daemon's own --debug switches on: plasmazones.* for
# the daemon and effect, kwin.effect.plasmazones.* for the effect's
# kwin-prefixed categories, and org.phosphor.* / phosphor* for the placement
# engines and the phosphor libraries both processes link. Under a host
# qtlogging.ini that turns debug off by default, any category left out here
# stays at warning level and the placement trace is silently incomplete. js is
# the category a KWin script's print() logs under, at debug level: it carries
# what dump-windows.sh and the regression probes read back, so the same host
# default must not drop it.
export QT_LOGGING_RULES="plasmazones.*=true;kwin.effect.plasmazones.*=true;org.phosphor.*=true;phosphor*=true;js=true"
# Without this the rules above buy you nothing. Qt's default handler sends
# messages to JOURNALD whenever stderr is not a tty, which is exactly the case
# here (the compositor's output is redirected), so the nested effect's log
# vanishes from this terminal. Worse than vanishing: `journalctl --user` then
# shows the HOST session's daemon and effect instead, which look like the
# nested ones and are not, so a run can be read completely backwards.
export QT_FORCE_STDERR_LOGGING=1

# D-BUS ACTIVATION, redirected to the build tree.
#
# The system ships /usr/share/dbus-1/services/org.phosphor_works.Placement.service, so the
# nested bus activates the INSTALLED daemon the moment the effect first calls
# org.phosphor_works.Placement. That daemon inherits the bus's environment — which names the
# HOST compositor, not this session — so it connects to the wrong Wayland
# display, answers queries with the host's screens, and outlives the nested bus
# when it dies. They accumulate across runs, compete for the bus name, and
# eventually make daemon.sh lose the race, at which point a run silently tests
# the host session. That failure reads exactly like a behaviour change.
#
# Shadowing the service file ahead of the system one turns activation from a
# hazard into the correct behaviour: the name resolves to the build-tree daemon
# started inside this session. dbus-daemon takes the FIRST match while scanning
# XDG_DATA_DIRS in order, so prepending the shadow leaves every other service
# (kglobalaccel, the portals) resolving from /usr/share as before.
SVC_DIR="$NEST/dbus-services/dbus-1/services"
mkdir -p "$SVC_DIR"
# The service file cannot carry environment, so it execs a wrapper that sources
# the session's own env.sh first. env.sh is written just before kwin_wayland
# starts, and activation can only happen after a client connects, so it is
# always present by the time this runs.
#
# The activated daemon logs to daemon.log and records its pid in daemon.pid,
# exactly as one daemon.sh starts does, so a daemon the bus activates (at
# session start, or again after the one daemon.sh started died) is never an
# untracked one answering while the log reads silence. activation-blocked is
# daemon.sh's: it holds activation off while it evicts the name owner and its
# own daemon claims the name, which otherwise races an activated one.
cat > "$NEST/activate-daemon.sh" <<ACTIVATE
#!/bin/sh
# D-Bus activation shim for the nested session. Never invoked by hand.
set -eu
[ -e "$NEST/activation-blocked" ] && exit 1
. "$NEST/env.sh"
echo \$\$ > "$NEST/daemon.pid"
exec "$DAEMON_BIN" >>"$NEST/daemon.log" 2>&1
ACTIVATE
chmod +x "$NEST/activate-daemon.sh"
cat > "$SVC_DIR/org.phosphor_works.Placement.service" <<SVC
[D-BUS Service]
Name=org.phosphor_works.Placement
Exec=$NEST/activate-daemon.sh
SVC
# No wallet in a nested session. A GTK test client starts xdg-desktop-portal,
# which activates the portal's KWallet backend (ksecretd), and that creates
# \$XDG_DATA_HOME/kwalletd and outlives the compositor: it wrote into the next
# run's wiped home and left a credential-store path that a sandbox masks. The
# shadows resolve those names to a command that exits at once, so activation
# fails cleanly and the portal carries on without a secret backend. Every name
# that execs ksecretd or the wallet UI is shadowed, not just the portal's:
# org.kde.secretservicecompat activates ksecretd directly.
for wallet_service in org.freedesktop.impl.portal.desktop.kwallet org.kde.kwalletd6 org.kde.kwalletd5 \
    org.freedesktop.secrets org.kde.secretservicecompat org.kde.secretprompter org.kde.kwalletmanager; do
    printf '[D-BUS Service]\nName=%s\nExec=/bin/false\n' "$wallet_service" > "$SVC_DIR/$wallet_service.service"
done
export XDG_DATA_DIRS="$NEST/dbus-services:$REPO_DATA:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

EXTRA_FLAGS=""
[ -n "$WIDTH" ] && EXTRA_FLAGS="$EXTRA_FLAGS --width $WIDTH"
[ -n "$HEIGHT" ] && EXTRA_FLAGS="$EXTRA_FLAGS --height $HEIGHT"
# --scale goes to the visible backend only (see the backend selection below).
# Opt-in Xwayland: X11 clients (Proton games are the ones that matter) take
# different KWin paths for fullscreen geometry, so X11-specific bugs cannot
# reproduce in a Wayland-only nested session. env.sh cannot carry the X11
# DISPLAY — it is written before kwin_wayland starts, and the nested
# Xwayland picks its display number at startup. Read it from the Xwayland
# process the nested kwin_wayland started, whose first argument is the
# display (`ps -o args= --ppid <kwin_wayland pid>`). Failing that, KWin
# opens two new sockets under /tmp/.X11-unix and serves the display on the
# LOWER-numbered one, and a client sent to the other never maps. To run an
# X11 test client: source env.sh, find the display number, then
#   DISPLAY=:<n> QT_QPA_PLATFORM=xcb <client>
# (QT_QPA_PLATFORM=xcb alone would connect to the HOST X server and
# silently test the wrong compositor).
[ -n "${PZ_NESTED_XWAYLAND:-}" ] && EXTRA_FLAGS="$EXTRA_FLAGS --xwayland"

# Backend selection: headless virtual outputs by default; a visible nested
# window when PZ_NESTED_VISIBLE is set (its host-session requirement was
# checked with the other inputs, before any state was reset).
BACKEND_FLAGS="--virtual --output-count $OUTPUTS"
if [ -n "${PZ_NESTED_VISIBLE:-}" ]; then
    BACKEND_FLAGS="--output-count $OUTPUTS"
    # kwin_wayland's own windowed default is 1024x768 (its --help); pick a
    # roomier one when the caller gave no size.
    if [ -z "$WIDTH" ]; then
        EXTRA_FLAGS="$EXTRA_FLAGS --width 1600 --height 900"
    fi
    [ -n "$SCALE" ] && EXTRA_FLAGS="$EXTRA_FLAGS --scale $SCALE"
elif [ -n "$SCALE" ]; then
    echo "scale $SCALE is not passed to the headless --virtual backend, which would grow the mode by it and keep scale 1." >&2
    echo "Set it once the session is up with kscreen-doctor output.Virtual-N.scale.$SCALE (see the header)." >&2
fi

# env.sh, written in two parts. Everything but the bus address is known now,
# so it is written HERE, by this shell, with each value single-quoted by
# shell_quote: interpolated into the `bash -c` text instead, a host value
# carrying a quote, a dollar sign or a backtick (PATH is the most free-form of
# them) would break env.sh or run as shell inside the session. The inner shell
# appends only the bus address, which does not exist until dbus-run-session
# has started.
#
# PATH is written to PREPEND the build tree at source time rather than to
# freeze this shell's PATH: a follow-up shell keeps its own PATH (and with it,
# for instance, the venv that supplies capture-output.py's Python modules).
shell_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}
{
    echo "export WAYLAND_DISPLAY=$(shell_quote "$PZ_NESTED_SOCKET")"
    echo "export XDG_CONFIG_HOME=$(shell_quote "$XDG_CONFIG_HOME")"
    echo "export XDG_DATA_HOME=$(shell_quote "$XDG_DATA_HOME")"
    echo "export XDG_CACHE_HOME=$(shell_quote "$XDG_CACHE_HOME")"
    echo "export XDG_STATE_HOME=$(shell_quote "$XDG_STATE_HOME")"
    echo "export XDG_DATA_DIRS=$(shell_quote "$XDG_DATA_DIRS")"
    echo "export QT_QPA_PLATFORM=wayland"
    echo "unset QT_WAYLAND_RECONNECT"
    echo "export PATH=$(shell_quote "$REPO/$BUILD/bin"):\"\$PATH\""
    echo "export QT_PLUGIN_PATH=$(shell_quote "$QT_PLUGIN_PATH")"
    echo "export KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1"
    echo "export QT_LOGGING_RULES=$(shell_quote "$QT_LOGGING_RULES")"
    echo "export QT_FORCE_STDERR_LOGGING=1"
    echo "export PZ_NESTED_DIR=$(shell_quote "$NEST")"
    echo "export PZ_NESTED_REPO=$(shell_quote "$REPO")"
    echo "export PZ_NESTED_BUILD=$(shell_quote "$BUILD")"
} > "$NEST/env.sh.static"

# The compositor's log file. KWin script print() output (category js) goes to
# the compositor's stderr under QT_FORCE_STDERR_LOGGING, never to the journal,
# so dump-windows.sh and the regression probes read it back from a file. When
# stderr already IS a regular file (run-shell.sh's launch log, the regression
# runner's per-probe log), that file is the log and nothing is duplicated.
# Otherwise (a terminal, a pipe, /dev/null) the compositor's output is also
# copied into $NEST/compositor.log. dump-windows.sh finds the right one through
# the compositor's own fd 2, so the two cases need no flag in env.sh.
COMPOSITOR_TEE=1
[ -f /dev/stderr ] && COMPOSITOR_TEE=0

exec dbus-run-session -- bash -c "
  # set -e inside the child: the outer set -eu does NOT cross a bash -c, so
  # without this a failed env.sh write (full disk, read-only or unwritable
  # runtime dir) falls straight through to the compositor start below and the
  # compositor comes up healthy with no state file — after which every helper
  # script reports 'no nested session' while a session is in fact running.
  set -e
  {
    echo \"export DBUS_SESSION_BUS_ADDRESS='\$DBUS_SESSION_BUS_ADDRESS'\"
    cat '$NEST/env.sh.static'
  } > '$NEST/env.sh.tmp'
  mv '$NEST/env.sh.tmp' '$NEST/env.sh'
  # Services this bus activates inherit dbus-daemon's environment, which is
  # still the HOST's display: without this they would start on the nested bus
  # and nested homes and then open their windows on the host desktop. This
  # changes only the bus's activation environment, not this shell's, so the
  # visible backend still reaches the host compositor. DISPLAY is emptied for
  # the same reason; a nested Xwayland's display is not known yet. A failure
  # does not stop the session, but it is said out loud: activated services
  # would then reach the host display.
  dbus-update-activation-environment WAYLAND_DISPLAY='$PZ_NESTED_SOCKET' DISPLAY= \\
    || echo 'warning: dbus-update-activation-environment failed; services the nested bus activates keep the HOST WAYLAND_DISPLAY and DISPLAY' >&2
  # \$EXTRA_FLAGS stays unquoted on purpose: it is a flag LIST and needs word
  # splitting. Everything else is quoted.
  # \$BACKEND_FLAGS is a flag list like \$EXTRA_FLAGS and needs the same
  # word splitting.
  #
  # kwin_wayland runs as a child rather than an exec so this shell is still
  # here when it exits, to stop what the bus activated. The sweep matches
  # processes on the bus address in their environment, not through the bus, so
  # it still works when the bus is already gone: on Ctrl+C, a hangup or a TERM
  # to the process group, dbus-run-session exits at once instead of waiting
  # for this shell. The trap keeps those signals from ending this shell before
  # the sweep: a trapped signal waits for the foreground command, and kwin
  # itself gets the default disposition back.
  #
  # The copy into compositor.log is a process substitution rather than a
  # pipeline so kwin stays this shell's own foreground command and its exit
  # status is the one reported. tee -i ignores the terminal's SIGINT, so a
  # Ctrl+C cannot kill the reader and leave kwin writing into a closed pipe
  # while it shuts down.
  trap : INT TERM HUP
  status=0
  run_kwin() {
    kwin_wayland $BACKEND_FLAGS$EXTRA_FLAGS --socket '$PZ_NESTED_SOCKET' --no-lockscreen --no-global-shortcuts --no-kactivities
  }
  if [ '$COMPOSITOR_TEE' = 1 ]; then
    run_kwin > >(exec tee -i -a '$NEST/compositor.log') 2>&1 || status=\$?
  else
    run_kwin || status=\$?
  fi
  bash '$NEST/sweep-activated.sh' \"\$DBUS_SESSION_BUS_ADDRESS\" || true
  exit \$status
"
