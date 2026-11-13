// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// ModuleHost's load record: the file that names the module being loaded and
// the modules disabled after a crash (ModuleHost::setSentinelPath). Private to
// the library.

#include <QLoggingCategory>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtTypes>

namespace PhosphorControl {

// Defined in modulehost.cpp. The load record logs under the host's category,
// so its messages are filtered with the host's.
Q_DECLARE_LOGGING_CATEGORY(lcModuleHost)

/// Remembers a crash while a module loads. The module's id is written to the
/// record before its library is loaded and taken out again once the load no
/// longer needs it. A record left naming a module at the next start disables
/// that module, and the record keeps it disabled across starts until it is
/// enabled again (enable()).
///
/// It owns the record's path and the set of disabled modules, and nothing of
/// the host. GUI thread only, like the host. One ModuleHost per record: two
/// hosts on one record, in one process or two, each write their own disabled
/// list over the other's.
class ModuleLoadRecord
{
public:
    /// How far a write of the record goes before it counts as done. Both
    /// replace the record atomically, so a reader sees the old record or the
    /// new one and never a part of one.
    enum class Write : quint8 {
        /// The new record's data reaches the disk before it replaces the old
        /// one (QSaveFile). For the writes that change the disabled list.
        Durable,
        /// The new record replaces the old one without waiting for the disk,
        /// which survives a crash of the process but not a power loss. For
        /// the pair of writes around every module load, which only have to
        /// outlive a crash of this process. A record that is a symbolic link
        /// is written durably through it, as Durable writes it.
        ProcessCrashSafe,
    };

    /// The record is at `$XDG_STATE_HOME/phosphor-works/control-loading`, or
    /// nowhere when no state directory is available.
    ModuleLoadRecord();

    /// Empty when crashes are not remembered.
    [[nodiscard]] QString path() const;
    /// A relative @p path is made absolute against the working directory of
    /// this call. Empty turns crash memory off.
    void setPath(const QString& path);

    /// Read the record a previous run left, once, as the host starts: remove
    /// the temporary files an interrupted write left behind, disable the
    /// module the record names as loading unless the process loading it is
    /// still running, and take the disabled list from the record.
    void restore();

    [[nodiscard]] bool isDisabled(const QString& moduleId) const;
    /// Take @p moduleId off the disabled list, and write the list (Durable).
    /// False when the list could not be written, so the record on disk still
    /// names the module, until a later write of the record succeeds.
    bool enable(const QString& moduleId);

    /// Write @p moduleId into the record as loading. False when it was not
    /// written (no record, or the write failed).
    bool recordLoading(const QString& moduleId);
    /// Take @p moduleId, the loading module, back out of the record, when
    /// recordLoading put it there (@p recorded).
    void clearLoading(const QString& moduleId, bool recorded);
    /// Write the record with the disabled list and no loading module.
    bool persistDisabled(Write mode = Write::Durable);

private:
    struct Record
    {
        QString loading;
        qint64 pid = 0;
        /// processIdentity() of the loading process, so a later process that
        /// reuses its pid is not mistaken for it.
        QString process;
        QStringList disabled;
    };

    [[nodiscard]] Record read() const;
    bool write(const Record& record, Write mode = Write::Durable) const;
    /// The disabled list, sorted, as the record holds it.
    [[nodiscard]] QStringList disabledList() const;

    QString m_path;
    /// setPath() was handed an empty path, which turns crash memory off on
    /// purpose, as opposed to there being no state directory.
    bool m_turnedOff = false;
    QSet<QString> m_disabled;
};

} // namespace PhosphorControl
