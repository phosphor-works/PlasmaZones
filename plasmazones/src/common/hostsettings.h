// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_export.h"

#include <QObject>
#include <QString>

namespace PlasmaZones {

class Settings;

/// Ready this process as a PlasmaZones host: its app directory name, and the
/// settings PlasmaZones adds to the engine's (HostSettings, installed as the
/// HostSettingsExtension). Call at the top of main(), before anything reads a
/// path or builds a Settings: a store built without the extension deletes
/// PlasmaZones' own keys as undeclared on its next save.
PLASMAZONES_EXPORT void initPlasmaZonesHost();

/**
 * @brief The settings PlasmaZones adds to the engine's: its choices about KWin
 *
 * They live in the engine's config file beside the keys the engine reads,
 * declared through the host settings extension, and both sit in the engine's
 * Workspaces.Behavior group:
 *  - the consent for PlasmaZones to turn KWin's per-output virtual desktops on
 *    (the PerOutputVirtualDesktops kwinrc key), a one-shot latch the daemon
 *    clears once it has used it, and machine-scoped;
 *  - the takeover of KWin's stock desktop-switch shortcuts while dynamic
 *    workspaces run.
 *
 * Each is a Q_PROPERTY for the settings app's QML, and announces a change
 * however it came about: a setter, a reload, a discard or a reset.
 */
class PLASMAZONES_EXPORT HostSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool workspacesManageKWinPerOutput READ workspacesManageKWinPerOutput WRITE
                   setWorkspacesManageKWinPerOutput NOTIFY workspacesManageKWinPerOutputChanged)
    Q_PROPERTY(bool workspacesRebindKWinShortcuts READ workspacesRebindKWinShortcuts WRITE
                   setWorkspacesRebindKWinShortcuts NOTIFY workspacesRebindKWinShortcutsChanged)

public:
    /// @param settings The engine settings that store these keys. Not owned;
    ///        outlives this object.
    explicit HostSettings(Settings* settings, QObject* parent = nullptr);

    bool workspacesManageKWinPerOutput() const;
    void setWorkspacesManageKWinPerOutput(bool consented);
    bool workspacesRebindKWinShortcuts() const;
    void setWorkspacesRebindKWinShortcuts(bool takeOver);

    /// The keys, in ConfigDefaults::workspacesBehaviorGroup().
    static QString manageKWinPerOutputKey();
    static QString rebindKWinDesktopShortcutsKey();

Q_SIGNALS:
    void workspacesManageKWinPerOutputChanged();
    void workspacesRebindKWinShortcutsChanged();

private:
    Settings* m_settings;
};

} // namespace PlasmaZones
