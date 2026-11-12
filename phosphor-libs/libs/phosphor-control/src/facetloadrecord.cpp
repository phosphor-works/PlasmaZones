// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "facetloadrecord_p.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimeZone>

#include <algorithm>
#include <cstdio>

namespace PhosphorControl {

namespace {
const QLatin1String RecordLoadingKey("loading");
const QLatin1String RecordPidKey("pid");
const QLatin1String RecordProcessKey("process");
const QLatin1String RecordDisabledKey("disabled");

// What tells one process apart from a later one that reuses its pid: the
// boot it runs in and the time it started, in clock ticks since boot (field
// 22 of /proc/<pid>/stat). Empty when it cannot be read, which includes a
// process that no longer exists. Wayland-only project, so /proc is there.
QString processIdentity(qint64 pid)
{
    if (pid <= 0) {
        return QString();
    }
    QFile bootFile(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    QFile statFile(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!bootFile.open(QIODevice::ReadOnly) || !statFile.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QByteArray boot = bootFile.readAll().trimmed();
    const QByteArray stat = statFile.readAll();
    // The command name in field 2 may hold spaces and parentheses, so count
    // fields from the last ')'. Field 3 is the first one after it.
    const qsizetype close = stat.lastIndexOf(')');
    if (boot.isEmpty() || close < 0) {
        return QString();
    }
    const QList<QByteArray> fields = stat.mid(close + 1).simplified().split(' ');
    constexpr qsizetype StartTimeIndex = 22 - 3;
    if (fields.size() <= StartTimeIndex) {
        return QString();
    }
    return QString::fromLatin1(boot + '/' + fields.at(StartTimeIndex));
}
} // namespace

FacetLoadRecord::FacetLoadRecord()
{
    // An empty state location would put the record at the filesystem root.
    // No location means no crash memory, said once by restore().
    const QString stateDir = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    if (!stateDir.isEmpty()) {
        m_path = stateDir + QStringLiteral("/phosphor-works/facet-loading");
    }
}

QString FacetLoadRecord::path() const
{
    return m_path;
}

void FacetLoadRecord::setPath(const QString& path)
{
    // Made absolute now, so the record stays where it is when the working
    // directory changes later. Empty stays empty: no crash memory.
    m_path = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
}

void FacetLoadRecord::restore()
{
    // A module left in the record's "loading" field was loading when a Facet
    // process ended without finishing it. Unless that process is still
    // running (a second instance mid-load), the module is disabled, and the
    // record keeps it disabled across starts until it is retried.
    if (m_path.isEmpty()) {
        qCInfo(lcFacetHost) << "FacetHost: no state directory, so a module that crashes Facet is not remembered";
    } else {
        // A process killed between naming a record write's temporary file and
        // renaming it over the record leaves that file behind. A minute is far
        // longer than a write takes, so a second instance writing right now
        // keeps its file. QTemporaryFile's suffix is six letters or digits,
        // and the match is case-sensitive, so a user's copy such as
        // "facet-loading.v1.bak" is left alone.
        const QFileInfo recordInfo(m_path);
        QDir recordDir = recordInfo.absoluteDir();
        const QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-60);
        const QString suffixChar = QStringLiteral("[A-Za-z0-9]");
        const QFileInfoList strays =
            recordDir.entryInfoList({recordInfo.fileName() + QLatin1Char('.') + suffixChar.repeated(6)},
                                    QDir::Files | QDir::Hidden | QDir::System | QDir::CaseSensitive);
        for (const QFileInfo& stray : strays) {
            if (stray.lastModified(QTimeZone::UTC) < cutoff) {
                recordDir.remove(stray.fileName());
            }
        }
    }
    Record record = read();
    if (!record.loading.isEmpty()) {
        if (record.pid != QCoreApplication::applicationPid() && !record.process.isEmpty()
            && processIdentity(record.pid) == record.process) {
            qCInfo(lcFacetHost).noquote()
                << "FacetHost: process" << record.pid << "is loading module" << record.loading;
        } else {
            const QString crashed = record.loading;
            record.loading.clear();
            record.pid = 0;
            record.process.clear();
            if (!record.disabled.contains(crashed)) {
                record.disabled.append(crashed);
            }
            if (write(record)) {
                qCWarning(lcFacetHost).noquote()
                    << "FacetHost: module" << crashed
                    << "was loading when Facet last stopped unexpectedly; it stays disabled until it is retried";
            } else {
                // Unable to clear the loading field, the record would name the
                // module again at every start. Ignore it rather than disable
                // the module forever.
                record.disabled.removeAll(crashed);
                qCWarning(lcFacetHost).noquote()
                    << "FacetHost: module" << crashed << "was loading when Facet last stopped, but" << m_path
                    << "cannot be written, so it is not disabled";
            }
        }
    }
    m_disabled = QSet<QString>(record.disabled.cbegin(), record.disabled.cend());
}

bool FacetLoadRecord::isDisabled(const QString& moduleId) const
{
    return m_disabled.contains(moduleId);
}

void FacetLoadRecord::enable(const QString& moduleId)
{
    m_disabled.remove(moduleId);
    persistDisabled();
}

QStringList FacetLoadRecord::disabledList() const
{
    QStringList disabled(m_disabled.cbegin(), m_disabled.cend());
    std::sort(disabled.begin(), disabled.end());
    return disabled;
}

FacetLoadRecord::Record FacetLoadRecord::read() const
{
    Record record;
    if (m_path.isEmpty()) {
        return record;
    }
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return record;
    }
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    record.loading = json.value(RecordLoadingKey).toString();
    record.pid = json.value(RecordPidKey).toInteger();
    record.process = json.value(RecordProcessKey).toString();
    for (const QJsonValue& id : json.value(RecordDisabledKey).toArray()) {
        if (id.isString() && !id.toString().isEmpty() && !record.disabled.contains(id.toString())) {
            record.disabled.append(id.toString());
        }
    }
    return record;
}

bool FacetLoadRecord::write(const Record& record, Write mode) const
{
    if (m_path.isEmpty()) {
        return false;
    }
    QJsonObject json;
    if (!record.loading.isEmpty()) {
        json.insert(RecordLoadingKey, record.loading);
        json.insert(RecordPidKey, record.pid);
        json.insert(RecordProcessKey, record.process);
    }
    if (!record.disabled.isEmpty()) {
        json.insert(RecordDisabledKey, QJsonArray::fromStringList(record.disabled));
    }
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    const QByteArray bytes = QJsonDocument(json).toJson(QJsonDocument::Compact);
    // Atomic replace in both modes, because the file also holds the disabled
    // list, which a write cut short by a crash must not lose.
    //
    // A record that is a symbolic link takes the QSaveFile path: it writes
    // through the link to its target, where a rename would put a regular file
    // in place of the link and leave the target stale.
    const QFileInfo existing(m_path);
    if (mode == Write::ProcessCrashSafe && !existing.isSymLink()) {
        // QSaveFile refuses to replace a record that is not writable, and so
        // does this path, so both modes fail on the same file.
        if (existing.exists() && !existing.isWritable()) {
            qCWarning(lcFacetHost).noquote() << "FacetHost: could not write the load record" << m_path;
            return false;
        }
        // QSaveFile::commit() syncs the file to disk (an fdatasync), which
        // costs milliseconds on the GUI thread for each write around a
        // module load. Once written and renamed into place the record is in
        // the kernel, which is all a crash of this process needs. A power
        // loss can then lose the write or leave an empty record, which
        // forgets the disabled list: each module on it is loaded once more
        // and, if it still crashes Facet, disabled again.
        //
        // fileName() gives the file a name in the record's directory when
        // Qt opened it unnamed, and is empty when it cannot.
        //
        // QTemporaryFile creates the file owner-only. An existing record keeps
        // its own permissions, as QSaveFile keeps them.
        QTemporaryFile file(m_path + QStringLiteral(".XXXXXX"));
        const bool written = file.open() && (!existing.exists() || file.setPermissions(existing.permissions()))
            && file.write(bytes) == bytes.size() && file.flush();
        const QString temporaryName = written ? file.fileName() : QString();
        if (!temporaryName.isEmpty()
            && std::rename(QFile::encodeName(temporaryName).constData(), QFile::encodeName(m_path).constData()) == 0) {
            // The name is the record's now. Removing the temporary file's
            // name on destruction would remove nothing, or whatever took the
            // old name since.
            file.setAutoRemove(false);
            return true;
        }
        qCWarning(lcFacetHost).noquote() << "FacetHost: could not write the load record" << m_path;
        return false;
    }
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) < 0 || !file.commit()) {
        qCWarning(lcFacetHost).noquote() << "FacetHost: could not write the load record" << m_path;
        return false;
    }
    return true;
}

