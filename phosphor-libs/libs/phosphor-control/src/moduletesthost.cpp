// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorControl/ModuleTestHost.h"

#include "PhosphorControl/ApplicationController.h"
#include "PhosphorControl/ModuleHost.h"
#include "PhosphorControl/ISearchProvider.h"
#include "PhosphorControl/PageController.h"
#include "PhosphorControl/PageRegistry.h"
#include "PhosphorControl/StagingDomain.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocale>
#include <QPluginLoader>
#include <QPointer>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <optional>
#include <utility>

namespace PhosphorControl {

namespace {

// How long to keep listening after the first result, for a second one.
constexpr int SecondResultGraceMs = 50;

struct ResultCount
{
    int count = 0;
    /// The error text of every result that reported failure.
    QStringList failures;
};

// Call @p trigger on @p domain and collect every @p signal it sends within
// the timeout, plus a short grace period to catch a second one.
template<typename Signal>
ResultCount countResults(StagingDomain* domain, Signal signal, void (StagingDomain::*trigger)(), int timeoutMs)
{
    ResultCount result;
    QEventLoop loop;
    const QMetaObject::Connection connection =
        QObject::connect(domain, signal, &loop, [&result, &loop](bool ok, const QString& error) {
            ++result.count;
            if (!ok) {
                result.failures.append(error.isEmpty() ? QStringLiteral("no error text") : error);
            }
            loop.quit();
        });
    (domain->*trigger)();
    if (result.count == 0) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        loop.exec();
    }
    // A second result for the same call is the bug this check exists for.
    // Wait for it in an event loop, so a result sent through a queued
    // connection or a timer arrives.
    QEventLoop grace;
    QTimer::singleShot(SecondResultGraceMs, &grace, &QEventLoop::quit);
    grace.exec();
    // Deliver a deleteLater() the call posted, so a domain that deleted
    // itself is seen as gone.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QObject::disconnect(connection);
    return result;
}

// Whether the QML file @p source names exists, for the schemes that can be
// checked without a QML engine. The module's library is loaded by now, so
// its compiled-in resources are registered.
std::optional<bool> qmlSourceExists(const QUrl& source)
{
    if (source.scheme() == QLatin1String("qrc")) {
        return QFile::exists(QLatin1Char(':') + source.path());
    }
    if (source.isLocalFile()) {
        return QFile::exists(source.toLocalFile());
    }
    return std::nullopt;
}

// The page a search entry on @p pageId lands on, read from the metadata of
// the module that owns it: the module under test, or a target module the
// host knows. Null when that module is not here or has no such page.
const ModulePageMetadata* pageOf(const ModuleHost& host, const QString& pageId, const QString& moduleId,
                                 const ModuleMetadata& metadata, QHash<QString, std::optional<ModuleMetadata>>& targets)
{
    const qsizetype colon = pageId.indexOf(QLatin1Char(':'));
    if (colon < 0) {
        return nullptr;
    }
    const QString owner = pageId.left(colon);
    const QString local = pageId.mid(colon + 1);
    if (owner == moduleId) {
        return metadata.page(local);
    }
    auto target = targets.find(owner);
    if (target == targets.end()) {
        target = targets.insert(owner, host.metadata(owner));
    }
    return target->has_value() ? (*target)->page(local) : nullptr;
}

// The search entries of the module's providers that do not resolve, and a
// module whose metadata promises providers it never registered.
QStringList checkSearchProviders(const ModuleHost& host, const QString& moduleId, const ModuleMetadata& metadata)
{
    QStringList problems;
    const QList<ISearchProvider*> providers = host.searchProviders(moduleId);
    if (metadata.searchProviders && providers.isEmpty()) {
        // The host loads such a module the first time the user searches, for
        // nothing.
        problems.append(
            QStringLiteral("its metadata says \"SearchProviders\": true, but it registered no search "
                           "provider"));
    }
    // The pages it extends, each with the anchors of its own sections there:
    // ModuleHost indexes an extension's section at `<module>:<extension>`, and
    // the section's ExtensionFrame registers that anchor with the page.
    QHash<QString, QSet<QString>> extended;
    for (const ModuleExtensionMetadata& extension : metadata.extensions) {
        extended[extension.target].insert(moduleId + QLatin1Char(':') + extension.id);
    }
    QHash<QString, std::optional<ModuleMetadata>> targets;
    for (const ISearchProvider* provider : providers) {
        const QVector<SearchEntry> entries = provider->searchEntries();
        for (const SearchEntry& entry : entries) {
            // An action carries a command, not a page.
            if (entry.kind == SearchEntry::Kind::Action) {
                continue;
            }
            const bool ownPage = entry.pageId.startsWith(moduleId + QLatin1Char(':'));
            const ModulePageMetadata* const page = pageOf(host, entry.pageId, moduleId, metadata, targets);
            if (ownPage ? !page : !extended.contains(entry.pageId)) {
                problems.append(QStringLiteral("search entry %1 (%2) is not on one of the module's pages or a page "
                                               "it extends")
                                    .arg(entry.title, entry.address()));
                continue;
            }
            const bool anchored = entry.kind == SearchEntry::Kind::Setting || entry.kind == SearchEntry::Kind::Section;
            if (!anchored || entry.anchor.isEmpty()) {
                continue;
            }
            if (ownPage) {
                if (!page->anchors.contains(entry.anchor)) {
                    problems.append(QStringLiteral("search entry %1 (%2) names an anchor its page does not list in "
                                                   "\"Anchors\"")
                                        .arg(entry.title, entry.address()));
                }
            } else if (page && !page->anchors.contains(entry.anchor)
                       && !extended.value(entry.pageId).contains(entry.anchor)) {
                // A page it extends, of a target module added with
                // addTargetModule(), at neither one of the page's anchors nor
                // one of the module's sections there. Without the target the
                // page's anchors are unknown here.
                problems.append(QStringLiteral("search entry %1 (%2) names an anchor page %3 does not list in "
                                               "\"Anchors\"")
                                    .arg(entry.title, entry.address(), entry.pageId));
            }
        }
    }
    return problems;
}

// Every page of the previous release that is gone, every anchor that is gone
// without a bump of its page's anchors version, and every named extension
// point that is gone or whose version went down.
QStringList checkAnchors(const QString& path, const ModuleMetadata& metadata)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {QStringLiteral("cannot read the previous anchors %1: %2").arg(path, file.errorString())};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return {QStringLiteral("the previous anchors %1 are not valid JSON: %2 at offset %3")
                    .arg(path, parseError.errorString(), QString::number(parseError.offset))};
    }
    if (!document.isObject()) {
        return {QStringLiteral("the previous anchors %1 are not a JSON object").arg(path)};
    }
    const QJsonObject previous = document.object();
    if (previous.value(QLatin1String("Module")).toString() != metadata.id) {
        return {QStringLiteral("the previous anchors %1 are for module %2, not %3")
                    .arg(path, previous.value(QLatin1String("Module")).toString(), metadata.id)};
    }
    QStringList problems;
    const QJsonObject pages = previous.value(QLatin1String("Pages")).toObject();
    for (auto it = pages.constBegin(); it != pages.constEnd(); ++it) {
        const ModulePageMetadata* const page = metadata.page(it.key());
        if (!page) {
            problems.append(
                QStringLiteral("page %1 of the previous release is gone; other modules may extend it").arg(it.key()));
            continue;
        }
        const QJsonObject previousPage = it.value().toObject();
        // Named points carry their own versions, apart from the anchors one.
        const QJsonObject points = previousPage.value(QLatin1String("ExtensionPoints")).toObject();
        for (auto point = points.constBegin(); point != points.constEnd(); ++point) {
            const int pointWas = point.value().toInt(1);
            const int pointNow = metadata.slotVersion(page->id, point.key());
            if (pointNow == 0) {
                problems.append(QStringLiteral("extension point %1 of page %2 is gone; other modules may extend it")
                                    .arg(point.key(), page->id));
            } else if (pointNow < pointWas) {
                problems.append(QStringLiteral("extension point %1 of page %2 went down from version %3 to %4")
                                    .arg(point.key(), page->id, QString::number(pointWas), QString::number(pointNow)));
            }
        }
        const int was = previousPage.value(QLatin1String("Version")).toInt(1);
        const int now = metadata.slotVersion(page->id, QStringLiteral("top"));
        if (now < was) {
            problems.append(QStringLiteral("page %1's anchors version went down from %2 to %3")
                                .arg(page->id, QString::number(was), QString::number(now)));
            continue;
        }
        if (now > was) {
            continue; // extensions built for the old anchors are refused, as they should be
        }
        const QJsonArray anchors = previousPage.value(QLatin1String("Anchors")).toArray();
        for (const QJsonValue& anchor : anchors) {
            if (!page->anchors.contains(anchor.toString())) {
                problems.append(QStringLiteral("anchor %1 of page %2 is gone, but the page's anchors version is "
                                               "still %3; raise the \"anchors\" extension point's version")
                                    .arg(anchor.toString(), page->id, QString::number(now)));
            }
        }
    }
    return problems;
}

} // namespace

