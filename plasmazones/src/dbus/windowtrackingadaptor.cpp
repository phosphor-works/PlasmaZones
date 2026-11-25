// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "windowtrackingadaptor.h"
#include "common/engineruntimenamespace.h"

#include "dbushelpers.h"
#include "core/utils/dbusvariantutils.h"
#include <PhosphorEngineRuntime/WindowTracker.h>

#include <PhosphorProtocol/ServiceConstants.h>

namespace PlasmaZones {

WindowTrackingAdaptor::WindowTrackingAdaptor(WindowTracker* tracker, QObject* parent)
    : QDBusAbstractAdaptor(parent)
    , m_tracker(tracker)
{
    Q_ASSERT(tracker);
    connect(tracker, &WindowTracker::setScreenDesktopRequested, this,
            &WindowTrackingAdaptor::setScreenDesktopRequested);
    connect(tracker, &WindowTracker::windowZoneChanged, this, &WindowTrackingAdaptor::windowZoneChanged);
    connect(tracker, &WindowTracker::parkDropped, this, &WindowTrackingAdaptor::parkDropped);
    connect(tracker, &WindowTracker::parkReseated, this, &WindowTrackingAdaptor::parkReseated);
    connect(tracker, &WindowTracker::windowFloatingChanged, this, &WindowTrackingAdaptor::windowFloatingChanged);
    connect(tracker, &WindowTracker::windowStateChanged, this, &WindowTrackingAdaptor::windowStateChanged);
    connect(tracker, &WindowTracker::pendingRestoresAvailable, this, &WindowTrackingAdaptor::pendingRestoresAvailable);
    connect(tracker, &WindowTracker::reapplyWindowGeometriesRequested, this,
            &WindowTrackingAdaptor::reapplyWindowGeometriesRequested);
    connect(tracker, &WindowTracker::navigationFeedback, this, &WindowTrackingAdaptor::navigationFeedback);
    connect(tracker, &WindowTracker::snapAllWindowsRequested, this, &WindowTrackingAdaptor::snapAllWindowsRequested);
    connect(tracker, &WindowTracker::moveSpecificWindowToZoneRequested, this,
            &WindowTrackingAdaptor::moveSpecificWindowToZoneRequested);
    connect(tracker, &WindowTracker::applyGeometryRequested, this, &WindowTrackingAdaptor::applyGeometryRequested);
    connect(tracker, &WindowTracker::activateWindowRequested, this, &WindowTrackingAdaptor::activateWindowRequested);
    connect(tracker, &WindowTracker::windowDesktopMoveRequested, this,
            &WindowTrackingAdaptor::windowDesktopMoveRequested);
    connect(tracker, &WindowTracker::windowWorkspaceMoveRequested, this,
            &WindowTrackingAdaptor::windowWorkspaceMoveRequested);
    connect(tracker, &WindowTracker::windowMetadataChanged, this, &WindowTrackingAdaptor::windowMetadataChanged);
    connect(tracker, &WindowTracker::windowUrgencyChanged, this, &WindowTrackingAdaptor::windowUrgencyChanged);
    connect(tracker, &WindowTracker::windowOutputMoveExpected, this, &WindowTrackingAdaptor::windowOutputMoveExpected);
    connect(tracker, &WindowTracker::applyGeometriesBatch, this, &WindowTrackingAdaptor::applyGeometriesBatch);
    connect(tracker, &WindowTracker::raiseWindowsRequested, this, &WindowTrackingAdaptor::raiseWindowsRequested);
    connect(tracker, &WindowTracker::fullscreenHandBackRequested, this,
            &WindowTrackingAdaptor::fullscreenHandBackRequested);
    connect(tracker, &WindowTracker::pendingRestoresChanged, this, &WindowTrackingAdaptor::pendingRestoresChanged);
}

WindowTrackingAdaptor::~WindowTrackingAdaptor() = default;

void WindowTrackingAdaptor::setBridgeCallerCheck(std::function<bool(const QString&)> check)
{
    m_isBridgeCaller = std::move(check);
}

QString WindowTrackingAdaptor::externalCaller() const
{
    // A bus caller other than the compositor bridge. Read from the message the
    // host is serving, as WindowTracker read it before it left the bus.
    const QString caller = DbusHelpers::busCaller(this);
    if (caller.isEmpty() || (m_isBridgeCaller && m_isBridgeCaller(caller))) {
        return QString();
    }
    return caller;
}

void WindowTrackingAdaptor::setWindowMetadata(const QString& instanceId, const QString& appId,
                                              const QString& desktopFile, const QString& title,
                                              const QString& windowRole, int pid, int virtualDesktop,
                                              const QString& activity, int windowType, const QVariantMap& extended)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    // The span lists arrive over the bus as QDBusArgument, which toList() reads
    // as empty (F1003, F426), so they are unwrapped here.
    QVariantMap unwrapped = extended;
    namespace Key = PhosphorProtocol::Service::WindowMetadataKey;
    for (const QLatin1String key : {Key::VirtualDesktops, Key::Activities}) {
        if (const auto it = unwrapped.find(QString(key)); it != unwrapped.end()) {
            *it = DBusVariantUtils::convertDbusArgument(*it);
        }
    }
    m_tracker->setWindowMetadata(instanceId, appId, desktopFile, title, windowRole, pid, virtualDesktop, activity,
                                 windowType, unwrapped);
}

