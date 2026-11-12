// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorControl/FacetTestHost.h"

#include "PhosphorControl/ApplicationController.h"
#include "PhosphorControl/FacetHost.h"
#include "PhosphorControl/ISearchProvider.h"
#include "PhosphorControl/PageController.h"
#include "PhosphorControl/PageRegistry.h"
#include "PhosphorControl/StagingDomain.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPluginLoader>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>

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

// The search entries of the module's providers that do not resolve.
QStringList checkSearchProviders(const FacetHost& host, const QString& moduleId, const FacetModuleMetadata& metadata)
{
    QStringList problems;
    QSet<QString> extended;
    for (const FacetExtensionMetadata& extension : metadata.extensions) {
        extended.insert(extension.target);
    }
    for (const ISearchProvider* provider : host.searchProviders(moduleId)) {
        const QVector<SearchEntry> entries = provider->searchEntries();
        for (const SearchEntry& entry : entries) {
            if (entry.kind == SearchEntry::Kind::Action) {
                continue;
            }
            const QString where =
                entry.anchor.isEmpty() ? entry.pageId : entry.pageId + QLatin1Char('#') + entry.anchor;
            const bool ownPage = entry.pageId.startsWith(moduleId + QLatin1Char(':'))
                && metadata.page(entry.pageId.mid(moduleId.size() + 1));
            if (!ownPage && !extended.contains(entry.pageId)) {
                problems.append(QStringLiteral("search entry %1 (%2) is not on one of the module's pages or a page "
                                               "it extends")
                                    .arg(entry.title, where));
                continue;
            }
            const bool anchored = entry.kind == SearchEntry::Kind::Setting || entry.kind == SearchEntry::Kind::Section;
            if (ownPage && anchored && !entry.anchor.isEmpty()
                && !metadata.page(entry.pageId.mid(moduleId.size() + 1))->anchors.contains(entry.anchor)) {
                problems.append(QStringLiteral("search entry %1 (%2) names an anchor its page does not list in "
                                               "\"Anchors\"")
                                    .arg(entry.title, where));
            }
        }
    }
    return problems;
}

