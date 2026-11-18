// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_daemon_lifecycle_order.cpp
 * @brief Ordering contracts in plasmazonesd's startup and shutdown that no
 *        fixture can observe, pinned by reading the source.
 *
 * Neither main() nor a running Daemon can be stood up in a unit test (both
 * need a Wayland compositor, KWin on the bus and kglobalacceld), so, like
 * test_connect_sweep_ordering, this pins a textual ORDER, not a behaviour. It
 * is brittle against renaming the calls it looks for, and fails loudly when
 * one of them is not found rather than passing on a missing match.
 *
 *  - Shutdown releases the shortcut grabs before the bus name.
 *    KGlobalAccel keys a shortcut by component and action name alone, so the
 *    setInactive and unregister calls the release sends would ungrab the keys
 *    of a --replace or bus-activated successor that registered first, and a
 *    successor can register as soon as the name is free. The release is in
 *    EngineRuntime::stop(), the name is plasmazonesd's: the runtime emits
 *    servicesClosing() after the release, and Daemon releases the name in
 *    the handler (withdrawFromBus), directly connected, and nowhere else.
 *  - EngineRuntime::stop() tears dynamic workspaces down before both. The
 *    teardown unregisters the named-workspace chords and gives KWin back its
 *    desktop chords, which needs the shortcut Registry still alive, and a
 *    restore sent after the bus name is free could hand the chords back to
 *    KWin underneath a successor that already took them.
 *  - main() reads the rendering backend only after
 *    ConfigMigration::ensureJsonConfig(), and before the Daemon builds the
 *    first QQuickWindow. A v1 config.json holds the backend under a key the
 *    v1→v2 step renames, so a read ahead of the migration ran the migrating
 *    start on the default backend (test_configmigration has the data side).
 *  - The exit status main() uses for a session it does not serve is listed in
 *    RestartPreventExitStatus= of plasmazones.service and of the units the
 *    Nix and Home Manager modules declare, so systemd neither restarts the
 *    daemon for it nor reports the start as a success.
 */

#include <QFile>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTest>

#include <utility>

