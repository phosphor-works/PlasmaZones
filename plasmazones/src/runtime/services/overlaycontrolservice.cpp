// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "overlaycontrolservice.h"
#include "runtime/common/lookups.h"
#include "core/interfaces/interfaces.h"
#include "core/interfaces/ioverlayservice.h"
#include <PhosphorZones/IZoneLayoutRegistry.h>
#include <PhosphorZones/Layout.h>
#include <PhosphorZones/Zone.h>
#include "core/types/constants.h"
#include "core/platform/logging.h"
#include <PhosphorScreens/Manager.h>
#include "core/utils/utils.h"
#include <PhosphorScreens/VirtualScreen.h>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace PhosphorEngineRuntime {

OverlayControlService::OverlayControlService(IOverlayService* overlay, PhosphorZones::IZoneDetector* detector,
                                             PhosphorZones::IZoneLayoutRegistry* layoutRegistry,
                                             PhosphorScreens::ScreenManager* screenManager, ISettings* settings,
                                             QObject* parent)
    : QObject(parent)
    , m_overlayService(overlay)
    , m_zoneDetector(detector)
    , m_layoutRegistry(layoutRegistry)
    , m_screenManager(screenManager)
    , m_settings(settings)
{
    Q_ASSERT(overlay);
    Q_ASSERT(detector);
    Q_ASSERT(layoutRegistry);
    Q_ASSERT(settings);

    connect(m_overlayService, &IOverlayService::visibilityChanged, this,
            &OverlayControlService::overlayVisibilityChanged);

    // Connect to interface signals (DIP)
    connect(m_zoneDetector, &PhosphorZones::IZoneDetector::zoneHighlighted, this, [this](PhosphorZones::Zone* zone) {
        Q_EMIT zoneHighlightChanged(zone ? zone->id().toString() : QString());
    });

    connect(m_zoneDetector, &PhosphorZones::IZoneDetector::highlightsCleared, this, [this]() {
        Q_EMIT zoneHighlightChanged(QString());
    });

    connect(m_overlayService, &IOverlayService::snapAssistShown, this,
            [this](const QString& screenId, const PhosphorProtocol::EmptyZoneList& emptyZones,
                   const PhosphorProtocol::SnapAssistCandidateList& candidates) {
                Q_EMIT snapAssistShown(screenId, emptyZones, candidates);
            });

    // Mirror snapAssistShown's path on the way out so external observers
    // (KCMs, debugging tools, the kwin-effect's thumbnail-injection
    // bookkeeping) can track visible/hidden symmetrically without
    // polling isSnapAssistVisible. The internal IOverlayService signal
    // collapses every dismiss reason (pick, Escape, backdrop, screen-
    // change cancel, explicit D-Bus hideSnapAssist) through one emit
    // site, so a parameterless forward is the right shape here.
    connect(m_overlayService, &IOverlayService::snapAssistDismissed, this, &OverlayControlService::snapAssistDismissed);
    connect(m_overlayService, &IOverlayService::snapAssistThumbnailCacheTrimmed, this,
            &OverlayControlService::snapAssistThumbnailCacheTrimmed);
}

void OverlayControlService::showOverlay()
{
    if (!m_overlayService) {
        qCWarning(lcDbus) << "showOverlay: overlay service not wired";
        return;
    }
    m_overlayService->show();
}

void OverlayControlService::hideOverlay()
{
    if (!m_overlayService) {
        qCWarning(lcDbus) << "hideOverlay: overlay service not wired";
        return;
    }
    m_overlayService->hide();
}

bool OverlayControlService::isOverlayVisible()
{
    return m_overlayService ? m_overlayService->isVisible() : false;
}

void OverlayControlService::highlightZone(const QString& zoneId)
{
    if (!m_overlayService || !m_zoneDetector) {
        qCWarning(lcDbus) << "highlightZone: overlay service or zone detector not wired";
        return;
    }
    auto* zone = Lookups::getZoneFromActiveLayout(m_layoutRegistry, zoneId, QStringLiteral("highlight zone"));
    if (!zone) {
        return;
    }

    m_zoneDetector->highlightZone(zone);
    m_overlayService->updateGeometries();
}

