// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_dbus_export.h"

#include <PhosphorProtocol/AutotileMarshalling.h>
#include <PhosphorProtocol/WindowMarshalling.h>

#include <QDBusAbstractAdaptor>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace PlasmaZones {

class TilingService;

/**
 * @brief The D-Bus binding of TilingService: org.phosphor_works.Placement.Tiling
 *
 * Every method, property and signal here is one the interface XML names, with
 * the signature it pins; each one's behaviour is documented on the
 * TilingService member of the same name. The binding forwards and relays
 * the service's signals.
 *
 * Its compositor-only window reports admit only the registered bridge (DbusHelpers::admitCompositorReport, #1155).
 */
class PLASMAZONES_DBUS_EXPORT TilingAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phosphor_works.Placement.Tiling")
    Q_PROPERTY(bool enabled READ enabled NOTIFY enabledChanged)
    Q_PROPERTY(QStringList managedScreens READ managedScreens NOTIFY managedScreensChanged)
    Q_PROPERTY(QVariantMap activeLayouts READ activeLayouts NOTIFY activeLayoutsChanged)

public:
    /// @param service Not owned. A call that arrives after it is gone is answered empty.
    /// @param parent The object the adaptor is exported through
    TilingAdaptor(TilingService* service, QObject* parent);
    ~TilingAdaptor() override;

    bool enabled() const;
    QStringList managedScreens() const;
    QVariantMap activeLayouts() const;

public Q_SLOTS:
    void retile(const QString& screenId);
    void retileAllScreens();
    void windowOpened(const QString& windowId, const QString& screenId, int minWidth, int minHeight);
    void windowReannounced(const QString& windowId, const QString& screenId, int minWidth, int minHeight);
    void windowsOpenedBatch(const PhosphorProtocol::WindowOpenedList& entries);
    void windowMinSizeUpdated(const QString& windowId, int minWidth, int minHeight);
    void windowClosed(const QString& windowId);
    void releaseWindowTracking(const QString& windowId);
    void notifyWindowFocused(const QString& windowId, const QString& screenId);
    QVariantMap scrollTabStrips() const;
    QVariantMap scrollTabPaintOverrides() const;
    QVariantMap scrollTabColors(const QString& windowId);
    QString currentTilesJson(const QString& screenId) const;
    QString managedFocusedWindow(const QString& screenId) const;

Q_SIGNALS:
    void enabledChanged(bool enabled);
    void managedScreensChanged(const QStringList& screenIds, bool isDesktopSwitch, const QVariantMap& screenDesktops);
    void activeLayoutsChanged(const QVariantMap& activeLayouts);
    void tilingChanged(const QString& screenId);
    void windowsTileRequested(const PhosphorProtocol::TileRequestList& tileRequests);
    void focusWindowRequested(const QString& windowId);
    void windowsReleasedFromTiling(const QStringList& windowIds);
    void scrollTabStripsChanged(const QString& screenId, const QString& stripsJson);
    void scrollTabPaintOverridesChanged(const QString& screenId, const QVariantMap& overrides);
    void scrollTabColorsChanged(const QString& windowId, const QVariantMap& colors);
    void windowFloatingChanged(const QString& windowId, bool isFloating, const QString& screenId);
    void focusedWindowChanged(const QString& screenId, const QString& windowId);

private:
    QPointer<TilingService> m_service;
};

} // namespace PlasmaZones