namespace {

QString readSource(const QString& relativePath)
{
    QFile file(QStringLiteral(P_SOURCE_DIR) + relativePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

/// The body of the free or member function whose definition starts with
/// @p signature: from the signature to the first closing brace in column 0.
QString functionBody(const QString& source, const QString& signature)
{
    const qsizetype start = source.indexOf(signature);
    if (start < 0) {
        return {};
    }
    const qsizetype end = source.indexOf(QStringLiteral("\n}\n"), start);
    if (end < 0) {
        return {};
    }
    // Whole-line comments out, so a comment that names a call cannot stand in
    // for the call when the order is checked.
    static const QRegularExpression commentLine(QStringLiteral(R"(^[ \t]*/{2}.*$)"),
                                                QRegularExpression::MultilineOption);
    return source.mid(start, end - start).remove(commentLine);
}

/// The statuses a unit lists in RestartPreventExitStatus=, matched by
/// @p pattern, whose first capture is the space-separated list.
QStringList restartPreventStatuses(const QString& unit, const QString& pattern)
{
    const QRegularExpression re(pattern, QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = re.match(unit);
    return match.hasMatch() ? match.captured(1).split(QLatin1Char(' '), Qt::SkipEmptyParts) : QStringList();
}

} // namespace

class TestDaemonLifecycleOrder : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void stopReleasesShortcutsBeforeTheServicesClose()
    {
        const QString source = readSource(QStringLiteral("/src/runtime/engine/lifecycle.cpp"));
        QVERIFY2(!source.isEmpty(), "could not read runtime/engine/lifecycle.cpp");
        const QString stop = functionBody(source, QStringLiteral("void EngineRuntime::stop()"));
        QVERIFY2(!stop.isEmpty(), "EngineRuntime::stop() not found in lifecycle.cpp");

        const QString release = QStringLiteral("m_shortcutManager->unregisterShortcuts()");
        const QString closing = QStringLiteral("Q_EMIT servicesClosing()");
        const qsizetype releaseAt = stop.indexOf(release);
        const qsizetype closingAt = stop.indexOf(closing);
        QVERIFY2(releaseAt >= 0, "EngineRuntime::stop() no longer calls m_shortcutManager->unregisterShortcuts()");
        QVERIFY2(closingAt >= 0, "EngineRuntime::stop() no longer emits servicesClosing()");
        QVERIFY2(releaseAt < closingAt,
                 "EngineRuntime::stop() must release the shortcut grabs before the host releases the bus name");
        QVERIFY2(stop.indexOf(release, releaseAt + 1) < 0,
                 "EngineRuntime::stop() releases the shortcuts twice; a second release after the bus name reopens "
                 "the race");
        QVERIFY2(stop.indexOf(closing, closingAt + 1) < 0, "EngineRuntime::stop() emits servicesClosing() twice");
    }

    void stopTearsWorkspacesDownBeforeTheShortcutsAndTheServicesClose()
    {
        const QString source = readSource(QStringLiteral("/src/runtime/engine/lifecycle.cpp"));
        QVERIFY2(!source.isEmpty(), "could not read runtime/engine/lifecycle.cpp");
        const QString stop = functionBody(source, QStringLiteral("void EngineRuntime::stop()"));
        QVERIFY2(!stop.isEmpty(), "EngineRuntime::stop() not found in lifecycle.cpp");

        const QString teardown = QStringLiteral("teardownWorkspaces()");
        const qsizetype teardownAt = stop.indexOf(teardown);
        const qsizetype releaseAt = stop.indexOf(QStringLiteral("m_shortcutManager->unregisterShortcuts()"));
        const qsizetype closingAt = stop.indexOf(QStringLiteral("Q_EMIT servicesClosing()"));
        QVERIFY2(teardownAt >= 0, "EngineRuntime::stop() no longer calls teardownWorkspaces()");
        QVERIFY2(releaseAt >= 0, "EngineRuntime::stop() no longer calls m_shortcutManager->unregisterShortcuts()");
        QVERIFY2(closingAt >= 0, "EngineRuntime::stop() no longer emits servicesClosing()");
        QVERIFY2(teardownAt < releaseAt,
                 "EngineRuntime::stop() must tear workspaces down while the shortcut Registry is still alive");
        QVERIFY2(teardownAt < closingAt,
                 "EngineRuntime::stop() must tear workspaces down before the host releases the bus name");
        QVERIFY2(stop.indexOf(teardown, teardownAt + 1) < 0,
                 "EngineRuntime::stop() tears workspaces down twice; a second teardown after the release finds no "
                 "Registry");
    }

    void daemonReleasesTheBusNameOnlyWhenTheServicesClose()
    {
        const QString source = readSource(QStringLiteral("/src/daemon/daemon.cpp"));
        QVERIFY2(!source.isEmpty(), "could not read daemon/daemon.cpp");

        const QString withdraw = functionBody(source, QStringLiteral("void Daemon::withdrawFromBus()"));
        QVERIFY2(!withdraw.isEmpty(), "Daemon::withdrawFromBus() not found in daemon.cpp");
        QVERIFY2(withdraw.contains(QStringLiteral("unregisterService(")),
                 "Daemon::withdrawFromBus() no longer releases the bus name");

        // Released anywhere else, the name could go before the runtime's
        // shortcut release. Only withdrawFromBus and registerDBusService's own
        // failure path may name it.
        const QString stop = functionBody(source, QStringLiteral("void Daemon::stop()"));
        QVERIFY2(!stop.isEmpty(), "Daemon::stop() not found in daemon.cpp");
        QVERIFY2(!stop.contains(QStringLiteral("unregisterService(")),
                 "Daemon::stop() must leave the bus name to withdrawFromBus, which runs after the shortcut release");
        QVERIFY2(source.count(QStringLiteral("unregisterService(")) == 2,
                 "daemon.cpp releases the bus name somewhere other than withdrawFromBus and the failed "
                 "registration");

        // Queued, the handler would run after stop() returned, and the name
        // would outlive the teardown it is supposed to precede.
        static const QRegularExpression wiring(QStringLiteral(
            R"(connect\(\s*m_runtime\.get\(\),\s*&EngineRuntime::servicesClosing,\s*this,\s*&Daemon::withdrawFromBus,\s*Qt::DirectConnection\s*\))"));
        QVERIFY2(wiring.match(source).hasMatch(),
                 "Daemon no longer connects servicesClosing to withdrawFromBus with Qt::DirectConnection");
    }

    void mainReadsTheBackendAfterTheMigration()
    {
        const QString source = readSource(QStringLiteral("/src/daemon/main.cpp"));
        QVERIFY2(!source.isEmpty(), "could not read daemon/main.cpp");
        const QString mainBody = functionBody(source, QStringLiteral("int main(int argc, char* argv[])"));
        QVERIFY2(!mainBody.isEmpty(), "main() not found in daemon/main.cpp");

        const qsizetype migrateAt = mainBody.indexOf(QStringLiteral("ConfigMigration::ensureJsonConfig()"));
        const qsizetype backendReadAt = mainBody.indexOf(QStringLiteral("readRenderingConfigFromDisk().backend"));
        const qsizetype probeAt = mainBody.indexOf(QStringLiteral("probeAndSetGraphicsApi("));
        const qsizetype daemonAt = mainBody.indexOf(QStringLiteral("Daemon daemon;"));
        QVERIFY2(migrateAt >= 0, "main() no longer calls ConfigMigration::ensureJsonConfig()");
        QVERIFY2(backendReadAt >= 0, "main() no longer reads readRenderingConfigFromDisk().backend");
        QVERIFY2(probeAt >= 0, "main() no longer calls probeAndSetGraphicsApi(");
        QVERIFY2(daemonAt >= 0, "main() no longer constructs `Daemon daemon;`");
        QVERIFY2(migrateAt < backendReadAt, "main() must read the rendering backend after ensureJsonConfig()");
        QVERIFY2(migrateAt < probeAt, "main() must choose the graphics API after ensureJsonConfig()");
        QVERIFY2(probeAt < daemonAt, "main() must choose the graphics API before the Daemon builds a QQuickWindow");
    }

    void sessionNotServedStatusIsNotRestarted()
    {
        const QString source = readSource(QStringLiteral("/src/daemon/main.cpp"));
        QVERIFY2(!source.isEmpty(), "could not read daemon/main.cpp");
        static const QRegularExpression statusRe(QStringLiteral(R"(constexpr int kExitSessionNotServed = (\d+);)"));
        const QRegularExpressionMatch status = statusRe.match(source);
        QVERIFY2(status.hasMatch(), "kExitSessionNotServed not found in daemon/main.cpp");
        const QString code = status.captured(1);
        QVERIFY2(code != QLatin1String("0"), "a session that is not served must not exit 0 (see main.cpp)");

        // The packaged unit, and the two Nix modules, which declare their own
        // unit with Restart=on-failure under graphical-session.target, a
        // target GNOME, Sway and X11 sessions reach too.
        const QString systemdForm = QStringLiteral(R"(^RestartPreventExitStatus=(.*)$)");
        const QString nixForm = QStringLiteral("^\\s*RestartPreventExitStatus = \"([^\"]*)\";$");
        const QList<std::pair<QString, QString>> units{
            {QStringLiteral("/src/daemon/plasmazones.service.in"), systemdForm},
            {QStringLiteral("/../packaging/nix/module.nix"), nixForm},
            {QStringLiteral("/../packaging/nix/hm-module.nix"), nixForm},
        };
        for (const auto& [path, pattern] : units) {
            const QString unit = readSource(path);
            QVERIFY2(!unit.isEmpty(), qPrintable(QStringLiteral("could not read %1").arg(path)));
            QVERIFY2(restartPreventStatuses(unit, pattern).contains(code),
                     qPrintable(QStringLiteral("%1: RestartPreventExitStatus must list kExitSessionNotServed (%2)")
                                    .arg(path, code)));
        }
    }
};

QTEST_GUILESS_MAIN(TestDaemonLifecycleOrder)
#include "test_daemon_lifecycle_order.moc"
