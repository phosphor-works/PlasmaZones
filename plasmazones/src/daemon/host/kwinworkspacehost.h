// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/engineruntimenamespace.h"

#include <PhosphorEngineRuntime/IWorkspaceHost.h>

namespace PhosphorEngineRuntime {
class Settings;
class ShortcutManager;
} // namespace PhosphorEngineRuntime

namespace PlasmaZones {

class HostSettings;

/**
 * @brief IWorkspaceHost for KWin: kwinrc's PerOutputVirtualDesktops and
 *        KWin's stock desktop-switch shortcuts
 *
 * The takeover backs each chord up in the state directory
 * (kwin-shortcut-backup.ini) before clearing it through KGlobalAccel's
 * foreign-rebind pass-through, and the restore consumes that backup, so a
 * takeover persisted by an earlier session is always undone. The user's
 * consent and takeover choice are PlasmaZones' own settings (HostSettings);
 * the daemon reports their changes to the runtime. A shortcut
 * backend that cannot rebind KWin's shortcuts (the portal) takes nothing.
 * Dynamic workspaces need KDE Frameworks to read and write kwinrc the way
 * KWin does, so a build without them does not support the feature.
 */
class KWinWorkspaceHost : public IWorkspaceHost
{
public:
    /// @param shortcutManager The daemon's; not owned, outlives this host.
    /// @param settings Saved once the consent is spent; not owned, outlives
    ///        this host.
    /// @param hostSettings The user's consent and takeover choice; not owned,
    ///        outlives this host.
    KWinWorkspaceHost(ShortcutManager* shortcutManager, Settings* settings, HostSettings* hostSettings);

    bool supportsDynamicWorkspaces() const override;
    bool perOutputDesktopsConfigured() const override;
    bool perOutputDesktopsConsented() const override;
    bool enablePerOutputDesktops() override;
    void perOutputDesktopsConsentUsed() override;
    bool stockDesktopShortcutsTakeoverWanted() const override;
    void takeOverStockDesktopShortcuts() override;
    void restoreStockDesktopShortcuts() override;
    QString defaultDesktopName(int oneBasedPosition) const override;

private:
    ShortcutManager* m_shortcutManager;
    Settings* m_settings;
    HostSettings* m_hostSettings;
};

} // namespace PlasmaZones
