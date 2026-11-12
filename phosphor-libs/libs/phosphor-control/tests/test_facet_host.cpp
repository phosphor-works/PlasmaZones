// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorControl/ApplicationController.h>
#include <PhosphorControl/FacetHost.h>
#include <PhosphorControl/PageController.h>
#include <PhosphorControl/PageRegistry.h>
#include <PhosphorControl/SearchController.h>
#include <PhosphorControl/StagingDomain.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>

#include <unistd.h>

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

// The start of every staging directory this test makes, so sweepStaging()
// can tell them from the other tests' directories.
const QLatin1String StagingPrefix("test_facet_host-");

// A QTemporaryDir template under the build tree's staging directory, which
// is made at configure time and made again here in case it was removed. The
// name carries this process's id, which sweepStaging() reads.
QString stagingTemplate(const QString& name)
{
    QDir().mkpath(QStringLiteral(PHOSPHOR_CONTROL_STAGING_DIR));
    return QStringLiteral(PHOSPHOR_CONTROL_STAGING_DIR "/") + StagingPrefix + name + QLatin1Char('-')
        + QString::number(QCoreApplication::applicationPid()) + QStringLiteral("-XXXXXX");
}

// Removes the staging directories a run of this test left behind when it
// ended without cleaning up (a crash, a kill, a timeout). Only those whose
// process is gone: ctest can run this test beside another copy of itself.
void sweepStaging()
{
    static const QRegularExpression owned(QLatin1String("^") + StagingPrefix + QStringLiteral("[a-z]+-(\\d+)-.{6}$"));
    const QFileInfoList entries = QDir(QStringLiteral(PHOSPHOR_CONTROL_STAGING_DIR))
                                      .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo& entry : entries) {
        const QRegularExpressionMatch match = owned.match(entry.fileName());
        if (!match.hasMatch()) {
            continue;
        }
        const qint64 pid = match.captured(1).toLongLong();
        if (pid == QCoreApplication::applicationPid() || QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid))) {
            continue;
        }
        // A test that takes the write bit off its directory puts it back on
        // the way out, which a run that died never did.
        QFile::setPermissions(entry.filePath(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QDir(entry.filePath()).removeRecursively();
    }
}

// Copy fixture @p variant into @p directory as <file>.so.
bool stage(const QString& directory, const QString& variant, const QString& file)
{
    const QString target = directory + QLatin1Char('/') + file + QStringLiteral(".so");
    QFile::remove(target);
    return QFile::copy(fixture(variant), target) && makeOwnerWritable(target);
}

// Move @p from onto @p to in one step, replacing what is there, as a package
// upgrade does. QFile::rename refuses to replace an existing file.
bool replaceFile(const QString& from, const QString& to)
{
    return std::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
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

// The application properties and environment variables the fixtures read
// and write (see fakemodule.cpp), cleared when a test that uses them starts
// and again when it ends. Declare it before the Harness: the config modes'
// backends record their destruction when the controller goes.
struct FixtureState
{
    FixtureState()
    {
        clear();
    }
    ~FixtureState()
    {
        clear();
    }
    Q_DISABLE_COPY_MOVE(FixtureState)

    static void clear()
    {
        for (const char* name :
             {"facetLoadRecord", "fakeConfigFactoryPaths", "fakeConfigBackendsDestroyed", "fakeConfigNestedBackend",
              "fakeConfigOuterBackend", "fakeForeignController", "fakeForeignDomain"}) {
            qApp->setProperty(name, QVariant());
        }
        qunsetenv("FACET_TEST_LOAD_RECORD");
        qunsetenv("FAKE_CONFIG_PATH");
    }
};

// Puts the working directory back when a test that changes it ends. Declare
// it after the Harness, so the directory is restored before the Harness
// removes its own.
struct WorkingDirectory
{
    WorkingDirectory() = default;
    ~WorkingDirectory()
    {
        QDir::setCurrent(saved);
    }
    Q_DISABLE_COPY_MOVE(WorkingDirectory)

    const QString saved = QDir::currentPath();
};

// The file the config modes open when they load (FAKE_CONFIG_PATH).
void setConfigPath(const QString& path)
{
    qputenv("FAKE_CONFIG_PATH", QFile::encodeName(path));
}

// The files the config modes' factory was asked to open, one per call.
QStringList factoryCalls()
{
    return qApp->property("fakeConfigFactoryPaths").toStringList();
}

// How many backends the config modes made have been destroyed.
int backendsDestroyed()
{
    return qApp->property("fakeConfigBackendsDestroyed").toInt();
}

// The domain a config mode writes its key through, or null.
StagingDomain* configDomainOf(const FacetHost& host, const QString& moduleId)
{
    const QList<StagingDomain*> domains = host.moduleDomains(moduleId);
    const auto found = std::find_if(domains.cbegin(), domains.cend(), [](const StagingDomain* domain) {
        return domain->objectName() == QLatin1String("config");
    });
    return found != domains.cend() ? *found : nullptr;
}

// The "Fake" group of the config file at @p path, where each config mode
// writes its module id on apply().
QJsonObject fakeGroupIn(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("Fake")).toObject();
}

// Apply what is dirty as the chrome's Apply button does, and wait for the
// batch to end. True when it ended and every domain succeeded.
bool applyAndWait(ApplicationController& controller)
{
    QSignalSpy done(&controller, &ApplicationController::applyAllComplete);
    controller.applyAllAsync();
    return QTest::qWaitFor(
               [&done] {
                   return !done.isEmpty();
               },
               10000)
        && done.size() == 1 && done.first().first().toBool();
}

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

// Whether any extensionsChanged notice @p spy caught named @p pageId.
bool noticed(const QSignalSpy& spy, const QString& pageId)
{
    return std::any_of(spy.cbegin(), spy.cend(), [&pageId](const QList<QVariant>& args) {
        return args.first().toString() == pageId;
    });
}

// The states @p spy caught moduleStateChanged announcing for @p moduleId,
// in order.
QList<State> statesOf(const QSignalSpy& spy, const QString& moduleId)
{
    QList<State> states;
    for (const QList<QVariant>& args : spy) {
        if (args.at(0).toString() == moduleId) {
            states.append(args.at(1).value<State>());
        }
    }
    return states;
}

// How many moduleLoaded announcements @p spy caught for @p moduleId.
qsizetype loadsOf(const QSignalSpy& spy, const QString& moduleId)
{
    return std::count_if(spy.cbegin(), spy.cend(), [&moduleId](const QList<QVariant>& args) {
        return args.first().toString() == moduleId;
    });
}

// A warning that reads exactly @p text.
QRegularExpression exactly(const QString& text)
{
    return QRegularExpression(QLatin1Char('^') + QRegularExpression::escape(text) + QLatin1Char('$'));
}

} // namespace

