// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace PlasmaZones {

/**
 * @brief What dynamic per-monitor workspaces need from the compositor that hosts the engine
 *
 * The feature itself, its gate, the WorkspaceController and every verb, is the
 * engine's. Whether the compositor can switch desktops per output, turning that
 * on with the user's consent, and the compositor's own desktop-switch shortcuts
 * are the host's: under KWin they are kwinrc and KWin's global shortcuts
 * (plasmazonesd's KWinWorkspaceHost), and a compositor with its own workspace
 * model answers them natively.
 */
class IWorkspaceHost
{
public:
    virtual ~IWorkspaceHost() = default;

    /// Whether this host can run dynamic workspaces at all. False keeps the
    /// feature off whatever the settings say.
    virtual bool supportsDynamicWorkspaces() const = 0;

    /// Whether the compositor is configured to switch desktops per output.
    virtual bool perOutputDesktopsConfigured() const = 0;

    /// Turn per-output desktops on in the compositor's configuration and ask it
    /// to apply them. Called only with the user's one-shot consent. False when
    /// the setting could not be written.
    virtual bool enablePerOutputDesktops() = 0;

    /// Take over the compositor's own desktop-switch shortcuts, keeping what
    /// is needed to give them back. Idempotent.
    virtual void takeOverStockDesktopShortcuts() = 0;

    /// Give back any desktop-switch shortcuts a takeover holds, this session's
    /// or an earlier one's. Idempotent.
    virtual void restoreStockDesktopShortcuts() = 0;
};

} // namespace PlasmaZones
