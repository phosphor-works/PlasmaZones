// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/engineruntimenamespace.h"

#include "plasmazones_dbus_export.h"

#include <QDBusAbstractAdaptor>
#include <QDBusVariant>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace PhosphorEngineRuntime {
class SettingsService;
} // namespace PhosphorEngineRuntime

namespace PlasmaZones {

/**
 * @brief The D-Bus binding of SettingsService: org.phosphor_works.Placement.Settings
 *
 * Every method, property and signal here is one the interface XML names, with
 * the signature it pins; each one's behaviour is documented on the
 * SettingsService member of the same name. The binding forwards and relays
 * the service's signals.
 *
 * It converts between the wire's QDBusVariant and QDBusArgument values and the plain QVariant the service takes, raises
 * the service's refusals as InvalidArgs errors, and admits provideRunningWindows only from the registered bridge
 * (#1155).
 */
class PLASMAZONES_DBUS_EXPORT SettingsAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phosphor_works.Placement.Settings")

public:
    /// @param service Not owned. A call that arrives after it is gone is answered empty.
    /// @param parent The object the adaptor is exported through
    SettingsAdaptor(SettingsService* service, QObject* parent);
    ~SettingsAdaptor() override;

public Q_SLOTS:
    QDBusVariant getSetting(const QString& key);
    bool setSetting(const QString& key, const QDBusVariant& value);
    void setPerScreenSetting(const QString& screenId, const QString& category, const QString& key,
                             const QDBusVariant& value);
    void clearPerScreenSettings(const QString& screenId, const QString& category);
    void reloadSettings();
    void saveSettings();
    void resetToDefaults();
    QString getAllSettings();
    QVariantMap getSettings(const QStringList& keys);
    bool setSettings(const QVariantMap& settings);
    QStringList getSettingKeys();
    QVariantMap getPerScreenSettings(const QString& screenId, const QString& category);
    bool setPerScreenSettings(const QString& screenId, const QString& category, const QVariantMap& values);
    void requestRunningWindows();
    void provideRunningWindows(const QString& json);
    QString getSettingSchema(const QString& key);
    QString getAllSettingSchemas();

Q_SIGNALS:
    void settingsChanged();
    void editorSettingsChanged();
    void motionProfileTreeChanged();
    void sessionIdleChanged(bool idle);
    void runningWindowsRequested();
    void runningWindowsAvailable(const QString& json);

private:
    QPointer<SettingsService> m_service;
};

} // namespace PlasmaZones
