// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <PhosphorConfig/HostDirectory.h>

#include <QByteArray>
#include <QFile>
#include <QString>

#include <functional>
#include <limits>
#include <optional>

#include <sys/types.h>

namespace PlasmaZones {

/// The kernel's name for a process: the base name of the program it runs, cut
/// to 15 bytes, from /proc/<pid>/comm (pass "self" for this process). Empty
/// when the file cannot be read, which includes a pid with no process.
inline QByteArray processName(const QString& pid)
{
    QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
    if (!comm.open(QIODevice::ReadOnly)) {
        return {};
    }
    return comm.readAll().trimmed();
}

/// The pid --replace waits on when the bus could not name the incumbent,
/// taken from the placement host record, or 0 when the record cannot be
/// believed.
///
/// That happens when the incumbent let go of the name between the successor's
/// failed registerService and its pid lookup: the incumbent is then in
/// Daemon::stop(), still saving, and is exactly the process to wait for. The
/// record holds the pid of the last daemon to start, and main() writes the
/// successor's own record only once the name is ours, so here it still
/// belongs to an earlier start.
///
/// The record outlives the daemon that wrote it, so its pid may now belong to
/// anything. It is believed only when the record names @p hostId (not another
/// host of the API, and not a user's choice, which has pid 0), the pid fits
/// pid_t, is above zero and is not @p ownPid, and the live process with that
/// pid runs a program named like this one (@p nameOf("self")). Matching the
/// name rather than the executable path keeps an incumbent whose binary a
/// package upgrade replaced (its exe link then reads "(deleted)"), and one
/// started from another build tree, while a reused pid now running some other
/// program is never waited on. @p nameOf is processName() outside tests.
inline pid_t believedIncumbentPid(const std::optional<PhosphorConfig::HostRecord>& record, const QString& hostId,
                                  qint64 ownPid, const std::function<QByteArray(const QString&)>& nameOf = processName)
{
    if (!record || record->host != hostId) {
        return 0;
    }
    // Zero and anything past pid_t would mean a process GROUP to kill(), and
    // our own pid would never be seen to exit.
    if (record->pid <= 0 || record->pid > qint64(std::numeric_limits<pid_t>::max()) || record->pid == ownPid) {
        return 0;
    }
    const pid_t pid = pid_t(record->pid);
    const QByteArray ownName = nameOf(QStringLiteral("self"));
    if (ownName.isEmpty() || nameOf(QString::number(pid)) != ownName) {
        return 0;
    }
    return pid;
}

} // namespace PlasmaZones
