// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_replace_incumbent.cpp
 * @brief believedIncumbentPid(): when --replace may wait on the pid the
 *        placement host record names.
 *
 * --replace waits for the incumbent process to exit before it loads state,
 * because the incumbent releases the bus name before its shutdown saves. When
 * the incumbent let go of the name before the successor asked the bus for its
 * pid, the pid comes from the host record instead. The record outlives the
 * daemon that wrote it, so each case below is a way the record can be stale,
 * and waiting on its pid would stall a start on an unrelated process.
 */

#include "daemon/replaceincumbent.h"

#include <QByteArray>
#include <QString>
#include <QTest>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

using namespace PlasmaZones;

namespace {

const QString kHost = QStringLiteral("plasmazones");
constexpr qint64 kOwnPid = 4000;

PhosphorConfig::HostRecord record(const QString& host, qint64 pid)
{
    PhosphorConfig::HostRecord r;
    r.host = host;
    r.configPath = QStringLiteral("/home/user/.config/plasmazones/config.json");
    r.pid = pid;
    return r;
}

/// A /proc stand-in: "self" and pid 4321 run plasmazonesd, pid 999 runs
/// something else, every other pid has no process.
QByteArray fakeNames(const QString& pid)
{
    if (pid == QLatin1String("self") || pid == QLatin1String("4321")) {
        return QByteArrayLiteral("plasmazonesd");
    }
    if (pid == QLatin1String("999")) {
        return QByteArrayLiteral("firefox");
    }
    return {};
}

/// Every pid runs plasmazonesd, so only the pid checks can refuse.
QByteArray everyoneIsTheDaemon(const QString&)
{
    return QByteArrayLiteral("plasmazonesd");
}

} // namespace

class TestReplaceIncumbent : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void liveDaemonNamedByTheRecordIsBelieved()
    {
        QCOMPARE(believedIncumbentPid(record(kHost, 4321), kHost, kOwnPid, fakeNames), pid_t(4321));
    }

    void noRecordGivesNothing()
    {
        QCOMPARE(believedIncumbentPid(std::nullopt, kHost, kOwnPid, fakeNames), pid_t(0));
    }

    // Another host of the placement API wrote the record last.
    void anotherHostsRecordIsNotBelieved()
    {
        QCOMPARE(believedIncumbentPid(record(QStringLiteral("phosphor"), 4321), kHost, kOwnPid, fakeNames), pid_t(0));
    }

    // A user's choice between hosts carries pid 0, which kill() would read as
    // this process group.
    void usersChoiceIsNotBelieved()
    {
        QCOMPARE(believedIncumbentPid(record(kHost, 0), kHost, kOwnPid, everyoneIsTheDaemon), pid_t(0));
    }

    // HostDirectory::record() refuses a negative pid, but the answer is 0 or
    // a real pid whatever record it is handed: kill() reads a negative pid as
    // a process group.
    void negativePidIsNotBelieved()
    {
        QCOMPARE(believedIncumbentPid(record(kHost, -5), kHost, kOwnPid, everyoneIsTheDaemon), pid_t(0));
    }

    // This process's own pid runs plasmazonesd too, so only the pid check
    // keeps it out; it would never be seen to exit.
    void ownPidIsNotBelieved()
    {
        QCOMPARE(believedIncumbentPid(record(kHost, 4321), kHost, 4321, everyoneIsTheDaemon), pid_t(0));
    }

    void pidPastPidTIsNotBelieved()
    {
        const qint64 tooBig = qint64(std::numeric_limits<pid_t>::max()) + 1;
        QCOMPARE(believedIncumbentPid(record(kHost, tooBig), kHost, kOwnPid, everyoneIsTheDaemon), pid_t(0));
    }

    // The daemon that wrote the record died long ago and the pid now belongs
    // to another program.
    void reusedPidOfAnotherProgramIsNotBelieved()
    {
        QCOMPARE(believedIncumbentPid(record(kHost, 999), kHost, kOwnPid, fakeNames), pid_t(0));
    }

    // The daemon that wrote the record is gone and nothing has the pid.
    void deadPidIsNotBelieved()
    {
        QCOMPARE(believedIncumbentPid(record(kHost, 1234), kHost, kOwnPid, fakeNames), pid_t(0));
    }

    // With no /proc to read, every name is empty, and two empty names must not
    // count as a match.
    void unreadableNamesBelieveNothing()
    {
        const auto noProc = [](const QString&) {
            return QByteArray();
        };
        QCOMPARE(believedIncumbentPid(record(kHost, 4321), kHost, kOwnPid, noProc), pid_t(0));
    }

    // The default name lookup against the real /proc: a forked copy of this
    // process runs the same program and is believed, the parent (the shell or
    // ctest) runs another and is not.
    void realProcessNamesDecide()
    {
        const pid_t child = ::fork();
        QVERIFY(child >= 0);
        if (child == 0) {
            ::pause();
            ::_exit(0);
        }
        const pid_t believedChild = believedIncumbentPid(record(kHost, child), kHost, ::getpid());
        const pid_t believedParent = believedIncumbentPid(record(kHost, ::getppid()), kHost, ::getpid());
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        QCOMPARE(believedChild, child);
        QCOMPARE(believedParent, pid_t(0));
    }
};

QTEST_GUILESS_MAIN(TestReplaceIncumbent)
#include "test_replace_incumbent.moc"