void WindowTrackingAdaptor::notifyDragOutUnsnap(const QString& windowId)
{
    // A compositor drag report (#1155): only the registered bridge may unsnap a
    // window as a drag-out over the bus. endDrag's in-process call is admitted,
    // since the host is serving endDrag then, not this method.
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->notifyDragOutUnsnap(windowId);
}

void WindowTrackingAdaptor::windowScreenChanged(const QString& windowId, const QString& newScreenId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->windowScreenChanged(windowId, newScreenId);
}

void WindowTrackingAdaptor::windowCrossedScreens(const QString& windowId, const QString& fromScreenId,
                                                 const QString& toScreenId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->windowCrossedScreens(windowId, fromScreenId, toScreenId);
}

PhosphorProtocol::OutputSettleVerdictList
WindowTrackingAdaptor::reportOutputSettle(const PhosphorProtocol::OutputSettleRowList& rows)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return {};
    }
    if (!m_tracker) {
        return {};
    }
    return m_tracker->reportOutputSettle(rows);
}

void WindowTrackingAdaptor::setWindowSticky(const QString& windowId, bool sticky)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->setWindowSticky(windowId, sticky);
}

bool WindowTrackingAdaptor::getPreFloatZone(const QString& windowId, QString& zoneId)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getPreFloatZone(windowId, zoneId);
}

void WindowTrackingAdaptor::clearPreFloatZone(const QString& windowId)
{
    // The inverse of a compositor-recorded float home, so it takes the same
    // gate as the writes it undoes (#1155): another peer must not wipe a
    // window's pre-float zone.
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->clearPreFloatZone(windowId);
}

void WindowTrackingAdaptor::storePreTileGeometry(const QString& windowId, int x, int y, int width, int height,
                                                 const QString& screenId, bool overwrite)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->storePreTileGeometry(windowId, x, y, width, height, screenId, overwrite);
}

bool WindowTrackingAdaptor::hasPreTileGeometry(const QString& windowId)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->hasPreTileGeometry(windowId);
}

void WindowTrackingAdaptor::clearPreTileGeometry(const QString& windowId)
{
    // The inverse of storePreTileGeometry takes its gate (#1155): another peer
    // must not wipe every screen's float-back of a window.
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->clearPreTileGeometry(windowId);
}

PhosphorProtocol::PreTileGeometryList WindowTrackingAdaptor::getPreTileGeometries()
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getPreTileGeometries();
}

void WindowTrackingAdaptor::windowClosed(const QString& windowId, const QString& screenId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->windowClosed(windowId, screenId);
}

void WindowTrackingAdaptor::windowActivated(const QString& windowId, const QString& screenId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->windowActivated(windowId, screenId);
}

void WindowTrackingAdaptor::setFrameGeometry(const QString& windowId, int x, int y, int width, int height)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->setFrameGeometry(windowId, x, y, width, height);
}

void WindowTrackingAdaptor::notifyWindowResized(const QString& windowId, int oldX, int oldY, int oldWidth,
                                                int oldHeight, int newX, int newY, int newWidth, int newHeight)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->notifyWindowResized(windowId, oldX, oldY, oldWidth, oldHeight, newX, newY, newWidth, newHeight);
}

void WindowTrackingAdaptor::cursorScreenChanged(const QString& screenId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->cursorScreenChanged(screenId);
}

void WindowTrackingAdaptor::activeWindowScreenChanged(const QString& windowId, const QString& screenId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->activeWindowScreenChanged(windowId, screenId);
}

void WindowTrackingAdaptor::screenDesktopChanged(const QString& screenId, int desktop)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->screenDesktopChanged(screenId, desktop);
}

void WindowTrackingAdaptor::seedScreenDesktop(const QString& screenId, int desktop)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->seedScreenDesktop(screenId, desktop);
}

void WindowTrackingAdaptor::reportPerOutputDesktopsMode(bool enabled)
{
    // Gated like its siblings screenDesktopChanged and seedScreenDesktop: this
    // report arms or disarms the whole dynamic-workspaces feature, so a bus
    // caller other than the registered compositor must not be able to flip it.
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->reportPerOutputDesktopsMode(enabled);
}

