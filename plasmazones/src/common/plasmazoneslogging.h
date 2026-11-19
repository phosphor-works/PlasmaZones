// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_export.h"

#include <QLoggingCategory>

// PlasmaZones' own logging categories: the layout editor's, and the settings
// app's daemon control. The engine runtime's are in core/platform/logging.h.

namespace PlasmaZones {

// clang-format off
// The layout editor: its controller, zones, undo stack and snapping
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcEditor)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcEditorZone)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcEditorUndo)
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcSnapping)

// Starting, stopping and enabling plasmazonesd from the settings app
PLASMAZONES_EXPORT Q_DECLARE_LOGGING_CATEGORY(lcKcm)
// clang-format on

} // namespace PlasmaZones
