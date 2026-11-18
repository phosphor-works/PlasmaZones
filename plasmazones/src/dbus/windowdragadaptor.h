// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/engineruntimenamespace.h"

#include "plasmazones_dbus_export.h"

#include <PhosphorProtocol/DragMarshalling.h>
#include <PhosphorProtocol/ZoneMarshalling.h>

#include <QDBusAbstractAdaptor>
#include <QHash>
#include <QPointer>
#include <QString>

class QDBusServiceWatcher;

namespace PhosphorEngineRuntime {
class DragResolver;
} // namespace PhosphorEngineRuntime

namespace PlasmaZones {

/**
 * @brief The D-Bus binding of DragResolver: org.phosphor_works.Placement.WindowDrag
 *
 * Every method and signal here is one the interface XML names, with the
 * signature it pins, plus clearForCompositorReconnect, which the effect calls
 * at shutdown although the XML does not list it. Each method's behaviour is
 * documented on the DragResolver method of the same name. The binding owns
 * what only the bus has: it admits the compositor-only reports
 * (DbusHelpers::admitCompositorReport, #1155), names the bus caller as a drop
 * proxy's owner, answers a refused one with AccessDenied, and retires a peer's
 * proxies when its bus name goes away. It relays DragResolver's signals onto
 * the bus unchanged.
 *
 * Exported through its parent, which carries the QDBusContext the admission
 * check reads.
 */
class PLASMAZONES_DBUS_EXPORT WindowDragAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phosphor_works.Placement.WindowDrag")

public:
    /// @param resolver Not owned. A call that arrives after it is gone is answered empty.
    /// @param parent The object the adaptor is exported through
    WindowDragAdaptor(DragResolver* resolver, QObject* parent);
    ~WindowDragAdaptor() override;

public Q_SLOTS:
    PhosphorProtocol::DragPolicy beginDrag(const QString& windowId, int frameX, int frameY, int frameWidth,
                                           int frameHeight, const QString& startScreenId, int mouseButtons);
    PhosphorProtocol::DragOutcome endDrag(const QString& windowId, int cursorX, int cursorY, int modifiers,
                                          int mouseButtons, bool cancelled);
    void updateDragCursor(const QString& windowId, int cursorX, int cursorY, int modifiers, int mouseButtons);
    void registerDropProxy(const QString& screenId, const QString& proxyJson);
    void unregisterDropProxy(const QString& screenId);
    void selectorScrollWheel(int angleDeltaY);
    void cancelSnap();
    void clearForCompositorReconnect();

Q_SIGNALS:
    void zoneGeometryDuringDragChanged(const QString& windowId, int x, int y, int width, int height);
    void restoreSizeDuringDragChanged(const QString& windowId, int width, int height);
    void dragPolicyChanged(const QString& windowId, const PhosphorProtocol::DragPolicy& newPolicy);
    void snapAssistReady(const QString& windowId, const QString& releaseScreenId,
                         const PhosphorProtocol::EmptyZoneList& emptyZones);

private:
    /// Watch a registering peer's bus name so its proxies retire when it dies,
    /// since a proxy outlives any single drag and a crashed shell would
    /// otherwise keep capturing snap drops. At most one watcher per peer: the
    /// shell re-registers on every miniature geometry change, so one per call
    /// would leak a bus match rule per repaint.
    void watchDropProxyOwner(const QString& service);

    QPointer<DragResolver> m_resolver;
    /// One watcher per registering bus name.
    QHash<QString, QDBusServiceWatcher*> m_dropProxyWatchers;
};

} // namespace PlasmaZones