ModuleTestHost::ModuleTestHost(QString modulePath)
    : m_modulePath(std::move(modulePath))
{
}

void ModuleTestHost::setResultTimeoutMs(int ms)
{
    m_resultTimeoutMs = ms > 0 ? ms : DefaultResultTimeoutMs;
}

void ModuleTestHost::setStagingDirectory(const QString& directory)
{
    m_stagingDirectory = directory;
}

void ModuleTestHost::addTargetModule(const QString& modulePath)
{
    m_targetPaths.append(modulePath);
}

void ModuleTestHost::setPreviousAnchors(const QString& anchorsPath)
{
    m_previousAnchorsPath = anchorsPath;
}

QStringList ModuleTestHost::run()
{
    QStringList problems;
    if (!QFileInfo(m_modulePath).isFile()) {
        return {QStringLiteral("no module file at %1").arg(m_modulePath)};
    }
    for (const QString& path : std::as_const(m_targetPaths)) {
        if (!QFileInfo(path).isFile()) {
            return {QStringLiteral("no target module file at %1").arg(path)};
        }
    }

    // Absolute, so every staged path is, and QPluginLoader reads the staged
    // file instead of searching its library paths for a relative name.
    const QString stagingRoot =
        QDir(m_stagingDirectory.isEmpty() ? QDir::tempPath() : m_stagingDirectory).absolutePath();
    QDir().mkpath(stagingRoot);
    QTemporaryDir root(stagingRoot + QStringLiteral("/module-test-XXXXXX"));
    if (!root.isValid()) {
        return {QStringLiteral("cannot create a temporary directory under %1").arg(stagingRoot)};
    }
    const QString searchPath = root.filePath(QStringLiteral("control"));
    QDir().mkpath(searchPath);
    // PluginCatalog refuses a plugin, or a directory, that others can write.
    const QFileDevice::Permissions mode = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
        | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther;
    QFile::setPermissions(searchPath, mode);

    // Each file is staged under a name of its own, whatever it was called:
    // the catalog reads only `*.so` files, and two targets may share a file
    // name. Messages name the file the caller passed, never the staged copy.
    QHash<QString, QString> originals;
    const auto stage = [&](const QString& path, const QString& name) -> QString {
        const QString staged = searchPath + QLatin1Char('/') + name;
        if (!QFile::copy(path, staged)) {
            return QString();
        }
        QFile::setPermissions(staged, mode);
        originals.insert(name, path);
        return staged;
    };
    // The module under test sorts first, so the catalog reads it first.
    const QString stagedModule = stage(m_modulePath, QStringLiteral("module.so"));
    if (stagedModule.isEmpty()) {
        return {QStringLiteral("cannot copy %1 into the test search path").arg(m_modulePath)};
    }
    // The module under test, by the id in its embedded metadata.
    const QJsonObject moduleMetaData =
        QPluginLoader(stagedModule).metaData().value(QLatin1String("MetaData")).toObject();
    const QString moduleId = moduleMetaData.value(QLatin1String("Id")).toString();
    // What a newer PhosphorControl may know (an unknown key, a search entry of an
    // unknown kind) is a warning to the host. Here it is built against the
    // same PhosphorControl as the module, so it is a typo or a version mismatch.
    // Its errors are the host's to report: the module fails to load.
    QStringList metadataErrors;
    QStringList metadataWarnings;
    ModuleMetadata::parse(moduleMetaData, QLocale::c(), &metadataErrors, &metadataWarnings);
    for (const QString& warning : std::as_const(metadataWarnings)) {
        problems.append(QStringLiteral("metadata: %1").arg(warning));
    }

    QHash<QString, QString> targetIds;
    for (qsizetype i = 0; i < m_targetPaths.size(); ++i) {
        const QString& path = m_targetPaths.at(i);
        const QString staged = stage(path, QStringLiteral("target-%1.so").arg(i, 3, 10, QLatin1Char('0')));
        if (staged.isEmpty()) {
            return {QStringLiteral("cannot copy %1 into the test search path").arg(path)};
        }
        const QString id = QPluginLoader(staged)
                               .metaData()
                               .value(QLatin1String("MetaData"))
                               .toObject()
                               .value(QLatin1String("Id"))
                               .toString();
        // The catalog would keep one of two files with one id and refuse
        // the other, so a target could stand in for the module under test.
        QString clash;
        if (!id.isEmpty() && id == moduleId) {
            clash = QStringLiteral("%1: not used as a target: it has the id %2 of the module under test").arg(path, id);
        } else if (const auto twin = targetIds.constFind(id); !id.isEmpty() && twin != targetIds.cend()) {
            clash = QStringLiteral("%1: not used as a target: target %2 has its id %3 too").arg(path, *twin, id);
        }
        if (!clash.isEmpty()) {
            problems.append(clash);
            QFile::remove(staged);
            originals.remove(QFileInfo(staged).fileName());
            continue;
        }
        if (!id.isEmpty()) {
            targetIds.insert(id, path);
        }
    }

    ApplicationController controller;
    ModuleHost host(&controller);
    host.setSearchPaths({{searchPath, PhosphorRegistry::PluginOrigin::Development}});
    host.setSentinelPath(root.filePath(QStringLiteral("control-loading")));
    // Under root, which is declared before the controller and so outlives
    // it: the controller owns the backends, and a JsonBackend writes into
    // this directory once more when it is destroyed.
    host.setConfigRedirect(root.filePath(QStringLiteral("config")));
    host.setLiveReload(false);
    host.start();

    bool moduleRefused = false;
    for (const PhosphorRegistry::RefusedPlugin& refused : host.refusedModules()) {
        const QString name = QFileInfo(refused.filePath).fileName();
        moduleRefused = moduleRefused || name == QFileInfo(stagedModule).fileName();
        problems.append(QStringLiteral("%1: refused: %2").arg(originals.value(name, refused.filePath), refused.reason));
    }
    if (!host.moduleIds().contains(moduleId)) {
        if (!moduleRefused) {
            problems.append(QStringLiteral("no control module found in %1").arg(m_modulePath));
        }
        return problems;
    }

    QString error;
    if (!host.loadModule(moduleId, &error)) {
        // The library's own error names the staged copy, which the caller
        // never sees.
        problems.append(
            QStringLiteral("module %1 did not load: %2").arg(moduleId, error.replace(stagedModule, m_modulePath)));
        return problems;
    }
    // Deliver a deleteLater() that code run while loading posted, which the
    // host's own checks ran too early to see, so what it deleted is seen as
    // gone below.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    struct Checked
    {
        QString name;
        QPointer<StagingDomain> domain;
    };
    QList<Checked> domains;
    const std::optional<ModuleMetadata> metadata = host.metadata(moduleId);
    if (!metadata) {
        problems.append(QStringLiteral("module %1 has no metadata after it loaded").arg(moduleId));
        return problems;
    }
    problems.append(checkSearchProviders(host, moduleId, *metadata));
    const QString ownPrefix = moduleId + QLatin1Char(':');
    for (const ModuleHost::RefusedExtension& refused : host.refusedExtensions()) {
        if (refused.id.startsWith(ownPrefix)) {
            problems.append(QStringLiteral("extension %1 is refused: %2").arg(refused.id, refused.reason));
        }
    }
    if (!m_previousAnchorsPath.isEmpty()) {
        problems.append(checkAnchors(m_previousAnchorsPath, *metadata));
    }
    const QStringList bound = host.boundPages(moduleId);
    for (const ModulePageMetadata& page : metadata->pages) {
        const QString scoped = ModuleHost::pageId(moduleId, page.id);
        if (!bound.contains(scoped)) {
            // Its controller went while the module loaded, or the registry
            // refused it, and the host keeps the placeholder, which cannot load
            // the module again.
            problems.append(QStringLiteral("page %1 was not bound, so the host keeps its placeholder").arg(scoped));
            continue;
        }
        const PageRegistry::Entry entry = controller.registry()->entry(scoped);
        if (qmlSourceExists(entry.qmlSource) == false) {
            problems.append(
                QStringLiteral("page %1's QML source %2 does not exist").arg(scoped, entry.qmlSource.toString()));
        }
        if (entry.controller) {
            domains.append(Checked{QStringLiteral("page %1").arg(scoped), entry.controller.data()});
        } else {
            problems.append(QStringLiteral("page %1's controller was deleted after it was bound").arg(scoped));
        }
    }
    // A null entry is a domain the module registered that is gone, deleted
    // while the module loaded, or that the application refused. The host
    // applies neither, so whatever it staged is lost.
    const QList<StagingDomain*> headless = host.moduleDomains(moduleId);
    for (qsizetype i = 0; i < headless.size(); ++i) {
        StagingDomain* const domain = headless.at(i);
        if (!domain) {
            problems.append(
                QStringLiteral("headless domain %1 was deleted while the module loaded, or the application refused it")
                    .arg(QString::number(i + 1)));
            continue;
        }
        const QString name = domain->objectName().isEmpty()
            ? QStringLiteral("%1 (headless domain %2)")
                  .arg(QString::fromLatin1(domain->metaObject()->className()), QString::number(i + 1))
            : domain->objectName();
        domains.append(Checked{name, domain});
    }
    // Every extension registered, whether or not its target is installed
    // here: a target is usually another module, which this run never loads.
    for (const ModuleHost::ModuleExtension& extension : host.moduleExtensions(moduleId)) {
        const QString scoped = ModuleHost::pageId(moduleId, extension.id);
        if (qmlSourceExists(extension.qmlSource) == false) {
            problems.append(QStringLiteral("extension %1's QML source %2 does not exist")
                                .arg(scoped, extension.qmlSource.toString()));
        }
        if (extension.domain) {
            domains.append(Checked{QStringLiteral("extension %1's domain").arg(scoped), extension.domain});
        } else {
            problems.append(QStringLiteral("extension %1's domain was deleted while the module loaded").arg(scoped));
        }
    }

    for (const Checked& checked : std::as_const(domains)) {
        if (checked.domain && checked.domain->isDirty()) {
            // The host would show unsaved changes the moment the page opens.
            problems.append(QStringLiteral("%1 is dirty right after loading").arg(checked.name));
        }
    }

    for (const Checked& checked : std::as_const(domains)) {
        if (!checked.domain) {
            // Deleted by another domain's isDirty(), apply() or discard().
            problems.append(QStringLiteral("%1 was deleted before its apply() was called").arg(checked.name));
            continue;
        }
        const ResultCount applied =
            countResults(checked.domain.data(), &StagingDomain::applyResult, &StagingDomain::apply, m_resultTimeoutMs);
        if (applied.count != 1) {
            problems.append(QStringLiteral("%1 emitted applyResult %2 times for one apply()")
                                .arg(checked.name, QString::number(applied.count)));
        }
        for (const QString& failure : applied.failures) {
            problems.append(QStringLiteral("%1 reported a failed apply(): %2").arg(checked.name, failure));
        }
        if (!checked.domain) {
            problems.append(QStringLiteral("%1 was deleted by apply()").arg(checked.name));
            continue;
        }
        const ResultCount discarded = countResults(checked.domain.data(), &StagingDomain::discardResult,
                                                   &StagingDomain::discard, m_resultTimeoutMs);
        if (discarded.count != 1) {
            problems.append(QStringLiteral("%1 emitted discardResult %2 times for one discard()")
                                .arg(checked.name, QString::number(discarded.count)));
        }
        for (const QString& failure : discarded.failures) {
            problems.append(QStringLiteral("%1 reported a failed discard(): %2").arg(checked.name, failure));
        }
        if (!checked.domain) {
            problems.append(QStringLiteral("%1 was deleted by discard()").arg(checked.name));
        }
    }
    return problems;
}

} // namespace PhosphorControl
