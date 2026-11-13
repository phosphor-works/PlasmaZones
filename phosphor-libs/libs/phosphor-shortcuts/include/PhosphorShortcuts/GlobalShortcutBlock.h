// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "phosphorshortcuts_export.h"

namespace PhosphorShortcuts {

/**
 * Session-wide suspension of global shortcuts, for a chord recorder.
 *
 * A recorder that captures key presses in its own window never sees a chord
 * some component already grabs globally: the binding service consumes the
 * press first and fires that action instead. Suspending the global shortcuts
 * for the length of the capture lets the press reach the window.
 *
 * Only kglobalaccel offers this, through its blockGlobalShortcuts call. That
 * call is not a flag kglobalacceld keeps: it ungrabs every key of every
 * registered component at once, and the unblock grabs them all again. So it
 * covers every keyboard shortcut registered with kglobalaccel, KWin's own
 * keyboard actions included (KWin registers them there). It does not cover
 * what KWin handles outside kglobalaccel, such as its pointer-button, wheel,
 * touchpad and touchscreen gesture shortcuts and its Alt+click and Alt+wheel
 * window actions, which stay live. A component that registers or
 * re-registers a shortcut while the block is in place grabs that key again.
 * The XDG portal and the D-Bus trigger fallback have no equivalent at all, so
 * on those a chord that is already taken cannot be recorded, and a recorder
 * should say so rather than wait for a press that never arrives.
 *
 * The block belongs to nobody. kglobalacceld keeps no record of who asked,
 * counts nothing, and does not watch the caller, so a later unblock from any
 * client lifts it and a caller that exits without unblocking leaves it in
 * place. Within one process the calls here are counted (see
 * setGlobalShortcutsBlocked), and a normal application exit releases what the
 * process still holds. A crash mid-capture leaves the session without its
 * keyboard shortcuts until something unblocks or KWin restarts (on Wayland
 * kglobalacceld runs inside KWin). releaseStrandedGlobalShortcutBlock() is
 * that something: a recorder's application calls it once at start-up.
 *
 * KWin drives the same call for its "Ignore global shortcuts" window rule: it
 * blocks when a window with that rule gains focus and unblocks when the focus
 * leaves it, so focusing such a window and then leaving it during a capture
 * lifts the block early. An ordinary focus change does not touch it.
 *
 * GUI thread only.
 */

/// Whether this session runs a binding service that can suspend global
/// shortcuts (org.kde.kglobalaccel is on the session bus).
PHOSPHORSHORTCUTS_EXPORT bool canBlockGlobalShortcuts();

/// Take (true) or give back (false) one hold on the suspension. The first
/// hold this process takes suspends the global shortcuts and giving back the
/// last one resumes them, so two recorders that overlap in one process cannot
/// lift each other's block. Every true that returned true must be matched by
/// exactly one false. A false with no hold outstanding is a no-op returning
/// true. Returns whether the binding service accepted the call, or true when
/// the hold count changed without one. A true that returns false takes no
/// hold. The call blocks for one local round trip when it reaches the
/// service, so the block is in place before the caller starts listening.
PHOSPHORSHORTCUTS_EXPORT bool setGlobalShortcutsBlocked(bool blocked);

/// Whether this process currently holds at least one hold taken through
/// setGlobalShortcutsBlocked().
PHOSPHORSHORTCUTS_EXPORT bool globalShortcutsBlocked();

/// Resume the global shortcuts whatever this process believes, to clear a
/// block a previous run left behind by crashing mid-capture. Nothing can read
/// the block's state back, so this always sends the unblock, and it lifts a
/// block anyone else holds at that moment as well: another client's recorder,
/// or KWin's for a focused window with the "Ignore global shortcuts" rule
/// (which a newly started application's window takes the focus from anyway).
/// Call it once, at start-up, before anything in the process blocks. It does
/// nothing and returns false while this process holds a block, and returns
/// true without a call when no binding service is running.
PHOSPHORSHORTCUTS_EXPORT bool releaseStrandedGlobalShortcutBlock();

} // namespace PhosphorShortcuts
