// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settingsadaptor.h"
#include "common/engineruntimenamespace.h"

#include "dbushelpers.h"
#include "core/utils/dbusvariantutils.h"

#include "runtime/services/settingsservice/settingsservice.h"

namespace PlasmaZones {

namespace {

/// Each value of @p values with a QDBusArgument from the wire unwrapped to
/// plain Qt types, as DBusVariantUtils::convertDbusArgument does.
QVariantMap unwrapValues(const QVariantMap& values)
{
    QVariantMap out;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        out.insert(it.key(), DBusVariantUtils::convertDbusArgument(it.value()));
    }
    return out;
}

} // namespace

SettingsAdaptor::SettingsAdaptor(SettingsService* service, QObject* parent)
    : QDBusAbstractAdaptor(parent)
    , m_service(service)
{
    Q_ASSERT(service);
    connect(service, &SettingsService::settingsChanged, this, &SettingsAdaptor::settingsChanged);
    connect(service, &SettingsService::editorSettingsChanged, this, &SettingsAdaptor::editorSettingsChanged);
    connect(service, &SettingsService::motionProfileTreeChanged, this, &SettingsAdaptor::motionProfileTreeChanged);
    connect(service, &SettingsService::sessionIdleChanged, this, &SettingsAdaptor::sessionIdleChanged);
    connect(service, &SettingsService::runningWindowsRequested, this, &SettingsAdaptor::runningWindowsRequested);
    connect(service, &SettingsService::runningWindowsAvailable, this, &SettingsAdaptor::runningWindowsAvailable);
}

SettingsAdaptor::~SettingsAdaptor() = default;

void SettingsAdaptor::reloadSettings()
{
    if (m_service) {
        m_service->reloadSettings();
    }
}

void SettingsAdaptor::saveSettings()
{
    if (m_service) {
        m_service->saveSettings();
    }
}

void SettingsAdaptor::resetToDefaults()
{
    if (m_service) {
        m_service->resetToDefaults();
    }
}

QString SettingsAdaptor::getAllSettings()
{
    return m_service ? m_service->getAllSettings() : QString{};
}

QVariantMap SettingsAdaptor::getSettings(const QStringList& keys)
{
    return m_service ? m_service->getSettings(keys) : QVariantMap{};
}

bool SettingsAdaptor::setSettings(const QVariantMap& settings)
{
    // Complex values (QVariantList of QVariantMaps, e.g. dragActivationTriggers)
    // arrive from D-Bus as QDBusArgument; the service takes plain Qt types.
    return m_service ? m_service->setSettings(unwrapValues(settings)) : false;
}

QStringList SettingsAdaptor::getSettingKeys()
{
    return m_service ? m_service->getSettingKeys() : QStringList{};
}

QVariantMap SettingsAdaptor::getPerScreenSettings(const QString& screenId, const QString& category)
{
    return m_service ? m_service->getPerScreenSettings(screenId, category) : QVariantMap{};
}

bool SettingsAdaptor::setPerScreenSettings(const QString& screenId, const QString& category, const QVariantMap& values)
{
    // Values arriving over the wire can be QDBusArgument-wrapped when they
    // contain lists or maps; the service takes plain Qt types.
    return m_service ? m_service->setPerScreenSettings(screenId, category, unwrapValues(values)) : false;
}

void SettingsAdaptor::requestRunningWindows()
{
    if (m_service) {
        m_service->requestRunningWindows();
    }
}

void SettingsAdaptor::provideRunningWindows(const QString& json)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->provideRunningWindows(json);
    }
}

QString SettingsAdaptor::getSettingSchema(const QString& key)
{
    return m_service ? m_service->getSettingSchema(key) : QString{};
}

QString SettingsAdaptor::getAllSettingSchemas()
{
    return m_service ? m_service->getAllSettingSchemas() : QString{};
}

QDBusVariant SettingsAdaptor::getSetting(const QString& key)
{
    if (!m_service) {
        return QDBusVariant(QVariant());
    }
    // An empty or unknown key is a real D-Bus error, not an empty value: a
    // caller that coerced an empty string would silently ship the
    // default-inverted value (see SettingsService::getSetting). A direct,
    // non-D-Bus call gets an invalid variant, the honest answer.
    QString error;
    const std::optional<QVariant> value = m_service->getSetting(key, &error);
    if (!value) {
        DbusHelpers::sendBusError(this, QDBusError::InvalidArgs, error);
        return QDBusVariant(QVariant());
    }
    return QDBusVariant(*value);
}

bool SettingsAdaptor::setSetting(const QString& key, const QDBusVariant& value)
{
    return m_service ? m_service->setSetting(key, DBusVariantUtils::convertDbusArgument(value.variant())) : false;
}

void SettingsAdaptor::setPerScreenSetting(const QString& screenId, const QString& category, const QString& key,
                                          const QDBusVariant& value)
{
    if (!m_service) {
        return;
    }
    // A void method: a refusal is raised as a D-Bus error, or the caller could
    // not tell a dropped write from an applied one. Per-screen values are
    // scalars, so the unwrap is a no-op for every payload the settings pages
    // send, and it keeps this path on the batch twin's conversion policy.
    QString rejection;
    if (!m_service->setPerScreenSetting(screenId, category, key, DBusVariantUtils::convertDbusArgument(value.variant()),
                                        &rejection)) {
        DbusHelpers::sendBusError(this, QDBusError::InvalidArgs, rejection);
    }
}

void SettingsAdaptor::clearPerScreenSettings(const QString& screenId, const QString& category)
{
    if (!m_service) {
        return;
    }
    QString rejection;
    if (!m_service->clearPerScreenSettings(screenId, category, &rejection)) {
        DbusHelpers::sendBusError(this, QDBusError::InvalidArgs, rejection);
    }
}

} // namespace PlasmaZones
