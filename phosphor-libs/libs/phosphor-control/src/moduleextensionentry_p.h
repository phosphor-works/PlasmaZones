// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// The keys of a section's entry in ApplicationController::extensionsFor(),
// which ModuleHost writes and ExtensionSlotModel reads. One spelling for both, so
// a key renamed on one side cannot leave the other reading a key nobody
// writes. Private: QML reads the entry through ExtensionSlotModel's roles, whose
// names are a separate contract.

#include <QLatin1String>

namespace PhosphorControl::ModuleExtensionEntry {

/// The section's scoped id, `<module>:<extension>`.
inline constexpr QLatin1String Id("id");
inline constexpr QLatin1String ModuleId("moduleId");
inline constexpr QLatin1String ModuleName("moduleName");
/// The section's own name.
inline constexpr QLatin1String Name("name");
/// A page of the section's module, empty when none is in the sidebar.
inline constexpr QLatin1String ModulePageId("modulePageId");
/// Set once the section's module is loaded and its domain is alive.
inline constexpr QLatin1String QmlSource("qmlSource");
inline constexpr QLatin1String Domain("domain");
/// Set, and true, when the section's module failed or was turned off.
inline constexpr QLatin1String ModuleFailed("moduleFailed");
/// Set, and true, when the section's module was turned off after a crash.
inline constexpr QLatin1String DisabledAfterCrash("disabledAfterCrash");
/// Present exactly when the section cannot be shown, holding why.
inline constexpr QLatin1String Error("error");

} // namespace PhosphorControl::ModuleExtensionEntry
