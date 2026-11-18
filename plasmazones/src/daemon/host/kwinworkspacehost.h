// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "runtime/host/iworkspacehost.h"

namespace PlasmaZones {

class ShortcutManager;

/**
 * @brief IWorkspaceHost for KWin: kwinrc's PerOutputVirtualDesktops and
 *        KWin's stock desktop-switch shortcuts
 *
 * The takeover backs each chord up in the state directory
 * (kwin-shortcut-backup.ini) before clearing it through KGlobalAccel's
 * foreign-rebind pass-through, and the restore consumes that backup, so a
 * takeover persisted by an earlier session is always undone. A shortcut
 * backend that cannot rebind KWin's shortcuts (the portal) takes nothing.
 * Dynamic workspaces need KDE Frameworks to read and write kwinrc the way
 * KWin does, so a build without them does not support the feature.
 */
class KWinWorkspaceHost : public IWorkspaceHost
{
public:
    /// @param shortcutManager The daemon's; not owned, outlives this host.
    explicit KWinWorkspaceHost(ShortcutManager* shortcutManager);

    bool supportsDynamicWorkspaces() const override;
    bool perOutputDesktopsConfigured() const override;
    bool enablePerOutputDesktops() override;
    void takeOverStockDesktopShortcuts() override;
    void restoreStockDesktopShortcuts() override;

private:
    ShortcutManager* m_shortcutManager;
};

} // namespace PlasmaZones
