// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/engineruntimenamespace.h"

#include "plasmazones_export.h"
#include "version.h"

#include <QLatin1String>
#include <QObject>
#include <QString>
#include <QStringList>

namespace PhosphorEngineRuntime {
class Settings;
} // namespace PhosphorEngineRuntime

namespace PlasmaZones {

/**
 * @brief PlasmaZones' id as a host of the placement API.
 *
 * plasmazonesd records itself under this id at start
 * (@c PhosphorConfig::HostDirectory::writeRecord in daemon/main.cpp), and the
 * descriptor the package installs, data/placement-hosts/plasmazones.json,
 * carries the same string as its @c Id. They have to agree:
 * @c PhosphorConfig::HostDirectory::resolve ignores a record that names a host
 * with no descriptor. tests/unit/data/test_placement_host_descriptor.cpp reads
 * the shipped descriptor and compares it with this constant.
 */
inline constexpr QLatin1String PlacementHostId{"plasmazones"};

/// Ready this process as a PlasmaZones host: its app directory name, the surface
/// packs' data subdirectory, and the settings PlasmaZones adds to the engine's (HostSettings, installed as the
/// HostSettingsExtension). Call at the top of main(), before anything reads a
/// path or builds a Settings: a store built without the extension deletes
/// PlasmaZones' own keys as undeclared on its next save.
PLASMAZONES_EXPORT void initPlasmaZonesHost();

/**
 * @brief The settings PlasmaZones adds to the engine's: its choices about KWin
 *
 * They live in the engine's config file beside the keys the engine reads,
 * declared through the host settings extension:
 *  - in the engine's Workspaces.Behavior group, the consent for PlasmaZones to
 *    turn KWin's per-output virtual desktops on (the PerOutputVirtualDesktops
 *    kwinrc key), a one-shot latch the daemon clears once it has used it, and
 *    machine-scoped; and the takeover of KWin's stock desktop-switch shortcuts
 *    while dynamic workspaces run;
 *  - in PlasmaZones' own Rendering group, the graphics API plasmazonesd draws
 *    its overlays with and the GPU it draws them on (machine-scoped). Both are
 *    read before QGuiApplication exists (readRenderingConfigFromDisk), so a
 *    change takes effect on the daemon's next start.
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
    Q_PROPERTY(QString renderingBackend READ renderingBackend WRITE setRenderingBackend NOTIFY renderingBackendChanged)
    Q_PROPERTY(QString gpuDevice READ gpuDevice WRITE setGpuDevice NOTIFY gpuDeviceChanged)

public:
    /// @param settings The engine settings that store these keys. Not owned;
    ///        outlives this object.
    explicit HostSettings(Settings* settings, QObject* parent = nullptr);

    bool workspacesManageKWinPerOutput() const;
    void setWorkspacesManageKWinPerOutput(bool consented);
    bool workspacesRebindKWinShortcuts() const;
    void setWorkspacesRebindKWinShortcuts(bool takeOver);
    /// One of renderingBackendOptions(); the schema coerces anything else to
    /// the default.
    QString renderingBackend() const;
    void setRenderingBackend(const QString& backend);
    /// "auto" or a "vendor:device" hex PCI pair; the schema coerces anything
    /// else to "auto".
    QString gpuDevice() const;
    void setGpuDevice(const QString& gpu);

    /// The keys, in ConfigDefaults::workspacesBehaviorGroup().
    static QString manageKWinPerOutputKey();
    static QString rebindKWinDesktopShortcutsKey();
    /// PlasmaZones' Rendering group and its keys.
    static QString renderingGroup();
    static QString backendKey();
    static QString gpuKey();

    /// "auto": Qt picks the graphics API.
    static QString defaultRenderingBackend();
    /// The legal backend tokens, in the order pickers offer them. The words
    /// for them are SettingsValueLabels'.
    static const QStringList& renderingBackendOptions();
    static QString normalizeRenderingBackend(const QString& raw);
    /// "auto": the driver picks the GPU.
    static QString defaultGpuDevice();
    /// "auto" or a well-formed lowercase "vendor:device" hex pair; anything
    /// else, hand-edited garbage included, is "auto", so startup never acts on
    /// a string DRI_PRIME or the Vulkan matcher cannot parse.
    static QString normalizeGpuDevice(const QString& raw);

    /// The Rendering group read straight from config.json, without a backend
    /// or a QCoreApplication: plasmazonesd pins its GPU and graphics API
    /// before QGuiApplication exists. An absent, unreadable or unparseable
    /// file, or a missing key (the stored form of the default), gives the
    /// default; the legacy INI is migration input only and is never read.
    /// A v1 config.json keeps its backend under Rendering/RenderingBackend,
    /// so read the backend after ConfigMigration::ensureJsonConfig(). The GPU
    /// pin is in no migration and can be read before it.
    struct RenderingBootConfig
    {
        QString backend;
        QString gpuDevice;
    };
    static RenderingBootConfig readRenderingConfigFromDisk();

Q_SIGNALS:
    void workspacesManageKWinPerOutputChanged();
    void workspacesRebindKWinShortcutsChanged();
    void renderingBackendChanged();
    void gpuDeviceChanged();

private:
    Settings* m_settings;
};

} // namespace PlasmaZones
