// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapadaptor.h"
#include "common/engineruntimenamespace.h"

#include "dbushelpers.h"

#include "runtime/services/snapservice/snapservice.h"

namespace PlasmaZones {

SnapAdaptor::SnapAdaptor(SnapService* service, QObject* parent)
    : QDBusAbstractAdaptor(parent)
    , m_service(service)
{
    Q_ASSERT(service);
}

SnapAdaptor::~SnapAdaptor() = default;

void SnapAdaptor::windowSnapped(const QString& windowId, const QString& zoneId, const QString& screenId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->windowSnapped(windowId, zoneId, screenId);
    }
}

void SnapAdaptor::windowSnappedMultiZone(const QString& windowId, const QStringList& zoneIds, const QString& screenId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->windowSnappedMultiZone(windowId, zoneIds, screenId);
    }
}

void SnapAdaptor::windowUnsnapped(const QString& windowId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->windowUnsnapped(windowId);
    }
}

void SnapAdaptor::windowsSnappedBatch(const PhosphorProtocol::SnapConfirmationList& entries)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->windowsSnappedBatch(entries);
    }
}

void SnapAdaptor::recordSnapIntent(const QString& windowId, bool wasUserInitiated)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->recordSnapIntent(windowId, wasUserInitiated);
    }
}

void SnapAdaptor::snapToLastZone(const QString& windowId, const QString& windowScreenId, bool sticky, int& snapX,
                                 int& snapY, int& snapWidth, int& snapHeight, bool& shouldSnap)
{
    // The out-arguments read as no snap on every refused or unanswered call.
    snapX = snapY = snapWidth = snapHeight = 0;
    shouldSnap = false;
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__)) || !m_service) {
        return;
    }
    m_service->snapToLastZone(windowId, windowScreenId, sticky, snapX, snapY, snapWidth, snapHeight, shouldSnap);
}

void SnapAdaptor::snapToAppRule(const QString& windowId, const QString& windowScreenId, bool sticky, int& snapX,
                                int& snapY, int& snapWidth, int& snapHeight, bool& shouldSnap)
{
    // The out-arguments read as no snap on every refused or unanswered call.
    snapX = snapY = snapWidth = snapHeight = 0;
    shouldSnap = false;
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__)) || !m_service) {
        return;
    }
    m_service->snapToAppRule(windowId, windowScreenId, sticky, snapX, snapY, snapWidth, snapHeight, shouldSnap);
}

void SnapAdaptor::snapToEmptyZone(const QString& windowId, const QString& windowScreenId, bool sticky, int& snapX,
                                  int& snapY, int& snapWidth, int& snapHeight, bool& shouldSnap)
{
    // The out-arguments read as no snap on every refused or unanswered call.
    snapX = snapY = snapWidth = snapHeight = 0;
    shouldSnap = false;
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__)) || !m_service) {
        return;
    }
    m_service->snapToEmptyZone(windowId, windowScreenId, sticky, snapX, snapY, snapWidth, snapHeight, shouldSnap);
}

void SnapAdaptor::resolveWindowRestore(const QString& windowId, const QString& screenId, bool sticky, int windowKind,
                                       int restoreReason, int minWidth, int minHeight, int& snapX, int& snapY,
                                       int& snapWidth, int& snapHeight, bool& shouldSnap)
{
    // The out-arguments read as no snap on every refused or unanswered call.
    snapX = snapY = snapWidth = snapHeight = 0;
    shouldSnap = false;
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__)) || !m_service) {
        return;
    }
    m_service->resolveWindowRestore(windowId, screenId, sticky, windowKind, restoreReason, minWidth, minHeight, snapX,
                                    snapY, snapWidth, snapHeight, shouldSnap);
}

void SnapAdaptor::resnapToNewLayout()
{
    if (m_service) {
        m_service->resnapToNewLayout();
    }
}

void SnapAdaptor::resnapCurrentAssignments(const QString& screenFilter)
{
    if (m_service) {
        m_service->resnapCurrentAssignments(screenFilter);
    }
}

