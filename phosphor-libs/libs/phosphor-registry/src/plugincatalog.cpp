// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorRegistry/PluginCatalog.h>

#include <PhosphorFsLoader/IScanStrategy.h>
#include <PhosphorFsLoader/WatchedDirectorySet.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonValue>
#include <QLibrary>
#include <QLibraryInfo>
#include <QLoggingCategory>
#include <QPluginLoader>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVersionNumber>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace PhosphorRegistry {

namespace {
Q_LOGGING_CATEGORY(lcPluginCatalog, "phosphorregistry.plugincatalog")

// Plugin files CONSIDERED per scan, summed over every search path. A plugin
// costs a metadata read, and no plausible install has four digits of them.
constexpr int DefaultMaxFilesPerScan = 1000;

const QLatin1String IidKey("IID");
const QLatin1String MetaDataKey("MetaData");
const QLatin1String QtVersionKey("version");
const QLatin1String IdKey("Id");
const QLatin1String ApiVersionKey("ApiVersion");

// The suffix QLibrary tries before the vetted path itself when the CPU has
// the Haswell feature set. Qt reports the vetted path and its metadata even
// when it mapped this sibling instead, so the sibling has to be refused
// before the load rather than detected after it.
const QLatin1String ArchVariantSuffix(".avx2");

bool isValidName(const QString& name)
{
    static const QRegularExpression pattern(
        QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9][A-Za-z0-9._-]*")));
    return pattern.match(name).hasMatch();
}

// The StrictModes rule OpenSSH and sudo apply to files they trust: a file or
// directory that someone other than this user (or root) owns or can write
// could be swapped for other code between the scan and the load. Returns why
// @p info fails it, or an empty string.
QString trustProblem(const struct stat& info, const QString& what)
{
    if (info.st_uid != ::geteuid() && info.st_uid != 0) {
        return QStringLiteral("%1 is owned by uid %2, not by this user or root").arg(what).arg(info.st_uid);
    }
    if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return QStringLiteral("%1 is group- or world-writable (chmod go-w)").arg(what);
    }
    return QString();
}

QString logPrefix(const PluginSeam& seam)
{
    return QStringLiteral("PluginCatalog(%1):").arg(seam.name);
}

QString originName(PluginOrigin origin)
{
    switch (origin) {
    case PluginOrigin::Development:
        return QStringLiteral("development");
    case PluginOrigin::User:
        return QStringLiteral("user");
    case PluginOrigin::System:
        return QStringLiteral("system");
    }
    return QString();
}

QString directoryOf(const QString& filePath)
{
    return QFileInfo(filePath).absolutePath();
}

// Sets a depth counter for the lifetime of the guard.
class DepthGuard
{
public:
    explicit DepthGuard(int& depth)
        : m_depth(depth)
    {
        ++m_depth;
    }
    ~DepthGuard()
    {
        --m_depth;
    }
    Q_DISABLE_COPY_MOVE(DepthGuard)

private:
    int& m_depth;
};
} // namespace

// Keeps a loaded plugin's QPluginLoader for the catalog's lifetime. The
// loader's destructor does not unload the library, and the catalog never calls
// unload(): an instance, or an object it created, may still be in use anywhere.
struct PluginCatalog::Loaded
{
    PluginMetadata metadata;
    std::unique_ptr<QPluginLoader> loader;
    QPointer<QObject> instance;
};

// The verdict on one plugin file: its metadata when it is acceptable, or the
// reason it is not and the id it claimed, when the refusal came that late.
// A file refused before it was known to be a readable regular file cannot
// take a file watch; its directory's watch still reports changes to it.
// identity is set for an accepted file, taken before its metadata was read.
struct PluginCatalog::Vetted
{
    std::optional<PluginMetadata> metadata;
    QString id;
    QString reason;
    bool watchable = true;
    FileIdentity identity = {};
};