bool FacetLoadRecord::recordLoading(const QString& moduleId)
{
    if (m_path.isEmpty()) {
        return false;
    }
    Record record;
    record.loading = moduleId;
    record.pid = QCoreApplication::applicationPid();
    record.process = processIdentity(record.pid);
    record.disabled = disabledList();
    // The disabled list is written unchanged, so this write only has to
    // outlive a crash of the module about to load.
    return write(record, Write::ProcessCrashSafe);
}

void FacetLoadRecord::clearLoading(const QString& moduleId, bool recorded)
{
    // Nothing named the module when its record could not be written, so there
    // is nothing to take back.
    if (!recorded || m_path.isEmpty()) {
        return;
    }
    // A record left naming the module would make the next start disable a
    // module that loaded fine, so try once more before giving up.
    if (!persistDisabled(Write::ProcessCrashSafe) && !persistDisabled(Write::ProcessCrashSafe)) {
        qCWarning(lcFacetHost).noquote() << "FacetHost: the load record" << m_path << "still names module" << moduleId
                                         << "as loading; the next start will treat it as having crashed";
    }
}

bool FacetLoadRecord::persistDisabled(Write mode)
{
    if (m_path.isEmpty()) {
        return false;
    }
    Record record;
    record.disabled = disabledList();
    return write(record, mode);
}

} // namespace PhosphorControl
