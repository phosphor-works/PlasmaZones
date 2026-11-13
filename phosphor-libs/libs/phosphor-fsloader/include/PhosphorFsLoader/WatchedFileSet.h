// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorFsLoader/phosphorfsloader_export.h>

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>

#include <memory>

QT_BEGIN_NAMESPACE
class QFileSystemWatcher;
QT_END_NAMESPACE

namespace PhosphorFsLoader {

namespace detail {
class IdentityWatches;
}

/**
 * @brief Watch an explicit set of files and report only the ones that really changed.
 *
 * The file-level sibling of `WatchedDirectorySet`. Where that class rescans a
 * directory tree through a strategy, this one watches a caller-chosen list of
 * individual paths (a config file and the theme it names, a shader's include
 * and asset dependencies, one user file a script asked about) and tells the
 * caller which of them changed.
 *
 *   • Any path may not exist yet, and any directory on the way to it may be
 *     a symlink. Each path is resolved component by component, the way the
 *     kernel resolves it, and the set watches every directory the walk
 *     passes through, from `/` down to the file's own directory (or the
 *     deepest one that exists), plus the file itself when it exists. Paths
 *     that share directories share their watches.
 *   • An inotify watch stays on the inode its path named when it was added,
 *     so a renamed or retargeted directory would leave every watch below it
 *     on the old tree. The set therefore records the identity chain of each
 *     path: the device and inode of every directory walked and the text of
 *     every symlink followed. After an event it walks the chains again, and
 *     only a watch whose path now names another inode is re-armed. A
 *     symlinked directory that is retargeted (Home Manager, stow, chezmoi), a
 *     parent or grandparent directory replaced by rename, a dangling link
 *     whose target appears and a missing directory that is created are all
 *     followed this way.
 *   • Each path carries a fingerprint (existence, device, inode, size and
 *     modification time to the nanosecond). After the debounce, one
 *     `filesChanged` carries exactly the paths whose fingerprint moved. A
 *     write to an unrelated file beside a watched one, or anything created,
 *     renamed or deleted in an ancestor directory, wakes the watcher but
 *     emits nothing. Its cost is a few `lstat()` calls for each path. Only
 *     events on a file or in the directory that holds it restart the
 *     debounce. An event further up opens a window of the same length that
 *     later events do not extend, so churn there cannot postpone a report.
 *
 * Unlike `WatchedDirectorySet`'s own watches, an ancestor watch here is never
 * refused for being a high-churn root such as `$HOME` or `/`: every one of
 * them can hold the entry that moves, and the identity chain turns unrelated
 * churn there into a few `lstat()` calls per debounce rather than a signal.
 *
 * Paths are normalised to clean absolute form (`QFileInfo::absoluteFilePath`
 * then `QDir::cleanPath`), and that is the spelling `paths()` returns and
 * `filesChanged` reports. Symlinks are followed for the fingerprint, so
 * retargeting a link counts as a change of the path. The watches themselves
 * are held under physical paths, with no symlink in them.
 *
 * POSIX only: the fingerprint is read with `stat()` and the chain with
 * `lstat()` and `readlink()`.
 *
 * ## Thread safety
 *
 * GUI-thread only, like `WatchedDirectorySet`. The watcher and the debounce
 * timer live on the thread the set was constructed on.
 */
class PHOSPHORFSLOADER_EXPORT WatchedFileSet : public QObject
{
    Q_OBJECT

public:
    /// Default debounce, long enough to coalesce an editor's
    /// write-temp-then-rename save into one report.
    static constexpr int DefaultDebounceMs = 100;

    explicit WatchedFileSet(QObject* parent = nullptr);
    ~WatchedFileSet() override;

    WatchedFileSet(const WatchedFileSet&) = delete;
    WatchedFileSet& operator=(const WatchedFileSet&) = delete;

    /**
     * @brief Replace the watched set with @p paths.
     *
     * Reconciles rather than accumulates: watches for paths no longer listed
     * are removed, new ones are added, and the underlying watcher ends up
     * holding exactly what the new set needs. Empty entries and duplicates
     * (after normalisation) are dropped.
     *
     * A newly listed path takes its current fingerprint as the baseline and
     * is not reported by this call. A path that stays listed keeps its
     * baseline, so a change still pending in the debounce window is reported
     * as usual.
     */
    void setPaths(const QStringList& paths);

    /// The watched paths in normalised form, in the order given to `setPaths`.
    QStringList paths() const;

    /// Change the debounce window, for reports and for identity checks
    /// alike. Values below 0 are clamped to 0. Takes effect from the next
    /// event; a pending window keeps its interval.
    void setDebounceInterval(int ms);
    int debounceInterval() const;

    /// Test-only: every path the underlying watcher currently holds (files
    /// and directories together, in physical form), sorted. Lets a test pin
    /// that `setPaths` reconciles instead of accumulating, and which ancestors
    /// are watched.
    QStringList watchedPathsForTest() const;

Q_SIGNALS:
    /**
     * @brief Emitted once per debounce window that changed something.
     *
     * @p paths lists, in `paths()` order, every watched path whose
     * fingerprint differs from the last one observed: created, deleted,
     * replaced, or edited in place. Never emitted with an empty list.
     */
    void filesChanged(const QStringList& paths);

private:
    /// Identity and content stamp for one path. `exists == false` compares
    /// equal to any other missing state, so a path that stays absent never
    /// reports.
    struct Fingerprint
    {
        bool exists = false;
        quint64 device = 0;
        quint64 inode = 0;
        qint64 size = 0;
        qint64 mtimeSec = 0;
        qint64 mtimeNsec = 0;

        bool operator==(const Fingerprint&) const = default;
    };

    static Fingerprint fingerprintOf(const QString& path);
    void onDebounceElapsed();
    void onIdentityWindowElapsed();

    QFileSystemWatcher* m_watcher = nullptr;
    /// The identity chains of `m_paths` and the watches they need, on
    /// `m_watcher`.
    std::unique_ptr<detail::IdentityWatches> m_watches;
    QStringList m_paths; ///< normalised, in caller order
    QHash<QString, Fingerprint> m_fingerprints;
    /// Restarted by every event that can change a watched file in place: on
    /// the file itself, or in the directory that holds it.
    QTimer m_debounceTimer;
    /// Opened by the first event in any other directory of a chain and never
    /// restarted, so that steady churn in `/tmp` or `$HOME` cannot postpone a
    /// report. Such an event matters only if it moved a chain, so the window
    /// walks the chains and runs the fingerprint pass only when one moved.
    QTimer m_identityTimer;
};

} // namespace PhosphorFsLoader