// Every page and anchor of the previous release that is gone without a bump
// of its page's anchors version.
QStringList checkAnchors(const QString& path, const FacetModuleMetadata& metadata)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {QStringLiteral("cannot read the previous anchors %1: %2").arg(path, file.errorString())};
    }
    const QJsonObject previous = QJsonDocument::fromJson(file.readAll()).object();
    if (previous.value(QLatin1String("Module")).toString() != metadata.id) {
        return {QStringLiteral("the previous anchors %1 are for module %2, not %3")
                    .arg(path, previous.value(QLatin1String("Module")).toString(), metadata.id)};
    }
    QStringList problems;
    const QJsonObject pages = previous.value(QLatin1String("Pages")).toObject();
    for (auto it = pages.constBegin(); it != pages.constEnd(); ++it) {
        const FacetPageMetadata* const page = metadata.page(it.key());
        if (!page) {
            problems.append(
                QStringLiteral("page %1 of the previous release is gone; other modules may extend it").arg(it.key()));
            continue;
        }
        const int was = it.value().toObject().value(QLatin1String("Version")).toInt(1);
        const int now = metadata.slotVersion(page->id, QStringLiteral("top"));
        if (now < was) {
            problems.append(QStringLiteral("page %1's anchors version went down from %2 to %3")
                                .arg(page->id, QString::number(was), QString::number(now)));
            continue;
        }
        if (now > was) {
            continue; // extensions built for the old anchors are refused, as they should be
        }
        const QJsonArray anchors = it.value().toObject().value(QLatin1String("Anchors")).toArray();
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

FacetTestHost::FacetTestHost(QString modulePath)
    : m_modulePath(std::move(modulePath))
{
}

void FacetTestHost::setResultTimeoutMs(int ms)
{
    m_resultTimeoutMs = ms > 0 ? ms : DefaultResultTimeoutMs;
}

void FacetTestHost::setStagingDirectory(const QString& directory)
{
    m_stagingDirectory = directory;
}

void FacetTestHost::addTargetModule(const QString& modulePath)
{
    m_targetPaths.append(modulePath);
}

void FacetTestHost::setPreviousAnchors(const QString& anchorsPath)
{
    m_previousAnchorsPath = anchorsPath;
}

QStringList FacetTestHost::run()
{
    QStringList problems;
    if (!QFileInfo(m_modulePath).isFile()) {
        return {QStringLiteral("no module file at %1").arg(m_modulePath)};
    }

    const QString stagingRoot = m_stagingDirectory.isEmpty() ? QDir::tempPath() : m_stagingDirectory;
    QDir().mkpath(stagingRoot);
    QTemporaryDir root(stagingRoot + QStringLiteral("/facet-test-XXXXXX"));
    if (!root.isValid()) {
        return {QStringLiteral("cannot create a temporary directory under %1").arg(stagingRoot)};
    }
    const QString searchPath = root.filePath(QStringLiteral("facet"));
    QDir().mkpath(searchPath);
    // PluginCatalog refuses a plugin, or a directory, that others can write.
    const QFileDevice::Permissions mode = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
        | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther;
    QFile::setPermissions(searchPath, mode);
    for (const QString& path : QStringList{m_modulePath} + m_targetPaths) {
        const QString staged = searchPath + QLatin1Char('/') + QFileInfo(path).fileName();
        if (!QFile::copy(path, staged)) {
            return {QStringLiteral("cannot copy %1 into the test search path").arg(path)};
        }
        QFile::setPermissions(staged, mode);
    }
    // The module under test, by the id in its embedded metadata.
    const QString moduleId = QPluginLoader(m_modulePath)
                                 .metaData()
                                 .value(QLatin1String("MetaData"))
                                 .toObject()
                                 .value(QLatin1String("Id"))
                                 .toString();

    ApplicationController controller;
    FacetHost host(&controller);
    host.setSearchPaths({{searchPath, PhosphorRegistry::PluginOrigin::Development}});
    host.setSentinelPath(root.filePath(QStringLiteral("facet-loading")));
    // Under root, which is declared before the controller and so outlives
    // it: the controller owns the backends, and a JsonBackend writes into
    // this directory once more when it is destroyed.
    host.setConfigRedirect(root.filePath(QStringLiteral("config")));
    host.setLiveReload(false);
    host.start();

    for (const PhosphorRegistry::RefusedPlugin& refused : host.refusedModules()) {
        problems.append(QStringLiteral("refused: %1").arg(refused.reason));
    }
    if (!host.moduleIds().contains(moduleId)) {
        if (problems.isEmpty()) {
            problems.append(QStringLiteral("no Facet module found in %1").arg(m_modulePath));
        }
        return problems;
    }

    QString error;
    if (!host.loadModule(moduleId, &error)) {
        problems.append(QStringLiteral("module %1 did not load: %2").arg(moduleId, error));
        return problems;
    }

    struct Checked
    {
        QString name;
        QPointer<StagingDomain> domain;
    };
    QList<Checked> domains;
    const std::optional<FacetModuleMetadata> metadata = host.metadata(moduleId);
    if (!metadata) {
        problems.append(QStringLiteral("module %1 has no metadata after it loaded").arg(moduleId));
        return problems;
    }
    problems.append(checkSearchProviders(host, moduleId, *metadata));
    const QString ownPrefix = moduleId + QLatin1Char(':');
    for (const FacetHost::RefusedExtension& refused : host.refusedExtensions()) {
        if (refused.id.startsWith(ownPrefix)) {
            problems.append(QStringLiteral("extension %1 is refused: %2").arg(refused.id, refused.reason));
        }
    }
    if (!m_previousAnchorsPath.isEmpty()) {
        problems.append(checkAnchors(m_previousAnchorsPath, *metadata));
    }
    const QStringList bound = host.boundPages(moduleId);
    for (const FacetPageMetadata& page : metadata->pages) {
        const QString scoped = FacetHost::pageId(moduleId, page.id);
        if (!bound.contains(scoped)) {
            // Its controller went while the module loaded, or the registry
            // refused it, and Facet keeps the placeholder, which cannot load
            // the module again.
            problems.append(QStringLiteral("page %1 was not bound, so Facet keeps its placeholder").arg(scoped));
            continue;
        }
        const PageRegistry::Entry entry = controller.registry()->entry(scoped);
        if (qmlSourceExists(entry.qmlSource) == false) {
            problems.append(
                QStringLiteral("page %1's QML source %2 does not exist").arg(scoped, entry.qmlSource.toString()));
        }
        if (entry.controller) {
            domains.append(Checked{scoped, entry.controller.data()});
        } else {
            problems.append(QStringLiteral("page %1's controller was deleted after it was bound").arg(scoped));
        }
    }
    const QList<StagingDomain*> headless = host.moduleDomains(moduleId);
    for (qsizetype i = 0; i < headless.size(); ++i) {
        StagingDomain* const domain = headless.at(i);
        const QString name = domain->objectName().isEmpty()
            ? QStringLiteral("%1 (headless domain %2)")
                  .arg(QString::fromLatin1(domain->metaObject()->className()), QString::number(i + 1))
            : domain->objectName();
        domains.append(Checked{name, domain});
    }
    // Every extension registered, whether or not its target is installed
    // here: a target is usually another module, which this run never loads.
    for (const FacetHost::ModuleExtension& extension : host.moduleExtensions(moduleId)) {
        const QString scoped = FacetHost::pageId(moduleId, extension.id);
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
            // Facet would show unsaved changes the moment the page opens.
            problems.append(QStringLiteral("%1 is dirty right after loading").arg(checked.name));
        }
    }

    for (const Checked& checked : std::as_const(domains)) {
        if (!checked.domain) {
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
