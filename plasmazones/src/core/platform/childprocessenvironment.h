// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/types/constants.h"

#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <initializer_list>

namespace PlasmaZones {

/// The environment a process spawned from this one starts with: this
/// process's own, with what it changed for itself undone. The names in
/// PGpuExportedVarsProperty and PDaemonEnvIntroducedVarsProperty are removed,
/// then the values in PGpuClearedVarsProperty and
/// PDaemonEnvInheritedVarsProperty are put back (the property names are in
/// core/types/constants.h). The one home for that logic, so the editor and the
/// settings app, which different link units spawn, cannot be given different
/// environments. In a process that publishes none of the properties this is
/// the process environment unchanged.
inline QProcessEnvironment childProcessEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QCoreApplication* const app = QCoreApplication::instance();
    if (!app) {
        return env;
    }
    for (const char* property : {PGpuExportedVarsProperty, PDaemonEnvIntroducedVarsProperty}) {
        const QStringList names = app->property(property).toStringList();
        for (const QString& name : names) {
            env.remove(name);
        }
    }
    for (const char* property : {PGpuClearedVarsProperty, PDaemonEnvInheritedVarsProperty}) {
        const QVariantMap values = app->property(property).toMap();
        for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
            env.insert(it.key(), it.value().toString());
        }
    }
    return env;
}

} // namespace PlasmaZones
