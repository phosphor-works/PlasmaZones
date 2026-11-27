// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QLatin1String>

/// The single-instance identities of PlasmaZones' programs. Each one claims
/// its service name and exports a small controller object, so a second
/// launch finds the running instance on the bus. They belong to PlasmaZones,
/// not to phosphor-libs: a library names no app.
namespace PlasmaZones::AppIdentity {
/// Facet, the settings app (plasmazones-settings).
namespace Settings {
inline constexpr QLatin1String ServiceName("org.phosphor_works.Facet");
inline constexpr QLatin1String ObjectPath("/org/phosphor_works/Facet");
inline constexpr QLatin1String Interface("org.phosphor_works.Facet");
}
/// The layout editor (plasmazones-editor).
namespace Editor {
inline constexpr QLatin1String ServiceName("org.phosphor_works.PlasmaZones.Editor");
inline constexpr QLatin1String ObjectPath("/org/phosphor_works/PlasmaZones/Editor");
inline constexpr QLatin1String Interface("org.phosphor_works.PlasmaZones.Editor");
}
} // namespace PlasmaZones::AppIdentity
