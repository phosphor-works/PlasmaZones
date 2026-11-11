// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorControl/ApplicationController.h>
#include <PhosphorControl/FacetHost.h>
#include <PhosphorControl/PageController.h>
#include <PhosphorControl/PageRegistry.h>
#include <PhosphorControl/SearchController.h>
#include <PhosphorControl/StagingDomain.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#ifndef PHOSPHOR_CONTROL_FIXTURE_DIR
#error "PHOSPHOR_CONTROL_FIXTURE_DIR must be defined by tests/CMakeLists.txt"
#endif
#ifndef PHOSPHOR_CONTROL_STAGING_DIR
#error "PHOSPHOR_CONTROL_STAGING_DIR must be defined by tests/CMakeLists.txt"
#endif

using namespace PhosphorControl;
using State = FacetHost::ModuleState;

namespace {

class SimplePage : public PageController
{
public:
    using PageController::PageController;
    bool isDirty() const override
    {
        return false;
    }
    void apply() override
    {
        Q_EMIT applyResult(true, QString());
    }
    void discard() override
    {
        Q_EMIT discardResult(true, QString());
    }
};

QString fixture(const QString& variant)
{
    return QStringLiteral(PHOSPHOR_CONTROL_FIXTURE_DIR) + QLatin1Char('/') + variant + QStringLiteral(".so");
}

// PluginCatalog refuses a plugin, or a directory, that others can write.
// Set the mode and confirm the filesystem kept it, so a test fails here and
// not later with a puzzling "refused".
bool makeOwnerWritable(const QString& path)
{
    const QFileDevice::Permissions mode = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
        | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther;
    return QFile::setPermissions(path, mode)
        && !(QFile::permissions(path) & (QFileDevice::WriteGroup | QFileDevice::WriteOther));
}

// A QTemporaryDir template under the build tree's staging directory, which
// is made at configure time and made again here in case it was removed.
QString stagingTemplate(const QString& name)
{
    QDir().mkpath(QStringLiteral(PHOSPHOR_CONTROL_STAGING_DIR));
    return QStringLiteral(PHOSPHOR_CONTROL_STAGING_DIR "/") + name + QStringLiteral("-XXXXXX");
}

// Copy fixture @p variant into @p directory as <file>.so.
bool stage(const QString& directory, const QString& variant, const QString& file)
{
    const QString target = directory + QLatin1Char('/') + file + QStringLiteral(".so");
    QFile::remove(target);
    return QFile::copy(fixture(variant), target) && makeOwnerWritable(target);
}

// A module directory under the build tree, a controller and a host over it.
// Construct with the variants to stage, check ok, then call start().
struct Harness
{
    QTemporaryDir root{stagingTemplate(QStringLiteral("harness"))};
    QString modules;
    QString sentinel;
    ApplicationController controller;
    std::unique_ptr<FacetHost> host;
    bool ok = false;
    /// Why ok is false, for QVERIFY2.
    QString error;

    explicit Harness(const QStringList& variants, const QString& sentinelPath = QString(), bool liveReload = false)
    {
        if (!root.isValid()) {
            error = QStringLiteral("cannot create a directory under %1: %2")
                        .arg(QStringLiteral(PHOSPHOR_CONTROL_STAGING_DIR), root.errorString());
            return;
        }
        modules = root.filePath(QStringLiteral("facet"));
        sentinel = sentinelPath.isEmpty() ? root.filePath(QStringLiteral("facet-loading")) : sentinelPath;
        if (!QDir().mkpath(modules) || !makeOwnerWritable(modules)) {
            error = QStringLiteral("cannot prepare %1").arg(modules);
            return;
        }
        for (const QString& variant : variants) {
            if (!stage(modules, variant, variant)) {
                error = QStringLiteral("cannot stage fixture %1").arg(variant);
                return;
            }
        }
        host = std::make_unique<FacetHost>(&controller);
        host->setSearchPaths({{modules, PhosphorRegistry::PluginOrigin::Development}});
        host->setSentinelPath(sentinel);
        host->setLiveReload(liveReload);
        host->setLocale(QLocale::c());
        ok = true;
    }

    PageRegistry* registry() const
    {
        return controller.registry();
    }

    QStringList childIds(const QString& parentId) const
    {
        QStringList ids;
        for (const PageRegistry::Entry& entry : registry()->childPages(parentId)) {
            ids.append(entry.id);
        }
        return ids;
    }
};

QJsonObject readRecord(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeRecord(const QString& path, const QJsonObject& record)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(record).toJson()) > 0;
}

