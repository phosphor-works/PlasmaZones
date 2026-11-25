// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tilingadaptor.h"
#include "common/engineruntimenamespace.h"

#include "dbushelpers.h"

#include <PhosphorEngineRuntime/TilingService.h>

namespace PlasmaZones {

TilingAdaptor::TilingAdaptor(TilingService* service, QObject* parent)
    : QDBusAbstractAdaptor(parent)
    , m_service(service)
{
    Q_ASSERT(service);
    connect(service, &TilingService::enabledChanged, this, &TilingAdaptor::enabledChanged);
    connect(service, &TilingService::managedScreensChanged, this, &TilingAdaptor::managedScreensChanged);
    connect(service, &TilingService::activeLayoutsChanged, this, &TilingAdaptor::activeLayoutsChanged);
    connect(service, &TilingService::tilingChanged, this, &TilingAdaptor::tilingChanged);
    connect(service, &TilingService::windowsTileRequested, this, &TilingAdaptor::windowsTileRequested);
    connect(service, &TilingService::focusWindowRequested, this, &TilingAdaptor::focusWindowRequested);
    connect(service, &TilingService::windowsReleasedFromTiling, this, &TilingAdaptor::windowsReleasedFromTiling);
    connect(service, &TilingService::scrollTabStripsChanged, this, &TilingAdaptor::scrollTabStripsChanged);
    connect(service, &TilingService::scrollTabPaintOverridesChanged, this,
            &TilingAdaptor::scrollTabPaintOverridesChanged);
    connect(service, &TilingService::scrollTabColorsChanged, this, &TilingAdaptor::scrollTabColorsChanged);
    connect(service, &TilingService::windowFloatingChanged, this, &TilingAdaptor::windowFloatingChanged);
    connect(service, &TilingService::focusedWindowChanged, this, &TilingAdaptor::focusedWindowChanged);
}

TilingAdaptor::~TilingAdaptor() = default;

bool TilingAdaptor::enabled() const
{
    return m_service ? m_service->enabled() : bool{};
}

QStringList TilingAdaptor::managedScreens() const
{
    return m_service ? m_service->managedScreens() : QStringList{};
}

QVariantMap TilingAdaptor::activeLayouts() const
{
    return m_service ? m_service->activeLayouts() : QVariantMap{};
}

void TilingAdaptor::retile(const QString& screenId)
{
    if (m_service) {
        m_service->retile(screenId);
    }
}

void TilingAdaptor::retileAllScreens()
{
    if (m_service) {
        m_service->retileAllScreens();
    }
}

void TilingAdaptor::windowOpened(const QString& windowId, const QString& screenId, int minWidth, int minHeight)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->windowOpened(windowId, screenId, minWidth, minHeight);
    }
}

void TilingAdaptor::windowReannounced(const QString& windowId, const QString& screenId, int minWidth, int minHeight)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->windowReannounced(windowId, screenId, minWidth, minHeight);
    }
}

void TilingAdaptor::windowsOpenedBatch(const PhosphorProtocol::WindowOpenedList& entries)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->windowsOpenedBatch(entries);
    }
}

void TilingAdaptor::windowMinSizeUpdated(const QString& windowId, int minWidth, int minHeight)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->windowMinSizeUpdated(windowId, minWidth, minHeight);
    }
}

void TilingAdaptor::windowClosed(const QString& windowId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->windowClosed(windowId);
    }
}

void TilingAdaptor::releaseWindowTracking(const QString& windowId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->releaseWindowTracking(windowId);
    }
}

void TilingAdaptor::notifyWindowFocused(const QString& windowId, const QString& screenId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->notifyWindowFocused(windowId, screenId);
    }
}

QVariantMap TilingAdaptor::scrollTabStrips() const
{
    return m_service ? m_service->scrollTabStrips() : QVariantMap{};
}

QVariantMap TilingAdaptor::scrollTabPaintOverrides() const
{
    return m_service ? m_service->scrollTabPaintOverrides() : QVariantMap{};
}

QVariantMap TilingAdaptor::scrollTabColors(const QString& windowId)
{
    return m_service ? m_service->scrollTabColors(windowId) : QVariantMap{};
}

QString TilingAdaptor::currentTilesJson(const QString& screenId) const
{
    return m_service ? m_service->currentTilesJson(screenId) : QString{};
}

QString TilingAdaptor::managedFocusedWindow(const QString& screenId) const
{
    return m_service ? m_service->managedFocusedWindow(screenId) : QString{};
}

} // namespace PlasmaZones
