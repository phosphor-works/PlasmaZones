// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "logging.h"

namespace PhosphorEngineRuntime {

// Core: layouts, screens and the shared helpers
Q_LOGGING_CATEGORY(lcCore, "plasmazones.core", QtInfoMsg)
Q_LOGGING_CATEGORY(lcScreen, "plasmazones.core.screen", QtInfoMsg)

// The runtime: its lifecycle, overlays and shortcuts
Q_LOGGING_CATEGORY(lcDaemon, "plasmazones.daemon", QtInfoMsg)
Q_LOGGING_CATEGORY(lcOverlay, "plasmazones.daemon.overlay", QtInfoMsg)
Q_LOGGING_CATEGORY(lcShortcuts, "plasmazones.daemon.shortcuts", QtInfoMsg)

// The runtime's services, by the interface each serves
Q_LOGGING_CATEGORY(lcDbus, "plasmazones.dbus", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusLayout, "plasmazones.dbus.layout", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusWindow, "plasmazones.dbus.window", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusSettings, "plasmazones.dbus.settings", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusRules, "plasmazones.dbus.rules", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusAutotile, "plasmazones.dbus.autotile", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusTiling, "plasmazones.dbus.tiling", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDbusScrolling, "plasmazones.dbus.scrolling", QtInfoMsg)

} // namespace PhosphorEngineRuntime