class PluginCatalog::ScanStrategy : public PhosphorFsLoader::IScanStrategy
{
public:
    explicit ScanStrategy(PluginCatalog* owner)
        : m_owner(owner)
    {
    }

    QStringList performScan(const QStringList& directoriesInScanOrder) override
    {
        return m_owner->performScan(directoriesInScanOrder);
    }

private:
    PluginCatalog* m_owner = nullptr;
};

PluginCatalog::PluginCatalog(PluginSeam seam, QObject* parent)
    : QObject(parent)
    , m_seam(std::move(seam))
    , m_maxFilesPerScan(DefaultMaxFilesPerScan)
    , m_strategy(std::make_unique<ScanStrategy>(this))
    , m_watcher(std::make_unique<PhosphorFsLoader::WatchedDirectorySet>(*m_strategy))
{
    m_seamValid = isValidName(m_seam.name) && !m_seam.iid.isEmpty() && m_seam.apiVersion >= 1;
    if (!m_seamValid) {
        qCWarning(lcPluginCatalog) << "PluginCatalog: invalid seam" << m_seam.name << m_seam.iid << m_seam.apiVersion
                                   << "(the name must be a plain directory name, the IID non-empty and the "
                                      "ApiVersion at least 1); it offers no plugins";
    }
    connect(m_watcher.get(), &PhosphorFsLoader::WatchedDirectorySet::rescanCompleted, this,
            &PluginCatalog::forwardScanCompleted);
}

PluginCatalog::~PluginCatalog() = default;

QList<PluginSearchPath> PluginCatalog::defaultSearchPaths(const QString& seamName)
{
    QList<PluginSearchPath> paths;
    QSet<QString> seen;
    const auto add = [&paths, &seen](const QString& directory, PluginOrigin origin) {
        if (QDir::isRelativePath(directory)) {
            qCWarning(lcPluginCatalog) << "PluginCatalog: ignoring the relative plugin search path" << directory;
            return;
        }
        const QString clean = QDir::cleanPath(directory);
        if (seen.contains(clean)) {
            return;
        }
        seen.insert(clean);
        paths.append({clean, origin});
    };

    const QString development = qEnvironmentVariable(DevelopmentPathVariable);
    for (const QString& root : development.split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
        add(root + QLatin1Char('/') + seamName, PluginOrigin::Development);
    }

    const QString dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!dataHome.isEmpty()) {
        add(dataHome + QStringLiteral("/phosphor-works/plugins/") + seamName, PluginOrigin::User);
    }

    // libraryPaths() also lists the application's own directory, which is
    // not where packages install plugins.
    const QString applicationDir =
        QCoreApplication::instance() ? QDir::cleanPath(QCoreApplication::applicationDirPath()) : QString();
    const QStringList libraryPaths = QCoreApplication::libraryPaths();
    for (const QString& libraryPath : libraryPaths) {
        if (!applicationDir.isEmpty() && QDir::cleanPath(libraryPath) == applicationDir) {
            continue;
        }
        add(libraryPath + QStringLiteral("/phosphor-works/") + seamName, PluginOrigin::System);
    }
    return paths;
}

const PluginSeam& PluginCatalog::seam() const
{
    return m_seam;
}

void PluginCatalog::setSearchPaths(const QList<PluginSearchPath>& paths)
{
    if (m_registered) {
        qCWarning(lcPluginCatalog) << qUtf8Printable(logPrefix(m_seam))
                                   << "setSearchPaths after scan() is ignored; the paths are fixed by then";
        return;
    }
    m_searchPaths.clear();
    QSet<QString> seen;
    for (const PluginSearchPath& path : paths) {
        if (path.directory.isEmpty()) {
            continue;
        }
        const QString clean = QDir::cleanPath(QFileInfo(path.directory).absoluteFilePath());
        if (seen.contains(clean)) {
            continue;
        }
        seen.insert(clean);
        m_searchPaths.append({clean, path.origin});
    }
}

QList<PluginSearchPath> PluginCatalog::searchPaths() const
{
    return m_searchPaths;
}

