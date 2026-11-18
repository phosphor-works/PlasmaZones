// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "appdirectoryname.h"

#include <QtGlobal>

namespace PlasmaZones {

namespace {
QString& storedName()
{
    static QString name;
    return name;
}
} // namespace

void setAppDirectoryName(const QString& name)
{
    // One path segment: empty would put the files at the root of the user's
    // config directory, and a separator would reach outside it.
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || name == QLatin1String(".")
        || name == QLatin1String("..")) {
        qFatal("setAppDirectoryName: \"%s\" is not a single directory name", qPrintable(name));
    }
    storedName() = name;
}

QString appDirectoryName()
{
    const QString& name = storedName();
    if (name.isEmpty()) {
        qFatal("appDirectoryName() read before setAppDirectoryName(): call it at the top of main()");
    }
    return name;
}

} // namespace PlasmaZones
