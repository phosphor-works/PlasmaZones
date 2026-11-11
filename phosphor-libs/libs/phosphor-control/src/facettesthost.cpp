// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorControl/FacetTestHost.h"

#include "PhosphorControl/ApplicationController.h"
#include "PhosphorControl/FacetHost.h"
#include "PhosphorControl/PageController.h"
#include "PhosphorControl/PageRegistry.h"
#include "PhosphorControl/StagingDomain.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
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
    const QString staged = searchPath + QLatin1Char('/') + QFileInfo(m_modulePath).fileName();
    if (!QFile::copy(m_modulePath, staged)) {
        return {QStringLiteral("cannot copy %1 into the test search path").arg(m_modulePath)};
    }
    QFile::setPermissions(staged, mode);

    ApplicationController controller;
    FacetHost host(&controller);
    host.setSearchPaths({{searchPath, PhosphorRegistry::PluginOrigin::Development}});
    host.setSentinelPath(root.filePath(QStringLiteral("facet-loading")));
    host.setLiveReload(false);
    host.start();

    for (const PhosphorRegistry::RefusedPlugin& refused : host.refusedModules()) {
        problems.append(QStringLiteral("refused: %1").arg(refused.reason));
    }
    const QStringList modules = host.moduleIds();
    if (modules.isEmpty()) {
        if (problems.isEmpty()) {
            problems.append(QStringLiteral("no Facet module found in %1").arg(m_modulePath));
        }
        return problems;
    }
    // One staged file is one plugin, so the catalog offers one module at
    // most.
    const QString moduleId = modules.first();

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
    for (const FacetPageMetadata& page : metadata->pages) {
        const QString scoped = FacetHost::pageId(moduleId, page.id);
        const PageRegistry::Entry entry = controller.registry()->entry(scoped);
        if (qmlSourceExists(entry.qmlSource) == false) {
            problems.append(
                QStringLiteral("page %1's QML source %2 does not exist").arg(scoped, entry.qmlSource.toString()));
        }
        if (entry.controller) {
            domains.append(Checked{scoped, entry.controller.data()});
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
