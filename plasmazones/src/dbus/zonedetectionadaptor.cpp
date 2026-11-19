// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "zonedetectionadaptor.h"
#include "common/engineruntimenamespace.h"

#include <PhosphorEngineRuntime/ZoneQueryService.h>

namespace PlasmaZones {

ZoneDetectionAdaptor::ZoneDetectionAdaptor(ZoneQueryService* service, QObject* parent)
    : QDBusAbstractAdaptor(parent)
    , m_service(service)
{
    Q_ASSERT(service);
    connect(service, &ZoneQueryService::zoneDetected, this, &ZoneDetectionAdaptor::zoneDetected);
}

ZoneDetectionAdaptor::~ZoneDetectionAdaptor() = default;

QString ZoneDetectionAdaptor::detectZoneAtPosition(int x, int y)
{
    return m_service ? m_service->detectZoneAtPosition(x, y) : QString();
}

QStringList ZoneDetectionAdaptor::detectMultiZoneAtPosition(int x, int y)
{
    return m_service ? m_service->detectMultiZoneAtPosition(x, y) : QStringList();
}

PhosphorProtocol::ZoneGeometryRect ZoneDetectionAdaptor::getZoneGeometry(const QString& zoneId)
{
    return m_service ? m_service->getZoneGeometry(zoneId) : PhosphorProtocol::ZoneGeometryRect{};
}

PhosphorProtocol::ZoneGeometryRect ZoneDetectionAdaptor::getZoneGeometryForScreen(const QString& zoneId,
                                                                                  const QString& screenId)
{
    return m_service ? m_service->getZoneGeometryForScreen(zoneId, screenId) : PhosphorProtocol::ZoneGeometryRect{};
}

QStringList ZoneDetectionAdaptor::getZonesForScreen(const QString& screenId)
{
    return m_service ? m_service->getZonesForScreen(screenId) : QStringList();
}

QString ZoneDetectionAdaptor::getAdjacentZone(const QString& currentZoneId, const QString& direction,
                                              const QString& screenId) const
{
    return m_service ? m_service->getAdjacentZone(currentZoneId, direction, screenId) : QString();
}

QString ZoneDetectionAdaptor::getFirstZoneInDirection(const QString& direction, const QString& screenId) const
{
    return m_service ? m_service->getFirstZoneInDirection(direction, screenId) : QString();
}

QString ZoneDetectionAdaptor::getZoneByNumber(int zoneNumber, const QString& screenId)
{
    return m_service ? m_service->getZoneByNumber(zoneNumber, screenId) : QString();
}

PhosphorProtocol::NamedZoneGeometryList ZoneDetectionAdaptor::getAllZoneGeometries(const QString& screenId)
{
    return m_service ? m_service->getAllZoneGeometries(screenId) : PhosphorProtocol::NamedZoneGeometryList();
}

int ZoneDetectionAdaptor::getKeyboardModifiers()
{
    return m_service ? m_service->getKeyboardModifiers() : 0;
}

QString ZoneDetectionAdaptor::detectZoneWithModifiers(int x, int y)
{
    return m_service ? m_service->detectZoneWithModifiers(x, y) : QString();
}

} // namespace PlasmaZones
