// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scrollingadaptor.h"
#include "common/engineruntimenamespace.h"

#include "dbushelpers.h"

#include "runtime/services/scrollingservice/scrollingservice.h"

namespace PlasmaZones {

ScrollingAdaptor::ScrollingAdaptor(ScrollingService* service, QObject* parent)
    : QDBusAbstractAdaptor(parent)
    , m_service(service)
{
    Q_ASSERT(service);
    connect(service, &ScrollingService::scrollingScreensChanged, this, &ScrollingAdaptor::scrollingScreensChanged);
    connect(service, &ScrollingService::stripContextChanged, this, &ScrollingAdaptor::stripContextChanged);
    connect(service, &ScrollingService::scrollEffectBehaviourChanged, this,
            &ScrollingAdaptor::scrollEffectBehaviourChanged);
    connect(service, &ScrollingService::scrollFocusScrollBlockedWindowsChanged, this,
            &ScrollingAdaptor::scrollFocusScrollBlockedWindowsChanged);
    connect(service, &ScrollingService::stripChanged, this, &ScrollingAdaptor::stripChanged);
}

ScrollingAdaptor::~ScrollingAdaptor() = default;

QStringList ScrollingAdaptor::scrollingScreens() const
{
    return m_service ? m_service->scrollingScreens() : QStringList{};
}

QVariantMap ScrollingAdaptor::scrollEffectBehaviour() const
{
    return m_service ? m_service->scrollEffectBehaviour() : QVariantMap{};
}

QStringList ScrollingAdaptor::scrollFocusScrollBlockedWindows() const
{
    return m_service ? m_service->scrollFocusScrollBlockedWindows() : QStringList{};
}

void ScrollingAdaptor::focusColumn(const QString& screenId, int delta)
{
    if (m_service) {
        m_service->focusColumn(screenId, delta);
    }
}

void ScrollingAdaptor::scrollView(const QString& screenId, int delta)
{
    if (m_service) {
        m_service->scrollView(screenId, delta);
    }
}

void ScrollingAdaptor::focusColumnAt(const QString& screenId, int index)
{
    if (m_service) {
        m_service->focusColumnAt(screenId, index);
    }
}

void ScrollingAdaptor::moveColumnTo(const QString& screenId, int from, int to)
{
    if (m_service) {
        m_service->moveColumnTo(screenId, from, to);
    }
}

void ScrollingAdaptor::scrollViewByPx(const QString& screenId, int px)
{
    if (m_service) {
        m_service->scrollViewByPx(screenId, px);
    }
}

void ScrollingAdaptor::setColumnWidthProportion(const QString& screenId, double proportion)
{
    if (m_service) {
        m_service->setColumnWidthProportion(screenId, proportion);
    }
}

void ScrollingAdaptor::setColumnWidthPixels(const QString& screenId, int px)
{
    if (m_service) {
        m_service->setColumnWidthPixels(screenId, px);
    }
}

void ScrollingAdaptor::setWindowHeightProportion(const QString& screenId, double proportion)
{
    if (m_service) {
        m_service->setWindowHeightProportion(screenId, proportion);
    }
}

void ScrollingAdaptor::setWindowHeightPixels(const QString& screenId, int px)
{
    if (m_service) {
        m_service->setWindowHeightPixels(screenId, px);
    }
}

bool ScrollingAdaptor::toggleMaximizeColumn(const QString& screenId, const QString& windowId)
{
    return m_service ? m_service->toggleMaximizeColumn(screenId, windowId) : bool{};
}

bool ScrollingAdaptor::toggleMaximizeToEdges(const QString& screenId, const QString& windowId)
{
    return m_service ? m_service->toggleMaximizeToEdges(screenId, windowId) : bool{};
}

void ScrollingAdaptor::clearWindowedFullscreen(const QString& windowId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->clearWindowedFullscreen(windowId);
    }
}

void ScrollingAdaptor::reapplyWindowGeometry(const QString& windowId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->reapplyWindowGeometry(windowId);
    }
}

void ScrollingAdaptor::reportCommittedSize(const QString& windowId, int offeredWidth, int offeredHeight,
                                           int committedWidth, int committedHeight)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->reportCommittedSize(windowId, offeredWidth, offeredHeight, committedWidth, committedHeight);
    }
}

bool ScrollingAdaptor::setWindowFullscreenFloat(const QString& windowId, const QString& screenId, bool floating)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return false;
    }
    return m_service ? m_service->setWindowFullscreenFloat(windowId, screenId, floating) : bool{};
}

QString ScrollingAdaptor::visibleStripJson(const QString& screenId) const
{
    return m_service ? m_service->visibleStripJson(screenId) : QString{};
}

QString ScrollingAdaptor::stripModelJson(const QString& screenId) const
{
    return m_service ? m_service->stripModelJson(screenId) : QString{};
}

QString ScrollingAdaptor::presetVocabularyJson(const QString& screenId) const
{
    return m_service ? m_service->presetVocabularyJson(screenId) : QString{};
}

QString ScrollingAdaptor::blueprintProgressJson(const QString& screenId) const
{
    return m_service ? m_service->blueprintProgressJson(screenId) : QString{};
}

} // namespace PlasmaZones
