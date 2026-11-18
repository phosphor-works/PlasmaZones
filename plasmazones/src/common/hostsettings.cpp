// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hostsettings.h"
#include "common/engineruntimenamespace.h"

#include "phosphor_i18n.h"

#include "config/configdefaults.h"
#include "config/settings.h"
#include "config/settingsschema.h"
#include "config/settingsschemachoices.h"
#include "core/platform/appdirectoryname.h"
#include "core/types/constants.h"

#include <PhosphorConfig/Schema.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

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
    // Each key coerces on read and write, so a hand-edited config cannot
    // persist a value startup would act on.
    schema.groups[HostSettings::renderingGroup()] = {
        {HostSettings::backendKey(), HostSettings::defaultRenderingBackend(), QMetaType::QString,
         QStringLiteral("Graphics API used for overlay rendering."),
         [](const QVariant& v) {
             return QVariant(HostSettings::normalizeRenderingBackend(v.toString()));
         },
         tokenChoices(HostSettings::renderingBackendOptions())},
        // A free string, not a token enum: the legal values are the machine's
        // GPUs, so there are no declared choices (the picker enumerates DRM
        // render nodes at runtime).
        {HostSettings::gpuKey(), HostSettings::defaultGpuDevice(), QMetaType::QString,
         QStringLiteral("GPU that draws the zone overlays and on-screen displays. Automatic lets the graphics driver "
                        "decide. KWin composites window contents, so those are unaffected."),
         [](const QVariant& v) {
             return QVariant(HostSettings::normalizeGpuDevice(v.toString()));
         }},
    };
}

/// The GPU pin names hardware another machine cannot satisfy. The consent
/// this machine's user gave for PlasmaZones to write KWin's
/// PerOutputVirtualDesktops key must never be granted by an import from
/// elsewhere, nor silently withdrawn by a profile switch.
bool isHostMachineScopedKey(const QString& group, const QString& key)
{
    if (group == HostSettings::renderingGroup()) {
        return key == HostSettings::gpuKey();
    }
    return group == ConfigDefaults::workspacesBehaviorGroup() && key == HostSettings::manageKWinPerOutputKey();
}

QList<HostEnumLabels> hostEnumLabels()
{
    return {
        {HostSettings::renderingGroup(),
         HostSettings::backendKey(),
         {
             {QStringLiteral("auto"), PhosphorI18n::tr("Automatic")},
             {QStringLiteral("vulkan"), PhosphorI18n::tr("Vulkan")},
             {QStringLiteral("opengl"), PhosphorI18n::tr("OpenGL")},
         }},
    };
}

} // namespace

void initPlasmaZonesHost()
{
    setAppDirectoryName(AppDirectoryName);
    setHostSettingsExtension({&appendHostSchema, &isHostMachineScopedKey, &hostEnumLabels});
}

HostSettings::HostSettings(Settings* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
    connect(m_settings, &Settings::hostValueChanged, this, [this](const QString& group, const QString& key) {
        if (group == renderingGroup()) {
            if (key == backendKey()) {
                Q_EMIT renderingBackendChanged();
            } else if (key == gpuKey()) {
                Q_EMIT gpuDeviceChanged();
            }
        } else if (group == ConfigDefaults::workspacesBehaviorGroup()) {
            if (key == manageKWinPerOutputKey()) {
                Q_EMIT workspacesManageKWinPerOutputChanged();
            } else if (key == rebindKWinDesktopShortcutsKey()) {
                Q_EMIT workspacesRebindKWinShortcutsChanged();
            }
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

QString HostSettings::renderingBackend() const
{
    return m_settings->hostValue(renderingGroup(), backendKey()).toString();
}

void HostSettings::setRenderingBackend(const QString& backend)
{
    m_settings->setHostValue(renderingGroup(), backendKey(), backend);
}

QString HostSettings::gpuDevice() const
{
    return m_settings->hostValue(renderingGroup(), gpuKey()).toString();
}

void HostSettings::setGpuDevice(const QString& gpu)
{
    m_settings->setHostValue(renderingGroup(), gpuKey(), gpu);
}

QString HostSettings::renderingGroup()
{
    return QStringLiteral("Rendering");
}

QString HostSettings::backendKey()
{
    return QStringLiteral("Backend");
}

QString HostSettings::gpuKey()
{
    return QStringLiteral("Gpu");
}

QString HostSettings::defaultRenderingBackend()
{
    return QStringLiteral("auto");
}

const QStringList& HostSettings::renderingBackendOptions()
{
    static const QStringList options = {
        QStringLiteral("auto"),
        QStringLiteral("vulkan"),
        QStringLiteral("opengl"),
    };
    return options;
}

QString HostSettings::normalizeRenderingBackend(const QString& raw)
{
    const QString normalized = raw.toLower().trimmed();
    return renderingBackendOptions().contains(normalized) ? normalized : defaultRenderingBackend();
}

QString HostSettings::defaultGpuDevice()
{
    return QStringLiteral("auto");
}

QString HostSettings::normalizeGpuDevice(const QString& raw)
{
    const QString normalized = raw.toLower().trimmed();
    if (normalized.isEmpty() || normalized == defaultGpuDevice()) {
        return defaultGpuDevice();
    }
    // Exactly four hex digits per field: sysfs prints PCI ids as 0x%04x and
    // GpuDeviceList emits them 0x-stripped, so a shorter form never comes from
    // a legitimate producer. anchoredPattern (\A...\z) keeps the anchoring
    // independent of the trim above (PCRE '$' alone would still match before a
    // trailing newline).
    static const QRegularExpression pciPair(
        QRegularExpression::anchoredPattern(QStringLiteral("[0-9a-f]{4}:[0-9a-f]{4}")));
    return pciPair.match(normalized).hasMatch() ? normalized : defaultGpuDevice();
}

HostSettings::RenderingBootConfig HostSettings::readRenderingConfigFromDisk()
{
    // Only the Rendering group is needed, so no backend is built (which would
    // parse and track the whole file), and the file is parsed once for both.
    RenderingBootConfig out{defaultRenderingBackend(), defaultGpuDevice()};
    QFile file(ConfigDefaults::configFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return out;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return out;
    }
    const QJsonObject rendering = doc.object().value(renderingGroup()).toObject();
    if (rendering.contains(backendKey())) {
        out.backend = normalizeRenderingBackend(rendering.value(backendKey()).toString(defaultRenderingBackend()));
    }
    if (rendering.contains(gpuKey())) {
        out.gpuDevice = normalizeGpuDevice(rendering.value(gpuKey()).toString(defaultGpuDevice()));
    }
    return out;
}

} // namespace PlasmaZones
