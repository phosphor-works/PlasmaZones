// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <dlfcn.h>

#ifndef PHOSPHOR_REGISTRY_FIXTURE_DIR
#error "PHOSPHOR_REGISTRY_FIXTURE_DIR must be defined by tests/CMakeLists.txt"
#endif

namespace PhosphorRegistryTestHelpers {

void messageCapturingHandler(QtMsgType type, const QMessageLogContext& context, const QString& message);

/// Captures every warning (and, on request, info line) logged while in scope.
/// qInstallMessageHandler takes a plain function, so the active capture is
/// reached through a static pointer. A capture opened inside another one
/// passes each line on to the outer capture as well.
class MessageCapture
{
public:
    explicit MessageCapture(QStringList& warnings, QStringList* infos = nullptr)
        : m_warnings(&warnings)
        , m_infos(infos)
    {
        m_priorActive = s_active;
        s_active = this;
        m_priorHandler = qInstallMessageHandler(messageCapturingHandler);
    }
    ~MessageCapture()
    {
        qInstallMessageHandler(m_priorHandler);
        s_active = m_priorActive;
    }
    Q_DISABLE_COPY_MOVE(MessageCapture)

private:
    inline static MessageCapture* s_active = nullptr;
    QtMessageHandler m_priorHandler = nullptr;
    QStringList* m_warnings = nullptr;
    QStringList* m_infos = nullptr;
    MessageCapture* m_priorActive = nullptr;

    friend void messageCapturingHandler(QtMsgType type, const QMessageLogContext& context, const QString& message);
};

inline void messageCapturingHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    MessageCapture* const active = MessageCapture::s_active;
    if (!active) {
        return;
    }
    if (type == QtWarningMsg && active->m_warnings) {
        active->m_warnings->append(message);
    } else if (type == QtInfoMsg && active->m_infos) {
        active->m_infos->append(message);
    }
    if (active->m_priorHandler) {
        // The prior handler is this function again when captures nest. It
        // must see the outer capture as active, or it would call itself
        // with the same capture forever.
        MessageCapture::s_active = active->m_priorActive;
        active->m_priorHandler(type, context, message);
        MessageCapture::s_active = active;
    }
}

/// How many entries of @p lines contain @p needle.
[[nodiscard]] inline int countContaining(const QStringList& lines, const QString& needle)
{
    int count = 0;
    for (const QString& line : lines) {
        if (line.contains(needle)) {
            ++count;
        }
    }
    return count;
}

/// The built fixture library for @p variant (see tests/CMakeLists.txt).
[[nodiscard]] inline QString fixturePath(const QString& variant)
{
    return QStringLiteral(PHOSPHOR_REGISTRY_FIXTURE_DIR) + QLatin1Char('/') + variant + QStringLiteral(".so");
}

/// rwxr-xr-x: the owner may write, nobody else may, which the catalog's
/// permission check requires of a plugin and its directory.
inline constexpr QFileDevice::Permissions OwnerWritableMode = QFileDevice::ReadOwner | QFileDevice::WriteOwner
    | QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther
    | QFileDevice::ExeOther;

/// Set @p path to OwnerWritableMode and confirm the filesystem kept it.
[[nodiscard]] inline bool makeOwnerWritable(const QString& path)
{
    if (!QFile::setPermissions(path, OwnerWritableMode)) {
        qWarning() << "cannot set the mode of" << path;
        return false;
    }
    if (QFile::permissions(path) & (QFileDevice::WriteGroup | QFileDevice::WriteOther)) {
        qWarning() << "the filesystem ignored the mode set on" << path;
        return false;
    }
    return true;
}

/// Write @p bytes to @p destination with OwnerWritableMode, replacing any
/// file there with a new one, so a library loaded from the old file stays
/// mapped unchanged. Returns the destination, or an empty string on failure.
[[nodiscard]] inline QString writePlugin(const QString& destination, const QByteArray& bytes)
{
    QFile::remove(destination);
    QFile file(destination);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        qWarning() << "cannot write" << destination;
        return QString();
    }
    file.close();
    return makeOwnerWritable(destination) ? destination : QString();
}

/// Copy the fixture @p variant into @p directory as @p fileName (default
/// `<variant>.so`), creating the directory, with OwnerWritableMode on the file
/// and the directory so the catalog's permission check passes. Returns the
/// destination path, or an empty string on failure.
[[nodiscard]] inline QString stagePlugin(const QString& directory, const QString& variant,
                                         const QString& fileName = QString())
{
    if (!QDir().mkpath(directory) || !makeOwnerWritable(directory)) {
        qWarning() << "stagePlugin: cannot prepare" << directory;
        return QString();
    }
    QFile source(fixturePath(variant));
    if (!source.open(QIODevice::ReadOnly)) {
        qWarning() << "stagePlugin: missing fixture" << source.fileName();
        return QString();
    }
    return writePlugin(
        QDir(directory).absoluteFilePath(fileName.isEmpty() ? variant + QStringLiteral(".so") : fileName),
        source.readAll());
}

/// Like stagePlugin, but the copy's plugin metadata says it was built against
/// Qt @p major.@p minor. Qt keeps that in the `qt-project!` ELF note, right
/// after a format byte. The bytes are Qt-private, so this fails loudly rather
/// than patching the wrong place when they do not match exactly once.
[[nodiscard]] inline QString stagePluginBuiltAgainstQt(const QString& directory, const QString& variant, int major,
                                                       int minor, const QString& fileName = QString())
{
    QFile source(fixturePath(variant));
    if (!QDir().mkpath(directory) || !makeOwnerWritable(directory) || !source.open(QIODevice::ReadOnly)) {
        qWarning() << "stagePluginBuiltAgainstQt: cannot prepare" << directory << "or read" << source.fileName();
        return QString();
    }
    QByteArray bytes = source.readAll();
    QByteArray header = QByteArrayLiteral("qt-project!");
    header.append('\0');
    header.append('\x01');
    header.append(char(QT_VERSION_MAJOR));
    header.append(char(QT_VERSION_MINOR));
    const qsizetype at = bytes.indexOf(header);
    if (at < 0 || bytes.indexOf(header, at + 1) >= 0) {
        qWarning() << "stagePluginBuiltAgainstQt: the Qt metadata header of" << source.fileName()
                   << "is not where this helper expects it";
        return QString();
    }
    bytes[at + header.size() - 2] = char(major);
    bytes[at + header.size() - 1] = char(minor);
    return writePlugin(
        QDir(directory).absoluteFilePath(fileName.isEmpty() ? variant + QStringLiteral(".so") : fileName), bytes);
}

/// Whether the library at @p canonicalPath is mapped into this process.
/// glibc matches an already-loaded library by its path or by its file, so
/// pass the canonical path, captured before the file is removed.
[[nodiscard]] inline bool isMapped(const QString& canonicalPath)
{
    void* const handle = ::dlopen(QFile::encodeName(canonicalPath).constData(), RTLD_NOW | RTLD_NOLOAD);
    if (handle) {
        ::dlclose(handle);
    }
    return handle != nullptr;
}

} // namespace PhosphorRegistryTestHelpers
