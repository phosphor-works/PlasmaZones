// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "identitychain_p.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLoggingCategory>

#include <algorithm>

#include <sys/stat.h>
#include <unistd.h>

namespace PhosphorFsLoader::detail {

namespace {

/// Linux's MAXSYMLINKS: past it the kernel fails the lookup with ELOOP.
constexpr int kMaxSymlinks = 40;

NodeIdentity identityOf(const struct stat& st)
{
    return {static_cast<quint64>(st.st_dev), static_cast<quint64>(st.st_ino)};
}

/// readlink() into a buffer that grows until the text fits. Empty when the
/// link went away or cannot be read.
QByteArray readLinkText(const QByteArray& path)
{
    QByteArray buffer(256, Qt::Uninitialized);
    for (;;) {
        const ssize_t length = ::readlink(path.constData(), buffer.data(), static_cast<size_t>(buffer.size()));
        if (length < 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.truncate(static_cast<qsizetype>(length));
            return buffer;
        }
        if (buffer.size() >= 64 * 1024) {
            return {}; // no sane link holds this much
        }
        buffer.resize(buffer.size() * 2);
    }
}

QString childPath(const QString& directory, const QString& name)
{
    if (directory == QLatin1String("/")) {
        return QLatin1Char('/') + name;
    }
    return directory + QLatin1Char('/') + name;
}

} // namespace

IdentityChain resolveIdentityChain(const QString& path)
{
    IdentityChain chain;
    // Components still to walk, the NEXT one last, so a symlink's text is
    // spliced in front of the rest by appending.
    QStringList pending = QDir::cleanPath(path).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    std::reverse(pending.begin(), pending.end());

    struct stat st{};
    QString current = QStringLiteral("/");
    if (::lstat("/", &st) != 0) {
        return chain;
    }
    chain.entries.append({current, identityOf(st), true, {}});

    int symlinks = 0;
    while (!pending.isEmpty()) {
        const QString name = pending.takeLast();
        if (name == QLatin1String(".")) {
            continue;
        }
        if (name == QLatin1String("..")) {
            // `current` is physical, so its lexical parent is its real parent.
            current = QFileInfo(current).absolutePath();
            continue;
        }
        const QString candidate = childPath(current, name);
        chain.holder = current;
        const QByteArray encoded = QFile::encodeName(candidate);
        if (::lstat(encoded.constData(), &st) != 0) {
            return chain; // missing (or unreadable): the chain ends in `current`
        }
        if (S_ISLNK(st.st_mode)) {
            const QByteArray text = readLinkText(encoded);
            if (++symlinks > kMaxSymlinks || text.isEmpty()) {
                return chain;
            }
            chain.entries.append({candidate, identityOf(st), false, text});
            const QString target = QFile::decodeName(text);
            const QStringList parts = target.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            for (auto it = parts.crbegin(); it != parts.crend(); ++it) {
                pending.append(*it);
            }
            if (target.startsWith(QLatin1Char('/'))) {
                current = QStringLiteral("/");
            }
            continue;
        }
        const bool isDirectory = S_ISDIR(st.st_mode);
        chain.entries.append({candidate, identityOf(st), isDirectory, {}});
        if (!isDirectory) {
            if (!pending.isEmpty()) {
                return chain; // a file used as a directory: nothing below it exists
            }
            chain.resolved = candidate;
            chain.resolvedIdentity = identityOf(st);
            return chain;
        }
        current = candidate;
    }
    // Every component walked and the last one was a directory (or the path
    // ended in `..`). Its identity is the last recorded entry for it.
    chain.resolved = current;
    for (auto it = chain.entries.crbegin(); it != chain.entries.crend(); ++it) {
        if (it->path == current) {
            chain.resolvedIdentity = it->identity;
            break;
        }
    }
    return chain;
}

IdentityWatches::IdentityWatches(QFileSystemWatcher& watcher, Leaf leaf, Category category)
    : m_watcher(watcher)
    , m_leaf(leaf)
    , m_category(category)
{
}

QStringList IdentityWatches::refresh(const QStringList& paths, const HeldByOwner& heldByOwner)
{
    QHash<QString, IdentityChain> chains;
    chains.reserve(paths.size());
    QHash<QString, NodeIdentity> desired;
    QSet<QString> resolved;
    QSet<QString> holders;
    QStringList moved;
    for (const QString& path : paths) {
        IdentityChain chain = resolveIdentityChain(path);
        const auto previous = m_chains.constFind(path);
        if (previous != m_chains.constEnd() && previous.value() != chain) {
            moved.append(path);
        }
        for (const IdentityChain::Entry& entry : std::as_const(chain.entries)) {
            if (entry.isDirectory && entry.path != chain.resolved) {
                desired.insert(entry.path, entry.identity);
            }
        }
        if (!chain.holder.isEmpty()) {
            holders.insert(chain.holder);
        }
        if (!chain.resolved.isEmpty()) {
            resolved.insert(chain.resolved);
            if (m_leaf == Leaf::Watch) {
                desired.insert(chain.resolved, chain.resolvedIdentity);
            }
        }
        chains.insert(path, std::move(chain));
    }
    m_chains = std::move(chains);
    m_resolved = std::move(resolved);
    m_holders = std::move(holders);
    arm(desired, heldByOwner);
    return moved;
}

bool IdentityWatches::holds(const QString& path) const
{
    return m_armed.contains(path);
}

bool IdentityWatches::isResolvedPath(const QString& path) const
{
    return m_resolved.contains(path);
}

bool IdentityWatches::isHolderDirectory(const QString& path) const
{
    return m_holders.contains(path);
}

void IdentityWatches::arm(const QHash<QString, NodeIdentity>& desired, const HeldByOwner& heldByOwner)
{
    const auto ownerHolds = [&heldByOwner](const QString& path) {
        return heldByOwner && heldByOwner(path);
    };

    // Diff against the watcher's REAL state as well as the record: Qt drops a
    // path from files()/directories() when the kernel drops its watch (the
    // directory was deleted), and that path has to be added again.
    const QStringList watchedFiles = m_watcher.files();
    const QStringList watchedDirectories = m_watcher.directories();
    QSet<QString> current(watchedFiles.cbegin(), watchedFiles.cend());
    current.unite(QSet<QString>(watchedDirectories.cbegin(), watchedDirectories.cend()));

    QStringList toRemove;
    for (auto it = m_armed.begin(); it != m_armed.end();) {
        if (desired.contains(it.key())) {
            ++it;
            continue;
        }
        if (current.contains(it.key()) && !ownerHolds(it.key())) {
            toRemove.append(it.key());
        }
        it = m_armed.erase(it);
    }

    QStringList toAdd;
    for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
        const QString& path = it.key();
        if (!current.contains(path)) {
            m_armed.remove(path);
            toAdd.append(path);
            continue;
        }
        const auto armed = m_armed.constFind(path);
        if (armed != m_armed.cend() && armed.value() == it.value()) {
            continue;
        }
        if (armed == m_armed.cend() && ownerHolds(path)) {
            // The owner armed it, and its own identity check keeps it fresh.
            m_armed.insert(path, it.value());
            continue;
        }
        // The path names another inode than the one its watch is on.
        toRemove.append(path);
        toAdd.append(path);
    }

