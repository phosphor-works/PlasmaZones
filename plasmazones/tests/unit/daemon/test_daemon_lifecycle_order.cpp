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
 *  - Daemon::stop() releases the shortcut grabs before the bus name.
 *    KGlobalAccel keys a shortcut by component and action name alone, so the
 *    setInactive and unregister calls the release sends would ungrab the keys
 *    of a --replace or bus-activated successor that registered first, and a
 *    successor can register as soon as the name is free.
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
    void stopReleasesShortcutsBeforeTheBusName()
    {
        const QString source = readSource(QStringLiteral("/src/daemon/daemon/lifecycle.cpp"));
        QVERIFY2(!source.isEmpty(), "could not read daemon/daemon/lifecycle.cpp");
        const QString stop = functionBody(source, QStringLiteral("void Daemon::stop()"));
        QVERIFY2(!stop.isEmpty(), "Daemon::stop() not found in lifecycle.cpp");

        const QString release = QStringLiteral("m_shortcutManager->unregisterShortcuts()");
        const qsizetype releaseAt = stop.indexOf(release);
        const qsizetype nameAt = stop.indexOf(QStringLiteral("unregisterService("));
        QVERIFY2(releaseAt >= 0, "Daemon::stop() no longer calls m_shortcutManager->unregisterShortcuts()");
        QVERIFY2(nameAt >= 0, "Daemon::stop() no longer calls unregisterService(");
        QVERIFY2(releaseAt < nameAt, "Daemon::stop() must release the shortcut grabs before it releases the bus name");
        QVERIFY2(stop.indexOf(release, releaseAt + 1) < 0,
                 "Daemon::stop() releases the shortcuts twice; a second release after the bus name reopens the race");
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
