// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorFsLoader/IScanStrategy.h>
#include <PhosphorFsLoader/WatchedDirectorySet.h>

#include "identitychain_p.h"
#include "nearestexistingancestor_p.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <optional>

#include <sys/stat.h>

namespace PhosphorFsLoader {

namespace {
Q_LOGGING_CATEGORY(lcWatcher, "phosphorfsloader.watcheddirset")

/// Paths we refuse to install a `QFileSystemWatcher` on. If the "nearest
/// existing ancestor" climb from `attachWatcherForDir` lands on one of
/// these, we decline the watch and wait for the target tree to be created
/// before arming anything. Watching `$HOME` turns every file operation in
/// the user's home directory into a full rescan of all configured dirs;
/// watching `/` is nonsensical.
///
/// Comparisons go through `samePath` (case-insensitive on macOS APFS /
/// NTFS, case-sensitive on Linux) so a hand-edited XDG dir spelt with
/// different case (`~/.Local/Share/...`) still trips the guard on
/// case-insensitive filesystems where it resolves to the same inode as
/// the canonical `$HOME`. Without this a re-cased ancestor would slip
/// past the byte-equal compare and end up watching `$HOME`.
///
/// Best-effort, platform-default only. Uses the OS family as a proxy for
/// "the typical filesystem on this platform" — won't detect a
/// case-sensitive HFSX volume on macOS or a case-insensitive exFAT mount
/// on Linux. Symlink canonicalisation in `isForbiddenWatchRoot` covers
/// the common foot-gun (deliberate `$HOME` symlink); the remaining
/// edge cases require a stat-and-compare-inode path that nothing in
/// the in-tree consumer set actually exercises.
bool samePath(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
#if defined(Q_OS_DARWIN) || defined(Q_OS_WIN)
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

bool isForbiddenWatchRoot(const QString& path)
{
    const QString cleaned = QDir::cleanPath(path);
    if (cleaned.isEmpty()) {
        return true;
    }
    // Resolve symlinks. A target spelled `~/sym → $HOME` (or
    // `/tmp/foo → ~/.cache`) cleans to itself but its inode is the
    // forbidden root — without canonicalisation, QFileSystemWatcher
    // would follow the symlink at the OS layer and watch the forbidden
    // inode anyway. Empty when the path doesn't exist (canonical
    // resolution requires every chain segment to exist) — the input's
    // cleaned form remains the only check in that case.
    const QString canonical = QFileInfo(path).canonicalFilePath();

    // Canonicalising a candidate root is a realpath() syscall, and this
    // predicate runs for EVERY registered directory on EVERY rescan (the
    // re-attach loops re-enter attachWatcherForDir each pass) against ~15
    // candidates. So cache the realpath() result keyed on the input SPELLING.
    //
    // The cache is process-lifetime and never invalidated, which is sound only
    // under one assumption: a given path spelling resolves to a stable inode
    // for the life of the process. Repointing XDG_DATA_HOME (as tests do)
    // changes the DERIVED candidate spelling, so it lands on a fresh key and is
    // NOT a stale-cache hazard. The one thing this does NOT catch is the same
    // spelling being re-symlinked to a different target mid-process — that
    // would return the stale canonical. We accept that: data roots are not
    // re-symlinked under a running instance, and the empty-result skip below
    // already handles the only real transition (a not-yet-existing root
    // appearing). GUI-thread-only per the class contract, so the static needs
    // no lock.
    const auto cachedCanonical = [](const QString& path) -> QString {
        static QHash<QString, QString> cache;
        const auto it = cache.constFind(path);
        if (it != cache.constEnd()) {
            return it.value();
        }
        const QString canonical = QFileInfo(path).canonicalFilePath();
        // Only cache successful resolutions: a not-yet-existing root (fresh
        // profile) canonicalises empty today and non-empty after first use.
        if (!canonical.isEmpty()) {
            cache.insert(path, canonical);
        }
        return canonical;
    };

    // Compare a candidate forbidden root against `cleaned` AND `canonical`,
    // and also against the candidate's canonical form so that two
    // independent symlinks pointing at the same inode collapse onto the
    // same forbidden-root verdict.
    const auto matches = [&](const QString& candidate) {
        if (candidate.isEmpty()) {
            return false;
        }
        const QString cleanedCandidate = QDir::cleanPath(candidate);
        if (samePath(cleaned, cleanedCandidate)) {
            return true;
        }
        if (!canonical.isEmpty() && samePath(canonical, cleanedCandidate)) {
            return true;
        }
        const QString canonicalCandidate = cachedCanonical(candidate);
        if (!canonicalCandidate.isEmpty()) {
            if (samePath(cleaned, canonicalCandidate)) {
                return true;
            }
            if (!canonical.isEmpty() && samePath(canonical, canonicalCandidate)) {
                return true;
            }
        }
        return false;
    };

    if (matches(QDir::rootPath()) || matches(QDir::homePath())) {
        return true;
    }
    // Also refuse to watch high-churn shared roots — a rescan fires for every
    // unrelated app writing into those trees, effectively equivalent to
    // watching $HOME. GenericData/Config catch the obvious XDG roots; Temp,
    // Runtime, and Cache catch consumers whose target lives under
    // /tmp, /run/user/UID, or ~/.cache and where the climb would otherwise
    // stop at the high-traffic root. Documents/Downloads/Music/Pictures/
    // Movies/Templates/Desktop catch the user-content trees that aren't
    // XDG-rooted but are still high-churn for most users (browser
    // downloads, editor saves, image scanners, screenshot tools, etc.).
    using QSP = QStandardPaths;
    static constexpr QSP::StandardLocation kForbidden[] = {
        QSP::GenericDataLocation, QSP::GenericConfigLocation, QSP::TempLocation,      QSP::RuntimeLocation,
        QSP::CacheLocation,       QSP::GenericCacheLocation,  QSP::DocumentsLocation, QSP::DownloadLocation,
        QSP::MusicLocation,       QSP::PicturesLocation,      QSP::MoviesLocation,    QSP::TemplatesLocation,
        QSP::DesktopLocation,
    };
    for (const auto loc : kForbidden) {
        if (matches(QSP::writableLocation(loc))) {
            return true;
        }
    }
    return false;
}

/// Whether @p path goes through a symlink below the deepest of the
/// registered @p directories that holds it, or anywhere when none does. The
/// identity chain of that directory already follows every link above it, so
/// only the components below it are looked at, one `lstat()` each. A
/// component that does not exist ends the walk: nothing below it can be
/// watched under any spelling.
bool crossesSymlinkBelowRegistered(const QStringList& directories, const QString& path)
{
    const QString clean = QDir::cleanPath(path);
    if (!clean.startsWith(QLatin1Char('/'))) {
        return false;
    }
    QString base = QStringLiteral("/");
    for (const QString& directory : directories) {
        if (clean == directory) {
            return false;
        }
        if (directory.size() > base.size() && clean.startsWith(directory)
            && clean.at(directory.size()) == QLatin1Char('/')) {
            base = directory;
        }
    }
    QString current = base;
    const QStringList below = clean.mid(base.size()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& name : below) {
        if (!current.endsWith(QLatin1Char('/'))) {
            current += QLatin1Char('/');
        }
        current += name;
        struct stat st{};
        if (::lstat(QFile::encodeName(current).constData(), &st) != 0) {
            return false;
        }
        if (S_ISLNK(st.st_mode)) {
            return true;
        }
    }
    return false;
}
} // namespace

WatchedDirectorySet::WatchedDirectorySet(IScanStrategy& strategy, QObject* parent)
    : QObject(parent)
    , m_strategy(&strategy)
{
    // Single-shot debounce coalesces the save-temp-rename storm most
    // editors produce (QSaveFile / most IDEs fire three events in the
    // same ~10 ms) into one rescan.
    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(50);
    connect(&m_debounceTimer, &QTimer::timeout, this, &WatchedDirectorySet::rescanAll);
    m_identityTimer.setSingleShot(true);
    m_identityTimer.setInterval(50);
    connect(&m_identityTimer, &QTimer::timeout, this, &WatchedDirectorySet::onIdentityCheckElapsed);
}

WatchedDirectorySet::~WatchedDirectorySet() = default;

int WatchedDirectorySet::registerDirectory(const QString& directory, LiveReload liveReload)
{
    // Single-path: order is irrelevant (no priority comparison to make
    // against zero peers). Forward with the canonical default.
    return registerDirectories(QStringList{directory}, liveReload, RegistrationOrder::LowestPriorityFirst);
}

namespace {
/// Reverse @p input when @p order says the caller passed
/// `[highest, ..., lowest]`. The base stores everything internally as
/// `[lowest, ..., highest]` so the strategy contract is uniform.
QStringList normaliseToCanonical(const QStringList& input, RegistrationOrder order)
{
    if (order == RegistrationOrder::LowestPriorityFirst) {
        return input;
    }
    QStringList reversed;
    reversed.reserve(input.size());
    for (auto it = input.crbegin(); it != input.crend(); ++it) {
        reversed.append(*it);
    }
    return reversed;
}
} // namespace

int WatchedDirectorySet::registerDirectories(const QStringList& directories, LiveReload liveReload,
                                             RegistrationOrder order)
{
    // Asserted AND guarded, for the same reason as `rescanNow` below and more
    // so: this is public API (MetadataPackLoader::addSearchPaths,
    // ScriptedAlgorithmLoader::scanAndRegister, PluginLoader::scanAndLoad all
    // reach it), and it mutates m_directories, installs kernel watches, and then
    // rescans — so a debug-only check would leave a release build corrupting
    // m_entries from a worker thread. Refuses rather than crashing, because
    // nothing destructive has happened yet at this point.
    if (thread() != QThread::currentThread()) {
        Q_ASSERT_X(false, "WatchedDirectorySet::registerDirectories", "GUI-thread only — see class docs");
        qCWarning(lcWatcher) << "registerDirectories called off the owning thread; refusing";
        return m_directories.size();
    }
    // Empty input is a no-op — skip the rescan entirely. Callers that
    // want to force a rescan call `requestRescan()` / `rescanNow()`
    // directly; routing them here would just be an alias.
    if (directories.isEmpty()) {
        return m_directories.size();
    }

    // Normalise to the canonical `[lowest, ..., highest]` shape ONCE at
    // the registration boundary so every strategy in the library sees
    // the same iteration shape regardless of which `RegistrationOrder`
    // the caller chose. Strategies remain free to reverse-iterate
    // first-wins; the convention is now compile-time visible instead of
    // a comment-driven contract.
    const QStringList canonical = normaliseToCanonical(directories, order);

    // Register all directories first so the rescan sees the complete
    // scan order in one pass.
    //
    // Canonicalise on insertion so two registrations of the same
    // directory under different spellings (`/foo/bar/` vs `/foo/bar`)
    // collapse to one entry.
    if (liveReload == LiveReload::On) {
        installWatcherIfNeeded();
    }
    for (const QString& dir : canonical) {
        const QString cleaned = QDir::cleanPath(dir);
        // Empty entries inside an otherwise non-empty list are not the
        // same as an empty list (which the early-exit above already
        // handles). Skip silently — appending "" to m_directories would
        // surface forever in directories() and on every rescan as a
        // "directory does not exist" debug log; the watch itself is
        // refused by isForbiddenWatchRoot, so the only effect of letting
        // it through would be permanent garbage in the registered set.
        if (cleaned.isEmpty()) {
            continue;
        }
        if (!m_directories.contains(cleaned)) {
            m_directories.append(cleaned);
        }
        if (liveReload == LiveReload::On) {
            attachWatcherForDir(cleaned);
        }
    }

    rescanAll();
    return m_directories.size();
}

int WatchedDirectorySet::setDirectories(const QStringList& directories, LiveReload liveReload, RegistrationOrder order)
{
    // Asserted AND guarded — see `registerDirectories` above for the rationale.
    if (thread() != QThread::currentThread()) {
        Q_ASSERT_X(false, "WatchedDirectorySet::setDirectories", "GUI-thread only — see class docs");
        qCWarning(lcWatcher) << "setDirectories called off the owning thread; refusing";
        return m_directories.size();
    }

    // Same normalisation pass as `registerDirectories` — strategies
    // always see the canonical `[lowest, ..., highest]` iteration shape
    // regardless of which `RegistrationOrder` the caller declared.
    const QStringList normalised = normaliseToCanonical(directories, order);

    // Canonicalise + dedup the new set, preserving the now-canonical
    // priority order. Same path-spelling canonicalisation rule as
    // `registerDirectories` so a `setDirectories` call followed by a
    // `registerDirectories` call doesn't double-register the same path
    // under two spellings.
    QStringList canonical;
    canonical.reserve(normalised.size());
    QSet<QString> canonicalSet;
    canonicalSet.reserve(normalised.size());
    for (const QString& dir : normalised) {
        const QString c = QDir::cleanPath(dir);
        // Empty entries are not legitimate directories — skip silently.
        // Same rationale as the empty-entry filter in registerDirectories:
        // letting "" through would leave permanent garbage in m_directories
        // for the set's lifetime since isForbiddenWatchRoot refuses the
        // watch but the m_directories list owns the entry forever.
        if (c.isEmpty()) {
            continue;
        }
        if (canonicalSet.contains(c)) {
            continue;
        }
        canonicalSet.insert(c);
        canonical.append(c);
    }

    // Diff old vs new — anything in the prior set that's missing from the
    // new set needs its watches torn down. Keep the diff against the
    // pre-replacement m_directories so we don't drop watches for entries
    // that survive the replacement.
    QStringList removed;
    for (const QString& existing : std::as_const(m_directories)) {
        if (!canonicalSet.contains(existing)) {
            removed.append(existing);
        }
    }

    m_directories = std::move(canonical);

    // Tear down direct + ancestor watches for dropped dirs. Per-file
    // watches are owned by the strategy and re-synced inside `rescanAll`
    // below, so we don't clean them up here — the strategy's new
    // `desiredFileWatches` return drives that.
    if (m_watcher) {
        for (const QString& dir : std::as_const(removed)) {
            // Direct watch removal — silent no-op when the path wasn't
            // directly watched (ancestor-proxied or never-existed targets).
            // A dropped directory can ALSO be serving as another target's
            // climbed ancestor proxy (`m_watchedParents`); the kernel watch
            // is shared between the two roles, so it must stay armed until
            // `releaseAncestorWatchFor`'s refcount sweep drops the last
            // dependent target — removing it here would strand the proxy
            // bookkeeping and kill hot-reload for the surviving target. The
            // same holds for a directory on another target's identity chain,
            // which the chain refresh in `rescanAll` releases once no chain
            // needs it.
            if (m_watcher->directories().contains(dir) && !m_watchedParents.contains(dir) && !m_identity->holds(dir)) {
                m_watcher->removePath(dir);
            }
            // Ancestor back-reference cleanup. Same refcount-by-iteration
            // shape as the promotion branch in `attachWatcherForDir` —
            // factored into `releaseAncestorWatchFor`.
            releaseAncestorWatchFor(dir);
        }
    }

    if (liveReload == LiveReload::On) {
        installWatcherIfNeeded();
        for (const QString& dir : std::as_const(m_directories)) {
            attachWatcherForDir(dir);
        }
    }

    rescanAll();
    return m_directories.size();
}

void WatchedDirectorySet::rescanNow()
{
    // Asserted AND guarded: this is public API now (ProfileLoader::rescanNow
    // forwards to it), so a debug-only check would leave a release build
    // re-entering the scan from a worker thread and corrupting m_entries. The
    // guard refuses rather than crashing, because a wrong-thread caller has
    // already done nothing destructive at this point.
    if (thread() != QThread::currentThread()) {
        Q_ASSERT_X(false, "WatchedDirectorySet::rescanNow", "GUI-thread only — see class docs");
        qCWarning(lcWatcher) << "rescanNow called off the owning thread; refusing";
        return;
    }
    // Cancel any pending debounce — its rescan would just re-do work
    // we're about to perform synchronously below.
    m_debounceTimer.stop();
    rescanAll();
}

void WatchedDirectorySet::requestRescan()
{
    // Asserted AND guarded, like its siblings. This is reached from
    // `onWatchedPathChanged` and from every consumer, and it WRITES
    // `m_rescanRequestedWhileRunning` while reading `m_rescanDepth` — so an
    // off-thread call races the replay flag, which is a worse outcome than the
    // timer's own "cannot be started from another thread" warning.
    if (thread() != QThread::currentThread()) {
        Q_ASSERT_X(false, "WatchedDirectorySet::requestRescan", "GUI-thread only — see class docs");
        qCWarning(lcWatcher) << "requestRescan called off the owning thread; refusing";
        return;
    }
    // A rescan may already be running on this thread (strategy's commit step
    // or a nested slot re-entered us via a signal — including the case where
    // a `rescanCompleted` slot calls `registerDirectories`, which invokes
    // `rescanAll` recursively). Starting the debounce timer here would be a
    // no-op — the timer is idle while rescanAll is executing — and the event
    // that triggered this call would be silently dropped because the rescan
    // has already captured the pre-event filesystem state. Defer to the
    // outermost rescanAll's exit, which checks the flag and re-arms the
    // timer for a follow-up scan.
    if (m_rescanDepth > 0) {
        m_rescanRequestedWhileRunning = true;
        return;
    }
    // Safe to call unconditionally — the single-shot timer coalesces
    // back-to-back calls into one rescanAll() on timeout.
    m_debounceTimer.start();
}

QStringList WatchedDirectorySet::directories() const
{
    return m_directories;
}

QStringList WatchedDirectorySet::filterNewSearchPaths(const QStringList& candidates,
                                                      const QStringList& alreadyRegistered)
{
    QStringList result;
    result.reserve(candidates.size());
    for (const QString& path : candidates) {
        if (path.isEmpty()) {
            continue;
        }
        const QString canonical = QDir::cleanPath(path);
        if (alreadyRegistered.contains(canonical) || result.contains(canonical)) {
            continue;
        }
        result.append(canonical);
    }
    return result;
}

// The three *ForTest accessors below use a bare Q_ASSERT_X thread check with no
// release-build counterpart, unlike the production entry points in this file
// which are "asserted AND guarded". That is deliberate: these are debug-only
// test surface with no production caller, so the release-build guard the
// production paths carry would be dead weight.
void WatchedDirectorySet::setDebounceIntervalForTest(int ms)
{
    Q_ASSERT_X(thread() == QThread::currentThread(), "WatchedDirectorySet::setDebounceIntervalForTest",
               "GUI-thread only — see class docs");
    m_debounceTimer.setInterval(ms);
    m_identityTimer.setInterval(ms);
}

QString WatchedDirectorySet::watchedAncestorForTest(const QString& target) const
{
    Q_ASSERT_X(thread() == QThread::currentThread(), "WatchedDirectorySet::watchedAncestorForTest",
               "GUI-thread only — see class docs");
    // Canonicalise the lookup key to match the insertion key used by
    // `attachWatcherForDir`, otherwise trailing-slash differences
    // between the caller's test path and the internal form would cause
    // the accessor to spuriously return an empty string.
    return m_parentWatchFor.value(QDir::cleanPath(target));
}

bool WatchedDirectorySet::hasParentWatchForTest(const QString& path) const
{
    Q_ASSERT_X(thread() == QThread::currentThread(), "WatchedDirectorySet::hasParentWatchForTest",
               "GUI-thread only — see class docs");
    return m_watchedParents.contains(QDir::cleanPath(path));
}

void WatchedDirectorySet::rescanAll()
{
    // Race guard — any `requestRescan` call delivered during this scan
    // (e.g. from within the strategy's commit step, or from a watcher
    // event that fires while we're iterating) is captured in
    // `m_rescanRequestedWhileRunning` and replayed at the end of the
    // OUTERMOST rescan. Without it, the single-shot debounce timer is
    // idle while this slot runs, so any `m_debounceTimer.start()` call
    // from inside the nested stack is a no-op and the observed state
    // can go stale until the next external event.
    //
    // Reentry depth — `m_rescanDepth` is incremented on entry and
    // decremented on exit. The outermost entry resets the replay flag
    // so a stale `true` left over from a prior outer scan doesn't
    // re-fire. Only the outermost exit triggers the replay — nested
    // exits leave `m_rescanRequestedWhileRunning` intact so the outer
    // scan still sees pending work captured by inner iterations.
    const int depth = ++m_rescanDepth;
    if (depth == 1) {
        m_rescanRequestedWhileRunning = false;
    }

    // If live-reload is enabled, the watcher needs a chance to promote
    // any parent-watched target that just materialised. Do this before
    // the strategy runs so a dir created between the last rescan and
    // this one is fully wired. `m_watcher != nullptr` is the canonical
    // "live-reload on" predicate — `installWatcherIfNeeded` only ever
    // runs from the `LiveReload::On` branch in `registerDirectories`,
    // and the watcher is never torn down for the set's lifetime.
    //
    // The identity chains go first: a directory whose chain moved since the
    // last walk has its content watches dropped there, so the attach below
    // and the file sync after the scan re-arm them on the new inodes.
    if (m_watcher) {
        refreshIdentityChains();
        for (const QString& dir : std::as_const(m_directories)) {
            attachWatcherForDir(dir);
        }
    }

    // Hand off to the strategy. It enumerates, parses, commits — and
    // returns the absolute paths we should install per-path watches on
    // after this rescan.
    const QStringList desiredFileWatches = m_strategy->performScan(m_directories);

    // Arm per-file watches AFTER commit so the watch set exactly
    // matches the strategy's view of "currently relevant" paths.
    // QFileSystemWatcher auto-drops entries on atomic-rename saves
    // (most editors), so we re-sync on every rescan — the add/remove
    // diff makes this cheap. `syncFileWatches` itself null-checks
    // `m_watcher`, but gating here saves the QSet allocation when
    // live-reload is off.
    if (m_watcher) {
        syncFileWatches(desiredFileWatches);
    }

    Q_EMIT rescanCompleted();

    // Race-guard replay. `Q_EMIT` is a synchronous call: every
    // `Qt::DirectConnection` slot wired to `rescanCompleted` runs
    // before the line below decrements `m_rescanDepth`, so any
    // `requestRescan()` call from those slots still takes the guarded
    // path (sets `m_rescanRequestedWhileRunning`). Slots wired with
    // `Qt::QueuedConnection` run later, after the flag is cleared, and
    // will arm the debounce timer normally — that's also fine.
    //
    // We decrement `m_rescanDepth` BEFORE re-arming the timer so the
    // single-shot timer's eventual `rescanAll` slot starts from a clean
    // state, rather than seeing a stale depth from the outer scan it's
    // chained off. Replay only fires when the OUTERMOST scan exits —
    // nested exits leave the work for the outer to drive.
    --m_rescanDepth;
    if (m_rescanDepth == 0 && m_rescanRequestedWhileRunning) {
        m_rescanRequestedWhileRunning = false;
        m_debounceTimer.start();
    }
}

void WatchedDirectorySet::syncFileWatches(const QStringList& desiredPaths)
{
    if (!m_watcher) {
        return;
    }

    // A reported path that goes through a symlink below its registered
    // directory (a linked script, a linked package folder, a file inside
    // one) cannot be watched under its own spelling. inotify follows the link
    // once, when the watch is added, and Qt keeps one watch per path string,
    // so the watch stays on the old target after the link is pointed
    // elsewhere, and there is none at all while the link dangles. Those paths
    // go to the identity chains, which watch every directory the link leads
    // through and the target under its physical path. Every other path keeps
    // its plain watch here: the chain of its registered directory already
    // covers everything above it, so a set without links costs what it did.
    QSet<QString> desired;
    desired.reserve(desiredPaths.size());
    QStringList linked;
    QSet<QString> linkedSet;
    for (const QString& path : desiredPaths) {
        if (!crossesSymlinkBelowRegistered(m_directories, path)) {
            desired.insert(path);
        } else if (!linkedSet.contains(path)) {
            linkedSet.insert(path);
            linked.append(path);
        }
    }

    // Drop watches for paths no longer relevant (deleted between rescans,
    // no longer reported by the strategy, or now reached through a link). A
    // path that an identity chain also holds stays armed for it, and the
    // chain refresh releases it once no chain needs it.
    for (auto it = m_watchedFiles.begin(); it != m_watchedFiles.end();) {
        if (!desired.contains(*it)) {
            if (!m_identity->holds(*it)) {
                m_watcher->removePath(*it);
            }
            it = m_watchedFiles.erase(it);
        } else {
            ++it;
        }
    }

    // Re-add every desired path. `addPath` no-ops (returns false) if
    // the path is already watched, but after a save-via-atomic-rename
    // Qt silently drops the old inode from the watch set — the user's
    // new inode must be explicitly re-added. Cheapest correct shape is
    // "always try to add; trust QFSW to dedupe the no-op case".
    //
    // Cached watcher state for the failure-path dedupe (case 1 below).
    // Lazy-built so the steady-state success path doesn't allocate two
    // QSets per rescan when nothing fails.
    std::optional<QSet<QString>> alreadyWatchedDirs;
    std::optional<QSet<QString>> alreadyWatchedFiles;
    const auto ensureAlreadyWatchedCaches = [&]() {
        if (alreadyWatchedDirs) {
            return;
        }
        const QStringList dirs = m_watcher->directories();
        const QStringList files = m_watcher->files();
        alreadyWatchedDirs.emplace(dirs.cbegin(), dirs.cend());
        alreadyWatchedFiles.emplace(files.cbegin(), files.cend());
    };

    for (const QString& path : desired) {
        if (m_watcher->addPath(path)) {
            m_watchedFiles.insert(path);
            // A previously-unwatchable path recovered (quota freed,
            // permissions fixed); clear its warn-once latch so a LATER
            // failure of the same path warns again.
            m_warnedFailedWatchPaths.remove(path);
        } else if (!m_watchedFiles.contains(path)) {
            // An identity chain holds the watch (the path is also the target
            // of a link, or a directory a link leads through). Share it, so
            // that neither side drops it while the other still needs it.
            if (m_identity->holds(path)) {
                m_watchedFiles.insert(path);
                continue;
            }
            // addPath returned false AND we didn't already have it in
            // OUR file-watch set. Three cases:
            //
            //   1) Qt is already watching this path because the strategy
            //      reported one of the registered roots that
            //      `attachWatcherForDir` already added — silent dedupe
            //      (strategies are *advised* not to do this, but this
            //      keeps a misbehaving strategy from producing a
            //      misleading log spam every rescan).
            //   2) Path vanished between the strategy's stat and
            //      addPath (file deleted by another process during the
            //      scan, common in CI runners with concurrent test
            //      churn). Transient — debug-log only.
            //   3) Genuinely unwatchable — permissions, OS-specific
            //      watch quota hit (Linux: inotify max_user_watches;
            //      macOS: kqueue file-descriptor limits; Windows:
            //      ReadDirectoryChangesW handle exhaustion). Warn —
            //      hot-reload silently degrades to "doesn't work" and
            //      users troubleshooting need this visible at default
            //      log levels.
            ensureAlreadyWatchedCaches();
            if (alreadyWatchedDirs->contains(path) || alreadyWatchedFiles->contains(path)) {
                continue; // case 1 — silent dedupe
            }
            if (!QFileInfo::exists(path)) {
                qCDebug(lcWatcher) << "syncFileWatches: path vanished before addPath" << path
                                   << "— transient, will re-arm on next rescan";
                continue; // case 2 — transient miss
            }
            // Warn ONCE per path: the causes named below (quota exhaustion,
            // permissions) are persistent by nature and hit many paths at
            // once, while rescans keep firing at the debounce interval — an
            // unconditional warn floods the journal with N lines per tick
            // for the lifetime of the condition. The success branch above
            // clears the latch, so a recovered-then-refailed path re-warns.
            if (!m_warnedFailedWatchPaths.contains(path)) {
                m_warnedFailedWatchPaths.insert(path);
                qCWarning(lcWatcher) << "syncFileWatches: failed to add watch for" << path
                                     << "— check permissions or filesystem-watcher quota (inotify on Linux, "
                                        "kqueue/FSEvents on macOS, ReadDirectoryChangesW on Windows)";
            }
        }
    }

    // The refresh at the top of the rescan walked the links of the previous
    // scan and re-armed whatever of theirs moved. Only a changed list needs a
    // walk here: it arms the chains of new links and releases the watches of
    // links gone. A chain that moved during the scan asks for another one.
    if (linked != m_linkedEntries) {
        m_linkedEntries = std::move(linked);
        if (refreshIdentityChains()) {
            requestRescan();
        }
    }
}

void WatchedDirectorySet::installWatcherIfNeeded()
{
    if (m_watcher) {
        return;
    }
    m_watcher = new QFileSystemWatcher(this);
    m_identity = std::make_unique<detail::IdentityWatches>(*m_watcher, detail::IdentityWatches::Leaf::Skip, &lcWatcher);
    // A file event and an event in a directory the strategy scans route
    // through the rescan debounce, without telling them apart: the rescan
    // sees the full filesystem state regardless. An event in a directory
    // that only an identity chain needs costs a walk of the chains, which
    // asks for a rescan only when one of them moved. Its window opens on the
    // first such event and is not extended by later ones, so steady churn
    // in `$HOME` cannot postpone the walk that notices a switch.
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString& path) {
        if (isContentWatch(path)) {
            onWatchedPathChanged();
        } else if (!m_identityTimer.isActive()) {
            m_identityTimer.start();
        }
    });
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString&) {
        onWatchedPathChanged();
    });
}

