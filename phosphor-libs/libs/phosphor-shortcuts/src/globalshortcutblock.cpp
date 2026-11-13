// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorShortcuts/GlobalShortcutBlock.h"

#include "shortcutslogging.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>

namespace PhosphorShortcuts {

namespace {

const QString kKGlobalAccelService = QStringLiteral("org.kde.kglobalaccel");
constexpr int kBlockCallTimeoutMs = 1000;

bool s_blocked = false;
bool s_quitHookInstalled = false;

bool sendBlock(bool blocked)
{
    QDBusMessage call =
        QDBusMessage::createMethodCall(kKGlobalAccelService, QStringLiteral("/kglobalaccel"),
                                       QStringLiteral("org.kde.KGlobalAccel"), QStringLiteral("blockGlobalShortcuts"));
    // Never start a standalone service just to flip its switch: without a
    // running kglobalaccel there are no global shortcuts to suspend.
    call.setAutoStartService(false);
    call << blocked;
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, kBlockCallTimeoutMs);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qCWarning(lcPhosphorShortcuts) << "blockGlobalShortcuts(" << blocked << ") failed:" << reply.errorMessage();
        return false;
    }
    return true;
}

} // namespace

bool canBlockGlobalShortcuts()
{
    const auto* bus = QDBusConnection::sessionBus().interface();
    return bus && bus->isServiceRegistered(kKGlobalAccelService);
}

bool setGlobalShortcutsBlocked(bool blocked)
{
    if (blocked == s_blocked) {
        return true;
    }
    if (!sendBlock(blocked)) {
        return false;
    }
    s_blocked = blocked;
    if (blocked && !s_quitHookInstalled && QCoreApplication::instance()) {
        // A recorder left recording when the application quits would
        // otherwise leave the whole session without global shortcuts.
        s_quitHookInstalled = true;
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [] {
            if (s_blocked) {
                setGlobalShortcutsBlocked(false);
            }
        });
    }
    return true;
}

bool globalShortcutsBlocked()
{
    return s_blocked;
}

} // namespace PhosphorShortcuts