void PluginCatalog::setLiveReload(bool enabled)
{
    if (m_registered) {
        qCWarning(lcPluginCatalog) << qUtf8Printable(logPrefix(m_seam))
                                   << "setLiveReload after scan() is ignored; the watch is set up by then";
        return;
    }
    m_liveReload = enabled;
}

void PluginCatalog::scan()
{
    if (m_scanDepth > 0) {
        // Called from a slot a running scan reached. Running a nested scan
        // here would leave the outer one emitting stale changes, so the
        // watcher replays the request once the outermost scan has finished.
        m_watcher->requestRescan();
        return;
    }
    if (!m_seamValid) {
        forwardScanCompleted();
        return;
    }
    if (m_registered) {
        m_watcher->rescanNow();
        return;
    }
    m_registered = true;
    if (m_searchPaths.isEmpty()) {
        m_searchPaths = defaultSearchPaths(m_seam.name);
    }
    QStringList directories;
    directories.reserve(m_searchPaths.size());
    for (const PluginSearchPath& path : std::as_const(m_searchPaths)) {
        directories.append(path.directory);
    }
    if (directories.isEmpty()) {
        forwardScanCompleted();
        return;
    }
    // registerDirectories runs the first scan synchronously and emits
    // rescanCompleted, which forwardScanCompleted passes on.
    m_watcher->registerDirectories(directories,
                                   m_liveReload ? PhosphorFsLoader::LiveReload::On : PhosphorFsLoader::LiveReload::Off,
                                   PhosphorFsLoader::RegistrationOrder::HighestPriorityFirst);
}

void PluginCatalog::forwardScanCompleted()
{
    const DepthGuard guard(m_scanDepth);
    Q_EMIT scanCompleted();
}

QList<PluginMetadata> PluginCatalog::plugins() const
{
    QList<PluginMetadata> list = m_plugins.values();
    std::sort(list.begin(), list.end(), [](const PluginMetadata& a, const PluginMetadata& b) {
        return a.id < b.id;
    });
    return list;
}

std::optional<PluginMetadata> PluginCatalog::plugin(const QString& id) const
{
    const auto it = m_plugins.constFind(id);
    if (it == m_plugins.cend()) {
        return std::nullopt;
    }
    return *it;
}

QList<RefusedPlugin> PluginCatalog::refused() const
{
    return m_refused;
}

bool PluginCatalog::isLoaded(const QString& id) const
{
    return m_loaded.contains(id);
}

void PluginCatalog::setMaxFilesPerScanForTest(int cap)
{
    m_maxFilesPerScan = cap > 0 ? cap : DefaultMaxFilesPerScan;
}

