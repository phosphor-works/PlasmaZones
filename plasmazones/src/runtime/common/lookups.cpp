// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lookups.h"

#include <PhosphorEngineRuntime/GeometryUtils.h>

#include <PhosphorScreens/Manager.h>
#include <PhosphorScreens/ScreenIdentity.h>
#include <PhosphorScreens/VirtualScreen.h>
#include <PhosphorZones/Layout.h>

#include <QGuiApplication>
#include <QScreen>

namespace PhosphorEngineRuntime {
namespace Lookups {

QString resolveScreenId(PhosphorScreens::ScreenManager* mgr, const QString& screenId)
{
    if (screenId.isEmpty()) {
        // Fall back to the primary screen's effective screen ID.
        // QCursor::pos() returns stale data for Wayland background daemons,
        // so we avoid it entirely and use the primary screen instead.
        QScreen* primary = QGuiApplication::primaryScreen();
        if (primary && mgr) {
            // If the primary screen has virtual subdivisions, returns the first
            // virtual screen ID. Otherwise returns the physical screen ID.
            const QStringList ids = mgr->virtualScreenIdsFor(PhosphorScreens::ScreenIdentity::identifierFor(primary));
            return ids.isEmpty() ? PhosphorScreens::ScreenIdentity::identifierFor(primary) : ids.first();
        }
        return primary ? PhosphorScreens::ScreenIdentity::identifierFor(primary) : QString();
    }
    return PhosphorIdentity::VirtualScreenId::isVirtual(screenId)
        ? screenId
        : PhosphorScreens::ScreenIdentity::idForName(screenId);
}

QRectF resolveScreenGeometry(PhosphorScreens::ScreenManager* mgr, PhosphorZones::Layout* layout,
                             const QString& screenId)
{
    if (mgr) {
        const QRect geom = mgr->screenGeometry(screenId);
        if (geom.isValid()) {
            return GeometryUtils::effectiveScreenGeometry(mgr, layout, screenId);
        }
    }
    // A virtual screen the manager does not resolve (an id left over from a
    // reconfigure) has no geometry of its own. Widened to its physical
    // monitor, zone math would run against the whole monitor for a screen
    // that no longer exists.
    if (PhosphorIdentity::VirtualScreenId::isVirtual(screenId)) {
        return {};
    }
    QScreen* screen = PhosphorScreens::ScreenIdentity::findByIdOrName(screenId);
    if (!screen && mgr) {
        screen = mgr->physicalScreenFor(screenId).qscreen;
    }
    if (screen) {
        return GeometryUtils::effectiveScreenGeometry(mgr, layout, screen);
    }
    return {};
}

QScreen* resolvePhysicalQScreen(PhosphorScreens::ScreenManager* mgr, const QString& screenId)
{
    if (mgr) {
        QScreen* screen = mgr->physicalScreenFor(screenId).qscreen;
        if (screen) {
            return screen;
        }
    }
    return PhosphorScreens::ScreenIdentity::findByIdOrName(
        PhosphorIdentity::VirtualScreenId::extractPhysicalId(screenId));
}

} // namespace Lookups
} // namespace PhosphorEngineRuntime
