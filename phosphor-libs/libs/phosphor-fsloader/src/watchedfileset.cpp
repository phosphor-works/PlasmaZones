// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorFsLoader/WatchedFileSet.h>

#include "identitychain_p.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLoggingCategory>
#include <QThread>

#include <sys/stat.h>

namespace PhosphorFsLoader {

namespace {
Q_LOGGING_CATEGORY(lcFileSet, "phosphorfsloader.watchedfileset")
} // namespace

WatchedFileSet::WatchedFileSet(QObject* parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_watches(std::make_unique<detail::IdentityWatches>(*m_watcher, detail::IdentityWatches::Leaf::Watch, &lcFileSet))
{
    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(DefaultDebounceMs);
    connect(&m_debounceTimer, &QTimer::timeout, this, &WatchedFileSet::onDebounceElapsed);
    m_identityTimer.setSingleShot(true);
    m_identityTimer.setInterval(DefaultDebounceMs);
    connect(&m_identityTimer, &QTimer::timeout, this, &WatchedFileSet::onIdentityWindowElapsed);
    // An event says only that SOMETHING in a directory on the way to a
    // watched path moved, and the chain walk and the fingerprint pass decide
    // whether it was one of ours. Events that can change a file in place
    // restart the debounce, so an editor's save lands in one report. Events
    // further up, where unrelated churn is the norm, only open an identity
    // window: restarting the debounce on them let a busy `/tmp` postpone a
    // report for as long as the churn lasted.
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this] {
        m_debounceTimer.start();
    });
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString& path) {
        if (m_watches->isHolderDirectory(path)) {
            m_debounceTimer.start();
        } else if (!m_identityTimer.isActive()) {
            m_identityTimer.start();
        }
    });
}

WatchedFileSet::~WatchedFileSet() = default;

void WatchedFileSet::setPaths(const QStringList& paths)
{
    // Asserted AND guarded, like WatchedDirectorySet's mutators: this installs
    // kernel watches and rewrites the fingerprint table, so a release build
    // must refuse an off-thread call rather than race the debounce slot.
    if (thread() != QThread::currentThread()) {
        Q_ASSERT_X(false, "WatchedFileSet::setPaths", "GUI-thread only — see class docs");
        qCWarning(lcFileSet) << "setPaths called off the owning thread; refusing";
        return;
    }

    QStringList normalised;
    normalised.reserve(paths.size());
    for (const QString& path : paths) {
        if (path.isEmpty()) {
            continue;
        }
        const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        if (!normalised.contains(clean)) {
            normalised.append(clean);
        }
    }
    m_paths = std::move(normalised);

    // Watches first, fingerprints second. A change landing after the watch is
    // armed raises an event and is caught by the next debounce; one landing
    // before it is already in the baseline. The reverse order leaves a window
    // in which a change is neither.
    m_watches->refresh(m_paths);

    QHash<QString, Fingerprint> fingerprints;
    fingerprints.reserve(m_paths.size());
    for (const QString& path : std::as_const(m_paths)) {
        const auto it = m_fingerprints.constFind(path);
        fingerprints.insert(path, it != m_fingerprints.constEnd() ? it.value() : fingerprintOf(path));
    }
    m_fingerprints = std::move(fingerprints);
}

QStringList WatchedFileSet::paths() const
{
    return m_paths;
}

void WatchedFileSet::setDebounceInterval(int ms)
{
    m_debounceTimer.setInterval(qMax(0, ms));
    m_identityTimer.setInterval(qMax(0, ms));
}

int WatchedFileSet::debounceInterval() const
{
    return m_debounceTimer.interval();
}

QStringList WatchedFileSet::watchedPathsForTest() const
{
    QStringList watched = m_watcher->files() + m_watcher->directories();
    watched.sort();
    return watched;
}

WatchedFileSet::Fingerprint WatchedFileSet::fingerprintOf(const QString& path)
{
    // stat(), not lstat(): a symlinked config is the common dotfiles layout and
    // its content is what the caller cares about. Retargeting the link changes
    // the inode seen through it, so that still reports.
    struct stat st{};
    if (::stat(QFile::encodeName(path).constData(), &st) != 0) {
        return {};
    }
    Fingerprint fp;
    fp.exists = true;
    fp.device = static_cast<quint64>(st.st_dev);
    fp.inode = static_cast<quint64>(st.st_ino);
    fp.size = static_cast<qint64>(st.st_size);
    fp.mtimeSec = static_cast<qint64>(st.st_mtim.tv_sec);
    fp.mtimeNsec = static_cast<qint64>(st.st_mtim.tv_nsec);
    return fp;
}

void WatchedFileSet::onDebounceElapsed()
{
    // Same order as setPaths: walk the chains and re-arm first, then
    // fingerprint, so nothing that lands in between is lost. A file replaced
    // by rename is a new inode at the end of its chain, so its watch is
    // re-armed there. That matters even though Qt drops a watch whose inode is
    // freed: the kernel frees it only once every open descriptor on it is
    // closed, so the watch can still be pinned to the retired inode here.
    m_watches->refresh(m_paths);

    QStringList changed;
    for (const QString& path : std::as_const(m_paths)) {
        const Fingerprint now = fingerprintOf(path);
        Fingerprint& previous = m_fingerprints[path];
        if (now == previous) {
            continue;
        }
        previous = now;
        changed.append(path);
    }
    if (!changed.isEmpty()) {
        Q_EMIT filesChanged(changed);
    }
}

void WatchedFileSet::onIdentityWindowElapsed()
{
    // A chain that did not move leaves every fingerprint as it was, and a
    // pass here could cut into the debounce of an edit in progress.
    if (!m_watches->refresh(m_paths).isEmpty()) {
        onDebounceElapsed();
    }
}

} // namespace PhosphorFsLoader