void WindowTrackingAdaptor::reportNavigationFeedback(bool success, const QString& action, const QString& reason,
                                                     const QString& sourceZoneId, const QString& targetZoneId,
                                                     const QString& screenId)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->reportNavigationFeedback(success, action, reason, sourceZoneId, targetZoneId, screenId);
}

bool WindowTrackingAdaptor::getValidatedPreTileGeometry(const QString& windowId, int& x, int& y, int& width,
                                                        int& height)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getValidatedPreTileGeometry(windowId, x, y, width, height);
}

QString WindowTrackingAdaptor::getZoneForWindow(const QString& windowId)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getZoneForWindow(windowId);
}

QStringList WindowTrackingAdaptor::getMultiZoneForWindow(const QString& windowId)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getMultiZoneForWindow(windowId);
}

QStringList WindowTrackingAdaptor::getWindowsInZone(const QString& zoneId)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getWindowsInZone(zoneId);
}

QStringList WindowTrackingAdaptor::getSnappedWindows()
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getSnappedWindows();
}

void WindowTrackingAdaptor::pruneStaleWindows(const QStringList& aliveWindowIds)
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->pruneStaleWindows(aliveWindowIds);
}

void WindowTrackingAdaptor::reapplyWindowAppearance()
{
    if (!DbusHelpers::admitCompositorReport(this, QLatin1StringView(__func__))) {
        return;
    }
    if (!m_tracker) {
        return;
    }
    m_tracker->reapplyWindowAppearance();
}

PhosphorProtocol::EmptyZoneList WindowTrackingAdaptor::getEmptyZones(const QString& screenId)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getEmptyZones(screenId);
}

QString WindowTrackingAdaptor::getLastUsedZoneId()
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getLastUsedZoneId();
}

PhosphorProtocol::WindowGeometryList WindowTrackingAdaptor::getUpdatedWindowGeometries()
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getUpdatedWindowGeometries();
}

QString WindowTrackingAdaptor::getPendingRestoreGeometries()
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getPendingRestoreGeometries();
}

PhosphorProtocol::WindowStateEntry WindowTrackingAdaptor::getWindowState(const QString& windowId)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getWindowState(windowId);
}

PhosphorProtocol::WindowStateList WindowTrackingAdaptor::getAllWindowStates()
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getAllWindowStates();
}

void WindowTrackingAdaptor::activateWindow(const QString& windowId)
{
    if (!m_tracker) {
        return;
    }
    m_tracker->activateWindow(windowId);
}

QString WindowTrackingAdaptor::getWindowMetadata(const QString& windowId, QString& title, QString& desktopFile)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getWindowMetadata(windowId, title, desktopFile);
}

void WindowTrackingAdaptor::moveWindowToDesktop(const QString& windowId, int desktop)
{
    if (!m_tracker) {
        return;
    }
    m_tracker->moveWindowToDesktop(windowId, desktop);
}

QStringList WindowTrackingAdaptor::getUrgentWindows()
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getUrgentWindows();
}

PhosphorProtocol::WindowStateList WindowTrackingAdaptor::getWindowStatesForDesktop(const QString& screenId,
                                                                                   int virtualDesktop)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getWindowStatesForDesktop(screenId, virtualDesktop);
}

QString WindowTrackingAdaptor::findWindowByPid(int pid)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->findWindowByPid(pid);
}

bool WindowTrackingAdaptor::isWindowFloating(const QString& windowId)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->isWindowFloating(windowId);
}

bool WindowTrackingAdaptor::queryWindowFloating(const QString& windowId)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->queryWindowFloating(windowId);
}

void WindowTrackingAdaptor::setWindowFloating(const QString& windowId, bool floating)
{
    if (!m_tracker) {
        return;
    }
    m_tracker->setWindowFloating(windowId, floating, externalCaller());
}

QStringList WindowTrackingAdaptor::getFloatingWindows()
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getFloatingWindows();
}

PhosphorProtocol::ZoneGeometryRect WindowTrackingAdaptor::getZoneGeometry(const QString& zoneId)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getZoneGeometry(zoneId);
}

PhosphorProtocol::ZoneGeometryRect WindowTrackingAdaptor::getZoneGeometryForScreen(const QString& zoneId,
                                                                                   const QString& screenId)
{
    if (!m_tracker) {
        return {};
    }
    return m_tracker->getZoneGeometryForScreen(zoneId, screenId);
}

void WindowTrackingAdaptor::setWindowFloatingForScreen(const QString& windowId, const QString& screenId, bool floating)
{
    if (!m_tracker) {
        return;
    }
    m_tracker->setWindowFloatingForScreen(windowId, screenId, floating, externalCaller());
}

} // namespace PlasmaZones
