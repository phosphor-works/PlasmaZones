// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "autotileadaptor.h"
#include "common/engineruntimenamespace.h"

#include <PhosphorEngineRuntime/AutotileService.h>

namespace PlasmaZones {

AutotileAdaptor::AutotileAdaptor(AutotileService* service, QObject* parent)
    : QDBusAbstractAdaptor(parent)
    , m_service(service)
{
    Q_ASSERT(service);
    connect(service, &AutotileService::algorithmChanged, this, &AutotileAdaptor::algorithmChanged);
    connect(service, &AutotileService::configChanged, this, &AutotileAdaptor::configChanged);
}

AutotileAdaptor::~AutotileAdaptor() = default;

QString AutotileAdaptor::algorithm() const
{
    return m_service ? m_service->algorithm() : QString{};
}

void AutotileAdaptor::setAlgorithm(const QString& value)
{
    if (m_service) {
        m_service->setAlgorithm(value);
    }
}

double AutotileAdaptor::masterRatio() const
{
    return m_service ? m_service->masterRatio() : double{};
}

void AutotileAdaptor::setMasterRatio(double value)
{
    if (m_service) {
        m_service->setMasterRatio(value);
    }
}

int AutotileAdaptor::masterCount() const
{
    return m_service ? m_service->masterCount() : int{};
}

void AutotileAdaptor::setMasterCount(int value)
{
    if (m_service) {
        m_service->setMasterCount(value);
    }
}

int AutotileAdaptor::innerGap() const
{
    return m_service ? m_service->innerGap() : int{};
}

int AutotileAdaptor::outerGap() const
{
    return m_service ? m_service->outerGap() : int{};
}

bool AutotileAdaptor::smartGaps() const
{
    return m_service ? m_service->smartGaps() : bool{};
}

void AutotileAdaptor::setSmartGaps(bool value)
{
    if (m_service) {
        m_service->setSmartGaps(value);
    }
}

bool AutotileAdaptor::focusNewWindows() const
{
    return m_service ? m_service->focusNewWindows() : bool{};
}

void AutotileAdaptor::setFocusNewWindows(bool value)
{
    if (m_service) {
        m_service->setFocusNewWindows(value);
    }
}

void AutotileAdaptor::swapWindows(const QString& windowId1, const QString& windowId2)
{
    if (m_service) {
        m_service->swapWindows(windowId1, windowId2);
    }
}

void AutotileAdaptor::promoteToMaster(const QString& windowId)
{
    if (m_service) {
        m_service->promoteToMaster(windowId);
    }
}

bool AutotileAdaptor::toggleMaximizeToEdges(const QString& screenId, const QString& windowId)
{
    return m_service ? m_service->toggleMaximizeToEdges(screenId, windowId) : false;
}

void AutotileAdaptor::demoteFromMaster(const QString& windowId)
{
    if (m_service) {
        m_service->demoteFromMaster(windowId);
    }
}

void AutotileAdaptor::focusMaster()
{
    if (m_service) {
        m_service->focusMaster();
    }
}

void AutotileAdaptor::focusNext()
{
    if (m_service) {
        m_service->focusNext();
    }
}

void AutotileAdaptor::focusPrevious()
{
    if (m_service) {
        m_service->focusPrevious();
    }
}

void AutotileAdaptor::increaseMasterRatio(double delta)
{
    if (m_service) {
        m_service->increaseMasterRatio(delta);
    }
}

void AutotileAdaptor::decreaseMasterRatio(double delta)
{
    if (m_service) {
        m_service->decreaseMasterRatio(delta);
    }
}

void AutotileAdaptor::increaseMasterCount()
{
    if (m_service) {
        m_service->increaseMasterCount();
    }
}

void AutotileAdaptor::decreaseMasterCount()
{
    if (m_service) {
        m_service->decreaseMasterCount();
    }
}

QStringList AutotileAdaptor::availableAlgorithms()
{
    return m_service ? m_service->availableAlgorithms() : QStringList{};
}

PhosphorProtocol::AlgorithmInfoEntry AutotileAdaptor::algorithmInfo(const QString& algorithmId)
{
    return m_service ? m_service->algorithmInfo(algorithmId) : PhosphorProtocol::AlgorithmInfoEntry{};
}

} // namespace PlasmaZones
