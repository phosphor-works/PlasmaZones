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
 * press first and fires that action instead. Suspending every global
 * shortcut for the length of the capture lets the press reach the window.
 *
 * Only kglobalaccel offers this (its blockGlobalShortcuts call, which also
 * covers KWin's own shortcuts because KWin registers them there). The XDG
 * portal and the D-Bus trigger fallback have no equivalent, so on those a
 * chord that is already taken cannot be recorded, and a recorder should say
 * so rather than wait for a press that never arrives.
 *
 * The block is global, not tied to the caller: kglobalacceld does not undo it
 * when the caller exits. Pair every block with an unblock on every way the
 * capture can end (accepted, cancelled, focus lost, item destroyed). A normal
 * application exit unblocks automatically once anything was blocked, but a
 * crash mid-capture leaves every global shortcut suspended until something
 * unblocks again or kglobalacceld restarts. KWin drives the same switch for
 * its "Ignore global shortcuts" window rule, so focusing a window with that
 * rule during a capture can lift the block early.
 *
 * GUI thread only.
 */

/// Whether this session runs a binding service that can suspend global
/// shortcuts (org.kde.kglobalaccel is on the session bus).
PHOSPHORSHORTCUTS_EXPORT bool canBlockGlobalShortcuts();

/// Suspend (true) or resume (false) every global shortcut in the session.
/// Returns true when the binding service accepted the call. Repeating the
/// current state is a no-op that returns true. The call blocks for one local
/// round trip, so the block is in place before the caller starts listening.
PHOSPHORSHORTCUTS_EXPORT bool setGlobalShortcutsBlocked(bool blocked);

/// Whether this process currently holds a block it set through
/// setGlobalShortcutsBlocked().
PHOSPHORSHORTCUTS_EXPORT bool globalShortcutsBlocked();

} // namespace PhosphorShortcuts
