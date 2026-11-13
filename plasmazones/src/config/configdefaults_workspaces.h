// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "configdefaults_scrolling_shortcuts.h"

namespace PlasmaZones {

// Chain link 9: dynamic per-monitor workspaces (Workspaces.*). The feature is
// a global layer below the three placement modes — the defaults here gate the
// daemon's WorkspaceController and the ownership-map lifecycle, not any one
// engine. Every accessor reaches call sites through the ConfigDefaults leaf.
class ConfigDefaultsWorkspaces : public ConfigDefaultsScrollingShortcuts
{
public:
    // ═══════════════════════════════════════════════════════════════════════════
    // Workspaces.Behavior
    // ═══════════════════════════════════════════════════════════════════════════

    /// Master opt-in for dynamic per-monitor workspaces. Off, the daemon
    /// never constructs a WorkspaceController (Daemon::initializeWorkspaces
    /// returns before it) and ShortcutManager registers none of the workspace
    /// verbs.
    static constexpr bool workspacesEnabled()
    {
        return false;
    }

    /// Consent latch for writing KWin's PerOutputVirtualDesktops key on the
    /// user's behalf. Never written silently; recorded when the user accepts
    /// the inline confirmation on the Workspaces settings page.
    static constexpr bool workspacesManageKWinPerOutput()
    {
        return false;
    }

    /// OSD hints from the workspace controller: when owner-wins snap-back
    /// returns a monitor to its own workspace after a switch to one another
    /// monitor owns, and when a window that landed on a workspace being
    /// removed is moved to its monitor's current one.
    static constexpr bool workspacesSnapBackOsdHint()
    {
        return true;
    }

    /// Neutralize KWin's stock "Switch One Desktop" and "Walk Through
    /// Desktops" chords while the feature is on (restored on disable). On by
    /// default: the stock chords iterate the GLOBAL desktop pool and would
    /// trip owner-wins snap-back on nearly every press, and the focus verbs'
    /// defaults deliberately reuse Meta+Ctrl+Up/Down (user decision,
    /// 2026-08-26).
    static constexpr bool workspacesRebindKWinShortcuts()
    {
        return true;
    }

    /// Named-workspace declarations: none by default. Entry shape (all
    /// QVariantMap fields): id (persistent braced QUuid, empty until a
    /// producer stamps one), name (unique, non-empty), output (screenId, empty
    /// = unpinned), position (preferred slice index, -1 = before the trailing
    /// empty), focusShortcut / moveShortcut (chords, empty = unbound).
    static QVariantList workspacesNamedEntries()
    {
        return {};
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Workspace verb shortcuts (Shortcuts.Global)
    //
    // The focus pair deliberately takes over KWin's stock "Switch One Desktop
    // Down/Up" chords (user decision, 2026-08-26). That takeover is real: the
    // daemon's stock-rebind pass (the KWinDesktopActions table in
    // Daemon::initializeWorkspaces) backs up and clears exactly the "Switch
    // One Desktop *" / "Walk Through Desktops" actions while the feature and
    // the rebind toggle are both on, and restores them on disable.
    //
    // Nothing else here may sit on a stock KWin chord, because that table
    // covers only the SWITCH family. In particular Meta+Ctrl+Shift+Arrow is
    // KWin's stock "Window One Desktop <direction>" quad, which the takeover
    // does NOT release — a default there would stay permanently uncaptured
    // (KGlobalAccel keeps the chord with KWin) and read to the user as a verb
    // that silently does nothing. Every other Meta+Arrow modifier tier is
    // taken: stock Plasma holds Meta, Meta+Shift and Meta+Alt, and
    // PlasmaZones holds Meta+Alt+Shift (moveWindow), Meta+Ctrl+Alt
    // (swapWindow) and Meta+Ctrl+Alt+Shift (swapVirtualScreen). See the span
    // chord comment in configdefaults.h for the full arrow-chord census.
    //
    // So the move-window pair and the move-to-monitor pair ship UNBOUND, like
    // the slot families. An unbound verb is honest: the user assigns a chord
    // that works, instead of a factory default that stock KWin or a sibling
    // family would swallow.
    // ═══════════════════════════════════════════════════════════════════════════

    static QString workspaceFocusUpShortcut()
    {
        return QStringLiteral("Meta+Ctrl+Up");
    }
    static QString workspaceFocusDownShortcut()
    {
        return QStringLiteral("Meta+Ctrl+Down");
    }
    static QString workspaceMoveWindowUpShortcut()
    {
        return QString();
    }
    static QString workspaceMoveWindowDownShortcut()
    {
        return QString();
    }
    static QString workspaceMoveColumnUpShortcut()
    {
        // NOT Meta+Ctrl+Alt+Arrow (the plan's first pick): that family is
        // the swap-window quad. PgUp/PgDown on the same modifier tier is
        // unclaimed and mirrors the reorder pair's PgUp/PgDown shape.
        return QStringLiteral("Meta+Ctrl+Alt+PgUp");
    }
    static QString workspaceMoveColumnDownShortcut()
    {
        return QStringLiteral("Meta+Ctrl+Alt+PgDown");
    }
    static QString workspaceReorderUpShortcut()
    {
        return QStringLiteral("Meta+Ctrl+Shift+PgUp");
    }
    static QString workspaceReorderDownShortcut()
    {
        return QStringLiteral("Meta+Ctrl+Shift+PgDown");
    }
    // The reorder pair above keeps Meta+Ctrl+Shift+PgUp/PgDown: the stock
    // window-to-desktop quad is arrow-only, so the page keys on that tier are
    // free. The move-to-monitor pair below is unbound for the reason in the
    // banner above.
    static QString workspaceMoveToMonitorLeftShortcut()
    {
        return QString();
    }
    static QString workspaceMoveToMonitorRightShortcut()
    {
        return QString();
    }

    /// Move-slot chords: unset by default for every slot. A slot's target
    /// workspace is unassigned until the user picks one in the settings app,
    /// so a factory chord here would claim a global binding that resolves to
    /// nothing on a fresh install. The user assigns both halves, the target
    /// and the chord, on the settings app's workspace shortcuts page. One
    /// accessor for the whole family, since the value does not vary by slot.
    static QString workspaceMoveSlotShortcut()
    {
        return QString();
    }

    /// Focus-slot chords: unset by default for every slot. Unlike the move
    /// slots these read no target (a focus slot is positional: the Nth
    /// workspace of the acting monitor's own list), so the reason is the
    /// chord space instead: the natural positional chords already belong to
    /// stock Plasma (KWin's Switch to Desktop N and the task manager's
    /// numbered entry activation), and the digit tiers PlasmaZones uses are
    /// taken by the quick-layout and snap-to-zone slots. The user binds them
    /// on the settings app's workspace shortcuts page. One accessor for the
    /// whole family, since the value does not vary by slot.
    static QString workspaceFocusSlotShortcut()
    {
        return QString();
    }

    /// Quick-slot targets: unassigned by default. An empty target means the
    /// slot's chord resolves to no workspace and the verb does nothing.
    /// workspaceSlotTarget is the stored name half (Workspaces.Slots TargetN),
    /// workspaceSlotTargetId the entry-id half (TargetIdN).
    static QString workspaceSlotTarget()
    {
        return QString();
    }
    static QString workspaceSlotTargetId()
    {
        return QString();
    }
};

} // namespace PlasmaZones