    if (!toRemove.isEmpty()) {
        m_watcher.removePaths(toRemove);
    }
    if (toAdd.isEmpty()) {
        return;
    }
    // inotify needs read permission on what it watches, and Qt warns on every
    // failed add but a missing path. A directory the walk may cross but not
    // list (mode 711) cannot be watched at all, so it is left out quietly
    // rather than retried and warned about at every event.
    QStringList addable;
    addable.reserve(toAdd.size());
    for (const QString& path : std::as_const(toAdd)) {
        if (::access(QFile::encodeName(path).constData(), R_OK) == 0) {
            addable.append(path);
        }
    }
    if (addable.isEmpty()) {
        return;
    }
    const QStringList failedList = m_watcher.addPaths(addable);
    const QSet<QString> failed(failedList.cbegin(), failedList.cend());
    for (const QString& path : std::as_const(addable)) {
        if (!failed.contains(path)) {
            m_armed.insert(path, desired.value(path));
            m_warnedFailedWatchPaths.remove(path);
            continue;
        }
        // A path that vanished between the walk and addPaths() is a race the
        // event that removed it resolves; only a path still present is a
        // real failure (inotify quota) worth a warning.
        if (QFileInfo::exists(path) && !m_warnedFailedWatchPaths.contains(path)) {
            m_warnedFailedWatchPaths.insert(path);
            qCWarning(m_category) << "failed to watch" << path
                                  << "— check the filesystem-watcher quota (inotify on Linux)";
        }
    }
}

} // namespace PhosphorFsLoader::detail
