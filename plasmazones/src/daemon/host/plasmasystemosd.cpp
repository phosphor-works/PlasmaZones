// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plasmasystemosd.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QString>

namespace PlasmaZones {

void PlasmaSystemOsd::showText(const QString& iconName, const QString& text)
{
    QDBusMessage msg =
        QDBusMessage::createMethodCall(QStringLiteral("org.kde.plasmashell"), QStringLiteral("/org/kde/osdService"),
                                       QStringLiteral("org.kde.osdService"), QStringLiteral("showText"));
    // An empty icon name is the runtime asking for the host's own icon.
    msg << (iconName.isEmpty() ? QStringLiteral("org.phosphor_works.PlasmaZones") : iconName) << text;
    QDBusConnection::sessionBus().asyncCall(msg);
}

} // namespace PlasmaZones