// The identity FacetHost records for a process: its boot and its start time
// (field 22 of /proc/<pid>/stat).
QString processIdentity(qint64 pid)
{
    QFile bootFile(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    QFile statFile(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!bootFile.open(QIODevice::ReadOnly) || !statFile.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QByteArray boot = bootFile.readAll().trimmed();
    const QByteArray stat = statFile.readAll();
    const qsizetype close = stat.lastIndexOf(')');
    const QList<QByteArray> fields = stat.mid(close + 1).simplified().split(' ');
    if (boot.isEmpty() || close < 0 || fields.size() <= 22 - 3) {
        return QString();
    }
    return QString::fromLatin1(boot + '/' + fields.at(22 - 3));
}

bool invokeSetDirty(QObject* object, bool dirty)
{
    return object && QMetaObject::invokeMethod(object, "setDirty", Q_ARG(bool, dirty));
}

} // namespace

class TestFacetHost : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void unloadedModulePagesAreOrdinaryPages()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("early"), QStringLiteral("child")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        PageRegistry* const registry = h.registry();
        QCOMPARE(h.host->moduleIds(),
                 (QStringList{QStringLiteral("child"), QStringLiteral("early"), QStringLiteral("good")}));
        for (const QString& id : h.host->moduleIds()) {
            QCOMPARE(h.host->state(id), std::optional<State>(State::Available));
        }

        // A module page shows the placeholder page until its module loads, so
        // it is a navigable page: in the rail, in search, and in keyboard
        // next/previous. The module and category nodes stay virtual.
        QCOMPARE(registry->entry(QStringLiteral("good:general")).qmlSource, FacetHost::placeholderPageSource());
        QVERIFY(registry->hasNavigablePage(QStringLiteral("good:general")));
        QVERIFY(!registry->hasNavigablePage(QStringLiteral("good")));
        QVERIFY(!registry->hasNavigablePage(FacetHost::categoryNodeId(QStringLiteral("input"))));
        QStringList visibleTop;
        for (const PageRegistry::Entry& entry : registry->visibleChildPages(QString())) {
            visibleTop.append(entry.id);
        }
        QVERIFY(visibleTop.contains(FacetHost::categoryNodeId(QStringLiteral("input"))));

        SearchController search(&h.controller);
        search.setQuery(QStringLiteral("advanced"));
        QStringList hits;
        const QVariantList results = search.results();
        for (const QVariant& result : results) {
            hits.append(result.toMap().value(QStringLiteral("pageId")).toString());
        }
        QVERIFY2(hits.contains(QStringLiteral("good:general/advanced")), qPrintable(hits.join(QLatin1Char(' '))));

        // Keyboard navigation reaches an unloaded module's page, and
        // opening it loads the module. The first page in tree order is the
        // one early placed in the appearance category.
        QCOMPARE(h.controller.gotoNextPage(), QStringLiteral("early:pinned"));
        QCOMPARE(h.host->state(QStringLiteral("early")), std::optional<State>(State::Loaded));

        // Module nodes are collapsible headers. A module page with child
        // pages is a drill parent with a page of its own, not a header,
        // which would hide its own page.
        QVERIFY(registry->entry(QStringLiteral("good")).isCollapsible);
        QVERIFY(!registry->entry(QStringLiteral("good:general")).isCollapsible);
        QCOMPARE(registry->parentIdOf(QStringLiteral("good:general/advanced")), QStringLiteral("good:general"));

        // Placeholders are never tracked: nothing is dirty and they are not
        // part of Apply.
        QVERIFY(!h.controller.isTrackedDomain(registry->controller(QStringLiteral("good:general"))));

        // Titles and icons come from the metadata, and the built-in
        // categories' names from the host.
        QCOMPARE(registry->entry(QStringLiteral("good")).title, QStringLiteral("Good"));
        QCOMPARE(registry->entry(QStringLiteral("good")).iconSource, QStringLiteral("good-icon"));
        QCOMPARE(registry->entry(QStringLiteral("good:general")).title, QStringLiteral("General"));
        QCOMPARE(registry->entry(FacetHost::categoryNodeId(QStringLiteral("input"))).title, QStringLiteral("Input"));
    }

    void siblingsFollowTheirOrder()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("early"), QStringLiteral("child")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        // Categories by their order, modules by their own Order, a module's
        // pages in declaration order with parents lifted, then its nested
        // modules.
        const QStringList top = h.childIds(QString());
        QCOMPARE(top,
                 (QStringList{FacetHost::categoryNodeId(QStringLiteral("appearance")),
                              FacetHost::categoryNodeId(QStringLiteral("input"))}));
        QCOMPARE(h.childIds(FacetHost::categoryNodeId(QStringLiteral("input"))),
                 (QStringList{QStringLiteral("early"), QStringLiteral("good")}));
        QCOMPARE(h.childIds(QStringLiteral("good")),
                 (QStringList{QStringLiteral("good:general"), QStringLiteral("good:about"), QStringLiteral("child")}));
    }

    void pageMetadataReachesTheSidebar()
    {
        Harness h({QStringLiteral("early")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        const PageRegistry::Entry simple = h.registry()->entry(QStringLiteral("early:simple"));
        QCOMPARE(simple.visibility, PageRegistry::PageVisibility::SimpleOnly);
        QVERIFY(simple.hasDividerAfter);
        QCOMPARE(simple.counterpartId, QStringLiteral("early:main"));
        QCOMPARE(h.registry()->entry(QStringLiteral("early:main")).counterpartId, QStringLiteral("early:simple"));

        // A page placed in a category directly.
        const PageRegistry::Entry pinned = h.registry()->entry(QStringLiteral("early:pinned"));
        QCOMPARE(pinned.parentId, FacetHost::categoryNodeId(QStringLiteral("appearance")));
        QCOMPARE(pinned.visibility, PageRegistry::PageVisibility::AdvancedOnly);
    }

    void placementFallsBackOnlyWhereItMust()
    {
        Harness h({QStringLiteral("orphan"), QStringLiteral("orphankid"), QStringLiteral("stray"),
                   QStringLiteral("owncat"), QStringLiteral("aaa"), QStringLiteral("zzz"), QStringLiteral("loopa"),
                   QStringLiteral("loopb"), QStringLiteral("looptail")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        PageRegistry* const registry = h.registry();

        // An absent parent module: its own known category. An absent parent
        // page: under its own module.
        QCOMPARE(registry->parentIdOf(QStringLiteral("orphan")), FacetHost::categoryNodeId(QStringLiteral("system")));
        QCOMPARE(registry->parentIdOf(QStringLiteral("orphan:page")), QStringLiteral("orphan"));
        // The page that fell back stays with the module's own pages, ahead
        // of the module nested under it.
        QCOMPARE(h.childIds(QStringLiteral("orphan")),
                 (QStringList{QStringLiteral("orphan:page"), QStringLiteral("orphankid")}));
        // An unknown category: apps. The parsed metadata still says what the
        // module declared.
        QCOMPARE(registry->parentIdOf(QStringLiteral("stray")), FacetHost::categoryNodeId(QStringLiteral("apps")));
        QCOMPARE(h.host->metadata(QStringLiteral("stray"))->category, QStringLiteral("no-such-category"));
        // A category a module defines.
        QCOMPARE(registry->parentIdOf(QStringLiteral("owncat")), FacetHost::categoryNodeId(QStringLiteral("games")));
        QCOMPARE(registry->entry(FacetHost::categoryNodeId(QStringLiteral("games"))).title, QStringLiteral("Games"));

        // A page under another module's page that is itself placed in a
        // category, whatever order the two modules were found in.
        QCOMPARE(registry->parentIdOf(QStringLiteral("zzz:hub")), FacetHost::categoryNodeId(QStringLiteral("system")));
        QCOMPARE(registry->parentIdOf(QStringLiteral("aaa:leaf")), QStringLiteral("zzz:hub"));

        // A nesting cycle loses one edge: the member that sorts first goes
        // to its category, the other stays nested under it, and a module
        // nested under a cycle member keeps its parent.
        QCOMPARE(registry->parentIdOf(QStringLiteral("loopa")), FacetHost::categoryNodeId(QStringLiteral("desktop")));
        QCOMPARE(registry->parentIdOf(QStringLiteral("loopb")), QStringLiteral("loopa"));
        QCOMPARE(registry->parentIdOf(QStringLiteral("looptail")), QStringLiteral("loopa"));
        QCOMPARE(h.host->metadata(QStringLiteral("loopa"))->parent, QStringLiteral("loopb"));
    }

    void metadataIsReadInTheHostLocale()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->setLocale(QLocale(QStringLiteral("de_DE")));
        h.host->start();
        QCOMPARE(h.registry()->entry(QStringLiteral("good:general")).title, QStringLiteral("Allgemein"));
        QCOMPARE(h.registry()->entry(QStringLiteral("good:about")).title, QStringLiteral("About"));
    }

    void openingAPageLoadsItsModule()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        PageController* const placeholder = h.registry()->controller(QStringLiteral("good:general"));
        QSignalSpy loaded(h.host.get(), &FacetHost::moduleLoaded);
        QSignalSpy bound(h.registry(), &PageRegistry::pageBound);
        QSignalSpy changed(h.host.get(), &FacetHost::moduleStateChanged);

        h.controller.setCurrentPageId(QStringLiteral("good:general"));

        QCOMPARE(loaded.count(), 1);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Loaded));
        QCOMPARE(h.controller.currentPageId(), QStringLiteral("good:general"));
        PageController* const real = h.registry()->controller(QStringLiteral("good:general"));
        QVERIFY(real);
        QVERIFY(real != placeholder);
        QCOMPARE(h.registry()->entry(QStringLiteral("good:general")).qmlSource,
                 QUrl(QStringLiteral("qrc:/fake/Page.qml")));
        QCOMPARE(bound.count(), 3);
        QCOMPARE(h.host->boundPages(QStringLiteral("good")).size(), 3);
        QCOMPARE(h.host->moduleDomains(QStringLiteral("good")).size(), 1);
        QVERIFY(h.controller.isTrackedDomain(real));
        QVERIFY(h.controller.isTrackedDomain(h.host->moduleDomains(QStringLiteral("good")).first()));

        // A second navigation does not load again.
        h.controller.setCurrentPageId(QStringLiteral("good:general/advanced"));
        QCOMPARE(loaded.count(), 1);
    }

    void aLoadedModuleJoinsApply()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        QVERIFY(!h.controller.isDirty());

        PageController* const page = h.registry()->controller(QStringLiteral("good:general"));
        QVERIFY(invokeSetDirty(page, true));
        QVERIFY(h.controller.isDirty());
        QCOMPARE(h.controller.dirtyPageIds(), QStringList{QStringLiteral("good:general")});
        h.controller.applyAll();
        QVERIFY(!h.controller.isDirty());

        StagingDomain* const domain = h.host->moduleDomains(QStringLiteral("good")).first();
        QVERIFY(invokeSetDirty(domain, true));
        QVERIFY(h.controller.isDirty());
        h.controller.discardAll();
        QVERIFY(!h.controller.isDirty());
    }

    void navigatingToAModuleNodeLoadsItButACategoryLoadsNothing()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("early")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        h.controller.setCurrentPageId(FacetHost::categoryNodeId(QStringLiteral("input")));
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Available));
        QCOMPARE(h.host->state(QStringLiteral("early")), std::optional<State>(State::Available));
        h.controller.setCurrentPageId(QStringLiteral("good"));
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Loaded));
        QCOMPARE(h.host->state(QStringLiteral("early")), std::optional<State>(State::Available));
    }

    void theLoadRecordNamesTheModuleWhileItLoads()
    {
        Harness h({QStringLiteral("recorder")});
        QVERIFY2(h.ok, qPrintable(h.error));
        qputenv("FACET_TEST_LOAD_RECORD", h.sentinel.toUtf8());
        const auto unset = qScopeGuard([] {
            qunsetenv("FACET_TEST_LOAD_RECORD");
        });
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("recorder")));
        const QJsonObject during =
            QJsonDocument::fromJson(qApp->property("facetLoadRecord").toString().toUtf8()).object();
        QCOMPARE(during.value(QStringLiteral("loading")).toString(), QStringLiteral("recorder"));
        QCOMPARE(during.value(QStringLiteral("pid")).toInteger(), QCoreApplication::applicationPid());
        QVERIFY(!readRecord(h.sentinel).contains(QStringLiteral("loading")));
    }

    void aModuleThatBreaksItsContractKeepsNothing_data()
    {
        QTest::addColumn<QString>("variant");
        QTest::addColumn<QString>("reason");
        QTest::newRow("skips a page") << QStringLiteral("missingpage") << QStringLiteral("did not register page about");
        QTest::newRow("registers an undeclared page")
            << QStringLiteral("extrapage") << QStringLiteral("registered page undeclared");
        QTest::newRow("wrong controller id") << QStringLiteral("wrongid") << QStringLiteral("wrong:id");
        QTest::newRow("registers a page twice") << QStringLiteral("twicepage") << QStringLiteral("twice");
        QTest::newRow("no controller") << QStringLiteral("nullcontroller") << QStringLiteral("with no controller");
        QTest::newRow("registers a domain twice") << QStringLiteral("twicedomain") << QStringLiteral("a domain twice");
        QTest::newRow("null domain") << QStringLiteral("nulldomain") << QStringLiteral("null domain");
        QTest::newRow("no QML") << QStringLiteral("noqml") << QStringLiteral("no valid QML source");
        QTest::newRow("relative QML") << QStringLiteral("relativeqml") << QStringLiteral("not an absolute URL");
    }

    void aModuleThatBreaksItsContractKeepsNothing()
    {
        QFETCH(QString, variant);
        QFETCH(QString, reason);
        Harness h({variant});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(h.host->moduleIds().contains(variant));
        QSignalSpy failed(h.host.get(), &FacetHost::moduleFailed);

        QString error;
        QVERIFY(!h.host->loadModule(variant, &error));
        QVERIFY2(error.contains(reason), qPrintable(error));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(h.host->state(variant), std::optional<State>(State::Failed));
        QVERIFY(h.host->stateReason(variant).contains(reason));

        // Nothing it registered took effect: every page still shows the
        // placeholder, and no domain joined Apply.
        const QString general = FacetHost::pageId(variant, QStringLiteral("general"));
        QCOMPARE(h.registry()->entry(general).qmlSource, FacetHost::placeholderPageSource());
        QVERIFY(!h.controller.isTrackedDomain(h.registry()->controller(general)));
        QVERIFY(h.host->boundPages(variant).isEmpty());
        QVERIFY(h.host->moduleDomains(variant).isEmpty());

        // So it can be retried, and a retry that fails the same way keeps
        // nothing either.
        QVERIFY(h.host->canRetry(variant));
        QVERIFY(!h.host->retryModule(variant));
        QCOMPARE(h.host->state(variant), std::optional<State>(State::Failed));
        QVERIFY(h.host->moduleDomains(variant).isEmpty());
    }

    void aRootThatIsNotAModuleFails()
    {
        Harness h({QStringLiteral("notamodule")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QString error;
        QVERIFY(!h.host->loadModule(QStringLiteral("notamodule"), &error));
        QVERIFY2(error.contains(QStringLiteral("IFacetModule")), qPrintable(error));
        QVERIFY(!h.host->canRetry(QStringLiteral("notamodule")));
    }

    void aModuleBuiltForAnotherApiVersionIsRefused()
    {
        Harness h({QStringLiteral("wrongapi")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(h.host->moduleIds().isEmpty());
        const auto refused = h.host->refusedModules();
        QCOMPARE(refused.size(), 1);
        QVERIFY(refused.first().reason.contains(QStringLiteral("ApiVersion 2")));
    }

    void invalidMetadataFailsWithoutAPlaceholder()
    {
        Harness h({QStringLiteral("badmetadata"), QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        // Announced once start() has built the sidebar, not while it is
        // still half built.
        bool sidebarBuilt = false;
        PageRegistry* const registry = h.registry();
        connect(h.host.get(), &FacetHost::moduleFailed, this, [&sidebarBuilt, registry](const QString& id) {
            if (id == QLatin1String("badmetadata")) {
                sidebarBuilt = registry->hasPage(QStringLiteral("good:general"));
            }
        });
        QSignalSpy failed(h.host.get(), &FacetHost::moduleFailed);
        h.host->start();
        QCOMPARE(failed.count(), 1);
        QVERIFY(sidebarBuilt);
        QVERIFY(h.host->moduleIds().contains(QStringLiteral("badmetadata")));
        QCOMPARE(h.host->state(QStringLiteral("badmetadata")), std::optional<State>(State::Failed));
        QVERIFY(h.host->stateReason(QStringLiteral("badmetadata")).contains(QStringLiteral("invalid metadata")));
        QVERIFY(!h.registry()->hasPage(QStringLiteral("badmetadata")));
        QVERIFY(!h.host->canRetry(QStringLiteral("badmetadata")));
        QVERIFY(!h.host->retryModule(QStringLiteral("badmetadata")));
    }

    void anUnknownModuleHasNoState()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(!h.host->state(QStringLiteral("nope")).has_value());
        QVERIFY(!h.host->canRetry(QStringLiteral("nope")));
    }

    void aModuleWhoseIdIsAnotherPageFails()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("child")});
        QVERIFY2(h.ok, qPrintable(h.error));
        auto* const appPage = new SimplePage(QStringLiteral("good"), &h.controller);
        h.controller.registerPage(appPage, QString(), QStringLiteral("App Page"), QUrl(QStringLiteral("qrc:/app.qml")));
        // Announced once, after start() has placed everything else.
        QList<bool> childPlacedAtEachFailure;
        PageRegistry* const registry = h.registry();
        connect(h.host.get(), &FacetHost::moduleFailed, this, [&childPlacedAtEachFailure, registry] {
            childPlacedAtEachFailure.append(registry->hasPage(QStringLiteral("child")));
        });
        h.host->start();
        QCOMPARE(childPlacedAtEachFailure, QList<bool>{true});
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Failed));
        QVERIFY(h.host->stateReason(QStringLiteral("good")).contains(QStringLiteral("already the id")));
        QCOMPARE(h.registry()->controller(QStringLiteral("good")), appPage);
        // The module nested under it falls back to its category.
        QCOMPARE(h.registry()->parentIdOf(QStringLiteral("child")), FacetHost::categoryNodeId(QStringLiteral("apps")));
        // Navigating to the app's page loads nothing.
        h.controller.setCurrentPageId(QStringLiteral("good"));
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Failed));
    }

    void aModuleWhosePageIdIsTakenFails()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        auto* const appPage = new SimplePage(QStringLiteral("good:general"), &h.controller);
        h.controller.registerPage(appPage, QString(), QStringLiteral("App Page"), QUrl(QStringLiteral("qrc:/app.qml")));
        h.host->start();
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Failed));
        QVERIFY(h.host->stateReason(QStringLiteral("good")).contains(QStringLiteral("good:general is already")));
        QVERIFY(!h.host->loadModule(QStringLiteral("good")));
        QCOMPARE(h.registry()->controller(QStringLiteral("good:general")), appPage);
    }

    void aHostPageThatOnlyLooksLikeAModulePageLoadsNothing()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.controller.registerPage(new SimplePage(QStringLiteral("good:other"), &h.controller), QString(),
                                  QStringLiteral("App Page"), QUrl(QStringLiteral("qrc:/app.qml")));
        h.host->start();
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Available));
        h.controller.setCurrentPageId(QStringLiteral("good:other"));
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Available));
    }

    void aCrashedModuleStaysDisabledUntilRetried()
    {
        QTemporaryDir state(stagingTemplate(QStringLiteral("state")));
        QVERIFY(state.isValid());
        const QString record = state.filePath(QStringLiteral("facet-loading"));
        QVERIFY(writeRecord(record, QJsonObject{{QStringLiteral("loading"), QStringLiteral("good")}}));

        {
            Harness h({QStringLiteral("good")}, record);
            QVERIFY2(h.ok, qPrintable(h.error));
            QSignalSpy changed(h.host.get(), &FacetHost::moduleStateChanged);
            h.host->start();
            QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::DisabledAfterCrash));
            QCOMPARE(changed.count(), 1);
            QCOMPARE(readRecord(record).value(QStringLiteral("disabled")).toArray(),
                     QJsonArray{QStringLiteral("good")});
            QVERIFY(!readRecord(record).contains(QStringLiteral("loading")));
            // Its pages are still shown, and opening one does not load it.
            h.controller.setCurrentPageId(QStringLiteral("good:general"));
            QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::DisabledAfterCrash));
        }
        {
            // The next start, without a retry: still disabled.
            Harness h({QStringLiteral("good")}, record);
            QVERIFY2(h.ok, qPrintable(h.error));
            h.host->start();
            QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::DisabledAfterCrash));

            // The placeholder page offers the retry, and a retry that loads
            // replaces the page that is on screen.
            h.controller.setCurrentPageId(QStringLiteral("good:general"));
            QObject* const placeholder = h.registry()->controller(QStringLiteral("good:general"));
            QVERIFY(placeholder->property("disabledAfterCrash").toBool());
            QVERIFY(placeholder->property("canRetry").toBool());
            QCOMPARE(placeholder->property("moduleName").toString(), QStringLiteral("Good"));
            QSignalSpy placeholderChanged(placeholder, SIGNAL(moduleStateChanged()));
            QSignalSpy bound(h.registry(), &PageRegistry::pageBound);
            QVERIFY(QMetaObject::invokeMethod(placeholder, "retry"));
            QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Loaded));
            QVERIFY(placeholderChanged.count() >= 1);
            QVERIFY(bound.count() >= 1);
            QCOMPARE(h.registry()->entry(QStringLiteral("good:general")).qmlSource,
                     QUrl(QStringLiteral("qrc:/fake/Page.qml")));
            QVERIFY(!readRecord(record).contains(QStringLiteral("disabled")));
        }
        {
            Harness h({QStringLiteral("good")}, record);
            QVERIFY2(h.ok, qPrintable(h.error));
            h.host->start();
            QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Available));
        }
    }

    void aLoadRecordThatCannotBeClearedIsIgnored()
    {
        QTemporaryDir state(stagingTemplate(QStringLiteral("state")));
        QVERIFY(state.isValid());
        const QString record = state.filePath(QStringLiteral("facet-loading"));
        QVERIFY(writeRecord(record, QJsonObject{{QStringLiteral("loading"), QStringLiteral("good")}}));
        QVERIFY(QFile::setPermissions(state.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto restore = qScopeGuard([&state] {
            QFile::setPermissions(state.path(),
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        });
        // Root, as in the container CI, writes regardless of the mode.
        QFile probe(state.filePath(QStringLiteral("probe")));
        if (probe.open(QIODevice::WriteOnly)) {
            probe.close();
            QFile::remove(probe.fileName());
            QSKIP("the directory is still writable (running as root), so the write cannot be made to fail");
        }
        Harness h({QStringLiteral("good")}, record);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        // Disabling it would last forever, since the record that names it
        // cannot be cleared.
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Available));
    }

    void aModuleAnotherLiveProcessIsLoadingIsNotDisabled()
    {
        QTemporaryDir state(stagingTemplate(QStringLiteral("state")));
        QVERIFY(state.isValid());
        const QString record = state.filePath(QStringLiteral("facet-loading"));
        QProcess other;
        other.start(QStringLiteral("sleep"), {QStringLiteral("30")});
        QVERIFY(other.waitForStarted());
        const auto stop = qScopeGuard([&other] {
            other.kill();
            other.waitForFinished();
        });
        const qint64 pid = other.processId();
        const QString identity = processIdentity(pid);
        QVERIFY2(!identity.isEmpty(), "cannot read the child's /proc entry");
        QVERIFY(writeRecord(record,
                            QJsonObject{{QStringLiteral("loading"), QStringLiteral("good")},
                                        {QStringLiteral("pid"), pid},
                                        {QStringLiteral("process"), identity}}));
        Harness h({QStringLiteral("good")}, record);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Available));
    }

    void aReusedPidIsNotTheProcessThatCrashed()
    {
        // The crashed process's pid now belongs to a live, unrelated process
        // (here process 1): its identity differs, so the crash counts.
        QTemporaryDir state(stagingTemplate(QStringLiteral("state")));
        QVERIFY(state.isValid());
        const QString record = state.filePath(QStringLiteral("facet-loading"));
        QVERIFY(writeRecord(record,
                            QJsonObject{{QStringLiteral("loading"), QStringLiteral("good")},
                                        {QStringLiteral("pid"), 1},
                                        {QStringLiteral("process"), QStringLiteral("another-boot/1")}}));
        Harness h({QStringLiteral("good")}, record);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::DisabledAfterCrash));
    }

    void aModuleChangedOnDiskSinceStartIsNotBound()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        // Replaced by rename, as a package upgrade does, before its first
        // load.
        QVERIFY(stage(h.modules, QStringLiteral("goodv2"), QStringLiteral("good.new")));
        QVERIFY(
            QFile::rename(h.modules + QStringLiteral("/good.new.so"), h.modules + QStringLiteral("/good.so"))
            || (QFile::remove(h.modules + QStringLiteral("/good.so"))
                && QFile::rename(h.modules + QStringLiteral("/good.new.so"), h.modules + QStringLiteral("/good.so"))));
        QString error;
        QVERIFY(!h.host->loadModule(QStringLiteral("good"), &error));
        QVERIFY2(error.contains(QStringLiteral("changed on disk")), qPrintable(error));
        QCOMPARE(h.registry()->entry(QStringLiteral("good:general")).qmlSource, FacetHost::placeholderPageSource());
        QVERIFY(!h.host->canRetry(QStringLiteral("good")));
    }

    void aLoadStartedDuringAnotherLoadIsRefused()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("early")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QString nestedError;
        bool nestedResult = true;
        FacetHost* const host = h.host.get();
        const auto connection =
            connect(h.registry(), &PageRegistry::pageBound, this, [&nestedError, &nestedResult, host] {
                if (nestedError.isEmpty()) {
                    nestedResult = host->loadModule(QStringLiteral("early"), &nestedError);
                }
            });
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        disconnect(connection);
        QVERIFY(!nestedResult);
        QVERIFY2(nestedError.contains(QStringLiteral("still loading")), qPrintable(nestedError));
        QCOMPARE(h.host->state(QStringLiteral("early")), std::optional<State>(State::Available));
        QVERIFY(h.host->loadModule(QStringLiteral("early")));
    }

    void theHookBelongsToItsOwner()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        QObject other;
        int otherCalls = 0;
        QVERIFY(h.controller.setNavigationHook(&other, [&otherCalls](const QString&) {
            ++otherCalls;
        }));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("already held by")));
        h.host->start();
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QCOMPARE(otherCalls, 1);
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Available));
        // The page on screen is the placeholder, and it can still load the
        // module.
        QObject* const placeholder = h.registry()->controller(QStringLiteral("good:general"));
        QVERIFY(placeholder->property("canRetry").toBool());
        QVERIFY(!placeholder->property("failed").toBool());
        QVERIFY(QMetaObject::invokeMethod(placeholder, "retry"));
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Loaded));
        // The host goes, and the hook it never held stays.
        h.host.reset();
        h.controller.setCurrentPageId(QStringLiteral("good:about"));
        QCOMPARE(otherCalls, 2);
    }

    void aDestroyedHostLeavesNoHookBehind()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        h.host.reset();
        // The placeholders belong to the controller and stay usable.
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QCOMPARE(h.controller.currentPageId(), QStringLiteral("good:general"));
        QVERIFY(h.registry()->controller(QStringLiteral("good:general")));
        QVERIFY(!h.registry()->controller(QStringLiteral("good:general"))->property("canRetry").toBool());
    }

    void modulesComingAndGoingWhileRunning()
    {
        Harness h({QStringLiteral("good")}, QString(), true);
        QVERIFY2(h.ok, qPrintable(h.error));
        QVERIFY(
            writeRecord(h.sentinel, QJsonObject{{QStringLiteral("disabled"), QJsonArray{QStringLiteral("early")}}}));
        h.host->start();

        // Added: slotted in by Order ahead of good, and still disabled.
        QVERIFY(stage(h.modules, QStringLiteral("early"), QStringLiteral("early")));
        QTRY_VERIFY_WITH_TIMEOUT(h.registry()->hasPage(QStringLiteral("early")), 10000);
        QCOMPARE(h.childIds(FacetHost::categoryNodeId(QStringLiteral("input"))),
                 (QStringList{QStringLiteral("early"), QStringLiteral("good")}));
        QCOMPARE(h.host->state(QStringLiteral("early")), std::optional<State>(State::DisabledAfterCrash));

        // Removed and installed again, it is still disabled: only a retry
        // turns it back on.
        QVERIFY(QFile::remove(h.modules + QStringLiteral("/early.so")));
        QTRY_COMPARE_WITH_TIMEOUT(h.host->state(QStringLiteral("early")), std::optional<State>(State::Failed), 10000);
        QVERIFY(stage(h.modules, QStringLiteral("early"), QStringLiteral("early")));
        QTRY_COMPARE_WITH_TIMEOUT(h.host->state(QStringLiteral("early")),
                                  std::optional<State>(State::DisabledAfterCrash), 10000);

        // Removed before loading: it fails, and coming back clears that.
        QVERIFY(QFile::remove(h.modules + QStringLiteral("/good.so")));
        QTRY_COMPARE_WITH_TIMEOUT(h.host->state(QStringLiteral("good")), std::optional<State>(State::Failed), 10000);
        QVERIFY(h.host->stateReason(QStringLiteral("good")).contains(QStringLiteral("removed")));
        QVERIFY(stage(h.modules, QStringLiteral("good"), QStringLiteral("good")));
        QTRY_COMPARE_WITH_TIMEOUT(h.host->state(QStringLiteral("good")), std::optional<State>(State::Available), 10000);
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
    }
};

QTEST_GUILESS_MAIN(TestFacetHost)
#include "test_facet_host.moc"
