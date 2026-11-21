// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/engineruntimenamespace.h"

#include "plasmazones_dbus_export.h"

#include <PhosphorProtocol/NavigationMarshalling.h>
#include <PhosphorProtocol/WindowMarshalling.h>

#include <QDBusAbstractAdaptor>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace PhosphorEngineRuntime {
class SnapService;
} // namespace PhosphorEngineRuntime

namespace PlasmaZones {

/**
 * @brief The D-Bus binding of SnapService: org.phosphor_works.Placement.Snap
 *
 * Every method, property and signal here is one the interface XML names, with
 * the signature it pins; each one's behaviour is documented on the
 * SnapService member of the same name. The binding forwards and relays
 * the service's signals.
 *
 * Its compositor-only members admit only the registered bridge (DbusHelpers::admitCompositorReport, #1155), and
 * handleBatchedResnap admits restore entries only from the bridge or a direct call.
 */
class PLASMAZONES_DBUS_EXPORT SnapAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phosphor_works.Placement.Snap")

public:
    /// @param service Not owned. A call that arrives after it is gone is answered empty.
    /// @param parent The object the adaptor is exported through
    SnapAdaptor(SnapService* service, QObject* parent);
    ~SnapAdaptor() override;

public Q_SLOTS:
    void windowSnapped(const QString& windowId, const QString& zoneId, const QString& screenId);
    void windowSnappedMultiZone(const QString& windowId, const QStringList& zoneIds, const QString& screenId);
    void windowUnsnapped(const QString& windowId);
    void windowsSnappedBatch(const PhosphorProtocol::SnapConfirmationList& entries);
    void recordSnapIntent(const QString& windowId, bool wasUserInitiated);
    void snapToLastZone(const QString& windowId, const QString& windowScreenId, bool sticky, int& snapX, int& snapY,
                        int& snapWidth, int& snapHeight, bool& shouldSnap);
    void snapToAppRule(const QString& windowId, const QString& windowScreenId, bool sticky, int& snapX, int& snapY,
                       int& snapWidth, int& snapHeight, bool& shouldSnap);
    void snapToEmptyZone(const QString& windowId, const QString& windowScreenId, bool sticky, int& snapX, int& snapY,
                         int& snapWidth, int& snapHeight, bool& shouldSnap);
    void resolveWindowRestore(const QString& windowId, const QString& screenId, bool sticky, int restoreReason,
                              int& snapX, int& snapY, int& snapWidth, int& snapHeight, bool& shouldSnap);
    void resnapToNewLayout();
    void resnapCurrentAssignments(const QString& screenFilter = QString());
    void resnapFromAutotileOrder(const QStringList& autotileWindowOrder, const QString& screenId);
    PhosphorProtocol::SnapAllResultList calculateSnapAllWindows(const QStringList& windowIds, const QString& screenId);
    void snapAllWindows(const QString& screenId);
    void handleBatchedResnap(const QString& resnapData);
    void moveWindowToAdjacentZone(const QString& direction);
    void focusAdjacentZone(const QString& direction);
    void pushToEmptyZone(const QString& screenId = QString());
    void restoreWindowSize();
    void swapWindowWithAdjacentZone(const QString& direction);
    void snapToZoneByNumber(int zoneNumber, const QString& screenId = QString());
    void rotateWindowsInLayout(bool clockwise, const QString& screenId = QString());
    void cycleWindowsInZone(bool forward);
    void moveWindowToZone(const QString& windowId, const QString& zoneId);
    void swapWindowsById(const QString& windowId1, const QString& windowId2);
    void toggleFloatForWindow(const QString& windowId, const QString& screenId);
    void setWindowFloat(const QString& windowId, bool floating);
    PhosphorProtocol::UnfloatRestoreResult calculateUnfloatRestore(const QString& windowId, const QString& screenId);
    void windowUnsnappedForFloat(const QString& windowId);

Q_SIGNALS:

private:
    QPointer<SnapService> m_service;
};

} // namespace PlasmaZones