void WatchedDirectorySet::attachWatcherForDir(const QString& directory)
{
    if (!m_watcher) {
        return;
    }
    // Canonicalise the target key: `m_parentWatchFor` is keyed by the
    // target's canonical-path form so symlinked / trailing-slash
    // duplicates (e.g. the same dir registered as both
    // `/a/b/profiles/` and `/a/b/profiles`) hash to the same bucket.
    const QString targetKey = QDir::cleanPath(directory);
    // Refuse to watch the registered target itself if it resolves to a
    // forbidden root (e.g. `$HOME`, `/`, an XDG_*Location root). The
    // climb-onto-ancestor branch below has its own guard — duplicating
    // it here means a misconfigured caller passing `$HOME` directly
    // gets the same protection as one whose target's parent climb would
    // land there. A misconfiguration here is silent-but-warned rather
    // than a daemon-killing flood of rescan events.
    if (isForbiddenWatchRoot(targetKey)) {
        qCWarning(lcWatcher) << "Refusing to watch forbidden root" << directory
                             << "— registered target resolves to $HOME, /, or an XDG data/config root";
        return;
    }
    const QFileInfo info(targetKey);
    if (info.exists() && info.isDir()) {
        // Target exists — watch it directly. If we were watching an
        // ancestor as a proxy, we can stop (direct watch is strictly
        // better and avoids duplicate rescan events when a sibling
        // file elsewhere in the parent changes).
        // `targetKey`, not the raw argument: every other piece of bookkeeping in
        // this function (`releaseAncestorWatchFor` below, `setDirectories`'
        // `removePath`) is keyed on the cleaned form, so using the uncleaned one
        // here would make the invariant depend on call-site discipline. Every
        // caller passes a cleaned string today, which is exactly why the two
        // spellings cannot be seen to diverge.
        if (!m_watcher->directories().contains(targetKey)) {
            if (!m_watcher->addPath(targetKey)) {
                // Same failure surface as the per-file warn in
                // `syncFileWatches`: without a directory watch, new files in
                // this root are never discovered at all, which is strictly
                // worse than a lost in-place-edit watch. Self-heals on the
                // next rescan (this branch gates on the watcher's real
                // state), so warn rather than latch.
                qCWarning(lcWatcher) << "attachWatcherForDir: failed to add directory watch for" << targetKey
                                     << "— check permissions or filesystem-watcher quota (inotify on Linux)";
            }
        }
        // Promotion cleanup — release the ancestor watch we recorded
        // for this target on a previous (target-not-yet-existing) call.
        // The same shape is used by `setDirectories` when a target is
        // dropped from the registered set; both call sites share
        // `releaseAncestorWatchFor`.
        releaseAncestorWatchFor(targetKey);
    } else {
        // Target doesn't exist (yet). Watch the nearest existing
        // ancestor as a proxy — when the ancestor changes (typically
        // because the target dir was just created), we rescan and
        // attachWatcherForDir promotes to a direct watch.
        //
        // Crucially: refuse to climb onto `$HOME`, root, or
        // GenericData/ConfigLocation roots. A pristine install where
        // `~/.local/share/plasmazones` does not exist would otherwise
        // end up watching `$HOME`, making every file operation in the
        // user's home trigger a full rescan. If the immediate
        // parent(s) don't exist, we silently skip the watch until the
        // tree materialises — consumers should call `requestRescan()`
        // after creating the directory structure.
        //
        // The climb is shared with `WatchedFileSet` (see
        // `nearestExistingAncestor` for why it is lexical and bounded). It
        // returns the cleaned form, so the parent-set equality check below
        // never holds one ancestor under two spellings, and it keeps symlink
        // identity at the ancestor level (callers may have deliberately
        // pointed at a symlinked location).
        const QString ancestor = detail::nearestExistingAncestor(targetKey);
        if (ancestor.isEmpty()) {
            qCDebug(lcWatcher) << "attachWatcherForDir: no existing ancestor for" << directory
                               << "— call requestRescan() after creating the directory tree.";
            return;
        }
        if (isForbiddenWatchRoot(ancestor)) {
            qCDebug(lcWatcher) << "Refusing to watch forbidden ancestor" << ancestor << "for target" << directory
                               << "— call requestRescan() after creating the directory tree.";
            return;
        }
        // The climb can land on a DIFFERENT ancestor than a previous call
        // recorded (the previously-recorded one was deleted, so this pass
        // climbed further up). Release the stale back-reference first, or the
        // overwrite below strands the old ancestor in `m_watchedParents`
        // forever — a leaked watch that fires a full rescan for every
        // unrelated event in that tree.
        const auto prior = m_parentWatchFor.constFind(targetKey);
        if (prior != m_parentWatchFor.constEnd() && prior.value() != ancestor) {
            releaseAncestorWatchFor(targetKey);
        }
        // Gate on the WATCHER's real state, not the `m_watchedParents`
        // mirror: Qt silently drops a watch when the watched directory is
        // deleted, and a mirror-gated skip would then never re-arm it after
        // the directory reappears (the direct-watch branch above gates on
        // real state for the same reason). Record the mirror entry only when
        // the kernel watch is actually armed, so a transient `addPath`
        // failure (inotify quota, EACCES) is retried by the next rescan
        // instead of being latched as watched.
        const bool armed = m_watcher->directories().contains(ancestor) || m_watcher->addPath(ancestor);
        if (!armed) {
            qCWarning(lcWatcher) << "attachWatcherForDir: failed to watch ancestor" << ancestor << "for target"
                                 << directory << "— check permissions or filesystem-watcher quota (inotify on Linux)";
            return;
        }
        m_watchedParents.insert(ancestor);
        // Record the per-target ancestor back-reference — the promotion
        // branch above uses this to find the ACTUALLY-watched ancestor
        // (may be the grandparent or further up).
        m_parentWatchFor.insert(targetKey, ancestor);
    }
}

