// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ruleadaptor.h"
#include "common/engineruntimenamespace.h"

#include "runtime/services/ruleservice.h"

namespace PlasmaZones {

RuleAdaptor::RuleAdaptor(RuleService* service, QObject* parent)
    : QDBusAbstractAdaptor(parent)
    , m_service(service)
{
    Q_ASSERT(service);
    connect(service, &RuleService::rulesChanged, this, &RuleAdaptor::rulesChanged);
}

RuleAdaptor::~RuleAdaptor() = default;

QString RuleAdaptor::getAllRules()
{
    return m_service ? m_service->getAllRules() : QString{};
}

bool RuleAdaptor::setAllRules(const QString& rulesJson)
{
    return m_service ? m_service->setAllRules(rulesJson) : bool{};
}

bool RuleAdaptor::addRule(const QString& ruleJson)
{
    return m_service ? m_service->addRule(ruleJson) : bool{};
}

bool RuleAdaptor::updateRule(const QString& ruleJson)
{
    return m_service ? m_service->updateRule(ruleJson) : bool{};
}

bool RuleAdaptor::removeRule(const QString& ruleId)
{
    return m_service ? m_service->removeRule(ruleId) : bool{};
}

bool RuleAdaptor::setRuleEnabled(const QString& ruleId, bool enabled)
{
    return m_service ? m_service->setRuleEnabled(ruleId, enabled) : bool{};
}

bool RuleAdaptor::setRulePriority(const QString& ruleId, int priority)
{
    return m_service ? m_service->setRulePriority(ruleId, priority) : bool{};
}

void RuleAdaptor::resetManagedDefaults()
{
    if (m_service) {
        m_service->resetManagedDefaults();
    }
}

void RuleAdaptor::reloadRules()
{
    if (m_service) {
        m_service->reloadRules();
    }
}

} // namespace PlasmaZones
