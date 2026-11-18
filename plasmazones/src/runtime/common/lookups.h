// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_export.h"

#include "core/interfaces/interfaces.h"
#include "core/platform/logging.h"
#include "core/utils/utils.h"

#include <PhosphorScreens/ScreenIdentity.h>
#include <PhosphorZones/Layout.h>
#include <PhosphorZones/Zone.h>

#include <QRectF>
#include <QScreen>
#include <QString>
#include <QUuid>

#include <optional>

namespace PhosphorScreens {
class ScreenManager;
}

namespace PhosphorEngineRuntime {

/**
 * @brief Lookup and validation helpers shared by the runtime's services and
 *        their bindings
 *
 * Consolidates common validation patterns:
 * - UUID parsing and validation
 * - PhosphorZones::Layout/screen null checking
 * - PhosphorZones::Zone lookup with proper error handling
 * - Screen ID and geometry resolution
 *
 * Nothing here touches a transport; the bus-context helpers are DbusHelpers
 * (dbus/dbushelpers.h). The helpers use template logging categories so each
 * caller logs under its own category while sharing the validation logic.
 */
namespace Lookups {

// ═══════════════════════════════════════════════════════════════════════════════
// UUID Validation
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * @brief Parse and validate a UUID string with logging
 * @param id UUID string to parse
 * @param operation Description for error logging (e.g., "highlightZone")
 * @param category Logging category to use
 * @return Valid QUuid or std::nullopt on failure (logs warning)
 *
 * Consolidates the common pattern:
 *   if (id.isEmpty()) { qCWarning() << "..."; return; }
 *   auto uuid = Utils::parseUuid(id);
 *   if (!uuid) { qCWarning() << "..."; return; }
 */
template<typename LogCategory>
std::optional<QUuid> parseAndValidateUuid(const QString& id, const QString& operation, LogCategory category)
{
    if (id.isEmpty()) {
        qCWarning(category) << "Cannot" << operation << "- empty ID";
        return std::nullopt;
    }

    auto uuidOpt = Utils::parseUuid(id);
    if (!uuidOpt) {
        qCWarning(category) << "Invalid UUID format for" << operation << ":" << id;
        return std::nullopt;
    }

    return uuidOpt;
}

/**
 * @brief Overload using default lcDbus category
 */
inline std::optional<QUuid> parseAndValidateUuid(const QString& id, const QString& operation)
{
    return parseAndValidateUuid(id, operation, lcDbus);
}

// ═══════════════════════════════════════════════════════════════════════════════
// PhosphorZones::Layout Validation
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * @brief Get the active layout with null check and warning
 * @param mgr PhosphorZones::Layout manager to query
 * @param operation Description for error logging
 * @param category Logging category to use
 * @return Active layout pointer or nullptr (logs warning if null)
 *
 * Consolidates the common pattern:
 *   auto* layout = m_layoutManager->activeLayout();
 *   if (!layout) { qCWarning() << "no active layout"; return; }
 */
template<typename LogCategory>
PhosphorZones::Layout* getActiveLayoutOrWarn(PhosphorZones::IZoneLayoutRegistry* mgr, const QString& operation,
                                             LogCategory category)
{
    if (!mgr) {
        qCWarning(category) << "Cannot" << operation << "- no layout manager";
        return nullptr;
    }

    auto* layout = mgr->activeLayout();
    if (!layout) {
        qCWarning(category) << "Cannot" << operation << "- no active layout";
        return nullptr;
    }

    return layout;
}

/**
 * @brief Get a zone by UUID from the active layout
 * @param mgr PhosphorZones::Layout manager to query
 * @param zoneId PhosphorZones::Zone UUID string
 * @param operation Description for error logging
 * @param category Logging category to use
 * @return PhosphorZones::Zone pointer or nullptr (logs appropriate warnings)
 *
 * Combines UUID validation + active layout check + zone lookup.
 */
template<typename LogCategory>
PhosphorZones::Zone* getZoneFromActiveLayout(PhosphorZones::IZoneLayoutRegistry* mgr, const QString& zoneId,
                                             const QString& operation, LogCategory category)
{
    auto uuidOpt = parseAndValidateUuid(zoneId, operation, category);
    if (!uuidOpt) {
        return nullptr;
    }

    auto* layout = getActiveLayoutOrWarn(mgr, operation, category);
    if (!layout) {
        return nullptr;
    }

    auto* zone = layout->zoneById(*uuidOpt);
    if (!zone) {
        qCWarning(category) << "Zone not found for" << operation << ":" << zoneId;
        return nullptr;
    }

    return zone;
}

/**
 * @brief Overload using default lcDbus category
 */
inline PhosphorZones::Zone* getZoneFromActiveLayout(PhosphorZones::IZoneLayoutRegistry* mgr, const QString& zoneId,
                                                    const QString& operation)
{
    return getZoneFromActiveLayout(mgr, zoneId, operation, lcDbus);
}

/**
 * @brief Find a zone by UUID across all layouts (not just active)
 * @param mgr PhosphorZones::Layout manager to search
 * @param zoneId PhosphorZones::Zone UUID string
 * @param operation Description for error logging
 * @param category Logging category to use
 * @return PhosphorZones::Zone pointer or nullptr (logs warning if not found)
 *
 * Searches active layout first, then all layouts.
 * Useful for per-screen layout assignments where zone may be in non-active layout.
 *
 * @note O(N×M) in (layouts, zones-per-layout) on the cold path. Acceptable
 * for the per-screen-assignment fallback use case where N is small (a few
 * layouts) and only triggers when the active layout misses; not suitable
 * for hot-path zone lookups (use the IZoneLayoutRegistry by-id surface).
 */
template<typename LogCategory>
PhosphorZones::Zone* findZoneInAnyLayout(PhosphorZones::IZoneLayoutRegistry* mgr, const QString& zoneId,
                                         const QString& operation, LogCategory category)
{
    // `activeLayout()` and `layouts()` are both on the interface - no
    // need to require the concrete registry just to reach them.
    auto uuidOpt = parseAndValidateUuid(zoneId, operation, category);
    if (!uuidOpt) {
        return nullptr;
    }

    if (!mgr) {
        qCWarning(category) << "Cannot" << operation << "- no layout manager";
        return nullptr;
    }

    QUuid uuid = *uuidOpt;
    PhosphorZones::Zone* zone = nullptr;

    // First try active layout
    if (auto* activeLayout = mgr->activeLayout()) {
        zone = activeLayout->zoneById(uuid);
    }

    // If not found, search all layouts
    if (!zone) {
        for (auto* layout : mgr->layouts()) {
            zone = layout->zoneById(uuid);
            if (zone) {
                break;
            }
        }
    }

    if (!zone) {
        qCWarning(category) << "Zone not found in any layout for" << operation << ":" << zoneId;
    }

    return zone;
}

/**
 * @brief Overload using default lcDbus category
 */
inline PhosphorZones::Zone* findZoneInAnyLayout(PhosphorZones::IZoneLayoutRegistry* mgr, const QString& zoneId,
                                                const QString& operation)
{
    return findZoneInAnyLayout(mgr, zoneId, operation, lcDbus);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Screen Validation
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * @brief Get primary screen with null check and warning
 * @param operation Description for error logging
 * @param category Logging category to use
 * @return Primary screen pointer or nullptr (logs warning if null)
 */
template<typename LogCategory>
QScreen* getPrimaryScreenOrWarn(const QString& operation, LogCategory category)
{
    QScreen* screen = Utils::primaryScreen();
    if (!screen) {
        qCWarning(category) << operation << ": no primary screen";
        return nullptr;
    }
    return screen;
}

/**
 * @brief Overload using default lcDbus category
 */
inline QScreen* getPrimaryScreenOrWarn(const QString& operation)
{
    return getPrimaryScreenOrWarn(operation, lcDbus);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Validation Helpers
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * @brief Validate that a required string parameter is not empty
 * @param value The value to check
 * @param paramName Parameter name for error message (e.g., "zone ID")
 * @param operation Operation name for error message
 * @param category Logging category to use
 * @return true if valid (non-empty), false if empty (logs warning)
 */
template<typename LogCategory>
bool validateNonEmpty(const QString& value, const QString& paramName, const QString& operation, LogCategory category)
{
    if (value.isEmpty()) {
        qCWarning(category) << "Cannot" << operation << "- empty" << paramName;
        return false;
    }
    return true;
}

/**
 * @brief Overload using default lcDbus category
 */
inline bool validateNonEmpty(const QString& value, const QString& paramName, const QString& operation)
{
    return validateNonEmpty(value, paramName, operation, lcDbus);
}

/**
 * @brief Validate a 1-based virtual desktop number arriving over the bus.
 *
 * Desktops are 1-based everywhere in the assignment API; 0 is the wildcard
 * meaning "any desktop" and is only legal on the slots documented to accept it,
 * which pass @p allowZero. A negative desktop names a context nothing can ever
 * resolve — persisting an assignment rule for it is a silent leak that also
 * grows the rule set without bound under repeated calls. The lower bound is the
 * whole guard: this helper has no view of the compositor's desktop count, so a
 * too-high desktop passes here and is left to the caller.
 *
 * @param virtualDesktop The desktop number as received.
 * @param operation Operation name for the warning.
 * @param allowZero Accept 0 as the screen-level wildcard.
 * @return true when the value is usable, false after logging a warning.
 */
template<typename LogCategory>
bool validateDesktopNumber(int virtualDesktop, const QString& operation, LogCategory category, bool allowZero = false)
{
    const int minimum = allowZero ? 0 : 1;
    if (virtualDesktop < minimum) {
        qCWarning(category) << "Cannot" << operation << "- virtual desktop" << virtualDesktop << "is below" << minimum;
        return false;
    }
    return true;
}

/**
 * @brief Overload using default lcDbus category
 */
inline bool validateDesktopNumber(int virtualDesktop, const QString& operation, bool allowZero = false)
{
    return validateDesktopNumber(virtualDesktop, operation, lcDbus, allowZero);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Screen ID Resolution (implemented in lookups.cpp to reduce header weight)
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * @brief Resolve a screen ID to an effective (virtual-aware) screen ID
 *
 * When screenId is empty, resolves to the primary screen, or to its first
 * virtual screen when it is subdivided. The cursor is never read: a Wayland
 * background daemon's cursor position is stale. When non-empty, passes virtual
 * screen IDs through and resolves physical names via
 * PhosphorScreens::ScreenIdentity::idForName.
 */
QString resolveScreenId(PhosphorScreens::ScreenManager* mgr, const QString& screenId);

/**
 * @brief Resolve a screen ID (physical or virtual) to its backing QScreen*
 *
 * Uses PhosphorScreens::ScreenManager::physicalScreenFor() when available, then falls back to
 * PhosphorScreens::ScreenIdentity::findByIdOrName(). Does NOT fall back to primaryScreen - returns
 * nullptr so the caller can decide the appropriate fallback behavior.
 */
QScreen* resolvePhysicalQScreen(PhosphorScreens::ScreenManager* mgr, const QString& screenId);

// ═══════════════════════════════════════════════════════════════════════════════
// Screen Geometry Resolution (implemented in lookups.cpp to reduce header weight)
// ═══════════════════════════════════════════════════════════════════════════════

/**
 * @brief Resolve effective screen geometry for a layout, handling virtual screens
 *
 * Prefers PhosphorScreens::ScreenManager geometry (virtual-screen-aware) when available,
 * falls back to resolving the physical QScreen and using the QScreen overload.
 * A virtual screen id the manager does not resolve is never widened to its
 * physical monitor. Returns an invalid QRectF if no geometry can be resolved.
 */
QRectF resolveScreenGeometry(PhosphorScreens::ScreenManager* mgr, PhosphorZones::Layout* layout,
                             const QString& screenId);

} // namespace Lookups
} // namespace PhosphorEngineRuntime