PluginCatalog::FileIdentity PluginCatalog::identityOf(const QString& filePath)
{
    struct stat info = {};
    if (::stat(QFile::encodeName(filePath).constData(), &info) != 0) {
        return FileIdentity{};
    }
    return FileIdentity{static_cast<quint64>(info.st_dev), static_cast<quint64>(info.st_ino),
                        static_cast<qint64>(info.st_size),
                        static_cast<qint64>(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec};
}

PluginCatalog::Vetted PluginCatalog::vet(const QString& filePath, PluginOrigin origin) const
{
    const auto refuse = [](const QString& id, const QString& reason) {
        return Vetted{std::nullopt, id, reason, true};
    };
    const auto refuseUnwatchable = [](const QString& reason) {
        return Vetted{std::nullopt, QString(), reason, false};
    };

    const QByteArray encodedPath = QFile::encodeName(filePath);
    struct stat fileInfo = {};
    if (::lstat(encodedPath.constData(), &fileInfo) != 0) {
        return refuseUnwatchable(
            QStringLiteral("it cannot be examined: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
    if (S_ISLNK(fileInfo.st_mode)) {
        return refuseUnwatchable(QStringLiteral("it is a symlink, and symlinked plugin files are not followed"));
    }
    if (!S_ISREG(fileInfo.st_mode)) {
        return refuseUnwatchable(QStringLiteral("it is not a regular file"));
    }
    if (::access(encodedPath.constData(), R_OK) != 0) {
        return refuseUnwatchable(QStringLiteral("it is not readable"));
    }
    if (const QString problem = trustProblem(fileInfo, QStringLiteral("it")); !problem.isEmpty()) {
        return refuse(QString(), problem);
    }
    struct stat directoryInfo = {};
    if (::stat(QFile::encodeName(directoryOf(filePath)).constData(), &directoryInfo) != 0) {
        return refuse(
            QString(),
            QStringLiteral("its directory cannot be examined: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
    }
    if (const QString problem = trustProblem(directoryInfo, QStringLiteral("its directory")); !problem.isEmpty()) {
        return refuse(QString(), problem);
    }
    const QFileInfo variant(QFileInfo(filePath).canonicalFilePath() + ArchVariantSuffix);
    if (variant.exists() || variant.isSymLink()) {
        return refuse(
            QString(),
            QStringLiteral("%1 exists beside it, and Qt would load that file in its place").arg(variant.fileName()));
    }

    // Taken before the metadata is read, so a file replaced between the two
    // is recorded as the older file, and the next scan sees it change and
    // offers it again. The other order would record the new file beside the
    // old metadata and never offer the new metadata.
    const FileIdentity identity = identityOf(filePath);
    const QJsonObject pluginMetaData = QPluginLoader(filePath).metaData();
    if (pluginMetaData.isEmpty()) {
        return refuse(QString(), QStringLiteral("not a Qt plugin"));
    }
    const QString iid = pluginMetaData.value(IidKey).toString();
    if (iid != m_seam.iid) {
        return refuse(QString(), QStringLiteral("its IID is %1, expected %2").arg(iid, m_seam.iid));
    }
    // Qt records the major and minor version the plugin was built against.
    // It loads a plugin from the same major version and an equal or older
    // minor one, and fails anything else only when it maps the library.
    const int builtAgainst = pluginMetaData.value(QtVersionKey).toInt(-1);
    const QVersionNumber running = QLibraryInfo::version();
    const int builtMajor = (builtAgainst >> 16) & 0xff;
    const int builtMinor = (builtAgainst >> 8) & 0xff;
    if (builtAgainst < 0 || builtMajor != running.majorVersion() || builtMinor > running.minorVersion()) {
        return refuse(QString(),
                      QStringLiteral("it is built against Qt %1.%2, and this process runs Qt %3.%4")
                          .arg(builtMajor)
                          .arg(builtMinor)
                          .arg(running.majorVersion())
                          .arg(running.minorVersion()));
    }
    const QJsonObject own = pluginMetaData.value(MetaDataKey).toObject();
    const QString id = own.value(IdKey).toString();
    if (!isValidName(id)) {
        return refuse(id, QStringLiteral("its metadata has no valid \"Id\""));
    }
    const QJsonValue apiValue = own.value(ApiVersionKey);
    const int apiVersion = apiValue.toInt(-1);
    if (!apiValue.isDouble() || static_cast<double>(apiVersion) != apiValue.toDouble()) {
        return refuse(id, QStringLiteral("its metadata has no integer \"ApiVersion\""));
    }
    if (apiVersion != m_seam.apiVersion) {
        return refuse(id,
                      QStringLiteral("plugin %1 is built for ApiVersion %2, this build loads %3")
                          .arg(id, QString::number(apiVersion), QString::number(m_seam.apiVersion)));
    }
    Vetted accepted{PluginMetadata{id, filePath, origin, own}, id, QString(), true};
    accepted.identity = identity;
    return accepted;
}

QObject* PluginCatalog::load(const QString& id, QString* errorString)
{
    const auto fail = [errorString](const QString& message) -> QObject* {
        if (errorString) {
            *errorString = message;
        }
        return nullptr;
    };

    const auto offered = m_plugins.constFind(id);
    if (offered == m_plugins.cend()) {
        return fail(QStringLiteral("no %1 plugin with id %2 is on offer").arg(m_seam.name, id));
    }
    if (const auto loaded = m_loaded.constFind(id); loaded != m_loaded.cend()) {
        if (!(*loaded)->instance) {
            return fail(QStringLiteral("plugin %1 was loaded, but its root object has since been deleted").arg(id));
        }
        return (*loaded)->instance.data();
    }
    const PluginMetadata offer = *offered;

    // The file may have changed since the scan vetted it. Vet it again right
    // before it is mapped. A file that no longer passes is not refused here:
    // load() schedules a rescan, which refuses and withdraws it, so the offer
    // always matches a scan.
    const Vetted vetted = vet(offer.filePath, offer.origin);
    if (!vetted.metadata || vetted.metadata->id != id) {
        m_offered.insert(id, identityOf(offer.filePath));
        m_watcher->requestRescan();
        return fail(
            vetted.metadata
                ? QStringLiteral("%1 now carries plugin %2, not %3").arg(offer.filePath, vetted.metadata->id, id)
                : QStringLiteral("%1 is no longer acceptable: %2").arg(offer.filePath, vetted.reason));
    }

    auto loader = std::make_unique<QPluginLoader>(offer.filePath);
    // Resolve every symbol now. With lazy binding a plugin built against a
    // newer library would load and then end the process the first time it
    // called the missing function.
    loader->setLoadHints(loader->loadHints() | QLibrary::ResolveAllSymbolsHint);
    // QPluginLoader falls back to similarly named files when the given one
    // is gone. Map only the file that was vetted.
    if (QFileInfo(loader->fileName()).canonicalFilePath() != QFileInfo(offer.filePath).canonicalFilePath()) {
        m_offered.insert(id, identityOf(offer.filePath));
        m_watcher->requestRescan();
        return fail(
            QStringLiteral("Qt resolved %1 to %2, which was not vetted").arg(offer.filePath, loader->fileName()));
    }
    QObject* const instance = loader->instance();
    if (!instance) {
        m_offered.insert(id, identityOf(offer.filePath));
        return fail(QStringLiteral("could not load %1: %2").arg(offer.filePath, loader->errorString()));
    }

    auto entry = std::make_shared<Loaded>();
    entry->metadata = *vetted.metadata;
    entry->loader = std::move(loader);
    entry->instance = instance;
    m_loaded.insert(id, entry);
    m_offered.remove(id);
    m_plugins.insert(id, entry->metadata);
    qCInfo(lcPluginCatalog) << qUtf8Printable(logPrefix(m_seam)) << "loaded" << id << "from" << offer.filePath;
    return instance;
}

PluginOrigin PluginCatalog::originOf(const QString& directory) const
{
    const QString clean = QDir::cleanPath(directory);
    for (const PluginSearchPath& path : m_searchPaths) {
        if (path.directory == clean) {
            return path.origin;
        }
    }
    return PluginOrigin::System;
}

bool PluginCatalog::shouldWarn(const QString& key)
{
    if (m_warned.contains(key)) {
        return false;
    }
    m_warned.insert(key);
    return true;
}

QStringList PluginCatalog::performScan(const QStringList& directoriesLowestPriorityFirst)
{
    const DepthGuard guard(m_scanDepth);

    // Winners by id, and every acceptable file by path including the ones a
    // higher-priority copy overrides.
    QHash<QString, PluginMetadata> found;
    QHash<QString, PluginMetadata> acceptable;
    // The file each acceptable path held when this scan vetted it.
    QHash<QString, FileIdentity> identities;
    QList<RefusedPlugin> refused;
    QSet<QString> raisedWarnings;
    // Files this scan reached, and directories it listed to the end.
    QSet<QString> reached;
    QSet<QString> listedDirectories;
    QStringList watchedFiles;
    int considered = 0;
    bool truncated = false;

    const auto refuse = [&](const QString& filePath, const QString& pluginId, const QString& reason) {
        refused.append({filePath, pluginId, reason});
        const QString key = filePath + QLatin1Char('|') + reason;
        raisedWarnings.insert(key);
        if (shouldWarn(key)) {
            qCWarning(lcPluginCatalog) << qUtf8Printable(logPrefix(m_seam)) << "refusing" << filePath << reason;
        }
    };
    const auto note = [&](const QString& key, const QString& message) {
        raisedWarnings.insert(key);
        if (shouldWarn(key)) {
            qCInfo(lcPluginCatalog) << qUtf8Printable(logPrefix(m_seam)) << message;
        }
    };

    // Highest priority first, so the first plugin found for an id wins.
    for (auto dirIt = directoriesLowestPriorityFirst.crbegin(); dirIt != directoriesLowestPriorityFirst.crend();
         ++dirIt) {
        const QString directory = *dirIt;
        const QDir dir(directory);
        if (!dir.exists()) {
            listedDirectories.insert(dir.absolutePath());
            continue;
        }
        const PluginOrigin origin = originOf(directory);
        // QDir::System lists dangling symlinks and special files too, so they
        // are refused with a reason instead of skipped in silence. QDir::Name
        // makes the order, and so the winner of an in-directory duplicate,
        // the same on every filesystem.
        const QStringList files = dir.entryList(QStringList{QStringLiteral("*.so")},
                                                QDir::Files | QDir::System | QDir::CaseSensitive, QDir::Name);
        QHash<QString, QString> idsInThisDirectory;
        for (const QString& fileName : files) {
            if (considered >= m_maxFilesPerScan) {
                truncated = true;
                break;
            }
            ++considered;
            const QString filePath = dir.absoluteFilePath(fileName);
            reached.insert(filePath);

            const Vetted vetted = vet(filePath, origin);
            if (vetted.watchable) {
                watchedFiles.append(filePath);
            }
            if (!vetted.metadata) {
                refuse(filePath, vetted.id, vetted.reason);
                continue;
            }
            const QString& id = vetted.metadata->id;
            if (const auto twin = idsInThisDirectory.constFind(id); twin != idsInThisDirectory.cend()) {
                refuse(filePath, id, QStringLiteral("%1 in the same directory already has id %2").arg(*twin, id));
                continue;
            }
            idsInThisDirectory.insert(id, fileName);
            acceptable.insert(filePath, *vetted.metadata);
            identities.insert(filePath, vetted.identity);

            if (const auto winner = found.constFind(id); winner != found.cend()) {
                note(QStringLiteral("override|") + filePath,
                     QStringLiteral("%1 (%2) overrides %3 (%4) for plugin %5")
                         .arg(winner->filePath, originName(winner->origin), filePath, originName(origin), id));
                continue;
            }
            found.insert(id, *vetted.metadata);
        }
        if (truncated) {
            break;
        }
        listedDirectories.insert(dir.absolutePath());
    }

    // A file the scan did not reach in a directory it did not finish listing
    // may still be there: keep what the last scan knew about it.
    const auto unreached = [&](const QString& filePath) {
        return truncated && !reached.contains(filePath) && !listedDirectories.contains(directoryOf(filePath));
    };
    if (truncated) {
        const QString key = QStringLiteral("truncated");
        raisedWarnings.insert(key);
        if (shouldWarn(key)) {
            qCWarning(lcPluginCatalog) << qUtf8Printable(logPrefix(m_seam)) << "stopped after" << m_maxFilesPerScan
                                       << "plugin files; plugins past that are not rescanned";
        }
        for (auto it = m_plugins.cbegin(); it != m_plugins.cend(); ++it) {
            if (!unreached(it->filePath)) {
                continue;
            }
            // Still acceptable as far as this scan can tell, which matters
            // for a loaded plugin even when a reached copy now wins its id.
            acceptable.insert(it->filePath, it.value());
            if (!found.contains(it.key())) {
                found.insert(it.key(), it.value());
            }
        }
        for (const RefusedPlugin& entry : std::as_const(m_refused)) {
            if (unreached(entry.filePath)) {
                refused.append(entry);
                raisedWarnings.insert(entry.filePath + QLatin1Char('|') + entry.reason);
            }
        }
    }

    // A loaded plugin keeps the code it was loaded from until the process
    // restarts, so while its file stays acceptable the offer describes that
    // file. Once the file is gone or refused the plugin is withdrawn, and a
    // copy elsewhere takes effect at the next start. Qt keeps reporting the
    // metadata of the loaded code for its path, so a file replaced at the
    // same path is not noticed until then.
    for (auto it = m_loaded.cbegin(); it != m_loaded.cend(); ++it) {
        const QString& id = it.key();
        const PluginMetadata& running = it.value()->metadata;
        const auto still = acceptable.constFind(running.filePath);
        const auto now = found.find(id);
        if (still == acceptable.cend() || still->id != id) {
            if (now != found.end()) {
                note(QStringLiteral("loaded-withdrawn|") + id + QLatin1Char('|') + now->filePath,
                     QStringLiteral("plugin %1 is withdrawn because %2 is gone or refused; %3 takes effect at the "
                                    "next start")
                         .arg(id, running.filePath, now->filePath));
                found.erase(now);
            }
            continue;
        }
        if (now != found.end() && now->filePath != running.filePath) {
            note(QStringLiteral("loaded-changed|") + id + QLatin1Char('|') + now->filePath,
                 QStringLiteral("plugin %1 now comes from %2, but %3 is already loaded; the change takes effect at "
                                "the next start")
                     .arg(id, now->filePath, running.filePath));
        }
        found.insert(id, running);
    }

    // A plugin on offer that is not loaded is offered again once the file
    // behind it is not the one it was last offered with, or the one a load
    // of it last failed on: a consumer that judged it by its metadata, or
    // gave up on a failed load, has to look again. One left untouched is not
    // offered again. A loaded plugin keeps its code and is never offered
    // again here. A file a cut-short scan did not reach keeps what was known.
    QStringList changed;
    QHash<QString, FileIdentity> offered;
    for (auto it = found.cbegin(); it != found.cend(); ++it) {
        const QString& id = it.key();
        if (m_loaded.contains(id)) {
            continue;
        }
        const auto before = m_offered.constFind(id);
        const auto now = identities.constFind(it->filePath);
        if (now == identities.cend()) {
            if (before != m_offered.cend()) {
                offered.insert(id, *before);
            }
            continue;
        }
        if (before != m_offered.cend() && *before != *now && m_plugins.contains(id)) {
            changed.append(id);
        }
        offered.insert(id, *now);
    }

    std::sort(refused.begin(), refused.end(), [](const RefusedPlugin& a, const RefusedPlugin& b) {
        return a.filePath < b.filePath;
    });

    QStringList added;
    QStringList removed;
    for (auto it = found.cbegin(); it != found.cend(); ++it) {
        if (!m_plugins.contains(it.key()) || changed.contains(it.key())) {
            added.append(it.key());
        }
    }
    for (auto it = m_plugins.cbegin(); it != m_plugins.cend(); ++it) {
        if (!found.contains(it.key())) {
            removed.append(it.key());
        }
    }
    std::sort(added.begin(), added.end());
    std::sort(removed.begin(), removed.end());

    m_plugins = std::move(found);
    // Before the signals: a slot can load() a plugin, which drops its entry
    // or records the file a failed load saw.
    m_offered = std::move(offered);
    m_refused = std::move(refused);
    // A problem that went away forgets its warning, so it warns again if it
    // comes back.
    m_warned = std::move(raisedWarnings);

    for (const QString& id : std::as_const(removed)) {
        Q_EMIT pluginRemoved(id);
    }
    for (const QString& id : std::as_const(added)) {
        Q_EMIT pluginAdded(id);
    }
    return watchedFiles;
}

} // namespace PhosphorRegistry
