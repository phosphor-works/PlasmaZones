// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>

QT_BEGIN_NAMESPACE
class QFileSystemWatcher;
class QLoggingCategory;
QT_END_NAMESPACE

namespace PhosphorFsLoader::detail {

/// The inode a path named when it was looked at.
struct NodeIdentity
{
    quint64 device = 0;
    quint64 inode = 0;

    bool operator==(const NodeIdentity&) const = default;
};

/// What one path resolves through, entry by entry, from `/` down.
///
/// An inotify watch stays on the inode its path named when the watch was
/// added. Renaming a watched directory, or a directory or symlink anywhere
/// above it, leaves the watch on the old inode, and Qt's directory watch does
/// not even ask for its own rename (no `IN_MOVE_SELF`). Such a change shows
/// only as an entry event in the directory that HOLDS the renamed entry, so
/// every directory on the way must be watched, and a watch can be trusted only
/// while each entry on the way still names what it named when the watch was
/// added. The chain records exactly that: each directory and symlink walked,
/// with its inode and, for a symlink, the text it holds. Two resolutions of a
/// path compare equal only when nothing on the way moved.
///
/// The walk is physical, like the kernel's own: a symlink's text is spliced in
/// front of the components still to walk, and `..` steps to the parent of the
/// directory reached so far. So every path in the chain is free of symlinks,
/// and a watch added on it lands on the inode the chain recorded.
struct IdentityChain
{
    struct Entry
    {
        QString path; ///< physical: no component of it is a symlink
        NodeIdentity identity;
        bool isDirectory = false;
        QByteArray linkTarget; ///< readlink() text of a symlink, empty for anything else

        bool operator==(const Entry&) const = default;
    };

    /// Every entry walked, `/` first. Stops at the first component that does
    /// not exist (or cannot be looked at), so a path that appears later grows
    /// the chain.
    QList<Entry> entries;
    /// Physical path of the entry the whole path names, empty when the walk
    /// did not reach it.
    QString resolved;
    NodeIdentity resolvedIdentity;
    /// Physical directory in which the walk looked up its last entry: the
    /// directory holding the file, or the deepest one that exists. An entry
    /// event there can change what the path names in place (a save by
    /// rename, a creation, a deletion). An event in any other directory of
    /// the chain can only matter by moving the chain itself.
    QString holder;

    bool operator==(const IdentityChain& other) const
    {
        return entries == other.entries;
    }
};

/// Resolve the clean absolute @p path component by component. A cycle of more
/// than 40 symlinks (the kernel's own limit) ends the walk where it is.
IdentityChain resolveIdentityChain(const QString& path);

/// Keeps the watches that the identity chains of a set of paths need, on a
/// borrowed `QFileSystemWatcher`.
///
/// Every directory a chain walked through is watched, shared between the
/// paths that walk through it, and so is the resolved entry when the owner
/// asks for it. Each watch is recorded with the inode its chain resolved, and
/// re-armed when a later resolution names another inode under the same path:
/// Qt deduplicates watches by path string, so without the record a replaced
/// directory would keep its stale watch forever.
class IdentityWatches
{
public:
    enum class Leaf : quint8 {
        Watch, ///< the resolved entry is watched too (a watched file)
        Skip, ///< the owner watches the resolved entry itself (a registered directory)
    };
    /// Whether the owner holds @p path in the same watcher for its own
    /// reasons. Such a path is never dropped here, and a chain that needs it
    /// adopts the owner's watch instead of re-arming it.
    using HeldByOwner = std::function<bool(const QString&)>;
    using Category = const QLoggingCategory& (*)();

    IdentityWatches(QFileSystemWatcher& watcher, Leaf leaf, Category category);

    /// Resolve every path in @p paths and @p entryPaths again and make the
    /// watcher hold what their chains need, on the inodes the chains name
    /// now. The resolved entry of a path in @p paths is watched as the leaf
    /// rule says, and that of a path in @p entryPaths always: an owner that
    /// skips its own leaves still hands over the ones it cannot watch under
    /// their own spelling (a symlink, whose watch would stay on the old
    /// target). Returns the paths of both lists whose chain differs from the
    /// previous call. A path new to the set is not in the result, and a path
    /// no longer listed is forgotten.
    QStringList refresh(const QStringList& paths, const HeldByOwner& heldByOwner = {},
                        const QStringList& entryPaths = {});

    /// Whether @p path is watched for a chain.
    bool holds(const QString& path) const;

    /// Whether @p path is the physical path that one of the listed paths
    /// resolves to. Qt reports an event under one spelling of a watched inode
    /// only, so an owner routing events by path uses this to recognise its
    /// own directory under its physical spelling.
    bool isResolvedPath(const QString& path) const;

    /// Whether @p path is the `holder` of one of the listed paths.
    bool isHolderDirectory(const QString& path) const;

private:
    void arm(const QHash<QString, NodeIdentity>& desired, const HeldByOwner& heldByOwner);

    QFileSystemWatcher& m_watcher;
    Leaf m_leaf;
    Category m_category;
    QHash<QString, IdentityChain> m_chains;
    QSet<QString> m_resolved;
    QSet<QString> m_holders;
    /// The inode each watch was armed for, by path.
    QHash<QString, NodeIdentity> m_armed;
    /// Warn-once latch for watches that fail while their path exists. A
    /// refresh runs on every event and the causes (inotify quota,
    /// permissions) persist. Cleared for a path once a later add succeeds.
    QSet<QString> m_warnedFailedWatchPaths;
};

} // namespace PhosphorFsLoader::detail
