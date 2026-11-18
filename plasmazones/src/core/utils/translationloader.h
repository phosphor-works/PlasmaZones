// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Loads Qt .qm translation files at runtime.
// Call once per application object, after it is created. The translator is
// parented to @p app and installed into it, so it lives exactly as long as that
// object, and a second call on the same object installs a duplicate. A process
// that replaces its application object calls it again for the new one: the
// daemon calls it on the QCoreApplication its early --help/--version path
// builds and destroys, then on its QGuiApplication.

#pragma once

#include "plasmazones_export.h"

class QCoreApplication;

namespace PlasmaZones {
PLASMAZONES_EXPORT void loadTranslations(QCoreApplication* app);
}
