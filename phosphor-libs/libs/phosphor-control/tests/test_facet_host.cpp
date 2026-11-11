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
#include <QThread>

#include <memory>

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

QString reasonOf(const QList<FacetHost::RefusedExtension>& refused, const QString& id)
{
    for (const FacetHost::RefusedExtension& r : refused) {
        if (r.id == id) {
            return r.reason;
        }
    }
    return QString();
}

// A domain that counts its resets.
class CountingDomain : public StagingDomain
{
public:
    using StagingDomain::StagingDomain;
    int resets = 0;
    bool isDirty() const override
    {
        return m_dirty;
    }
    void setDirty(bool dirty)
    {
        if (m_dirty != dirty) {
            m_dirty = dirty;
            Q_EMIT dirtyChanged();
        }
    }
    void apply() override
    {
        setDirty(false);
        Q_EMIT applyResult(true, QString());
    }
    void discard() override
    {
        setDirty(false);
        Q_EMIT discardResult(true, QString());
    }
    void resetToDefaults() override
    {
        ++resets;
        setDirty(false);
    }

private:
    bool m_dirty = false;
};

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
        QVERIFY(h.host->metadata(QStringLiteral("stray")));
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
        QVERIFY(h.host->metadata(QStringLiteral("loopa")));
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

        QCOMPARE(h.host->moduleDomains(QStringLiteral("good")).size(), 1);
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
        QTest::newRow("skips an extension")
            << QStringLiteral("missingextension") << QStringLiteral("did not register extension second");
        QTest::newRow("registers an undeclared extension")
            << QStringLiteral("extraextension") << QStringLiteral("registered extension undeclared");
        QTest::newRow("registers an extension twice")
            << QStringLiteral("extraextension") << QStringLiteral("registered extension first twice");
        QTest::newRow("registers a page twice") << QStringLiteral("twicepage") << QStringLiteral("twice");
        QTest::newRow("no controller") << QStringLiteral("nullcontroller") << QStringLiteral("with no controller");
        QTest::newRow("registers a domain twice")
            << QStringLiteral("twicedomain") << QStringLiteral("already registered as a headless domain");
        QTest::newRow("null domain") << QStringLiteral("nulldomain") << QStringLiteral("null domain");
        QTest::newRow("no QML") << QStringLiteral("noqml") << QStringLiteral("no valid QML source");
        QTest::newRow("relative QML") << QStringLiteral("relativeqml") << QStringLiteral("not an absolute URL");
        QTest::newRow("extension with no domain")
            << QStringLiteral("extensionnodomain") << QStringLiteral("registered extension first with no domain");
        QTest::newRow("extension domain already registered")
            << QStringLiteral("extensionshared")
            << QStringLiteral("extension first's domain is already registered as a headless domain");
        QTest::newRow("extension with a relative QML source")
            << QStringLiteral("extensionrelative")
            << QStringLiteral("extension first's QML source Section.qml is not an absolute URL");
        QTest::newRow("extension domain in another thread")
            << QStringLiteral("extensionthread") << QStringLiteral("extension first's domain lives in another thread");
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
        QVERIFY(h.host->moduleExtensions(variant).isEmpty());

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
            QVERIFY(placeholder);
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
        QVERIFY(placeholder);
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

    void extensionsResolveAgainstTheirTargetPage()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("extender"), QStringLiteral("extender2"),
                   QStringLiteral("badextender")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();

        // Order, then module, then id: top-a's Order puts it after top-b
        // although its id sorts first, and extender2:aaa follows extender's
        // Order 0 sections although its id sorts first. The one aimed at a
        // module that is not installed is neither shown nor refused.
        QCOMPARE(h.host->extensionsOn(QStringLiteral("good:general")),
                 (QStringList{QStringLiteral("extender:after-speed"), QStringLiteral("extender:panel"),
                              QStringLiteral("extender:top-b"), QStringLiteral("extender2:aaa"),
                              QStringLiteral("extender:top-a")}));
        QVERIFY(h.host->extensionsOn(QStringLiteral("absent:page")).isEmpty());

        const QList<FacetHost::RefusedExtension> refused = h.host->refusedExtensions();
        QStringList ids;
        for (const FacetHost::RefusedExtension& r : refused) {
            ids.append(r.id);
        }
        QCOMPARE(ids,
                 (QStringList{QStringLiteral("badextender:new-anchors"), QStringLiteral("badextender:no-anchor"),
                              QStringLiteral("badextender:no-page"), QStringLiteral("badextender:no-point"),
                              QStringLiteral("badextender:old-panel")}));
        QCOMPARE(reasonOf(refused, QStringLiteral("badextender:new-anchors")),
                 QStringLiteral("it is built for version 2 of slot top on good:general, which is at version 1"));
        QCOMPARE(reasonOf(refused, QStringLiteral("badextender:old-panel")),
                 QStringLiteral("it is built for version 1 of slot panel on good:general, which is at version 2"));
        QCOMPARE(reasonOf(refused, QStringLiteral("badextender:no-anchor")),
                 QStringLiteral("page good:general has no slot before:nope"));
        QCOMPARE(reasonOf(refused, QStringLiteral("badextender:no-point")),
                 QStringLiteral("page good:general/advanced has no slot panel"));
        QCOMPARE(reasonOf(refused, QStringLiteral("badextender:no-page")),
                 QStringLiteral("its target page good:nowhere does not exist"));

        // Nothing is listed for a slot before its module loads.
        QVERIFY(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).isEmpty());
    }

    void openingATargetPageLoadsTheModulesThatExtendIt()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("extender"), QStringLiteral("badextender")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QSignalSpy changed(&h.controller, &ApplicationController::extensionsChanged);
        // What a FacetSlot reading the list on the notice sees: the module
        // must be loaded by the time it is told.
        qsizetype topAtLastNotice = -1;
        ApplicationController* const controller = &h.controller;
        connect(controller, &ApplicationController::extensionsChanged, this, [controller, &topAtLastNotice] {
            topAtLastNotice = controller->extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).size();
        });

        h.controller.setCurrentPageId(QStringLiteral("good:general"));

        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Loaded));
        QCOMPARE(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Loaded));
        // Every one of its extensions was refused, so nothing needs it here.
        QCOMPARE(h.host->state(QStringLiteral("badextender")), std::optional<State>(State::Available));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.first().first().toString(), QStringLiteral("good:general"));
        QCOMPARE(topAtLastNotice, 2);

        const QVariantList top = h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top"));
        QCOMPARE(top.size(), 2);
        const QVariantMap first = top.first().toMap();
        QCOMPARE(first.value(QStringLiteral("id")).toString(), QStringLiteral("extender:top-b"));
        QCOMPARE(first.value(QStringLiteral("moduleId")).toString(), QStringLiteral("extender"));
        QCOMPARE(first.value(QStringLiteral("moduleName")).toString(), QStringLiteral("Extender"));
        QCOMPARE(first.value(QStringLiteral("name")).toString(), QStringLiteral("Top B"));
        QCOMPARE(first.value(QStringLiteral("modulePageId")).toString(), QStringLiteral("extender:own"));
        QCOMPARE(first.value(QStringLiteral("qmlSource")).toUrl(), QUrl(QStringLiteral("qrc:/fake/Page.qml")));
        auto* domain = first.value(QStringLiteral("domain")).value<QObject*>();
        QVERIFY(domain);
        QCOMPARE(domain->objectName(), QStringLiteral("top-b"));
        QVERIFY(!first.contains(QStringLiteral("error")));
        QCOMPARE(top.last().toMap().value(QStringLiteral("id")).toString(), QStringLiteral("extender:top-a"));

        QCOMPARE(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("after:speed")).size(), 1);
        QCOMPARE(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("panel")).size(), 1);
        QVERIFY(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("bottom")).isEmpty());

        // Its own headless domain, and every declared section, the inactive
        // ghost included. Only an active section's domain joins Apply.
        QCOMPARE(h.host->moduleDomains(QStringLiteral("extender")).size(), 1);
        const QList<FacetHost::ModuleExtension> sections = h.host->moduleExtensions(QStringLiteral("extender"));
        QCOMPARE(sections.size(), 5);
        StagingDomain* ghost = nullptr;
        for (const FacetHost::ModuleExtension& section : sections) {
            QVERIFY(section.domain);
            QCOMPARE(h.controller.isTrackedDomain(section.domain), section.id != QLatin1String("ghost"));
            if (section.id == QLatin1String("ghost")) {
                ghost = section.domain;
            }
        }
        QVERIFY(ghost);
        // Nothing shows it, so it cannot leave the application dirty with no
        // page to clean it from.
        QVERIFY(invokeSetDirty(ghost, true));
        QVERIFY(!h.controller.isDirty());
        QVERIFY(h.controller.dirtyPageIds().isEmpty());

        // A section whose domain is gone shows an error instead of a section
        // with no domain, and its page is told so.
        changed.clear();
        delete domain;
        QTRY_VERIFY(!changed.isEmpty());
        QCOMPARE(changed.first().first().toString(), QStringLiteral("good:general"));
        const QVariantList afterDelete =
            h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top"));
        QCOMPARE(afterDelete.size(), 2);
        QVERIFY(afterDelete.first().toMap().contains(QStringLiteral("error")));
        QVERIFY(!afterDelete.first().toMap().contains(QStringLiteral("domain")));
    }

    void anExtensionDirtiesAndResetsWithItsPage()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("extender")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        const QVariantList panel = h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("panel"));
        QCOMPARE(panel.size(), 1);
        QObject* const domain = panel.first().toMap().value(QStringLiteral("domain")).value<QObject*>();
        QVERIFY(qobject_cast<StagingDomain*>(domain));
        QVERIFY(!h.controller.isDirty());

        QVERIFY(invokeSetDirty(domain, true));
        QVERIFY(h.controller.isDirty());
        QCOMPARE(h.controller.dirtyPageIds(), QStringList{QStringLiteral("good:general")});

        // good's headless domain belongs to no page, so resetting the page
        // leaves it alone.
        QCOMPARE(h.host->moduleDomains(QStringLiteral("good")).size(), 1);
        StagingDomain* const headless = h.host->moduleDomains(QStringLiteral("good")).first();
        QVERIFY(invokeSetDirty(headless, true));

        h.controller.resetCurrentPage();
        QVERIFY(!qobject_cast<StagingDomain*>(domain)->isDirty());
        QVERIFY(h.controller.dirtyPageIds().isEmpty());
        QVERIFY(headless->isDirty());
        QVERIFY(h.controller.isDirty());
    }

    void aFailedExtenderShowsAnErrorInItsSlot()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("missingextension")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QSignalSpy changed(&h.controller, &ApplicationController::extensionsChanged);
        h.controller.setCurrentPageId(QStringLiteral("good:general"));

        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Loaded));
        QCOMPARE(h.host->state(QStringLiteral("missingextension")), std::optional<State>(State::Failed));
        // The failure reaches the page, so an open slot shows the error.
        bool told = false;
        for (const QList<QVariant>& args : std::as_const(changed)) {
            told = told || args.first().toString() == QLatin1String("good:general");
        }
        QVERIFY(told);
        const QVariantList bottom =
            h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("bottom"));
        QCOMPARE(bottom.size(), 2);
        for (const QVariant& entry : bottom) {
            const QVariantMap map = entry.toMap();
            QVERIFY(
                map.value(QStringLiteral("error")).toString().contains(QStringLiteral("did not register extension")));
            QVERIFY(!map.contains(QStringLiteral("domain")));
            QVERIFY(!map.value(QStringLiteral("disabledAfterCrash")).toBool());
        }
        // A failed load keeps none of its sections.
        QVERIFY(h.host->moduleExtensions(QStringLiteral("missingextension")).isEmpty());
        QVERIFY(h.controller.dirtyPageIds().isEmpty());
    }

    void aDisabledExtenderSaysSoInItsSlot()
    {
        QTemporaryDir state(stagingTemplate(QStringLiteral("state")));
        QVERIFY(state.isValid());
        const QString record = state.filePath(QStringLiteral("facet-loading"));
        QVERIFY(writeRecord(record, QJsonObject{{QStringLiteral("disabled"), QJsonArray{QStringLiteral("extender")}}}));
        Harness h({QStringLiteral("good"), QStringLiteral("extender")}, record);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QCOMPARE(h.host->state(QStringLiteral("extender")), std::optional<State>(State::DisabledAfterCrash));
        const QVariantList top = h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top"));
        QCOMPARE(top.size(), 2);
        for (const QVariant& entry : top) {
            const QVariantMap map = entry.toMap();
            QVERIFY(map.value(QStringLiteral("disabledAfterCrash")).toBool());
            QVERIFY(!map.value(QStringLiteral("error")).toString().isEmpty());
            QCOMPARE(map.value(QStringLiteral("modulePageId")).toString(), QStringLiteral("extender:own"));
            QVERIFY(!map.contains(QStringLiteral("domain")));
        }
    }

    void extendersWaitForThePagesOwnModule()
    {
        // The page's module is off, so the page shows its placeholder, which
        // has no slots: the extenders' code would run for nothing.
        QTemporaryDir state(stagingTemplate(QStringLiteral("state")));
        QVERIFY(state.isValid());
        const QString record = state.filePath(QStringLiteral("facet-loading"));
        QVERIFY(writeRecord(record, QJsonObject{{QStringLiteral("disabled"), QJsonArray{QStringLiteral("good")}}}));
        Harness h({QStringLiteral("good"), QStringLiteral("extender")}, record);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::DisabledAfterCrash));
        QCOMPARE(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Available));
        // Loaded some other way, its sections still have no page to show on,
        // so their domains stay out of Apply.
        QVERIFY(h.host->loadModule(QStringLiteral("extender")));
        QVERIFY(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).isEmpty());
        for (const FacetHost::ModuleExtension& section : h.host->moduleExtensions(QStringLiteral("extender"))) {
            QVERIFY(!h.controller.isTrackedDomain(section.domain));
        }
    }

    void aRetryOnTheOpenPageShowsItsSections()
    {
        // The page is open on its placeholder. extender is already loaded,
        // extender2 is not. The retry loads the page's module, and the open
        // page gets every section without being opened again.
        QTemporaryDir state(stagingTemplate(QStringLiteral("state")));
        QVERIFY(state.isValid());
        const QString record = state.filePath(QStringLiteral("facet-loading"));
        QVERIFY(writeRecord(record, QJsonObject{{QStringLiteral("disabled"), QJsonArray{QStringLiteral("good")}}}));
        Harness h({QStringLiteral("good"), QStringLiteral("extender"), QStringLiteral("extender2")}, record);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("extender")));
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QCOMPARE(h.host->state(QStringLiteral("extender2")), std::optional<State>(State::Available));
        QSignalSpy changed(&h.controller, &ApplicationController::extensionsChanged);

        QVERIFY(h.host->retryModule(QStringLiteral("good")));

        bool told = false;
        for (const QList<QVariant>& args : std::as_const(changed)) {
            told = told || args.first().toString() == QLatin1String("good:general");
        }
        QVERIFY(told);
        QCOMPARE(h.host->state(QStringLiteral("extender2")), std::optional<State>(State::Loaded));
        QCOMPARE(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).size(), 3);
        for (const FacetHost::ModuleExtension& section : h.host->moduleExtensions(QStringLiteral("extender"))) {
            QCOMPARE(h.controller.isTrackedDomain(section.domain), section.id != QLatin1String("ghost"));
        }
    }

    void aNavigationToTheSamePageDuringALoadStillLoadsItsExtenders()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("extender")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        ApplicationController* const controller = &h.controller;
        bool navigated = false;
        connect(h.registry(), &PageRegistry::pageBound, this, [controller, &navigated] {
            if (!navigated) {
                navigated = true;
                controller->setCurrentPageId(QStringLiteral("good:general"));
            }
        });
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QVERIFY(navigated);
        QCOMPARE(h.controller.currentPageId(), QStringLiteral("good:general"));
        QCOMPARE(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Loaded));
    }

    void aNavigationDuringALoadStopsTheRest()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("extender"), QStringLiteral("extender2")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        ApplicationController* const controller = &h.controller;
        connect(h.host.get(), &FacetHost::moduleLoaded, this, [controller](const QString& moduleId) {
            if (moduleId == QLatin1String("good")) {
                controller->setCurrentPageId(QStringLiteral("good:about"));
            }
        });
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        // The newer navigation stands, and nothing more was loaded for the
        // page it replaced.
        QCOMPARE(h.controller.currentPageId(), QStringLiteral("good:about"));
        QCOMPARE(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Available));
        QCOMPARE(h.host->state(QStringLiteral("extender2")), std::optional<State>(State::Available));
    }

    void moduleCodeThatDeletesWhileItLoadsDoesNotCrashTheHost()
    {
        // Its headless domain deletes a section's domain and a page's
        // controller once the host's checks have passed.
        Harness h({QStringLiteral("good"), QStringLiteral("deletesduringload")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral(
                                 "deleted the controller of page deletesduringload:general while it loaded")));
        QString error;
        QVERIFY2(h.host->loadModule(QStringLiteral("deletesduringload"), &error), qPrintable(error));
        // The page keeps its placeholder, and the section says its domain is
        // gone.
        QCOMPARE(h.registry()->entry(QStringLiteral("deletesduringload:general")).qmlSource,
                 FacetHost::placeholderPageSource());
        const QVariantList bottom =
            h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("bottom"));
        QCOMPARE(bottom.size(), 2);
        QVERIFY(bottom.at(0).toMap().value(QStringLiteral("error")).toString().contains(QStringLiteral("deleted")));
        QVERIFY(bottom.at(1).toMap().value(QStringLiteral("domain")).value<QObject*>());
    }

    void aNavigationAwayAndBackStopsTheRest()
    {
        // A slot goes to another page and back to the one being left: the
        // controller drops the navigation in progress, so nothing more is
        // loaded for it.
        Harness h({QStringLiteral("good"), QStringLiteral("extender"), QStringLiteral("early")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        h.controller.setCurrentPageId(QStringLiteral("early:main"));
        ApplicationController* const controller = &h.controller;
        connect(h.host.get(), &FacetHost::moduleLoaded, this, [controller](const QString& moduleId) {
            if (moduleId == QLatin1String("good")) {
                controller->setCurrentPageId(QStringLiteral("good:about"));
                controller->setCurrentPageId(QStringLiteral("early:main"));
            }
        });
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QCOMPARE(h.controller.currentPageId(), QStringLiteral("early:main"));
        QCOMPARE(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Available));
    }

    void aSlotOfALoadedModuleCanLoadAnother()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("early")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        FacetHost* const host = h.host.get();
        QString nestedError;
        bool nestedResult = false;
        connect(host, &FacetHost::moduleLoaded, this, [&nestedError, &nestedResult, host](const QString& moduleId) {
            if (moduleId == QLatin1String("good")) {
                nestedResult = host->loadModule(QStringLiteral("early"), &nestedError);
            }
        });
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        QVERIFY2(nestedResult, qPrintable(nestedError));
        QCOMPARE(h.host->state(QStringLiteral("early")), std::optional<State>(State::Loaded));
    }

    void aRetryThatCannotRunNowChangesNothing()
    {
        QTemporaryDir state(stagingTemplate(QStringLiteral("state")));
        QVERIFY(state.isValid());
        const QString record = state.filePath(QStringLiteral("facet-loading"));
        QVERIFY(writeRecord(record, QJsonObject{{QStringLiteral("disabled"), QJsonArray{QStringLiteral("early")}}}));
        Harness h({QStringLiteral("good"), QStringLiteral("early")}, record);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        FacetHost* const host = h.host.get();
        QString nestedError;
        bool nestedResult = true;
        const auto connection =
            connect(h.registry(), &PageRegistry::pageBound, this, [&nestedError, &nestedResult, host] {
                if (nestedError.isEmpty()) {
                    nestedResult = host->retryModule(QStringLiteral("early"), &nestedError);
                }
            });
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        disconnect(connection);
        QVERIFY(!nestedResult);
        QVERIFY2(nestedError.contains(QStringLiteral("still loading")), qPrintable(nestedError));
        // Still off, and still remembered as off.
        QCOMPARE(h.host->state(QStringLiteral("early")), std::optional<State>(State::DisabledAfterCrash));
        QVERIFY(readRecord(record).value(QStringLiteral("disabled")).toArray().contains(QStringLiteral("early")));
    }

    void loadingOffTheGuiThreadIsRefused()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        bool result = true;
        QString error;
        FacetHost* const host = h.host.get();
        std::unique_ptr<QThread> thread(QThread::create([host, &result, &error] {
            result = host->loadModule(QStringLiteral("good"), &error);
        }));
        thread->start();
        QVERIFY(thread->wait(10000));
        QVERIFY(!result);
        QVERIFY2(error.contains(QStringLiteral("GUI thread")), qPrintable(error));
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Available));
    }

    void unplacedModulesTakeNoPartInExtensions()
    {
        // clashext's id is an application page, so it is never placed and
        // its extension goes nowhere. The app page "good" does the same to
        // the target, whose extensions are then refused with the reason.
        Harness h({QStringLiteral("good"), QStringLiteral("clashext"), QStringLiteral("extender")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.controller.registerPage(new SimplePage(QStringLiteral("clashext"), &h.controller), QString(),
                                  QStringLiteral("App Page"), QUrl(QStringLiteral("qrc:/app.qml")));
        h.controller.registerPage(new SimplePage(QStringLiteral("good"), &h.controller), QString(),
                                  QStringLiteral("App Page"), QUrl(QStringLiteral("qrc:/app.qml")));
        h.host->start();
        QCOMPARE(h.host->state(QStringLiteral("clashext")), std::optional<State>(State::Failed));
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Failed));
        QVERIFY(h.host->extensionsOn(QStringLiteral("good:general")).isEmpty());
        const QList<FacetHost::RefusedExtension> refused = h.host->refusedExtensions();
        QVERIFY(reasonOf(refused, QStringLiteral("clashext:top")).isEmpty());
        QCOMPARE(reasonOf(refused, QStringLiteral("extender:top-a")),
                 QStringLiteral("its target module good is installed but has invalid metadata or could not be placed "
                                "in the sidebar"));
    }

    void extensionsThatArriveWhileThePageIsOpenLoad()
    {
        Harness h({QStringLiteral("good")}, QString(), true);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Loaded));
        QSignalSpy changed(&h.controller, &ApplicationController::extensionsChanged);
        QSignalSpy refusedChanged(h.host.get(), &FacetHost::refusedExtensionsChanged);
        qsizetype topAtLastNotice = -1;
        ApplicationController* const controller = &h.controller;
        connect(controller, &ApplicationController::extensionsChanged, this, [controller, &topAtLastNotice] {
            topAtLastNotice = controller->extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).size();
        });

        // The open page gets the new module's sections without being opened
        // again, and each notice names only that page.
        QVERIFY(stage(h.modules, QStringLiteral("extender"), QStringLiteral("extender")));
        QTRY_COMPARE_WITH_TIMEOUT(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Loaded),
                                  10000);
        QVERIFY(changed.count() >= 1);
        for (const QList<QVariant>& args : std::as_const(changed)) {
            QCOMPARE(args.first().toString(), QStringLiteral("good:general"));
        }
        QCOMPARE(topAtLastNotice, 2);
        QCOMPARE(refusedChanged.count(), 0);

        // Its sections count toward the page.
        const QVariantList panel = h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("panel"));
        QCOMPARE(panel.size(), 1);
        QVERIFY(invokeSetDirty(panel.first().toMap().value(QStringLiteral("domain")).value<QObject*>(), true));
        QCOMPARE(h.controller.dirtyPageIds(), QStringList{QStringLiteral("good:general")});

        // A module whose extensions do not fit changes the refused list, and
        // stays unloaded.
        QVERIFY(stage(h.modules, QStringLiteral("badextender"), QStringLiteral("badextender")));
        QTRY_VERIFY_WITH_TIMEOUT(!h.host->refusedExtensions().isEmpty(), 10000);
        QCOMPARE(refusedChanged.count(), 1);
        QCOMPARE(h.host->state(QStringLiteral("badextender")), std::optional<State>(State::Available));

        // Resolving again for the next module does not warn about them again.
        changed.clear();
        QTest::failOnWarning(QRegularExpression(QStringLiteral("skipping extension")));
        QVERIFY(stage(h.modules, QStringLiteral("early"), QStringLiteral("early")));
        QTRY_VERIFY_WITH_TIMEOUT(h.registry()->hasPage(QStringLiteral("early")), 10000);
        QCOMPARE(refusedChanged.count(), 1);
        // Nothing on the open page changed, so it was not told.
        QCOMPARE(changed.count(), 0);
    }

    void aRemovedExtenderLeavesThePage()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("extender")}, QString(), true);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QCOMPARE(h.host->extensionsOn(QStringLiteral("good:general")).size(), 4);
        QSignalSpy changed(&h.controller, &ApplicationController::extensionsChanged);

        // Removed before it loaded: its sections go, as if it had never been
        // installed, and come back with it.
        QVERIFY(QFile::remove(h.modules + QStringLiteral("/extender.so")));
        QTRY_VERIFY_WITH_TIMEOUT(h.host->extensionsOn(QStringLiteral("good:general")).isEmpty(), 10000);
        QVERIFY(changed.count() >= 1);
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QVERIFY(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).isEmpty());

        QVERIFY(stage(h.modules, QStringLiteral("extender"), QStringLiteral("extender")));
        QTRY_COMPARE_WITH_TIMEOUT(h.host->extensionsOn(QStringLiteral("good:general")).size(), 4, 10000);
        // The page is open, so it loads.
        QTRY_COMPARE_WITH_TIMEOUT(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Loaded),
                                  10000);
        QCOMPARE(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).size(), 2);
    }

    void anExtenderLoadsWithoutItsTarget()
    {
        Harness h({QStringLiteral("extender")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QString error;
        QVERIFY2(h.host->loadModule(QStringLiteral("extender"), &error), qPrintable(error));
        QVERIFY(h.host->refusedExtensions().isEmpty());
        QVERIFY(h.host->extensionsOn(QStringLiteral("good:general")).isEmpty());
        // No section is active, so none of their domains joined Apply.
        for (const FacetHost::ModuleExtension& section : h.host->moduleExtensions(QStringLiteral("extender"))) {
            QVERIFY(!h.controller.isTrackedDomain(section.domain));
        }
    }

    void attachingNeedsATrackedDomainAndARegisteredPage()
    {
        ApplicationController controller;
        controller.registerPage(new SimplePage(QStringLiteral("page"), &controller), QString(), QStringLiteral("Page"),
                                QUrl(QStringLiteral("qrc:/page.qml")));
        auto* const domain = new CountingDomain(&controller);
        QVERIFY(controller.registerDomain(domain));

        // An unknown page: the attachment would hang on, and mark a page
        // registered later under that id.
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral("attachDomainToPage: null domain or unknown page")));
        controller.attachDomainToPage(domain, QStringLiteral("missing"));
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral("attachDomainToPage: null domain or unknown page")));
        controller.attachDomainToPage(nullptr, QStringLiteral("page"));
        controller.registerPage(new SimplePage(QStringLiteral("missing"), &controller), QString(),
                                QStringLiteral("Missing"), QUrl(QStringLiteral("qrc:/missing.qml")));
        domain->setDirty(true);
        QVERIFY(controller.dirtyPageIds().isEmpty());

        // A domain that is not tracked is never applied, so it must not mark
        // a page either.
        CountingDomain untracked;
        untracked.setDirty(true);
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral("attachDomainToPage: domain is not registered")));
        controller.attachDomainToPage(&untracked, QStringLiteral("page"));
        QVERIFY(controller.dirtyPageIds().isEmpty());

        // Attached twice, it is reset once.
        controller.attachDomainToPage(domain, QStringLiteral("page"));
        controller.attachDomainToPage(domain, QStringLiteral("page"));
        QCOMPARE(controller.dirtyPageIds(), QStringList{QStringLiteral("page")});
        controller.setCurrentPageId(QStringLiteral("page"));
        controller.resetCurrentPage();
        QCOMPARE(domain->resets, 1);
        QVERIFY(controller.dirtyPageIds().isEmpty());
    }

    void theExtensionProviderBelongsToItsOwner()
    {
        Harness h({QStringLiteral("good"), QStringLiteral("extender")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QCOMPARE(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).size(), 2);

        // Another component cannot take it over while the host lives.
        QObject other;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("provider is already held by")));
        QVERIFY(!h.controller.setExtensionProvider(&other, [](const QString&, const QString&) {
            return QVariantList{QVariantMap{{QStringLiteral("id"), QStringLiteral("other:x")}}};
        }));
        h.controller.clearExtensionProvider(&other);
        QCOMPARE(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).size(), 2);

        // The host goes: its provider goes with it, and the open page is told.
        QSignalSpy changed(&h.controller, &ApplicationController::extensionsChanged);
        h.host.reset();
        QVERIFY(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).isEmpty());
        bool told = false;
        for (const QList<QVariant>& args : std::as_const(changed)) {
            told = told || args.first().toString() == QLatin1String("good:general");
        }
        QVERIFY(told);
    }
};

QTEST_GUILESS_MAIN(TestFacetHost)
#include "test_facet_host.moc"