void SnapAdaptor::resnapFromAutotileOrder(const QStringList& autotileWindowOrder, const QString& screenId)
{
    if (m_service) {
        m_service->resnapFromAutotileOrder(autotileWindowOrder, screenId);
    }
}

PhosphorProtocol::SnapAllResultList SnapAdaptor::calculateSnapAllWindows(const QStringList& windowIds,
                                                                         const QString& screenId)
{
    // Compositor-only: it records each candidate's free frame, which a
    // foreign caller could otherwise overwrite for any window.
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return {};
    }
    return m_service ? m_service->calculateSnapAllWindows(windowIds, screenId) : PhosphorProtocol::SnapAllResultList{};
}

void SnapAdaptor::snapAllWindows(const QString& screenId)
{
    if (m_service) {
        m_service->snapAllWindows(screenId);
    }
}

void SnapAdaptor::handleBatchedResnap(const QString& resnapData)
{
    // A restore entry is taken only from the registered compositor bridge or a
    // direct call: the caller of this method, when the host is serving it.
    const std::optional<QString> caller = DbusHelpers::busCallerOf(this, QLatin1StringView(__func__));
    const bool restoresAdmitted = !caller || DbusHelpers::isBridgeCaller(this, *caller);
    if (m_service) {
        m_service->handleBatchedResnap(resnapData, restoresAdmitted);
    }
}

void SnapAdaptor::moveWindowToAdjacentZone(const QString& direction)
{
    if (m_service) {
        m_service->moveWindowToAdjacentZone(direction);
    }
}

void SnapAdaptor::focusAdjacentZone(const QString& direction)
{
    if (m_service) {
        m_service->focusAdjacentZone(direction);
    }
}

void SnapAdaptor::pushToEmptyZone(const QString& screenId)
{
    if (m_service) {
        m_service->pushToEmptyZone(screenId);
    }
}

void SnapAdaptor::restoreWindowSize()
{
    if (m_service) {
        m_service->restoreWindowSize();
    }
}

void SnapAdaptor::swapWindowWithAdjacentZone(const QString& direction)
{
    if (m_service) {
        m_service->swapWindowWithAdjacentZone(direction);
    }
}

void SnapAdaptor::snapToZoneByNumber(int zoneNumber, const QString& screenId)
{
    if (m_service) {
        m_service->snapToZoneByNumber(zoneNumber, screenId);
    }
}

void SnapAdaptor::rotateWindowsInLayout(bool clockwise, const QString& screenId)
{
    if (m_service) {
        m_service->rotateWindowsInLayout(clockwise, screenId);
    }
}

void SnapAdaptor::cycleWindowsInZone(bool forward)
{
    if (m_service) {
        m_service->cycleWindowsInZone(forward);
    }
}

void SnapAdaptor::moveWindowToZone(const QString& windowId, const QString& zoneId)
{
    if (m_service) {
        m_service->moveWindowToZone(windowId, zoneId);
    }
}

void SnapAdaptor::swapWindowsById(const QString& windowId1, const QString& windowId2)
{
    if (m_service) {
        m_service->swapWindowsById(windowId1, windowId2);
    }
}

void SnapAdaptor::toggleFloatForWindow(const QString& windowId, const QString& screenId)
{
    if (m_service) {
        m_service->toggleFloatForWindow(windowId, screenId);
    }
}

void SnapAdaptor::setWindowFloat(const QString& windowId, bool floating)
{
    if (m_service) {
        m_service->setWindowFloat(windowId, floating);
    }
}

PhosphorProtocol::UnfloatRestoreResult SnapAdaptor::calculateUnfloatRestore(const QString& windowId,
                                                                            const QString& screenId)
{
    return m_service ? m_service->calculateUnfloatRestore(windowId, screenId)
                     : PhosphorProtocol::UnfloatRestoreResult{};
}

void SnapAdaptor::windowUnsnappedForFloat(const QString& windowId)
{
    // Compositor-only: from any other caller it unsnapped a window that its
    // gated second half (setWindowFloat) could then refuse to float, leaving
    // it in neither state.
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (m_service) {
        m_service->windowUnsnappedForFloat(windowId);
    }
}

} // namespace PlasmaZones
