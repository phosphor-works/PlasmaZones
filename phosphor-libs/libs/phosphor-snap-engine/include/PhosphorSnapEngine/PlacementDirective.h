// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace PhosphorSnapEngine {

/**
 * @brief What a matched open-placement rule directs for an opening window.
 *
 * A small DTO crossing the daemon ↔ snap-engine boundary: the daemon resolves
 * the window's matched rules and fills this in, the engine acts on it. It lives
 * in its own lightweight header so both the engine's public API and the daemon's
 * resolver declaration can reference it by value without pulling in the full
 * SnapEngine definition.
 *
 *  - @c zoneOrdinals — the `SnapToZone` ordinal list (1-based).
 *  - @c zoneNames — the `SnapToZone` zone-name list. Each name resolves to the
 *    lowest-numbered zone of that name (case-insensitive, trimmed) in the
 *    placement layout, so a name-keyed rule follows the zone across layouts that number
 *    it differently. Names and ordinals union into one span. Both lists empty
 *    means no SnapToZone rule matched; the engine then has nothing to snap.
 *  - @c targetScreenId — the monitor the zones resolve on (canonical screen-id
 *    form). When non-empty the zones resolve on THAT screen and the window is
 *    moved there, restoring the per-monitor pinning the retired per-layout app
 *    rules carried. Empty means resolve on the window's opening screen (a
 *    `ScreenId` match leaf only SCOPES a rule; a route is what ROUTES it).
 *  - @c targetDesktop — the virtual desktop the snap commits in (1-based; 0 = no
 *    desktop routing). When set the zones resolve against THAT desktop's layout
 *    for the placement screen and the snap commits in that desktop's context, so
 *    a combined "snap to zone N + open on desktop M" rule places the window into
 *    zone N of desktop M's layout (not the desktop it opened on).
 *
 * Where the two targets come from: on a resolve that receives the open's
 * routing verdict (the first resolve of an open pass whose routing ran, see
 * SnapEngine::PlacementZonesResolver), a realized `RouteToWorkspace` names the
 * workspace's desktop and owner monitor and outranks any `RouteToDesktop` and
 * `RouteToScreen` in the same cascade, and a declared workspace that cannot be
 * realized right now names neither. Otherwise they are the rules' own
 * `RouteToScreen` and `RouteToDesktop`.
 */
struct PlacementDirective
{
    QList<int> zoneOrdinals{};
    QString targetScreenId{};
    int targetDesktop = 0;
    // Trailing so the positional `{ordinals, screen}` aggregate form the
    // engine tests use keeps its meaning.
    QStringList zoneNames{};
};

} // namespace PhosphorSnapEngine
