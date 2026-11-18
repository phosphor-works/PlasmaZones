// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_export.h"
#include <PhosphorProtocol/ZoneTypes.h>
#include <QObject>
#include <QString>

namespace PhosphorScreens {
class ScreenManager;
}

namespace PhosphorZones {
class IZoneLayoutRegistry;
class IZoneDetector;
}

namespace PhosphorEngineRuntime {

class IOverlayService;
class ISettings;

/**
 * @brief Overlay control: the service behind org.phosphor_works.Placement.Overlay
 *
 * PhosphorZones::Zone overlay visibility, highlighting and Snap Assist.
 * Bound to D-Bus by OverlayAdaptor (dbus/overlayadaptor.h), which also
 * carries the KWin effect's thumbnail ingest and the compositor peer check
 * that guards it.
 *
 * Note: PhosphorZones::Zone detection and window tracking are handled by separate services
 * (ZoneQueryService and WindowTracker).
 *
 * Uses interface types for loose coupling
 */
class PLASMAZONES_EXPORT OverlayControlService : public QObject
{
    Q_OBJECT

public:
    explicit OverlayControlService(IOverlayService* overlay, PhosphorZones::IZoneDetector* detector,
                                   PhosphorZones::IZoneLayoutRegistry* layoutRegistry,
                                   PhosphorScreens::ScreenManager* screenManager, ISettings* settings,
                                   QObject* parent = nullptr);
    ~OverlayControlService() override = default;

public Q_SLOTS:
    // Visibility control
    void showOverlay();
    void hideOverlay();
    bool isOverlayVisible();

    // PhosphorZones::Zone highlighting (requires layout manager for backward compatibility)
    void highlightZone(const QString& zoneId);
    void highlightZones(const QStringList& zoneIds);
    void clearHighlight();

    // Performance constants
    int getPollIntervalMs();
    int getMinimumZoneSizePx();
    int getMinimumZoneDisplaySizePx();

    // Snap Assist overlay (window picker after snapping). Compositor-only on
    // the bus: OverlayAdaptor admits only the registered bridge (#1155).
    bool showSnapAssist(const QString& screenId, const PhosphorProtocol::EmptyZoneList& emptyZones,
                        const PhosphorProtocol::SnapAssistCandidateList& candidates);
    void hideSnapAssist();
    bool isSnapAssistVisible();

Q_SIGNALS:
    void overlayVisibilityChanged(bool visible);
    void zoneHighlightChanged(const QString& zoneId);
    void snapAssistShown(const QString& screenId, const PhosphorProtocol::EmptyZoneList& emptyZones,
                         const PhosphorProtocol::SnapAssistCandidateList& candidates);
    /**
     * @brief Emitted when the Snap Assist overlay closes.
     *
     * Forwarded directly from `IOverlayService::snapAssistDismissed`,
     * which fires regardless of dismiss source (user pick, Escape,
     * backdrop click, screen-change cancel, or an explicit
     * `hideSnapAssist()` D-Bus call). Pairs with @ref snapAssistShown
     * so external observers can mirror visibility without polling
     * @ref isSnapAssistVisible. No payload — the dismiss reason is
     * intentionally not surfaced because the internal signal collapses
     * every path through the same emit site.
     */
    void snapAssistDismissed();
    /**
     * @brief Emitted when the daemon's idle-grace trim empties its
     * snap-assist thumbnail stores.
     *
     * Forwarded from `IOverlayService::snapAssistThumbnailCacheTrimmed`.
     * The kwin-effect subscribes and drops its recently-posted dedup set so
     * the next snap-assist re-captures thumbnails the daemon no longer
     * holds instead of stranding on icons.
     */
    void snapAssistThumbnailCacheTrimmed();

private:
    IOverlayService* m_overlayService; // Interface type (DIP)
    PhosphorZones::IZoneDetector* m_zoneDetector; // Interface type (DIP) - only for highlighting
    // Narrow to IZoneLayoutRegistry — overlay adaptor only reads the active
    // layout, never per-context assignments / quick slots / persistence.
    PhosphorZones::IZoneLayoutRegistry* m_layoutRegistry;
    PhosphorScreens::ScreenManager* m_screenManager;
    ISettings* m_settings; // Interface type (DIP) - for configurable constants
};

} // namespace PhosphorEngineRuntime
