// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/engine/engineruntime.h"

#include "runtime/overlay/overlayservice.h"
#include "runtime/services/zonequeryservice.h"

#include <PhosphorProtocol/ZoneTypes.h>

namespace PlasmaZones {

void EngineRuntime::initServiceWiring()
{
    // No disconnect-first below: initCoreAdaptors' delete preamble
    // (init_adaptors.cpp) tears down the WHOLE previous service set,
    // m_zoneQueryService included, and re-news it before this function runs,
    // so a stop() -> init() cycle hands us a fresh object with no connections
    // to sweep.

    // Highlight the detected zone. Narrow on purpose: this fires per drag
    // poll, and the old full updateGeometries() rebuilt every screen's
    // overlay geometry each time to answer a question about one zone.
    //
    // highlightZone is SINGLE-zone: it also resets highlightedZoneIds, so it
    // collapses a multi-zone (zone-span) highlight to one zone. Its only
    // trigger is DetectZoneAtPosition, an externally-callable D-Bus method
    // with no in-repo caller — the daemon's own drag path writes highlights
    // directly and never routes through here. An external client polling it
    // DURING a zone-span drag would clobber that drag's multi-highlight;
    // accepted, since the two uses cannot both win and the drag path repaints
    // on its next poll.
    connect(m_zoneQueryService, &ZoneQueryService::zoneDetected, this,
            [this](const QString& zoneId, const PhosphorProtocol::ZoneGeometryRect& geometry) {
                Q_UNUSED(geometry)
                m_overlayService->highlightZone(zoneId);
            });
}

} // namespace PlasmaZones
