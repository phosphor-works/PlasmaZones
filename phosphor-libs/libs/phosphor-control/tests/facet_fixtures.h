// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// What the Facet tests that load fixture modules share: where tests/
// CMakeLists.txt puts the built fixtures, and the sweep of the staging
// directories a run that died left behind.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>

#ifndef PHOSPHOR_CONTROL_FIXTURE_DIR
#error "PHOSPHOR_CONTROL_FIXTURE_DIR must be defined by tests/CMakeLists.txt"
#endif
#ifndef PHOSPHOR_CONTROL_STAGING_DIR
#error "PHOSPHOR_CONTROL_STAGING_DIR must be defined by tests/CMakeLists.txt"
#endif

namespace PhosphorControlTest {

/// The built fixture module @p variant.
inline QString fixturePath(const QString& variant)
{
    return QStringLiteral(PHOSPHOR_CONTROL_FIXTURE_DIR) + QLatin1Char('/') + variant + QStringLiteral(".so");
}

/// Remove every directory directly under the build tree's staging directory
/// whose name @p owned matches, with the id of the process that made it as
/// its first capture, when that process is gone: a run that crashed, was
/// killed or timed out never cleaned up. ctest can run a test beside another
/// copy of itself, so a live process's directories stay.
inline void sweepDeadStaging(const QRegularExpression& owned)
{
    const QFileInfoList entries = QDir(QStringLiteral(PHOSPHOR_CONTROL_STAGING_DIR))
                                      .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo& entry : entries) {
        const QRegularExpressionMatch match = owned.match(entry.fileName());
        if (!match.hasMatch()) {
            continue;
        }
        const qint64 pid = match.captured(1).toLongLong();
        if (pid == QCoreApplication::applicationPid() || QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid))) {
            continue;
        }
        // A test that takes the write bit off its directory puts it back on
        // the way out, which a run that died never did.
        QFile::setPermissions(entry.filePath(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QDir(entry.filePath()).removeRecursively();
    }
}

} // namespace PhosphorControlTest
