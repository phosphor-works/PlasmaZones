// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/engineruntimenamespace.h"

#include "plasmazones_dbus_export.h"

#include <PhosphorProtocol/AutotileMarshalling.h>

#include <QDBusAbstractAdaptor>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace PhosphorEngineRuntime {
class AutotileService;
} // namespace PhosphorEngineRuntime

namespace PlasmaZones {

/**
 * @brief The D-Bus binding of AutotileService: org.phosphor_works.Placement.Autotile
 *
 * Every method, property and signal here is one the interface XML names, with
 * the signature it pins; each one's behaviour is documented on the
 * AutotileService member of the same name. The binding forwards and relays
 * the service's signals.
 */
class PLASMAZONES_DBUS_EXPORT AutotileAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phosphor_works.Placement.Autotile")
    Q_PROPERTY(QString algorithm READ algorithm WRITE setAlgorithm NOTIFY algorithmChanged)
    Q_PROPERTY(double masterRatio READ masterRatio WRITE setMasterRatio NOTIFY configChanged)
    Q_PROPERTY(int masterCount READ masterCount WRITE setMasterCount NOTIFY configChanged)
    Q_PROPERTY(int innerGap READ innerGap NOTIFY configChanged)
    Q_PROPERTY(int outerGap READ outerGap NOTIFY configChanged)
    Q_PROPERTY(bool smartGaps READ smartGaps WRITE setSmartGaps NOTIFY configChanged)
    Q_PROPERTY(bool focusNewWindows READ focusNewWindows WRITE setFocusNewWindows NOTIFY configChanged)

public:
    /// @param service Not owned. A call that arrives after it is gone is answered empty.
    /// @param parent The object the adaptor is exported through
    AutotileAdaptor(AutotileService* service, QObject* parent);
    ~AutotileAdaptor() override;

    QString algorithm() const;
    void setAlgorithm(const QString& value);
    double masterRatio() const;
    void setMasterRatio(double value);
    int masterCount() const;
    void setMasterCount(int value);
    int innerGap() const;
    int outerGap() const;
    bool smartGaps() const;
    void setSmartGaps(bool value);
    bool focusNewWindows() const;
    void setFocusNewWindows(bool value);

public Q_SLOTS:
    void swapWindows(const QString& windowId1, const QString& windowId2);
    void promoteToMaster(const QString& windowId);
    void demoteFromMaster(const QString& windowId);
    bool toggleMaximizeToEdges(const QString& screenId, const QString& windowId);
    void focusMaster();
    void focusNext();
    void focusPrevious();
    void increaseMasterRatio(double delta);
    void decreaseMasterRatio(double delta);
    void increaseMasterCount();
    void decreaseMasterCount();
    QStringList availableAlgorithms();
    PhosphorProtocol::AlgorithmInfoEntry algorithmInfo(const QString& algorithmId);

Q_SIGNALS:
    void algorithmChanged(const QString& algorithmId);
    void configChanged();

private:
    QPointer<AutotileService> m_service;
};

} // namespace PlasmaZones
