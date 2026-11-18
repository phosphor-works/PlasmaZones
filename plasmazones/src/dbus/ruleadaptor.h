// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_dbus_export.h"

#include <QDBusAbstractAdaptor>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace PlasmaZones {

class RuleService;

/**
 * @brief The D-Bus binding of RuleService: org.phosphor_works.Placement.Rules
 *
 * Every method, property and signal here is one the interface XML names, with
 * the signature it pins; each one's behaviour is documented on the
 * RuleService member of the same name. The binding forwards and relays
 * the service's signals.
 */
class PLASMAZONES_DBUS_EXPORT RuleAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phosphor_works.Placement.Rules")

public:
    /// @param service Not owned. A call that arrives after it is gone is answered empty.
    /// @param parent The object the adaptor is exported through
    RuleAdaptor(RuleService* service, QObject* parent);
    ~RuleAdaptor() override;

public Q_SLOTS:
    QString getAllRules();
    bool setAllRules(const QString& rulesJson);
    bool addRule(const QString& ruleJson);
    bool updateRule(const QString& ruleJson);
    bool removeRule(const QString& ruleId);
    bool setRuleEnabled(const QString& ruleId, bool enabled);
    bool setRulePriority(const QString& ruleId, int priority);
    void resetManagedDefaults();
    void reloadRules();

Q_SIGNALS:
    void rulesChanged(bool persisted);

private:
    QPointer<RuleService> m_service;
};

} // namespace PlasmaZones
