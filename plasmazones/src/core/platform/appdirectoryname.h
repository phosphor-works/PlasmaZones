// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_export.h"

#include <QString>

namespace PhosphorEngineRuntime {

/// The application's directory under the XDG config and data roots: every
/// config file (config.json, session.json, rules.json, ...) lives in
/// `<config root>/<name>/`, and every data subdirectory (layouts, algorithms,
/// animations, ...) is `<name>/<kind>` under the data roots.
///
/// Set once, at the top of main(), before anything reads a path; each host
/// names its own (PlasmaZones' through initPlasmaZonesHost()). Nothing here supplies a
/// default, so an engine host names its own tree. Reading it unset is a
/// programming error and aborts, rather than handing out a path at the root of
/// the user's config directory or a subdirectory that matches every data root.
PLASMAZONES_EXPORT void setAppDirectoryName(const QString& name);
PLASMAZONES_EXPORT QString appDirectoryName();

} // namespace PhosphorEngineRuntime