class TestFacetHost : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        sweepStaging();
    }

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

    void pagesPlacedUnderEachOtherStayWithTheirModules()
    {
        // Each page waits for the other's, which waits for it: no order of
        // placing them works, so both fall back under their own module
        // instead of waiting for ever.
        Harness h({QStringLiteral("pagecyclea"), QStringLiteral("pagecycleb")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QCOMPARE(h.registry()->parentIdOf(QStringLiteral("pagecyclea:leaf")), QStringLiteral("pagecyclea"));
        QCOMPARE(h.registry()->parentIdOf(QStringLiteral("pagecycleb:leaf")), QStringLiteral("pagecycleb"));
        QCOMPARE(h.host->state(QStringLiteral("pagecyclea")), std::optional<State>(State::Available));
        QCOMPARE(h.host->state(QStringLiteral("pagecycleb")), std::optional<State>(State::Available));
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

    void settingsMadeAfterStartAreIgnored()
    {
        QTemporaryDir other(stagingTemplate(QStringLiteral("other")));
        QVERIFY(other.isValid());
        const QString otherRecord = other.filePath(QStringLiteral("facet-loading"));
        Harness h({QStringLiteral("badmetadata")}, QString(), true);
        QVERIFY2(h.ok, qPrintable(h.error));
        QSignalSpy changed(h.host.get(), &FacetHost::moduleStateChanged);
        h.host->start();
        // badmetadata's failure.
        QCOMPARE(changed.count(), 1);

        // Each is refused with a warning, and the host goes on as it started.
        QTest::ignoreMessage(QtWarningMsg,
                             exactly(QStringLiteral("FacetHost: setSearchPaths after start() is ignored")));
        h.host->setSearchPaths({{other.path(), PhosphorRegistry::PluginOrigin::Development}});
        QTest::ignoreMessage(QtWarningMsg,
                             exactly(QStringLiteral("FacetHost: setSentinelPath after start() is ignored")));
        h.host->setSentinelPath(otherRecord);
        QTest::ignoreMessage(QtWarningMsg, exactly(QStringLiteral("FacetHost: setLocale after start() is ignored")));
        h.host->setLocale(QLocale(QStringLiteral("de_DE")));
        QTest::ignoreMessage(QtWarningMsg,
                             exactly(QStringLiteral("FacetHost: setLiveReload after start() is ignored")));
        h.host->setLiveReload(false);
        QTest::ignoreMessage(QtWarningMsg,
                             exactly(QStringLiteral("FacetHost: setConfigRedirect after start() is ignored")));
        h.host->setConfigRedirect(other.filePath(QStringLiteral("config")));
        // A second start() finds and announces nothing again.
        QTest::ignoreMessage(QtWarningMsg, exactly(QStringLiteral("FacetHost: start() called twice")));
        h.host->start();
        QCOMPARE(changed.count(), 1);

        // Live reload is still on and still watches the first search path,
        // and a module that arrives is read in the C locale: good has a
        // German name for its general page.
        QVERIFY(stage(h.modules, QStringLiteral("good"), QStringLiteral("good")));
        QTRY_VERIFY_WITH_TIMEOUT(h.registry()->hasPage(QStringLiteral("good:general")), 10000);
        QCOMPARE(h.registry()->entry(QStringLiteral("good:general")).title, QStringLiteral("General"));
        // The load record stays where it was.
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        QVERIFY(QFileInfo::exists(h.sentinel));
        QVERIFY(!QFileInfo::exists(otherRecord));
    }

    void openingAPageLoadsItsModule()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        PageController* const placeholder = h.registry()->controller(QStringLiteral("good:general"));
        QVERIFY(placeholder);
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
        FixtureState fixtures;
        Harness h({QStringLiteral("recorder")});
        QVERIFY2(h.ok, qPrintable(h.error));
        qputenv("FACET_TEST_LOAD_RECORD", h.sentinel.toUtf8());
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("recorder")));
        const QJsonObject during =
            QJsonDocument::fromJson(qApp->property("facetLoadRecord").toString().toUtf8()).object();
        QCOMPARE(during.value(QStringLiteral("loading")).toString(), QStringLiteral("recorder"));
        QCOMPARE(during.value(QStringLiteral("pid")).toInteger(), QCoreApplication::applicationPid());
        QVERIFY(!readRecord(h.sentinel).contains(QStringLiteral("loading")));
    }

    void aRelativeLoadRecordPathStaysWhereItWasSet()
    {
        // Set relative in one working directory, the record stays there when
        // the working directory moves on before the module loads.
        FixtureState fixtures;
        Harness h({QStringLiteral("recorder")});
        QVERIFY2(h.ok, qPrintable(h.error));
        WorkingDirectory cwd;
        const QString first = h.root.filePath(QStringLiteral("first"));
        const QString second = h.root.filePath(QStringLiteral("second"));
        QVERIFY(QDir().mkpath(first));
        QVERIFY(QDir().mkpath(second));
        QVERIFY(QDir::setCurrent(first));
        const QString relative = QStringLiteral("state/facet-loading");
        const QString record = QFileInfo(relative).absoluteFilePath();
        h.host->setSentinelPath(relative);
        QVERIFY(QDir::setCurrent(second));
        qputenv("FACET_TEST_LOAD_RECORD", QFile::encodeName(record));
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("recorder")));
        const QJsonObject during =
            QJsonDocument::fromJson(qApp->property("facetLoadRecord").toString().toUtf8()).object();
        QCOMPARE(during.value(QStringLiteral("loading")).toString(), QStringLiteral("recorder"));
        QVERIFY(QDir(second).isEmpty());
        // Each write replaced the record by a rename, leaving nothing beside it.
        QCOMPARE(QDir(QFileInfo(record).absolutePath()).entryList(QDir::Files | QDir::Hidden | QDir::System),
                 QStringList{QStringLiteral("facet-loading")});
    }

    void anEmptyLoadRecordPathRemembersNothing()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->setSentinelPath(QString());
        // A write of a record to the empty path would warn about the load
        // record it could not write.
        QTest::failOnWarning(QRegularExpression(QStringLiteral("load record")));
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        QVERIFY(!QFileInfo::exists(h.sentinel));
    }

    void aLoadRecordThatIsALinkStaysALink()
    {
        QTemporaryDir state(stagingTemplate(QStringLiteral("state")));
        QVERIFY(state.isValid());
        const QString target = state.filePath(QStringLiteral("target"));
        const QString link = state.filePath(QStringLiteral("facet-loading"));
        // A key FacetHost never writes, so its absence afterwards shows the
        // writes went through the link to the target.
        QVERIFY(writeRecord(target, QJsonObject{{QStringLiteral("unrelated"), 1}}));
        QVERIFY(QFile::link(target, link));
        Harness h({QStringLiteral("good")}, link);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        QVERIFY(QFileInfo(link).isSymLink());
        QVERIFY(QFileInfo::exists(target));
        QCOMPARE(readRecord(target), QJsonObject());
    }

    void aLoadRecordKeepsItsPermissions()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        QVERIFY(writeRecord(h.sentinel, QJsonObject()));
        QVERIFY(QFile::setPermissions(h.sentinel,
                                      QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup));
        // Read back rather than spelled out: Qt also reports the *User bits,
        // derived from the owner bits for the file's owner.
        const QFileDevice::Permissions before = QFile::permissions(h.sentinel);
        QVERIFY(before.testFlag(QFileDevice::ReadGroup));
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        // A temporary file renamed into place would be owner-only.
        QCOMPARE(QFile::permissions(h.sentinel), before);
    }

    void anOldTemporaryRecordIsRemovedAtStart()
    {
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        const QString stale = h.sentinel + QStringLiteral(".aBc123");
        const QString fresh = h.sentinel + QStringLiteral(".dEf456");
        // Names QTemporaryFile never gives the record's temporary file: a
        // shorter suffix, a six-character suffix holding a dot, and the
        // record's name in another case (with a suffix of its own, so a
        // case-insensitive filesystem cannot make it the stale file).
        const QFileInfo sentinelInfo(h.sentinel);
        const QStringList others{
            h.sentinel + QStringLiteral(".bak"),
            h.sentinel + QStringLiteral(".v1.bak"),
            sentinelInfo.absoluteDir().filePath(sentinelInfo.fileName().toUpper() + QStringLiteral(".gHi789")),
        };
        QVERIFY(writeRecord(stale, QJsonObject()));
        QVERIFY(writeRecord(fresh, QJsonObject()));
        for (const QString& path : others) {
            QVERIFY(writeRecord(path, QJsonObject()));
        }
        const QDateTime old = QDateTime::currentDateTimeUtc().addSecs(-3600);
        for (const QString& path : QStringList{stale} + others) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(old, QFileDevice::FileModificationTime));
        }
        h.host->start();
        QVERIFY(!QFileInfo::exists(stale));
        // Possibly another instance's write in progress.
        QVERIFY(QFileInfo::exists(fresh));
        for (const QString& path : others) {
            QVERIFY2(QFileInfo::exists(path), qPrintable(path));
        }
    }

    void aLoadRecordThatIsNotWritableIsLeftAlone()
    {
        if (::geteuid() == 0) {
            QSKIP("root writes a file whatever its mode");
        }
        Harness h({QStringLiteral("good")});
        QVERIFY2(h.ok, qPrintable(h.error));
        // A key FacetHost never writes, so a record renamed over this one
        // would show by its absence.
        QVERIFY(writeRecord(h.sentinel, QJsonObject{{QStringLiteral("unrelated"), 1}}));
        QVERIFY(QFile::setPermissions(h.sentinel, QFileDevice::ReadOwner | QFileDevice::ReadGroup));
        const QFileDevice::Permissions before = QFile::permissions(h.sentinel);
        const QJsonObject content = readRecord(h.sentinel);
        h.host->start();
        // Refused like QSaveFile refuses it, not renamed over: the module
        // loads unrecorded, and the record keeps its mode and content.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("could not write the load record")));
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        QCOMPARE(QFile::permissions(h.sentinel), before);
        QCOMPARE(readRecord(h.sentinel), content);
        QVERIFY(QFile::setPermissions(h.sentinel, before | QFileDevice::WriteOwner));
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
        QTest::newRow("registers a page twice")
            << QStringLiteral("twicepage") << QStringLiteral("registered page general twice");
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
        QTest::newRow("opens a config file by a relative path")
            << QStringLiteral("relativeconfig") << QStringLiteral("relative.json, which is not an absolute path");
        QTest::newRow("page controller in another thread")
            << QStringLiteral("pagethread") << QStringLiteral("page general's controller lives in another thread");
        QTest::newRow("page controller already a headless domain")
            << QStringLiteral("pageasdomain")
            << QStringLiteral("page general's controller is already registered as a headless domain");
        QTest::newRow("domain in another thread")
            << QStringLiteral("domainthread") << QStringLiteral("registered a domain that lives in another thread");
        QTest::newRow("extension with no QML")
            << QStringLiteral("extensionnoqml") << QStringLiteral("extension first has no valid QML source");
        QTest::newRow("page controller deleted while registering")
            << QStringLiteral("deletespage")
            << QStringLiteral("page general's controller was deleted during registration");
        QTest::newRow("domain deleted while registering")
            << QStringLiteral("deletesdomain") << QStringLiteral("a domain was deleted during registration");
        QTest::newRow("page controller that is another page's")
            << QStringLiteral("foreignpage") << QStringLiteral("page general's controller is already another page's");
        QTest::newRow("domain the application tracks")
            << QStringLiteral("foreigndomain") << QStringLiteral("registered a domain the application already tracks");
    }

    void aModuleThatBreaksItsContractKeepsNothing()
    {
        QFETCH(QString, variant);
        QFETCH(QString, reason);
        FixtureState fixtures;
        Harness h({variant});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(h.host->moduleIds().contains(variant));
        QSignalSpy failed(h.host.get(), &FacetHost::moduleFailed);

        const QString general = FacetHost::pageId(variant, QStringLiteral("general"));
        // What foreignpage and foreigndomain hand over (see fakemodule.cpp):
        // a controller the registry holds already, this page's own
        // placeholder, and a domain the application tracks already.
        QVERIFY(h.registry()->controller(general));
        qApp->setProperty("fakeForeignController", QVariant::fromValue<QObject*>(h.registry()->controller(general)));
        auto* const tracked = new SimplePage(QStringLiteral("tracked"), &h.controller);
        QVERIFY(h.controller.registerDomain(tracked));
        qApp->setProperty("fakeForeignDomain", QVariant::fromValue<QObject*>(tracked));

        QString error;
        QVERIFY(!h.host->loadModule(variant, &error));
        QVERIFY2(error.contains(reason), qPrintable(error));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(h.host->state(variant), std::optional<State>(State::Failed));
        QVERIFY(h.host->stateReason(variant).contains(reason));

        // Nothing it registered took effect: every page still shows the
        // placeholder, and no domain joined Apply.
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

    void aConfigFileThatCannotBeOpenedFailsTheLoad_data()
    {
        QTest::addColumn<QString>("variant");
        // "@root" stands for the Harness's directory.
        QTest::addColumn<QString>("configPath");
        // "%1" stands for configPath.
        QTest::addColumn<QString>("reason");
        // The factory calls a load and a retry make.
        QTest::addColumn<int>("calls");
        QTest::newRow("a factory that makes no backend")
            << QStringLiteral("confignull") << QStringLiteral("@root/config.json")
            << QStringLiteral("could not open config file %1") << 2;
        QTest::newRow("no factory") << QStringLiteral("configempty") << QStringLiteral("@root/config.json")
                                    << QStringLiteral("could not open config file %1") << 0;
        QTest::newRow("a Qt resource") << QStringLiteral("sharera") << QStringLiteral(":/fake/config.json")
                                       << QStringLiteral("opened config file %1, which is a Qt resource path") << 0;
        QTest::newRow("a directory") << QStringLiteral("sharera") << QStringLiteral("@root/config/")
                                     << QStringLiteral("opened config file %1, which has no file name") << 0;
        QTest::newRow("a directory named by .") << QStringLiteral("sharera") << QStringLiteral("@root/config/.")
                                                << QStringLiteral("opened config file %1, which has no file name") << 0;
        QTest::newRow("a directory named by ..")
            << QStringLiteral("sharera") << QStringLiteral("@root/config/..")
            << QStringLiteral("opened config file %1, which has no file name") << 0;
    }

    void aConfigFileThatCannotBeOpenedFailsTheLoad()
    {
        QFETCH(QString, variant);
        QFETCH(QString, configPath);
        QFETCH(QString, reason);
        QFETCH(int, calls);
        FixtureState fixtures;
        Harness h({variant});
        QVERIFY2(h.ok, qPrintable(h.error));
        const QString path = configPath.replace(QLatin1String("@root"), h.root.path());
        setConfigPath(path);
        h.host->start();

        QString error;
        QVERIFY(!h.host->loadModule(variant, &error));
        QVERIFY2(error.contains(reason.arg(path)), qPrintable(error));
        QCOMPARE(h.host->state(variant), std::optional<State>(State::Failed));
        QVERIFY(h.host->boundPages(variant).isEmpty());
        QVERIFY(h.host->moduleDomains(variant).isEmpty());
        // Nothing is kept for the file, so a retry asks again.
        QVERIFY(!h.host->configBackend(path));
        QVERIFY(h.host->canRetry(variant));
        QVERIFY(!h.host->retryModule(variant, &error));
        QVERIFY2(error.contains(reason.arg(path)), qPrintable(error));
        QVERIFY(!h.host->configBackend(path));
        QCOMPARE(factoryCalls().size(), calls);
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

    void startAnnouncesEachModuleOnceWhenASlotRetriesAnother()
    {
        // start() found both disabled. A slot of early's announcement
        // retries good before start() reaches it, and that retry announces
        // good's changes itself, so start() says nothing more about good.
        QTemporaryDir state(stagingTemplate(QStringLiteral("state")));
        QVERIFY(state.isValid());
        const QString record = state.filePath(QStringLiteral("facet-loading"));
        QVERIFY(writeRecord(
            record,
            QJsonObject{{QStringLiteral("disabled"), QJsonArray{QStringLiteral("early"), QStringLiteral("good")}}}));
        QList<State> goodStates;
        QList<State> earlyStates;
        bool retried = false;
        bool retryResult = false;
        Harness h({QStringLiteral("early"), QStringLiteral("good")}, record);
        QVERIFY2(h.ok, qPrintable(h.error));
        FacetHost* const host = h.host.get();
        connect(host, &FacetHost::moduleStateChanged, this,
                [host, &goodStates, &earlyStates, &retried, &retryResult](const QString& moduleId, State state) {
                    if (moduleId == QLatin1String("good")) {
                        goodStates.append(state);
                    } else if (moduleId == QLatin1String("early")) {
                        earlyStates.append(state);
                        if (!retried) {
                            retried = true;
                            retryResult = host->retryModule(QStringLiteral("good"));
                        }
                    }
                });
        h.host->start();
        QVERIFY(retried);
        QVERIFY(retryResult);
        QCOMPARE(earlyStates, QList<State>{State::DisabledAfterCrash});
        QCOMPARE(goodStates, (QList<State>{State::Available, State::Loaded}));
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Loaded));
    }

    void aLoadRecordThatCannotBeClearedIsIgnored()
    {
        QTemporaryDir state(stagingTemplate(QStringLiteral("state")));
        QVERIFY(state.isValid());
        // A record whose name takes the whole NAME_MAX of 255 bytes can be
        // read, but not replaced: the replacement is written under a longer
        // name beside it first, which fails with ENAMETOOLONG for every user.
        // A mode bit would not do, since root (the container CI) writes
        // regardless, and a record under a regular file could not be read
        // either, which would never reach the guard.
        // A filesystem with a shorter NAME_MAX (ecryptfs allows 143 bytes)
        // cannot hold the record at all.
        if (::pathconf(QFile::encodeName(state.path()).constData(), _PC_NAME_MAX) < 255) {
            QSKIP("the temporary directory's filesystem cannot hold a 255-byte file name");
        }
        const QString record = state.filePath(QString(255, QLatin1Char('r')));
        QVERIFY(writeRecord(record, QJsonObject{{QStringLiteral("loading"), QStringLiteral("good")}}));
        {
            // Where O_TMPFILE is missing, QSaveFile names its temporary file
            // at open(), which already fails. Either step failing is a
            // replacement that fails.
            QSaveFile probe(record);
            const bool replaced = probe.open(QIODevice::WriteOnly) && probe.write("{}") > 0 && probe.commit();
            QVERIFY2(!replaced, "replacing the record must fail, or the guard is not reached");
        }
        QCOMPARE(readRecord(record).value(QStringLiteral("loading")).toString(), QStringLiteral("good"));
        Harness h({QStringLiteral("good")}, record);
        QVERIFY2(h.ok, qPrintable(h.error));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("could not write the load record")));
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral("was loading when Facet last stopped, but .* cannot be "
                                                               "written, so it is not disabled")));
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
        // Replaced in one rename over the old file, as a package upgrade
        // does, before its first load.
        QVERIFY(stage(h.root.path(), QStringLiteral("goodv2"), QStringLiteral("good-next")));
        QVERIFY(replaceFile(h.root.filePath(QStringLiteral("good-next.so")), h.modules + QStringLiteral("/good.so")));
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
        QTest::ignoreMessage(
            QtWarningMsg,
            QRegularExpression(QStringLiteral("setNavigationHook: refused, the hook is already held by")));
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
        // A slot of the notices the host sends as it goes navigates. The
        // hook is gone by then, so no module code runs for a host that is
        // being destroyed: the page stays on its placeholder.
        bool navigated = false;
        Harness h({QStringLiteral("good"), QStringLiteral("extender")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        ApplicationController* const controller = &h.controller;
        connect(controller, &ApplicationController::extensionsChanged, this, [controller, &navigated] {
            if (!navigated) {
                navigated = true;
                controller->setCurrentPageId(QStringLiteral("good:about"));
            }
        });
        h.host.reset();
        QVERIFY(navigated);
        QCOMPARE(h.controller.currentPageId(), QStringLiteral("good:about"));
        QCOMPARE(h.registry()->entry(QStringLiteral("good:about")).qmlSource, FacetHost::placeholderPageSource());
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

    void aModuleWhoseMetadataChangedWhileItWasAwayNeedsARestart()
    {
        // Its placeholders were built from the metadata it had, so the file
        // that comes back with other metadata cannot be loaded into them.
        Harness h({QStringLiteral("good")}, QString(), true);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(QFile::remove(h.modules + QStringLiteral("/good.so")));
        QTRY_VERIFY_WITH_TIMEOUT(h.host->stateReason(QStringLiteral("good")).contains(QStringLiteral("removed")),
                                 10000);
        QVERIFY(stage(h.modules, QStringLiteral("goodv2"), QStringLiteral("good")));
        QTRY_VERIFY_WITH_TIMEOUT(
            h.host->stateReason(QStringLiteral("good")).contains(QStringLiteral("metadata changed on disk")), 10000);
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Failed));
        QVERIFY(!h.host->canRetry(QStringLiteral("good")));
        QCOMPARE(h.registry()->entry(QStringLiteral("good")).title, QStringLiteral("Good"));
    }

    void aModuleFixedOnDiskIsPlacedAndAnnouncedOnce()
    {
        // Invalid metadata gives it no placeholders. Removed, then offered
        // again with valid metadata, it is read and placed afresh, and its
        // way back to Available is announced once.
        Harness h({QStringLiteral("badmetadata")}, QString(), true);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QCOMPARE(h.host->state(QStringLiteral("badmetadata")), std::optional<State>(State::Failed));
        QVERIFY(QFile::remove(h.modules + QStringLiteral("/badmetadata.so")));
        QTRY_VERIFY_WITH_TIMEOUT(h.host->stateReason(QStringLiteral("badmetadata")).contains(QStringLiteral("removed")),
                                 10000);
        QSignalSpy changed(h.host.get(), &FacetHost::moduleStateChanged);

        QVERIFY(stage(h.modules, QStringLiteral("fixedmetadata"), QStringLiteral("badmetadata")));
        QTRY_VERIFY_WITH_TIMEOUT(h.registry()->hasPage(QStringLiteral("badmetadata:page")), 10000);
        QCOMPARE(h.host->state(QStringLiteral("badmetadata")), std::optional<State>(State::Available));
        QCOMPARE(statesOf(changed, QStringLiteral("badmetadata")), QList<State>{State::Available});
        QCOMPARE(h.registry()->entry(QStringLiteral("badmetadata")).title, QStringLiteral("Fixed Metadata"));
        QVERIFY(h.host->loadModule(QStringLiteral("badmetadata")));
    }

    void aModuleFixedByARenameOverItsFileIsPlaced()
    {
        // The same fix, as a package upgrade delivers it: one rename over the
        // file, so the catalog never sees the module go. Its library was never
        // loaded, so the catalog offers the changed file again, and the host
        // reads and places it without a restart.
        Harness h({QStringLiteral("badmetadata")}, QString(), true);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QCOMPARE(h.host->state(QStringLiteral("badmetadata")), std::optional<State>(State::Failed));
        QSignalSpy changed(h.host.get(), &FacetHost::moduleStateChanged);

        QVERIFY(stage(h.root.path(), QStringLiteral("fixedmetadata"), QStringLiteral("badmetadata-next")));
        QVERIFY(replaceFile(h.root.filePath(QStringLiteral("badmetadata-next.so")),
                            h.modules + QStringLiteral("/badmetadata.so")));
        QTRY_VERIFY_WITH_TIMEOUT(h.registry()->hasPage(QStringLiteral("badmetadata:page")), 10000);
        QCOMPARE(h.host->state(QStringLiteral("badmetadata")), std::optional<State>(State::Available));
        QCOMPARE(statesOf(changed, QStringLiteral("badmetadata")), QList<State>{State::Available});
        QCOMPARE(h.registry()->entry(QStringLiteral("badmetadata")).title, QStringLiteral("Fixed Metadata"));
        QVERIFY(h.host->loadModule(QStringLiteral("badmetadata")));
    }

    void aModuleOfferedAgainAfterItsCodeLoadedWaitsForARestart()
    {
        // Its library loaded and its registerInto broke the contract, so the
        // process keeps that code mapped. The file coming back cannot be
        // tried: a retry would run the old code again.
        Harness h({QStringLiteral("missingpage")}, QString(), true);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        const QString id = QStringLiteral("missingpage");
        QVERIFY(!h.host->loadModule(id));
        QCOMPARE(h.host->state(id), std::optional<State>(State::Failed));
        QVERIFY(h.host->canRetry(id));

        QVERIFY(QFile::remove(h.modules + QStringLiteral("/missingpage.so")));
        QTRY_VERIFY_WITH_TIMEOUT(h.host->stateReason(id).contains(QStringLiteral("removed")), 10000);
        QSignalSpy changed(h.host.get(), &FacetHost::moduleStateChanged);
        QSignalSpy failed(h.host.get(), &FacetHost::moduleFailed);
        QVERIFY(stage(h.modules, id, id));
        const QString reason = QStringLiteral(
            "its file was offered again after its code had been loaded, so the file now on disk can be used only "
            "after a restart");
        QTRY_COMPARE_WITH_TIMEOUT(h.host->stateReason(id), reason, 10000);
        QCOMPARE(h.host->state(id), std::optional<State>(State::Failed));
        QVERIFY(!h.host->canRetry(id));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(statesOf(changed, id), QList<State>{State::Failed});
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.first().at(1).toString(), reason);
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
        // What a FacetSlot reading the list on the notice sees: the module
        // must be loaded by the time it is told. Declared before the
        // Harness, since the host sends notices as it is destroyed.
        qsizetype topAtLastNotice = -1;
        Harness h({QStringLiteral("good"), QStringLiteral("extender"), QStringLiteral("badextender")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QSignalSpy changed(&h.controller, &ApplicationController::extensionsChanged);
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
        QVERIFY(noticed(changed, QStringLiteral("good:general")));
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

    void anExtenderThatFailedToLoadLoadsOnceItsFileIsReplaced()
    {
        // extender's metadata in a library that cannot be loaded, on the
        // open page. A working build replaces it in one rename, so the
        // catalog offers the file again without ever seeing it go, and the
        // open page gets the sections without being opened again.
        Harness h({QStringLiteral("good")}, QString(), true);
        QVERIFY2(h.ok, qPrintable(h.error));
        QVERIFY(stage(h.modules, QStringLiteral("extenderbroken"), QStringLiteral("extender")));
        h.host->start();
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Loaded));
        QCOMPARE(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Failed));
        QVERIFY2(h.host->stateReason(QStringLiteral("extender")).contains(QStringLiteral("could not load")),
                 qPrintable(h.host->stateReason(QStringLiteral("extender"))));
        QVERIFY(h.host->canRetry(QStringLiteral("extender")));
        const QVariantList failedTop =
            h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top"));
        QCOMPARE(failedTop.size(), 2);
        for (const QVariant& entry : failedTop) {
            QVERIFY(entry.toMap().value(QStringLiteral("moduleFailed")).toBool());
        }
        QSignalSpy changed(h.host.get(), &FacetHost::moduleStateChanged);

        QVERIFY(stage(h.root.path(), QStringLiteral("extender"), QStringLiteral("extender-next")));
        QVERIFY(replaceFile(h.root.filePath(QStringLiteral("extender-next.so")),
                            h.modules + QStringLiteral("/extender.so")));
        QTRY_COMPARE_WITH_TIMEOUT(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Loaded),
                                  10000);
        QCOMPARE(h.controller.currentPageId(), QStringLiteral("good:general"));
        // Back to Available, its library never having loaded, and loaded
        // for the open page.
        QCOMPARE(statesOf(changed, QStringLiteral("extender")), (QList<State>{State::Available, State::Loaded}));
        const QVariantList top = h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top"));
        QCOMPARE(top.size(), 2);
        for (const QVariant& entry : top) {
            const QVariantMap map = entry.toMap();
            QVERIFY(map.value(QStringLiteral("domain")).value<QObject*>());
            QVERIFY(!map.contains(QStringLiteral("error")));
        }
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
        const QList<FacetHost::ModuleExtension> sections = h.host->moduleExtensions(QStringLiteral("extender"));
        QCOMPARE(sections.size(), 5);
        for (const FacetHost::ModuleExtension& section : sections) {
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
        // The top slot's size at each notice for the page.
        QList<qsizetype> topAtNotices;
        Harness h({QStringLiteral("good"), QStringLiteral("extender"), QStringLiteral("extender2")}, record);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("extender")));
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QCOMPARE(h.host->state(QStringLiteral("extender2")), std::optional<State>(State::Available));
        QSignalSpy changed(&h.controller, &ApplicationController::extensionsChanged);
        ApplicationController* const controller = &h.controller;
        connect(controller, &ApplicationController::extensionsChanged, this,
                [controller, &topAtNotices](const QString& pageId) {
                    if (pageId == QLatin1String("good:general")) {
                        topAtNotices.append(
                            controller->extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).size());
                    }
                });

        QVERIFY(h.host->retryModule(QStringLiteral("good")));

        QVERIFY(noticed(changed, QStringLiteral("good:general")));
        // Told as soon as good loaded, with extender's sections alone, and
        // again once extender2 loaded and added its own.
        QVERIFY(!topAtNotices.isEmpty());
        QCOMPARE(topAtNotices.first(), 2);
        QCOMPARE(topAtNotices.last(), 3);
        QCOMPARE(h.host->state(QStringLiteral("extender2")), std::optional<State>(State::Loaded));
        QCOMPARE(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).size(), 3);
        const QList<FacetHost::ModuleExtension> sections = h.host->moduleExtensions(QStringLiteral("extender"));
        QCOMPARE(sections.size(), 5);
        for (const FacetHost::ModuleExtension& section : sections) {
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

    void aPageASlotOpensWhileItsModuleLoadsGetsItsExtenders()
    {
        // Opening good:about loads good, and a slot of that load opens
        // good:general. That page's extenders cannot load while good does,
        // so they load once it has.
        bool navigated = false;
        Harness h({QStringLiteral("good"), QStringLiteral("extender")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        ApplicationController* const controller = &h.controller;
        connect(h.registry(), &PageRegistry::pageBound, this, [controller, &navigated] {
            if (!navigated) {
                navigated = true;
                controller->setCurrentPageId(QStringLiteral("good:general"));
            }
        });
        QSignalSpy loaded(h.host.get(), &FacetHost::moduleLoaded);
        h.controller.setCurrentPageId(QStringLiteral("good:about"));
        QVERIFY(navigated);
        QCOMPARE(h.controller.currentPageId(), QStringLiteral("good:general"));
        QCOMPARE(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Loaded));
        QCOMPARE(loadsOf(loaded, QStringLiteral("extender")), 1);
        QCOMPARE(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).size(), 2);
    }

    void aPageASlotOpensWhileAnotherModuleLoadsGetsItsExtenders()
    {
        // good is loaded. Opening early:main loads early, and a slot of that
        // load opens good:general, whose extenders load once early has.
        bool navigated = false;
        Harness h({QStringLiteral("good"), QStringLiteral("extender"), QStringLiteral("early")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        ApplicationController* const controller = &h.controller;
        connect(h.registry(), &PageRegistry::pageBound, this, [controller, &navigated] {
            if (!navigated) {
                navigated = true;
                controller->setCurrentPageId(QStringLiteral("good:general"));
            }
        });
        QSignalSpy loaded(h.host.get(), &FacetHost::moduleLoaded);
        h.controller.setCurrentPageId(QStringLiteral("early:main"));
        QVERIFY(navigated);
        QCOMPARE(h.controller.currentPageId(), QStringLiteral("good:general"));
        QCOMPARE(h.host->state(QStringLiteral("early")), std::optional<State>(State::Loaded));
        QCOMPARE(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Loaded));
        QCOMPARE(loadsOf(loaded, QStringLiteral("extender")), 1);
        QCOMPARE(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).size(), 2);
    }

    void aPageASlotOpensDuringADirectLoadGetsItsExtenders()
    {
        // The same with no navigation around the load: loadModule() itself,
        // as a placeholder's retry or the host application calls it.
        bool navigated = false;
        Harness h({QStringLiteral("good"), QStringLiteral("extender"), QStringLiteral("early")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        ApplicationController* const controller = &h.controller;
        connect(h.registry(), &PageRegistry::pageBound, this, [controller, &navigated] {
            if (!navigated) {
                navigated = true;
                controller->setCurrentPageId(QStringLiteral("good:general"));
            }
        });
        QSignalSpy loaded(h.host.get(), &FacetHost::moduleLoaded);
        QVERIFY(h.host->loadModule(QStringLiteral("early")));
        QVERIFY(navigated);
        QCOMPARE(h.controller.currentPageId(), QStringLiteral("good:general"));
        QCOMPARE(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Loaded));
        QCOMPARE(loadsOf(loaded, QStringLiteral("extender")), 1);
        QCOMPARE(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).size(), 2);
    }

    void aPageASlotOpensAfterALoadGetsItsExtendersOnce()
    {
        // The control for the tests above: opened from moduleLoaded, when the
        // load is over, the page loads its extenders itself, and nothing is
        // left owed to load them a second time.
        bool navigated = false;
        Harness h({QStringLiteral("good"), QStringLiteral("extender"), QStringLiteral("early")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        ApplicationController* const controller = &h.controller;
        connect(h.host.get(), &FacetHost::moduleLoaded, this, [controller, &navigated](const QString& moduleId) {
            if (moduleId == QLatin1String("early") && !navigated) {
                navigated = true;
                controller->setCurrentPageId(QStringLiteral("good:general"));
            }
        });
        QSignalSpy loaded(h.host.get(), &FacetHost::moduleLoaded);
        QVERIFY(h.host->loadModule(QStringLiteral("early")));
        QVERIFY(navigated);
        QCOMPARE(h.controller.currentPageId(), QStringLiteral("good:general"));
        QCOMPARE(h.host->state(QStringLiteral("extender")), std::optional<State>(State::Loaded));
        QCOMPARE(loadsOf(loaded, QStringLiteral("extender")), 1);
    }

    void aPageASlotOpensDuringALoadKeepsItsUnloadedModulesPlaceholder()
    {
        // The page a slot opens belongs to a module that is not loaded, and
        // that load was refused while good loaded. Nothing loads it later on
        // its own: the page shows its placeholder, which offers the load.
        bool navigated = false;
        Harness h({QStringLiteral("good"), QStringLiteral("early")});
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        ApplicationController* const controller = &h.controller;
        connect(h.registry(), &PageRegistry::pageBound, this, [controller, &navigated] {
            if (!navigated) {
                navigated = true;
                controller->setCurrentPageId(QStringLiteral("early:main"));
            }
        });
        QVERIFY(h.host->loadModule(QStringLiteral("good")));
        QVERIFY(navigated);
        QCOMPARE(h.controller.currentPageId(), QStringLiteral("early:main"));
        QCOMPARE(h.host->state(QStringLiteral("early")), std::optional<State>(State::Available));
        QCOMPARE(h.registry()->entry(QStringLiteral("early:main")).qmlSource, FacetHost::placeholderPageSource());
        QObject* const placeholder = h.registry()->controller(QStringLiteral("early:main"));
        QVERIFY(placeholder);
        QVERIFY(placeholder->property("canRetry").toBool());
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

    void aRetryFromAFailureSlotSupersedesTheFailure()
    {
        // The first load is refused because the module registers a domain
        // the application already tracks. A slot of the Failed announcement
        // takes that domain away and retries, which loads it: the failure is
        // over before moduleFailed would have said it, so it is not said.
        FixtureState fixtures;
        bool retried = false;
        bool retryResult = false;
        QString retryError;
        Harness h({QStringLiteral("foreigndomain")});
        QVERIFY2(h.ok, qPrintable(h.error));
        auto* const tracked = new SimplePage(QStringLiteral("tracked"), &h.controller);
        QVERIFY(h.controller.registerDomain(tracked));
        qApp->setProperty("fakeForeignDomain", QVariant::fromValue<QObject*>(tracked));
        h.host->start();
        FacetHost* const host = h.host.get();
        connect(host, &FacetHost::moduleStateChanged, this,
                [host, &retried, &retryResult, &retryError](const QString& moduleId, State state) {
                    if (state == State::Failed && !retried) {
                        retried = true;
                        qApp->setProperty("fakeForeignDomain", QVariant());
                        retryResult = host->retryModule(moduleId, &retryError);
                    }
                });
        QSignalSpy failed(host, &FacetHost::moduleFailed);
        QSignalSpy loaded(host, &FacetHost::moduleLoaded);
        QString error;
        // The outcome reported is the retry's.
        QVERIFY2(h.host->loadModule(QStringLiteral("foreigndomain"), &error), qPrintable(error));
        QVERIFY(retried);
        QVERIFY2(retryResult, qPrintable(retryError));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(loaded.count(), 1);
        QCOMPARE(h.host->state(QStringLiteral("foreigndomain")), std::optional<State>(State::Loaded));
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

    void anInvalidModuleArrivingRefusesTheExtensionsAimedAtIt()
    {
        // targetsbad's extension is aimed at a module that is not installed,
        // so it is only inactive. Once that module arrives with invalid
        // metadata, it is installed but never placed, and the extension is
        // refused.
        Harness h({QStringLiteral("targetsbad")}, QString(), true);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        QVERIFY(h.host->refusedExtensions().isEmpty());
        QSignalSpy refusedChanged(h.host.get(), &FacetHost::refusedExtensionsChanged);
        QVERIFY(stage(h.modules, QStringLiteral("badmetadata"), QStringLiteral("badmetadata")));
        QTRY_COMPARE_WITH_TIMEOUT(h.host->state(QStringLiteral("badmetadata")), std::optional<State>(State::Failed),
                                  10000);
        QCOMPARE(refusedChanged.count(), 1);
        QCOMPARE(h.host->refusedExtensions().size(), 1);
        QCOMPARE(reasonOf(h.host->refusedExtensions(), QStringLiteral("targetsbad:top")),
                 QStringLiteral("its target module badmetadata is installed but has invalid metadata or could not be "
                                "placed in the sidebar"));
    }

    void extensionsThatArriveWhileThePageIsOpenLoad()
    {
        // Declared before the Harness, since the host sends notices as it is
        // destroyed.
        qsizetype topAtLastNotice = -1;
        Harness h({QStringLiteral("good")}, QString(), true);
        QVERIFY2(h.ok, qPrintable(h.error));
        h.host->start();
        h.controller.setCurrentPageId(QStringLiteral("good:general"));
        QCOMPARE(h.host->state(QStringLiteral("good")), std::optional<State>(State::Loaded));
        QSignalSpy changed(&h.controller, &ApplicationController::extensionsChanged);
        QSignalSpy refusedChanged(h.host.get(), &FacetHost::refusedExtensionsChanged);
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
        const QList<FacetHost::ModuleExtension> sections = h.host->moduleExtensions(QStringLiteral("extender"));
        QCOMPARE(sections.size(), 5);
        for (const FacetHost::ModuleExtension& section : sections) {
            QVERIFY(!h.controller.isTrackedDomain(section.domain));
        }
    }

    void modulesWritingOneFileShareItsBackend()
    {
        FixtureState fixtures;
        Harness h({QStringLiteral("sharera"), QStringLiteral("sharerb")});
        QVERIFY2(h.ok, qPrintable(h.error));
        const QString path = h.root.filePath(QStringLiteral("shared/config.json"));
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        setConfigPath(path);
        h.host->start();
        // Both load before either applies, so separate backends would each
        // hold a document without the other's key and the last write would win.
        QString error;
        QVERIFY2(h.host->loadModule(QStringLiteral("sharera"), &error), qPrintable(error));
        QVERIFY2(h.host->loadModule(QStringLiteral("sharerb"), &error), qPrintable(error));
        QVERIFY(h.host->configBackend(path));
        // The same file by another spelling is the same backend.
        QCOMPARE(h.host->configBackend(h.root.filePath(QStringLiteral("shared/../shared/config.json"))),
                 h.host->configBackend(path));
        // Made once, by the first module to open the file.
        QCOMPARE(factoryCalls().size(), 1);

        for (const QString& module : {QStringLiteral("sharera"), QStringLiteral("sharerb")}) {
            StagingDomain* const domain = configDomainOf(*h.host, module);
            QVERIFY2(domain, qPrintable(module));
            QVERIFY(invokeSetDirty(domain, true));
        }
        QVERIFY(applyAndWait(h.controller));

        const QJsonObject fake = fakeGroupIn(path);
        QCOMPARE(fake.value(QStringLiteral("sharera")).toString(), QStringLiteral("applied"));
        QCOMPARE(fake.value(QStringLiteral("sharerb")).toString(), QStringLiteral("applied"));
    }

    void everySpellingOfAFileSharesItsBackend()
    {
        // real/config.json, a link to that file, and a link to a directory
        // under real/ followed by "..", which the OS resolves to real/ and
        // a purely lexical clean-up would not.
        FixtureState fixtures;
        Harness h({QStringLiteral("sharera"), QStringLiteral("sharerb")});
        QVERIFY2(h.ok, qPrintable(h.error));
        QVERIFY(QDir().mkpath(h.root.filePath(QStringLiteral("real/sub"))));
        const QString plain = h.root.filePath(QStringLiteral("real/config.json"));
        const QString fileLink = h.root.filePath(QStringLiteral("link.json"));
        QVERIFY(QFile::link(plain, fileLink));
        QVERIFY(QFile::link(h.root.filePath(QStringLiteral("real/sub")), h.root.filePath(QStringLiteral("deep"))));
        h.host->start();

        setConfigPath(plain);
        QVERIFY(h.host->loadModule(QStringLiteral("sharera")));
        setConfigPath(fileLink);
        QVERIFY(h.host->loadModule(QStringLiteral("sharerb")));
        // The second module, opening the file through the link, got the
        // first one's backend.
        QCOMPARE(factoryCalls().size(), 1);

        PhosphorConfig::IBackend* const backend = h.host->configBackend(plain);
        QVERIFY(backend);
        QCOMPARE(h.host->configBackend(fileLink), backend);
        QCOMPARE(h.host->configBackend(h.root.filePath(QStringLiteral("deep/../config.json"))), backend);
        QCOMPARE(h.host->configBackend(h.root.path() + QStringLiteral("/real/.//config.json")), backend);
        // A relative path names no file a module opened, even one the
        // working directory would lead to.
        {
            WorkingDirectory cwd;
            QVERIFY(QDir::setCurrent(h.root.filePath(QStringLiteral("real"))));
            QVERIFY(!h.host->configBackend(QStringLiteral("config.json")));
            QVERIFY(!h.host->configBackend(QStringLiteral("./config.json")));
        }

        StagingDomain* const first = configDomainOf(*h.host, QStringLiteral("sharera"));
        StagingDomain* const second = configDomainOf(*h.host, QStringLiteral("sharerb"));
        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(invokeSetDirty(first, true));
        QVERIFY(invokeSetDirty(second, true));
        QVERIFY(applyAndWait(h.controller));
        const QJsonObject fake = fakeGroupIn(plain);
        QCOMPARE(fake.value(QStringLiteral("sharera")).toString(), QStringLiteral("applied"));
        QCOMPARE(fake.value(QStringLiteral("sharerb")).toString(), QStringLiteral("applied"));
        // Written where the link leads, which leaves the link a link.
        QVERIFY(QFileInfo(fileLink).isSymLink());
    }

    void separateFilesKeepSeparateBackends()
    {
        // The same name in another directory, and another name in the same
        // directory, are other files.
        FixtureState fixtures;
        Harness h({QStringLiteral("sharera"), QStringLiteral("sharerb")});
        QVERIFY2(h.ok, qPrintable(h.error));
        const QString first = h.root.filePath(QStringLiteral("a/config.json"));
        const QString second = h.root.filePath(QStringLiteral("b/config.json"));
        QVERIFY(QDir().mkpath(h.root.filePath(QStringLiteral("a"))));
        QVERIFY(QDir().mkpath(h.root.filePath(QStringLiteral("b"))));
        h.host->start();
        setConfigPath(first);
        QVERIFY(h.host->loadModule(QStringLiteral("sharera")));
        setConfigPath(second);
        QVERIFY(h.host->loadModule(QStringLiteral("sharerb")));
        QCOMPARE(factoryCalls().size(), 2);

        PhosphorConfig::IBackend* const firstBackend = h.host->configBackend(first);
        PhosphorConfig::IBackend* const secondBackend = h.host->configBackend(second);
        QVERIFY(firstBackend);
        QVERIFY(secondBackend);
        QVERIFY(firstBackend != secondBackend);
        // Null for a file no module opened.
        QVERIFY(!h.host->configBackend(h.root.filePath(QStringLiteral("a/other.json"))));
        QVERIFY(!h.host->configBackend(h.root.filePath(QStringLiteral("c/config.json"))));

        StagingDomain* const firstDomain = configDomainOf(*h.host, QStringLiteral("sharera"));
        StagingDomain* const secondDomain = configDomainOf(*h.host, QStringLiteral("sharerb"));
        QVERIFY(firstDomain);
        QVERIFY(secondDomain);
        QVERIFY(invokeSetDirty(firstDomain, true));
        QVERIFY(invokeSetDirty(secondDomain, true));
        QVERIFY(applyAndWait(h.controller));
        QCOMPARE(fakeGroupIn(first).keys(), QStringList{QStringLiteral("sharera")});
        QCOMPARE(fakeGroupIn(second).keys(), QStringList{QStringLiteral("sharerb")});
    }

    void aFileKeepsItsBackendWhenItsDirectoryIsCreated()
    {
        // The file is opened through a link to a directory, in a directory
        // that does not exist yet. The first Apply creates it, and a module
        // that opens the file after that still gets the same backend: two
        // would each write a document without the other's key.
        FixtureState fixtures;
        Harness h({QStringLiteral("sharera"), QStringLiteral("sharerb")});
        QVERIFY2(h.ok, qPrintable(h.error));
        QVERIFY(QDir().mkpath(h.root.filePath(QStringLiteral("real"))));
        QVERIFY(QFile::link(h.root.filePath(QStringLiteral("real")), h.root.filePath(QStringLiteral("link"))));
        const QString path = h.root.filePath(QStringLiteral("link/app/config.json"));
        QVERIFY(!QFileInfo::exists(h.root.filePath(QStringLiteral("real/app"))));
        setConfigPath(path);
        h.host->start();

        QVERIFY(h.host->loadModule(QStringLiteral("sharera")));
        PhosphorConfig::IBackend* const backend = h.host->configBackend(path);
        QVERIFY(backend);
        QCOMPARE(h.host->configBackend(h.root.path() + QStringLiteral("/link//app/../app/./config.json")), backend);
        StagingDomain* const first = configDomainOf(*h.host, QStringLiteral("sharera"));
        QVERIFY(first);
        QVERIFY(invokeSetDirty(first, true));
        QVERIFY(applyAndWait(h.controller));
        QVERIFY(QFileInfo::exists(h.root.filePath(QStringLiteral("real/app/config.json"))));
        QCOMPARE(h.host->configBackend(path), backend);

        QVERIFY(h.host->loadModule(QStringLiteral("sharerb")));
        QCOMPARE(h.host->configBackend(path), backend);
        QCOMPARE(factoryCalls().size(), 1);
        StagingDomain* const second = configDomainOf(*h.host, QStringLiteral("sharerb"));
        QVERIFY(second);
        // The later module applies first, then the earlier one: with a
        // backend each, the earlier one's write would drop the later key.
        QVERIFY(invokeSetDirty(second, true));
        QVERIFY(applyAndWait(h.controller));
        QVERIFY(invokeSetDirty(first, true));
        QVERIFY(applyAndWait(h.controller));
        const QJsonObject fake = fakeGroupIn(path);
        QCOMPARE(fake.value(QStringLiteral("sharera")).toString(), QStringLiteral("applied"));
        QCOMPARE(fake.value(QStringLiteral("sharerb")).toString(), QStringLiteral("applied"));
    }

    void aFactoryThatOpensItsOwnFileKeepsTheNestedBackend()
    {
        // confignested's factory opens the same file through its context
        // before it makes a backend of its own. The nested call's backend is
        // the one the module goes on using, so it is the one that stays.
        FixtureState fixtures;
        Harness h({QStringLiteral("confignested")});
        QVERIFY2(h.ok, qPrintable(h.error));
        const QString path = h.root.filePath(QStringLiteral("config.json"));
        setConfigPath(path);
        h.host->start();
        QString error;
        QVERIFY2(h.host->loadModule(QStringLiteral("confignested"), &error), qPrintable(error));

        PhosphorConfig::IBackend* const backend = h.host->configBackend(path);
        QVERIFY(backend);
        const quintptr nested = qApp->property("fakeConfigNestedBackend").value<quintptr>();
        QCOMPARE(reinterpret_cast<quintptr>(backend), nested);
        QCOMPARE(qApp->property("fakeConfigOuterBackend").value<quintptr>(), nested);
        // Both factory calls ran, and the backend the outer one made is gone.
        QCOMPARE(factoryCalls().size(), 2);
        QCOMPARE(backendsDestroyed(), 1);

        StagingDomain* const domain = configDomainOf(*h.host, QStringLiteral("confignested"));
        QVERIFY(domain);
        QVERIFY(invokeSetDirty(domain, true));
        QVERIFY(applyAndWait(h.controller));
        QCOMPARE(fakeGroupIn(path).value(QStringLiteral("confignested")).toString(), QStringLiteral("applied"));
    }

    void aFailedRegistrationKeepsTheBackendItOpened()
    {
        // configthenbreaks opens its file, then registers a null domain. The
        // registration is not kept, but the backend was shared the moment it
        // was opened, and the retry gets it back without a second factory
        // call.
        FixtureState fixtures;
        Harness h({QStringLiteral("configthenbreaks")});
        QVERIFY2(h.ok, qPrintable(h.error));
        const QString path = h.root.filePath(QStringLiteral("config.json"));
        setConfigPath(path);
        h.host->start();
        QString error;
        QVERIFY(!h.host->loadModule(QStringLiteral("configthenbreaks"), &error));
        QVERIFY2(error.contains(QStringLiteral("registered a null domain")), qPrintable(error));
        QCOMPARE(h.host->state(QStringLiteral("configthenbreaks")), std::optional<State>(State::Failed));
        PhosphorConfig::IBackend* const backend = h.host->configBackend(path);
        QVERIFY(backend);
        QCOMPARE(factoryCalls().size(), 1);

        QVERIFY(h.host->canRetry(QStringLiteral("configthenbreaks")));
        QVERIFY(!h.host->retryModule(QStringLiteral("configthenbreaks")));
        QCOMPARE(h.host->configBackend(path), backend);
        QCOMPARE(factoryCalls().size(), 1);
        QCOMPARE(backendsDestroyed(), 0);
    }

    void aDomainAppliesThroughItsBackendAfterTheHostIsGone()
    {
        // The backends belong to the controller, so a domain the controller
        // still applies keeps a backend to write through when the host has
        // gone, and the backends go with the controller.
        FixtureState fixtures;
        QString path;
        {
            Harness h({QStringLiteral("sharera")});
            QVERIFY2(h.ok, qPrintable(h.error));
            path = h.root.filePath(QStringLiteral("config.json"));
            setConfigPath(path);
            h.host->start();
            QVERIFY(h.host->loadModule(QStringLiteral("sharera")));
            StagingDomain* const domain = configDomainOf(*h.host, QStringLiteral("sharera"));
            QVERIFY(domain);
            QVERIFY(invokeSetDirty(domain, true));

            h.host.reset();
            QCOMPARE(backendsDestroyed(), 0);
            QVERIFY(h.controller.isDirty());
            QVERIFY(applyAndWait(h.controller));
            QCOMPARE(fakeGroupIn(path).value(QStringLiteral("sharera")).toString(), QStringLiteral("applied"));
        }
        QCOMPARE(backendsDestroyed(), 1);
    }

    void aConfigRedirectMovesEveryFileItsModulesOpen()
    {
        FixtureState fixtures;
        Harness h({QStringLiteral("sharera"), QStringLiteral("sharerb"), QStringLiteral("sharerc")});
        QVERIFY2(h.ok, qPrintable(h.error));
        const QString redirect = h.root.filePath(QStringLiteral("redirect"));
        h.host->setConfigRedirect(redirect);
        QVERIFY(QDir().mkpath(h.root.filePath(QStringLiteral("real"))));
        QVERIFY(QFile::link(h.root.filePath(QStringLiteral("real")), h.root.filePath(QStringLiteral("link"))));
        // In a directory that does not exist, which the redirect creates
        // under itself rather than here.
        const QString path = h.root.filePath(QStringLiteral("real/app/config.json"));
        const QString linked = h.root.filePath(QStringLiteral("link/app/config.json"));
        const QString other = h.root.filePath(QStringLiteral("real/other.json"));
        // The file under the redirect: its own path, links resolved, below
        // the redirect directory.
        const QString real = QFileInfo(h.root.filePath(QStringLiteral("real"))).canonicalFilePath();
        QVERIFY(!real.isEmpty());
        const QString moved = QDir::cleanPath(redirect + real + QStringLiteral("/app/config.json"));
        const QString movedOther = QDir::cleanPath(redirect + real + QStringLiteral("/other.json"));
        h.host->start();

        setConfigPath(path);
        QVERIFY(h.host->loadModule(QStringLiteral("sharera")));
        QCOMPARE(factoryCalls(), QStringList{moved});
        // Another spelling of the file shares its backend.
        setConfigPath(linked);
        QVERIFY(h.host->loadModule(QStringLiteral("sharerb")));
        QCOMPARE(factoryCalls(), QStringList{moved});
        // Another file has its own.
        setConfigPath(other);
        QVERIFY(h.host->loadModule(QStringLiteral("sharerc")));
        QCOMPARE(factoryCalls(), (QStringList{moved, movedOther}));

        // Answered for the path the module named, not where the file went.
        PhosphorConfig::IBackend* const backend = h.host->configBackend(path);
        QVERIFY(backend);
        QCOMPARE(h.host->configBackend(linked), backend);
        QVERIFY(h.host->configBackend(other));
        QVERIFY(h.host->configBackend(other) != backend);
        QVERIFY(!h.host->configBackend(moved));

        for (const QString& module :
             {QStringLiteral("sharera"), QStringLiteral("sharerb"), QStringLiteral("sharerc")}) {
            StagingDomain* const domain = configDomainOf(*h.host, module);
            QVERIFY2(domain, qPrintable(module));
            QVERIFY(invokeSetDirty(domain, true));
        }
        QVERIFY(applyAndWait(h.controller));
        const QJsonObject fake = fakeGroupIn(moved);
        QCOMPARE(fake.value(QStringLiteral("sharera")).toString(), QStringLiteral("applied"));
        QCOMPARE(fake.value(QStringLiteral("sharerb")).toString(), QStringLiteral("applied"));
        QCOMPARE(fakeGroupIn(movedOther).keys(), QStringList{QStringLiteral("sharerc")});
        // Nothing was made where the modules pointed.
        QVERIFY(!QFileInfo::exists(h.root.filePath(QStringLiteral("real/app"))));
        QVERIFY(!QFileInfo::exists(other));
    }

    void aConfigRedirectToTheRootIsRefused()
    {
        // The root would put every file back where it is, so it is refused
        // and the files stay where the modules point.
        FixtureState fixtures;
        Harness h({QStringLiteral("sharera")});
        QVERIFY2(h.ok, qPrintable(h.error));
        QTest::ignoreMessage(
            QtWarningMsg,
            exactly(
                QStringLiteral("FacetHost: setConfigRedirect to the filesystem root would redirect nothing, ignored")));
        h.host->setConfigRedirect(QStringLiteral("/"));
        const QString path = h.root.filePath(QStringLiteral("config.json"));
        setConfigPath(path);
        h.host->start();
        QVERIFY(h.host->loadModule(QStringLiteral("sharera")));
        QCOMPARE(factoryCalls(),
                 QStringList{QFileInfo(h.root.path()).canonicalFilePath() + QStringLiteral("/config.json")});
    }

    void aConfigRedirectSetAfterStartMovesNothing()
    {
        FixtureState fixtures;
        Harness h({QStringLiteral("sharera")});
        QVERIFY2(h.ok, qPrintable(h.error));
        const QString path = h.root.filePath(QStringLiteral("config.json"));
        setConfigPath(path);
        h.host->start();
        QTest::ignoreMessage(QtWarningMsg,
                             exactly(QStringLiteral("FacetHost: setConfigRedirect after start() is ignored")));
        h.host->setConfigRedirect(h.root.filePath(QStringLiteral("redirect")));
        QVERIFY(h.host->loadModule(QStringLiteral("sharera")));
        QCOMPARE(factoryCalls(),
                 QStringList{QFileInfo(h.root.path()).canonicalFilePath() + QStringLiteral("/config.json")});
        QVERIFY(!QFileInfo::exists(h.root.filePath(QStringLiteral("redirect"))));
    }

    void theExtensionProviderBelongsToItsOwner()
    {
        // The top slot's size at each notice the host sends as it goes.
        // Declared before the Harness, since those notices come from the
        // host's destructor.
        QList<qsizetype> topAtNotices;
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
        // A FacetSlot rereading the list on that notice already finds it
        // empty.
        QSignalSpy changed(&h.controller, &ApplicationController::extensionsChanged);
        ApplicationController* const controller = &h.controller;
        connect(controller, &ApplicationController::extensionsChanged, this,
                [controller, &topAtNotices](const QString& pageId) {
                    if (pageId == QLatin1String("good:general")) {
                        topAtNotices.append(
                            controller->extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).size());
                    }
                });
        h.host.reset();
        QVERIFY(h.controller.extensionsFor(QStringLiteral("good:general"), QStringLiteral("top")).isEmpty());
        QVERIFY(noticed(changed, QStringLiteral("good:general")));
        QVERIFY(!topAtNotices.isEmpty());
        QCOMPARE(topAtNotices, QList<qsizetype>(topAtNotices.size(), 0));
    }
};

QTEST_GUILESS_MAIN(TestFacetHost)
#include "test_facet_host.moc"
