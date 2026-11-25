// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// PlasmaZones' own code (namespace PlasmaZones) is a host of the engine
// runtime (namespace PhosphorEngineRuntime): it names the runtime's types,
// services and settings unqualified, as it did when both were one namespace.
// Names PlasmaZones declares itself still win over the runtime's, since
// a using-directive only makes them visible from the global scope.

namespace PhosphorEngineRuntime {
}

namespace PlasmaZones {
using namespace PhosphorEngineRuntime;
}