void WatchedDirectorySet::releaseAncestorWatchFor(const QString& targetKey)
{
    if (!m_watcher) {
        return;
    }
    // Look up the ancestor WE actually watched for this target. The climb
    // in `attachWatcherForDir` may land on a grandparent (or further) when
    // the immediate parent doesn't exist either, so removing
    // `info.absolutePath()` would miss that case and leak the ancestor
    // watch for the set's lifetime — firing a rescan for every unrelated
    // file event in that tree.
    const auto it = m_parentWatchFor.constFind(targetKey);
    if (it == m_parentWatchFor.constEnd()) {
        return;
    }
    const QString watchedAncestor = it.value();
    m_parentWatchFor.erase(it);
    if (!m_watchedParents.contains(watchedAncestor)) {
        return;
    }
    // Only remove the ancestor watch if no OTHER configured target still
    // relies on it (multiple missing-sibling targets can share one
    // climbed ancestor). Iterate the remaining `m_parentWatchFor` values
    // — that map tracks exactly which other targets still need this
    // ancestor.
    //
    // O(N) per release in the count of missing-target dirs sharing this
    // ancestor. N is bounded by the consumer's registered-directory count
    // (typical single-digit: one user dir, a handful of system dirs), so
    // the linear sweep is preferable to a refcount mirror. If a future
    // consumer registers hundreds of missing-target paths under one
    // ancestor, swap this for a `QHash<ancestor, int>` refcount.
    for (auto pit = m_parentWatchFor.constBegin(); pit != m_parentWatchFor.constEnd(); ++pit) {
        if (pit.value() == watchedAncestor) {
            return; // still needed by another target
        }
    }
    // The ancestor may ALSO be a registered directory holding its own direct
    // watch (register A and A/b where A/b does not exist yet: A is both
    // directly watched and A/b's proxy). The kernel watch is shared between
    // the two roles, so drop only the proxy bookkeeping here and leave the
    // watch armed for the direct role — removing it would silence every
    // event under A with nothing left to re-arm it.
    // The same goes for an ancestor that an identity chain also needs, which
    // is the usual case after a promotion: the proxy was the target's parent.
    m_watchedParents.remove(watchedAncestor);
    if (!m_directories.contains(watchedAncestor) && !m_identity->holds(watchedAncestor)) {
        m_watcher->removePath(watchedAncestor);
    }
}

