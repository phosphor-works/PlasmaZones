// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hostsettings.h"

#include "config/configdefaults.h"
#include "config/settings.h"
#include "config/settingsschema.h"
#include "core/platform/appdirectoryname.h"
#include "core/types/constants.h"

#include <PhosphorConfig/Schema.h>

namespace PlasmaZones {

namespace {

/// Off: the kwinrc key is never written without the user accepting the
/// inline confirmation on the Workspaces settings page.
constexpr bool DefaultManageKWinPerOutput = false;

/// On: KWin's stock chords walk the GLOBAL desktop pool and would trip
/// owner-wins snap-back on nearly every press, and the focus verbs' defaults
/// deliberately reuse Meta+Ctrl+Up/Down (user decision, 2026-08-26).
constexpr bool DefaultRebindKWinShortcuts = true;

void appendHostSchema(PhosphorConfig::Schema& schema)
{
    schema.groups[ConfigDefaults::workspacesBehaviorGroup()] += QList<PhosphorConfig::KeyDef>{
        {HostSettings::manageKWinPerOutputKey(), DefaultManageKWinPerOutput, QMetaType::Bool,
         QStringLiteral("Records that the user consented to PlasmaZones writing KWin's PerOutputVirtualDesktops key. "
                        "The key is never written without this, and never reverted when the feature is disabled.")},
        {HostSettings::rebindKWinDesktopShortcutsKey(), DefaultRebindKWinShortcuts, QMetaType::Bool,
         QStringLiteral("Takes over KWin's stock Switch One Desktop and Walk Through Desktops shortcuts while the "
                        "feature is on, since they walk the whole shared desktop pool rather than one monitor's "
                        "list. Restored on disable.")},
    };
}

/// The consent this machine's user gave for PlasmaZones to write KWin's
/// PerOutputVirtualDesktops key: an import from elsewhere must never grant
/// it, and a profile switch must never silently withdraw it.
bool isHostMachineScopedKey(const QString& group, const QString& key)
{
    return group == ConfigDefaults::workspacesBehaviorGroup() && key == HostSettings::manageKWinPerOutputKey();
}

} // namespace

void initPlasmaZonesHost()
{
    setAppDirectoryName(AppDirectoryName);
    setHostSettingsExtension({&appendHostSchema, &isHostMachineScopedKey});
}

HostSettings::HostSettings(Settings* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
    connect(m_settings, &Settings::hostValueChanged, this, [this](const QString& group, const QString& key) {
        if (group != ConfigDefaults::workspacesBehaviorGroup()) {
            return;
        }
        if (key == manageKWinPerOutputKey()) {
            Q_EMIT workspacesManageKWinPerOutputChanged();
        } else if (key == rebindKWinDesktopShortcutsKey()) {
            Q_EMIT workspacesRebindKWinShortcutsChanged();
        }
    });
}

QString HostSettings::manageKWinPerOutputKey()
{
    return QStringLiteral("ManageKWinPerOutput");
}

QString HostSettings::rebindKWinDesktopShortcutsKey()
{
    return QStringLiteral("RebindKWinDesktopShortcuts");
}

bool HostSettings::workspacesManageKWinPerOutput() const
{
    return m_settings->hostValue(ConfigDefaults::workspacesBehaviorGroup(), manageKWinPerOutputKey()).toBool();
}

void HostSettings::setWorkspacesManageKWinPerOutput(bool consented)
{
    m_settings->setHostValue(ConfigDefaults::workspacesBehaviorGroup(), manageKWinPerOutputKey(), consented);
}

bool HostSettings::workspacesRebindKWinShortcuts() const
{
    return m_settings->hostValue(ConfigDefaults::workspacesBehaviorGroup(), rebindKWinDesktopShortcutsKey()).toBool();
}

void HostSettings::setWorkspacesRebindKWinShortcuts(bool takeOver)
{
    m_settings->setHostValue(ConfigDefaults::workspacesBehaviorGroup(), rebindKWinDesktopShortcutsKey(), takeOver);
}

} // namespace PlasmaZones