void OverlayControlService::highlightZones(const QStringList& zoneIds)
{
    if (zoneIds.isEmpty()) {
        qCWarning(lcDbus) << "highlightZones: empty zone ID list";
        return;
    }

    if (!m_layoutRegistry || !m_layoutRegistry->activeLayout()) {
        qCWarning(lcDbus) << "highlightZones: no active layout";
        return;
    }

    if (!m_overlayService || !m_zoneDetector) {
        qCWarning(lcDbus) << "highlightZones: overlay service or zone detector not wired";
        return;
    }

    QVector<PhosphorZones::Zone*> zones;
    for (const auto& id : zoneIds) {
        auto uuidOpt = Utils::parseUuid(id);
        if (uuidOpt) {
            auto* zone = m_layoutRegistry->activeLayout()->zoneById(*uuidOpt);
            if (zone) {
                zones.append(zone);
            }
        }
    }

    if (!zones.isEmpty()) {
        m_zoneDetector->highlightZones(zones);
        m_overlayService->updateGeometries();
    }
}

void OverlayControlService::clearHighlight()
{
    if (!m_zoneDetector) {
        return;
    }
    m_zoneDetector->clearHighlights();
}

// Window tracking and zone detection methods moved to separate adaptors
// See WindowTracker and ZoneDetectionAdaptor

int OverlayControlService::getPollIntervalMs()
{
    return m_settings ? m_settings->pollIntervalMs() : Defaults::PollIntervalMs;
}

int OverlayControlService::getMinimumZoneSizePx()
{
    return m_settings ? m_settings->minimumZoneSizePx() : Defaults::MinimumZoneSizePx;
}

int OverlayControlService::getMinimumZoneDisplaySizePx()
{
    return m_settings ? m_settings->minimumZoneDisplaySizePx() : Defaults::MinimumZoneDisplaySizePx;
}

bool OverlayControlService::showSnapAssist(const QString& screenId, const PhosphorProtocol::EmptyZoneList& emptyZones,
                                           const PhosphorProtocol::SnapAssistCandidateList& candidates)
{
    if (!m_overlayService) {
        qCWarning(lcDbus) << "showSnapAssist: overlay service not wired";
        return false;
    }
    // Respect master toggle — don't show snap assist when the feature is disabled
    if (m_settings && !m_settings->snapAssistFeatureEnabled()) {
        return false;
    }
    // Return false when we know overlay won't be shown (avoids misleading "success")
    if (emptyZones.isEmpty() || candidates.isEmpty()) {
        return false;
    }
    // When the effect sends a physical screen ID for a subdivided monitor,
    // resolve to the correct virtual screen so snap assist appears on the
    // right side. The first empty zone's center decides which virtual screen
    // is the target. The zones are local to the monitor, not global
    // (WindowTrackingService::getEmptyZones translates them to its origin),
    // so the center is moved onto the monitor before the lookup, and the
    // zones are rebased onto the virtual screen's origin, where its overlay
    // is anchored.
    QString resolvedScreen = screenId;
    PhosphorProtocol::EmptyZoneList zones = emptyZones;
    if (!PhosphorIdentity::VirtualScreenId::isVirtual(screenId)) {
        auto* mgr = m_screenManager;
        const QRect monitor = mgr && mgr->hasVirtualScreens(screenId) ? mgr->screenGeometry(screenId) : QRect();
        if (monitor.isValid()) {
            const PhosphorProtocol::EmptyZoneEntry& first = zones.first();
            const QPoint center = monitor.topLeft() + QPoint(first.x + first.width / 2, first.y + first.height / 2);
            const QString vsId = mgr->effectiveScreenAt(center);
            const QRect vsGeometry = vsId.isEmpty() ? QRect() : mgr->screenGeometry(vsId);
            if (vsGeometry.isValid()) {
                resolvedScreen = vsId;
                const QPoint shift = monitor.topLeft() - vsGeometry.topLeft();
                for (PhosphorProtocol::EmptyZoneEntry& zone : zones) {
                    zone.x += shift.x();
                    zone.y += shift.y();
                }
            }
        }
    }

    // Defer actual work so we return immediately — the KWin effect blocks on this D-Bus
    // call; returning quickly prevents compositor freeze during overlay creation.
    // Note: Return value means "request accepted for deferred processing", not "overlay shown".
    // The service is captured rather than this adaptor: the timer's context is
    // the service, so the lambda runs only while the service lives, and the
    // adaptor may already be gone by then.
    QTimer::singleShot(0, m_overlayService, [overlay = m_overlayService, resolvedScreen, zones, candidates]() {
        overlay->showSnapAssist(resolvedScreen, zones, candidates);
    });
    return true;
}

void OverlayControlService::hideSnapAssist()
{
    if (!m_overlayService) {
        return;
    }
    m_overlayService->hideSnapAssist();
}

bool OverlayControlService::isSnapAssistVisible()
{
    return m_overlayService ? m_overlayService->isSnapAssistVisible() : false;
}

} // namespace PhosphorEngineRuntime