bool WatchedDirectorySet::refreshIdentityChains()
{
    const QStringList moved = m_identity->refresh(
        m_directories,
        [this](const QString& path) {
            return isContentWatch(path);
        },
        m_linkedEntries);
    if (moved.isEmpty()) {
        return false;
    }
    qCDebug(lcWatcher) << "identity chain moved for" << moved << "— re-arming its watches";
    // Every watch of a linked entry belongs to its chain, which the refresh
    // has just re-armed. Only a registered directory has content watches of
    // its own to drop.
    QStringList movedDirectories;
    for (const QString& path : moved) {
        if (m_directories.contains(path)) {
            movedDirectories.append(path);
        }
    }
    if (!movedDirectories.isEmpty()) {
        dropWatchesOfMovedDirectories(movedDirectories);
    }
    return true;
}

void WatchedDirectorySet::dropWatchesOfMovedDirectories(const QStringList& moved)
{
    // Qt keeps one watch per path string and skips an add of a path it
    // already holds, so a watch whose path now names another inode has to be
    // removed before the attach and the file sync can arm the new one. A path
    // the identity chains also hold is left alone: their refresh has just
    // re-armed it if its inode changed.
    //
    // Proxies first. A proxy is the nearest existing ancestor of a missing
    // target, so it is on that target's chain, and a proxy that moved moved
    // every target that depends on it: the refcount sweep releases it once
    // the last of them is released here.
    for (const QString& dir : moved) {
        releaseAncestorWatchFor(dir);
    }
    const QStringList watchedDirectories = m_watcher->directories();
    QStringList stale;
    for (const QString& dir : moved) {
        if (watchedDirectories.contains(dir) && !m_watchedParents.contains(dir) && !m_identity->holds(dir)) {
            stale.append(dir);
        }
    }
    // The strategy's files below a moved directory are watched on the old
    // inodes too. `syncFileWatches` re-adds them after the rescan.
    for (auto it = m_watchedFiles.begin(); it != m_watchedFiles.end();) {
        const bool below = std::any_of(moved.cbegin(), moved.cend(), [&it](const QString& dir) {
            return *it == dir || it->startsWith(dir + QLatin1Char('/'));
        });
        if (!below) {
            ++it;
            continue;
        }
        if (!m_identity->holds(*it)) {
            stale.append(*it);
        }
        it = m_watchedFiles.erase(it);
    }
    if (!stale.isEmpty()) {
        m_watcher->removePaths(stale);
    }
}

bool WatchedDirectorySet::isContentWatch(const QString& path) const
{
    // Qt reports an event on an inode watched under two spellings under one
    // of them only. A registered directory reached through a symlink can
    // also sit on another registered directory's chain under its physical
    // spelling, and an event there is still content. So is an event in the
    // target of a linked folder, which only its chain watches.
    return m_directories.contains(path) || m_watchedParents.contains(path) || m_watchedFiles.contains(path)
        || m_identity->isResolvedPath(path);
}

void WatchedDirectorySet::onIdentityCheckElapsed()
{
    if (refreshIdentityChains()) {
        requestRescan();
    }
}

void WatchedDirectorySet::onWatchedPathChanged()
{
    // Debounce — a single save typically fires 2-3 inotify events in
    // the same ~10 ms window (create temp → rename → fsync). The
    // single-shot timer collapses that into one rescan.
    requestRescan();
}

} // namespace PhosphorFsLoader
