// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorShortcuts/GlobalShortcutBlock.h"

#include "shortcutslogging.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QPointer>

namespace PhosphorShortcuts {

namespace {

const QString kKGlobalAccelService = QStringLiteral("org.kde.kglobalaccel");
constexpr int kBlockCallTimeoutMs = 1000;

/// Holds taken through setGlobalShortcutsBlocked and not yet given back.
int s_holds = 0;
/// Whether the last block call kglobalacceld accepted from this process was a
/// block. Separate from s_holds because an unblock can fail after the last
/// hold is given back, and the quit hook must still try again then.
bool s_serviceBlocked = false;
/// The application the quit hook is connected to. A QPointer, so a test that
/// destroys its QCoreApplication and makes another gets a fresh hook.
QPointer<QCoreApplication> s_quitHookApp;

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

/// Give back every hold and lift the block this process placed, if any.
void releaseAllHolds()
{
    s_holds = 0;
    if (s_serviceBlocked && sendBlock(false)) {
        s_serviceBlocked = false;
    }
}

void installQuitHook()
{
    QCoreApplication* app = QCoreApplication::instance();
    if (!app || s_quitHookApp == app) {
        return;
    }
    s_quitHookApp = app;
    // A recorder left recording when the application quits would otherwise
    // leave the whole session without global shortcuts. The application is
    // the context, so the connection goes with it.
    QObject::connect(app, &QCoreApplication::aboutToQuit, app, [] {
        releaseAllHolds();
    });
}

} // namespace

bool canBlockGlobalShortcuts()
{
    const auto* bus = QDBusConnection::sessionBus().interface();
    return bus && bus->isServiceRegistered(kKGlobalAccelService);
}

bool setGlobalShortcutsBlocked(bool blocked)
{
    if (blocked) {
        // Sent for the first hold even after an unblock failed: something
        // else may have lifted that block since, and nothing can read it back.
        if (s_holds == 0) {
            if (!sendBlock(true)) {
                return false;
            }
            s_serviceBlocked = true;
        }
        ++s_holds;
        installQuitHook();
        return true;
    }
    if (s_holds == 0) {
        return true;
    }
    if (--s_holds > 0) {
        return true;
    }
    if (!sendBlock(false)) {
        // s_serviceBlocked stays set, so the quit hook tries again.
        return false;
    }
    s_serviceBlocked = false;
    return true;
}

bool globalShortcutsBlocked()
{
    return s_holds > 0;
}

bool releaseStrandedGlobalShortcutBlock()
{
    if (s_holds > 0) {
        qCWarning(lcPhosphorShortcuts)
            << "releaseStrandedGlobalShortcutBlock: refused while this process holds a block";
        return false;
    }
    if (!canBlockGlobalShortcuts()) {
        return true;
    }
    if (!sendBlock(false)) {
        return false;
    }
    s_serviceBlocked = false;
    return true;
}

} // namespace PhosphorShortcuts
