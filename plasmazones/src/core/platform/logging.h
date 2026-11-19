// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_export.h"
#include <QLoggingCategory>

/**
 * @file logging.h
 * @brief The engine runtime's logging categories (a host declares its own)
 *
 * This file defines Qt logging categories that enable runtime filtering
 * of log messages. Use these categories instead of plain qDebug/qWarning.
 *
 * Usage:
 *   #include "logging.h"
 *   qCDebug(lcCore) << "Debug message";
 *   qCInfo(lcCore) << "Info message";
 *   qCWarning(lcCore) << "Warning message";
 *   qCCritical(lcCore) << "Critical message";
 *
 * Runtime filtering via environment variable:
 *   QT_LOGGING_RULES="plasmazones.*=true"                    # Enable all
 *   QT_LOGGING_RULES="plasmazones.*.debug=false"             # Disable debug only
 *   QT_LOGGING_RULES="plasmazones.dbus.*=true"               # Enable D-Bus only
 *
 * Severity Guidelines:
 *   qCDebug    - Development tracing, disabled in release builds
 *   qCInfo     - Significant operational events (startup, shutdown, layout loaded)
 *   qCWarning  - Recoverable errors, invalid input, missing resources
 *   qCCritical - System failures preventing normal operation
 *
 * Log Message Format (keep output consistent and grep-friendly):
 *   - Key-value pairs: use "key= " << value so output is "key= value" (one space after =).
 *   - Same for multiple keys: "label key1= " << v1 << " key2= " << v2.
 *   - Sub-items (e.g. list entries): start message with "  " and use same key= pattern.
 *   - Use consistent names across categories: screen=, layout=, zone=, id=, name=, etc.
 */

namespace PhosphorEngineRuntime {

// clang-format off
// Core: layouts, screens and the shared helpers
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcCore)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcScreen)

// The runtime: its lifecycle, overlays and shortcuts
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcDaemon)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcOverlay)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcShortcuts)

// The runtime's services, by the interface each serves
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcDbus)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcDbusLayout)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcDbusWindow)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcDbusSettings)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcDbusRules)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcDbusAutotile)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcDbusTiling)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcDbusScrolling)
// clang-format on

} // namespace PhosphorEngineRuntime
