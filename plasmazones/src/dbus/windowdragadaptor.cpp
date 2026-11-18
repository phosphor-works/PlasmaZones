// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "windowdragadaptor.h"
#include "common/engineruntimenamespace.h"

#include "dbushelpers.h"
#include "runtime/dragresolver/dragresolver.h"

#include <QDBusConnection>
#include <QDBusServiceWatcher>

namespace PlasmaZones {

WindowDragAdaptor::WindowDragAdaptor(DragResolver* resolver, QObject* parent)
    : QDBusAbstractAdaptor(parent)
    , m_resolver(resolver)
{
    Q_ASSERT(resolver);
    connect(resolver, &DragResolver::zoneGeometryDuringDragChanged, this,
            &WindowDragAdaptor::zoneGeometryDuringDragChanged);
    connect(resolver, &DragResolver::restoreSizeDuringDragChanged, this,
            &WindowDragAdaptor::restoreSizeDuringDragChanged);
    connect(resolver, &DragResolver::dragPolicyChanged, this, &WindowDragAdaptor::dragPolicyChanged);
    connect(resolver, &DragResolver::snapAssistReady, this, &WindowDragAdaptor::snapAssistReady);
}

WindowDragAdaptor::~WindowDragAdaptor() = default;

PhosphorProtocol::DragPolicy WindowDragAdaptor::beginDrag(const QString& windowId, int frameX, int frameY,
                                                          int frameWidth, int frameHeight, const QString& startScreenId,
                                                          int mouseButtons)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__)) || !m_resolver) {
        return {};
    }
    return m_resolver->beginDrag(windowId, frameX, frameY, frameWidth, frameHeight, startScreenId, mouseButtons);
}

PhosphorProtocol::DragOutcome WindowDragAdaptor::endDrag(const QString& windowId, int cursorX, int cursorY,
                                                         int modifiers, int mouseButtons, bool cancelled)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__)) || !m_resolver) {
        // The refused answer names the window, as DragResolver's own empty
        // outcome does, so the caller can match it to its drag.
        PhosphorProtocol::DragOutcome outcome;
        outcome.windowId = windowId;
        return outcome;
    }
    return m_resolver->endDrag(windowId, cursorX, cursorY, modifiers, mouseButtons, cancelled);
}

void WindowDragAdaptor::updateDragCursor(const QString& windowId, int cursorX, int cursorY, int modifiers,
                                         int mouseButtons)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__)) || !m_resolver) {
        return;
    }
    m_resolver->updateDragCursor(windowId, cursorX, cursorY, modifiers, mouseButtons);
}

void WindowDragAdaptor::registerDropProxy(const QString& screenId, const QString& proxyJson)
{
    if (!m_resolver) {
        return;
    }
    // The bus caller owns what it registers. A direct (non-D-Bus) call, as the
    // unit tests make through QMetaObject, has no peer and registers unowned.
    const QString sender = DbusHelpers::busCaller(this);
    switch (m_resolver->registerDropProxy(screenId, proxyJson, sender)) {
    case DropProxyRegistration::RefusedOwner:
        DbusHelpers::sendBusError(this, QDBusError::AccessDenied,
                                  QStringLiteral("The drop proxy on this screen belongs to another peer"));
        return;
    case DropProxyRegistration::Registered:
        if (!sender.isEmpty()) {
            watchDropProxyOwner(sender);
        }
        return;
    case DropProxyRegistration::Rejected:
        return;
    }
}

void WindowDragAdaptor::unregisterDropProxy(const QString& screenId)
{
    if (!m_resolver) {
        return;
    }
    if (!m_resolver->unregisterDropProxy(screenId, DbusHelpers::busCaller(this))) {
        DbusHelpers::sendBusError(this, QDBusError::AccessDenied,
                                  QStringLiteral("The drop proxy on this screen belongs to another peer"));
    }
}

void WindowDragAdaptor::selectorScrollWheel(int angleDeltaY)
{
    // A compositor report like beginDrag and the cursor ticks: only the
    // registered bridge may send it over the bus (#1155).
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__)) || !m_resolver) {
        return;
    }
    m_resolver->selectorScrollWheel(angleDeltaY);
}

void WindowDragAdaptor::cancelSnap()
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__)) || !m_resolver) {
        return;
    }
    m_resolver->cancelSnap();
}

void WindowDragAdaptor::clearForCompositorReconnect()
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__)) || !m_resolver) {
        return;
    }
    m_resolver->clearForCompositorReconnect();
}

void WindowDragAdaptor::watchDropProxyOwner(const QString& service)
{
    if (m_dropProxyWatchers.contains(service)) {
        return;
    }
    auto* watcher = new QDBusServiceWatcher(service, QDBusConnection::sessionBus(),
                                            QDBusServiceWatcher::WatchForUnregistration, this);
    QObject::connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this](const QString& gone) {
        if (m_resolver) {
            m_resolver->dropProxyOwnerGone(gone);
        }
        if (auto* stale = m_dropProxyWatchers.take(gone)) {
            stale->deleteLater();
        }
    });
    m_dropProxyWatchers.insert(service, watcher);
}

} // namespace PlasmaZones
