// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_dbus_export.h"

#include <PhosphorProtocol/AutotileMarshalling.h>
#include <PhosphorProtocol/WindowMarshalling.h>
#include <PhosphorProtocol/ZoneMarshalling.h>

#include <QDBusAbstractAdaptor>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

namespace PlasmaZones {

class WindowTracker;

/**
 * @brief The D-Bus binding of WindowTracker: org.phosphor_works.Placement.WindowTracking
 *
 * Every method and signal here is one the interface XML names, with the
 * signature it pins; each method's behaviour is documented on the
 * WindowTracker method of the same name. The binding owns what only the bus
 * has: it admits the compositor-only reports (DbusHelpers::admitCompositorReport,
 * #1155), tells setWindowFloating and setWindowFloatingForScreen whether their
 * caller is external, and unwraps the QDBusArgument lists of setWindowMetadata's
 * extended map. It relays WindowTracker's signals onto the bus unchanged.
 *
 * Exported through its parent, which carries the QDBusContext the admission
 * check reads.
 */
class PLASMAZONES_DBUS_EXPORT WindowTrackingAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phosphor_works.Placement.WindowTracking")

public:
    /// @param tracker Not owned. A call that arrives after it is gone is answered empty.
    /// @param parent The object the adaptor is exported through
    WindowTrackingAdaptor(WindowTracker* tracker, QObject* parent);
    ~WindowTrackingAdaptor() override;

    /// Whether a unique bus name is the registered compositor bridge's, whose screen-form float is not gated (F1108).
    void setBridgeCallerCheck(std::function<bool(const QString&)> check);

public Q_SLOTS:
    void setWindowMetadata(const QString& instanceId, const QString& appId, const QString& desktopFile,
                           const QString& title, const QString& windowRole, int pid, int virtualDesktop,
                           const QString& activity, int windowType, const QVariantMap& extended);
    void notifyDragOutUnsnap(const QString& windowId);
    void windowScreenChanged(const QString& windowId, const QString& newScreenId);
    void windowCrossedScreens(const QString& windowId, const QString& fromScreenId, const QString& toScreenId);
    PhosphorProtocol::OutputSettleVerdictList reportOutputSettle(const PhosphorProtocol::OutputSettleRowList& rows);
    void setWindowSticky(const QString& windowId, bool sticky);
    bool getPreFloatZone(const QString& windowId, QString& zoneId);
    void clearPreFloatZone(const QString& windowId);
    void storePreTileGeometry(const QString& windowId, int x, int y, int width, int height, const QString& screenId,
                              bool overwrite);
    bool hasPreTileGeometry(const QString& windowId);
    void clearPreTileGeometry(const QString& windowId);
    PhosphorProtocol::PreTileGeometryList getPreTileGeometries();
    void windowClosed(const QString& windowId, int windowKind, const QString& screenId = QString());
    void windowActivated(const QString& windowId, const QString& screenId);
    void setFrameGeometry(const QString& windowId, int x, int y, int width, int height);
    void notifyWindowResized(const QString& windowId, int oldX, int oldY, int oldWidth, int oldHeight, int newX,
                             int newY, int newWidth, int newHeight);
    void cursorScreenChanged(const QString& screenId);
    void activeWindowScreenChanged(const QString& windowId, const QString& screenId);
    void screenDesktopChanged(const QString& screenId, int desktop);
    void seedScreenDesktop(const QString& screenId, int desktop);
    void reportPerOutputDesktopsMode(bool enabled);
    void reportNavigationFeedback(bool success, const QString& action, const QString& reason,
                                  const QString& sourceZoneId, const QString& targetZoneId, const QString& screenId);
    bool getValidatedPreTileGeometry(const QString& windowId, int& x, int& y, int& width, int& height);
    QString getZoneForWindow(const QString& windowId);
    QStringList getMultiZoneForWindow(const QString& windowId);
    QStringList getWindowsInZone(const QString& zoneId);
    QStringList getSnappedWindows();
    void pruneStaleWindows(const QStringList& aliveWindowIds);
    void reapplyWindowAppearance();
    PhosphorProtocol::EmptyZoneList getEmptyZones(const QString& screenId);
    QString getLastUsedZoneId();
    PhosphorProtocol::WindowGeometryList getUpdatedWindowGeometries();
    QString getPendingRestoreGeometries();
    PhosphorProtocol::WindowStateEntry getWindowState(const QString& windowId);
    PhosphorProtocol::WindowStateList getAllWindowStates();
    void activateWindow(const QString& windowId);
    QString getWindowMetadata(const QString& windowId, QString& title, QString& desktopFile);
    void moveWindowToDesktop(const QString& windowId, int desktop);
    QStringList getUrgentWindows();
    PhosphorProtocol::WindowStateList getWindowStatesForDesktop(const QString& screenId, int virtualDesktop);
    QString findWindowByPid(int pid);
    bool isWindowFloating(const QString& windowId);
    bool queryWindowFloating(const QString& windowId);
    void setWindowFloating(const QString& windowId, bool floating);
    QStringList getFloatingWindows();
    PhosphorProtocol::ZoneGeometryRect getZoneGeometry(const QString& zoneId);
    PhosphorProtocol::ZoneGeometryRect getZoneGeometryForScreen(const QString& zoneId, const QString& screenId);
    void setWindowFloatingForScreen(const QString& windowId, const QString& screenId, bool floating);

Q_SIGNALS:
    void setScreenDesktopRequested(const QString& screenId, const QString& desktopId, int purpose);
    void windowZoneChanged(const QString& windowId, const QString& zoneId);
    void parkDropped(const QString& windowId, const QString& outputUuid);
    void parkReseated(const QString& windowId, const QString& outputUuid);
    void windowFloatingChanged(const QString& windowId, bool isFloating, const QString& screenId);
    void windowStateChanged(const QString& windowId, const PhosphorProtocol::WindowStateEntry& state);
    void pendingRestoresAvailable();
    void reapplyWindowGeometriesRequested();
    void navigationFeedback(bool success, const QString& action, const QString& reason, const QString& sourceZoneId,
                            const QString& targetZoneId, const QString& screenId);
    void snapAllWindowsRequested(const QString& screenId);
    void moveSpecificWindowToZoneRequested(const QString& windowId, const QString& zoneId, int x, int y, int width,
                                           int height);
    void applyGeometryRequested(const QString& windowId, int x, int y, int width, int height, const QString& zoneId,
                                const QString& screenId, bool sizeOnly, int purpose);
    void activateWindowRequested(const QString& windowId);
    void windowDesktopMoveRequested(const QString& windowId, int desktop);
    void windowWorkspaceMoveRequested(const QString& windowId, const QString& desktopId, int desktop,
                                      const QString& targetScreenId);
    void windowMetadataChanged(const QString& windowId, const QString& appId, const QString& title);
    void windowUrgencyChanged(const QString& windowId, bool urgent);
    void windowOutputMoveExpected(const QString& windowId, const QString& targetScreenId,
                                  const QString& sourceScreenId);
    void applyGeometriesBatch(const PhosphorProtocol::WindowGeometryList& geometries, const QString& action);
    void raiseWindowsRequested(const QStringList& windowIds);
    void fullscreenHandBackRequested(const QString& windowId);
    void pendingRestoresChanged();

private:
    /// The bus caller when it is external (not the bridge), else empty.
    QString externalCaller() const;

    QPointer<WindowTracker> m_tracker;
    std::function<bool(const QString&)> m_isBridgeCaller;
};

} // namespace PlasmaZones
