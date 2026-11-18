// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_dbus_export.h"

#include <PhosphorProtocol/ZoneMarshalling.h>

#include <QDBusAbstractAdaptor>
#include <QPointer>
#include <QString>
#include <QStringList>

namespace PlasmaZones {

class ZoneQueryService;

/**
 * @brief The D-Bus binding of ZoneQueryService: org.phosphor_works.Placement.ZoneDetection
 *
 * Every method and signal here is one the interface XML names, with the
 * signature it pins; each method's behaviour is documented on the
 * ZoneQueryService method of the same name. Nothing here is compositor-only,
 * so the binding only forwards and relays zoneDetected.
 */
class PLASMAZONES_DBUS_EXPORT ZoneDetectionAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phosphor_works.Placement.ZoneDetection")

public:
    /// @param service Not owned. A call that arrives after it is gone is answered empty.
    /// @param parent The object the adaptor is exported through
    ZoneDetectionAdaptor(ZoneQueryService* service, QObject* parent);
    ~ZoneDetectionAdaptor() override;

public Q_SLOTS:
    QString detectZoneAtPosition(int x, int y);
    QStringList detectMultiZoneAtPosition(int x, int y);
    PhosphorProtocol::ZoneGeometryRect getZoneGeometry(const QString& zoneId);
    PhosphorProtocol::ZoneGeometryRect getZoneGeometryForScreen(const QString& zoneId, const QString& screenId);
    QStringList getZonesForScreen(const QString& screenId);
    Q_INVOKABLE QString getAdjacentZone(const QString& currentZoneId, const QString& direction,
                                        const QString& screenId = QString()) const;
    Q_INVOKABLE QString getFirstZoneInDirection(const QString& direction, const QString& screenId = QString()) const;
    QString getZoneByNumber(int zoneNumber, const QString& screenId = QString());
    PhosphorProtocol::NamedZoneGeometryList getAllZoneGeometries(const QString& screenId = QString());
    int getKeyboardModifiers();
    QString detectZoneWithModifiers(int x, int y);

Q_SIGNALS:
    void zoneDetected(const QString& zoneId, const PhosphorProtocol::ZoneGeometryRect& geometry);

private:
    QPointer<ZoneQueryService> m_service;
};

} // namespace PlasmaZones
