// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorControl/ModuleTestHost.h>

#include "module_fixtures.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

#include <algorithm>

#ifndef PHOSPHOR_CONTROL_GOOD_ANCHORS
#error "PHOSPHOR_CONTROL_GOOD_ANCHORS must be defined by tests/CMakeLists.txt"
#endif
#ifndef PHOSPHOR_CONTROL_GOODNEXT_ANCHORS
#error "PHOSPHOR_CONTROL_GOODNEXT_ANCHORS must be defined by tests/CMakeLists.txt"
#endif

using namespace PhosphorControl;
using PhosphorControlTest::fixturePath;

namespace {

// Where this run's ModuleTestHosts stage, under the build tree: one directory
// per process, which initTestCase() sweeps when its process is gone.
QString stagingDirectory()
{
    return QStringLiteral(PHOSPHOR_CONTROL_STAGING_DIR "/test_module_test_host-")
        + QString::number(QCoreApplication::applicationPid());
}

// A ModuleTestHost for fixture @p variant, staging under the build tree.
ModuleTestHost hostFor(const QString& variant)
{
    ModuleTestHost host(fixturePath(variant));
    host.setStagingDirectory(stagingDirectory());
    return host;
}

QString joined(const QStringList& problems)
{
    return problems.join(QLatin1Char('\n'));
}

bool anyStartsWith(const QStringList& problems, const QString& start)
{
    return std::any_of(problems.cbegin(), problems.cend(), [&start](const QString& problem) {
        return problem.startsWith(start);
    });
}

bool writeFile(const QString& path, const QByteArray& content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
}

// What the config fixtures read and write (see fakemodule.cpp), cleared
// before and after a test that loads one.
void clearConfigFixtureState()
{
    qunsetenv("FAKE_CONFIG_PATH");
    qApp->setProperty("fakeConfigFactoryPaths", QVariant());
    qApp->setProperty("fakeConfigBackendsDestroyed", QVariant());
}

} // namespace

class TestModuleTestHost : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        PhosphorControlTest::sweepDeadStaging(QRegularExpression(QStringLiteral("^test_module_test_host-(\\d+)$")));
        QVERIFY(QDir().mkpath(stagingDirectory()));
    }

    void cleanupTestCase()
    {
        QDir(stagingDirectory()).removeRecursively();
    }

    void aWellBehavedModulePasses_data()
    {
        QTest::addColumn<QString>("variant");
        QTest::newRow("good") << QStringLiteral("good");
        // Its file is goodnext.so and its id is good: the module is found by
        // the id in its metadata.
        QTest::newRow("an id that is not its file name") << QStringLiteral("goodnext");
    }

    void aWellBehavedModulePasses()
    {
        QFETCH(QString, variant);
        ModuleTestHost host = hostFor(variant);
        const QStringList problems = host.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));
    }

    void reportsAContractBreach_data()
    {
        QTest::addColumn<QString>("variant");
        QTest::addColumn<QString>("expected");
        QTest::newRow("skips a page") << QStringLiteral("missingpage") << QStringLiteral("did not register page about");
        QTest::newRow("an undeclared page")
            << QStringLiteral("extrapage")
            << QStringLiteral("registered page undeclared, which the metadata does not declare");
        QTest::newRow("a controller with the wrong id") << QStringLiteral("wrongid") << QStringLiteral("wrong:id");
        QTest::newRow("no QML") << QStringLiteral("noqml") << QStringLiteral("no valid QML source");
        QTest::newRow("relative QML") << QStringLiteral("relativeqml") << QStringLiteral("not an absolute URL");
        QTest::newRow("missing QML file") << QStringLiteral("missingqml") << QStringLiteral("does not exist");
        QTest::newRow("invalid metadata") << QStringLiteral("badmetadata") << QStringLiteral("invalid metadata");
        QTest::newRow("not a module") << QStringLiteral("notamodule") << QStringLiteral("IControlModule");
        QTest::newRow("a library that cannot be loaded") << QStringLiteral("extenderbroken")
                                                         << fixturePath(QStringLiteral("extenderbroken"))
                + QStringLiteral(": undefined symbol: phosphor_control_test_missing_function");
        QTest::newRow("skips an extension")
            << QStringLiteral("missingextension") << QStringLiteral("did not register extension second");
        QTest::newRow("an undeclared extension")
            << QStringLiteral("extraextension")
            << QStringLiteral("registered extension undeclared, which the metadata does not declare");
        QTest::newRow("an extension with relative QML")
            << QStringLiteral("extensionrelative")
            << QStringLiteral("extension first's QML source Section.qml is not an absolute URL");
        QTest::newRow("an extension with no QML")
            << QStringLiteral("extensionnoqml") << QStringLiteral("extension first has no valid QML source");
        QTest::newRow("missing extension QML file")
            << QStringLiteral("extensionmissingqml")
            << QStringLiteral(
                   "extension extensionmissingqml:first's QML source qrc:/fake/MissingSection.qml does not "
                   "exist");
        QTest::newRow("page controller deleted while loading")
            << QStringLiteral("deletesduringload")
            << QStringLiteral("page deletesduringload:general was not bound, so the host keeps its placeholder");
        QTest::newRow("section domain deleted while loading")
            << QStringLiteral("deletesduringload")
            << QStringLiteral("extension deletesduringload:first's domain was deleted while the module loaded");
        QTest::newRow("a provider without the metadata flag")
            << QStringLiteral("undeclaredprovider")
            << QStringLiteral("registered a search provider without \"SearchProviders\": true in its metadata");
        QTest::newRow("a provider in a broken registration")
            << QStringLiteral("providersbroken") << QStringLiteral("registered a null domain");
        QTest::newRow("a null provider") << QStringLiteral("badproviders")
                                         << QStringLiteral("registered a null search provider");
        QTest::newRow("a provider registered twice")
            << QStringLiteral("badproviders") << QStringLiteral("registered a search provider twice");
    }

    void reportsAContractBreach()
    {
        QFETCH(QString, variant);
        QFETCH(QString, expected);
        ModuleTestHost host = hostFor(variant);
        const QString problems = joined(host.run());
        QVERIFY2(problems.contains(expected), qPrintable(problems));
    }

    void reportsMetadataWarnings()
    {
        // An unknown key loads, for a newer PhosphorControl's sake, and is still a
        // problem here: the module is built against this PhosphorControl.
        ModuleTestHost host = hostFor(QStringLiteral("typokey"));
        const QStringList problems = host.run();
        QCOMPARE(problems.size(), 1);
        QVERIFY2(problems.first().startsWith(QStringLiteral("metadata: "))
                     && problems.first().contains(QStringLiteral("unknown key \"Keywrods\"")),
                 qPrintable(problems.first()));
    }

    void neverWritesTheConfigFileAModuleOpens()
    {
        // sharera writes its key on apply(), which run() calls on every
        // domain. The file it opens is in a directory that does not exist,
        // and neither is made.
        clearConfigFixtureState();
        const auto cleanup = qScopeGuard(&clearConfigFixtureState);
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString path = scratch.filePath(QStringLiteral("app/config.json"));
        qputenv("FAKE_CONFIG_PATH", QFile::encodeName(path));
        ModuleTestHost host = hostFor(QStringLiteral("sharera"));
        const QStringList problems = host.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));
        // Opened under the staging directory, as the same path below its
        // redirect directory, and gone with it once run() returned.
        const QStringList opened = qApp->property("fakeConfigFactoryPaths").toStringList();
        QCOMPARE(opened.size(), 1);
        const QRegularExpression redirected(QLatin1Char('^') + QRegularExpression::escape(stagingDirectory())
                                            + QStringLiteral("/module-test-[^/]+/config")
                                            + QRegularExpression::escape(QFileInfo(path).absoluteFilePath())
                                            + QLatin1Char('$'));
        QVERIFY2(redirected.match(opened.first()).hasMatch(), qPrintable(opened.first()));
        QVERIFY(!QFileInfo::exists(opened.first()));
        QVERIFY(QDir(stagingDirectory()).isEmpty());
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(scratch.filePath(QStringLiteral("app"))));
    }

    void reportsAConfigFileItCannotOpen_data()
    {
        QTest::addColumn<QString>("variant");
        // "@dir" stands for a scratch directory, in both columns.
        QTest::addColumn<QString>("configPath");
        QTest::addColumn<QString>("expected");
        QTest::newRow("a factory that makes no backend")
            << QStringLiteral("confignull") << QStringLiteral("@dir/config.json")
            << QStringLiteral("could not open config file @dir/config.json");
        QTest::newRow("a relative path") << QStringLiteral("relativeconfig") << QString()
                                         << QStringLiteral("relative.json, which is not an absolute path");
        QTest::newRow("a Qt resource") << QStringLiteral("sharera") << QStringLiteral(":/fake/config.json")
                                       << QStringLiteral(":/fake/config.json, which is a Qt resource path");
        QTest::newRow("a directory") << QStringLiteral("sharera") << QStringLiteral("@dir/config/")
                                     << QStringLiteral("@dir/config/, which has no file name");
        QTest::newRow("a directory named by .") << QStringLiteral("sharera") << QStringLiteral("@dir/config/.")
                                                << QStringLiteral("@dir/config/., which has no file name");
        QTest::newRow("a directory named by ..") << QStringLiteral("sharera") << QStringLiteral("@dir/config/..")
                                                 << QStringLiteral("@dir/config/.., which has no file name");
    }

    void reportsAConfigFileItCannotOpen()
    {
        QFETCH(QString, variant);
        QFETCH(QString, configPath);
        QFETCH(QString, expected);
        clearConfigFixtureState();
        const auto cleanup = qScopeGuard(&clearConfigFixtureState);
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        if (!configPath.isEmpty()) {
            qputenv("FAKE_CONFIG_PATH", QFile::encodeName(configPath.replace(QLatin1String("@dir"), scratch.path())));
        }
        ModuleTestHost host = hostFor(variant);
        const QString problems = joined(host.run());
        const QString reason = expected.replace(QLatin1String("@dir"), scratch.path());
        QVERIFY2(problems.contains(QStringLiteral("did not load")) && problems.contains(reason), qPrintable(problems));
    }

    void anExtenderPassesWithoutItsTargets()
    {
        // None of its targets is installed here, and its sections' QML and
        // domains are checked all the same.
        ModuleTestHost host = hostFor(QStringLiteral("extender"));
        const QStringList problems = host.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));
    }

    void checksExtensionDomainsLikeAnyOther()
    {
        // The page, the headless domain and each section answer apply()
        // twice, and each is named so its author can find it.
        ModuleTestHost host = hostFor(QStringLiteral("extensiondouble"));
        const QStringList problems = host.run();
        QCOMPARE(problems.size(), 4);
        const QString all = joined(problems);
        for (const QString& name :
             {QStringLiteral("page extensiondouble:general"),
              QStringLiteral("extension extensiondouble:first's domain"),
              QStringLiteral("extension extensiondouble:second's domain"), QStringLiteral("FakeDomain")}) {
            QVERIFY2(all.contains(name + QStringLiteral(" emitted applyResult 2 times")), qPrintable(all));
        }
    }

    void checksSearchProviders()
    {
        ModuleTestHost providers = hostFor(QStringLiteral("providers"));
        QStringList problems = providers.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));

        ModuleTestHost stray = hostFor(QStringLiteral("strayprovider"));
        problems = stray.run();
        QCOMPARE(problems.size(), 1);
        QVERIFY2(problems.first().contains(QStringLiteral("elsewhere:page")), qPrintable(problems.first()));

        // Of its entries only the setting at an anchor its own page does not
        // list is wrong. Those on the page it extends cannot be checked
        // without that page's module, the action has no page to check, and
        // the entity needs no anchor.
        ModuleTestHost checks = hostFor(QStringLiteral("providerchecks"));
        problems = checks.run();
        QCOMPARE(problems.size(), 1);
        QVERIFY2(problems.first().contains(QStringLiteral("(providerchecks:general#nope) names an anchor its page")),
                 qPrintable(problems.first()));

        // With that module, the setting at an anchor the page does not list
        // is wrong too, and so is the section at the anchor of an extension
        // the module does not have. The section at one the page lists and
        // the one at the anchor of the module's own section there are not.
        ModuleTestHost withTarget = hostFor(QStringLiteral("providerchecks"));
        withTarget.addTargetModule(fixturePath(QStringLiteral("good")));
        problems = withTarget.run();
        QCOMPARE(problems.size(), 3);
        QVERIFY2(problems.at(1).contains(
                     QStringLiteral("(good:general#gone) names an anchor page good:general does not list")),
                 qPrintable(problems.at(1)));
        QVERIFY2(problems.at(2).contains(QStringLiteral(
                     "(good:general#providerchecks:missing) names an anchor page good:general does not list")),
                 qPrintable(problems.at(2)));
        QVERIFY2(!joined(problems).contains(QStringLiteral("providerchecks:first")), qPrintable(joined(problems)));

        ModuleTestHost none = hostFor(QStringLiteral("providersnone"));
        problems = none.run();
        QCOMPARE(problems,
                 QStringList{QStringLiteral(
                     "its metadata says \"SearchProviders\": true, but it registered no search provider")});
    }

    void checksExtensionsAgainstTheirTargets()
    {
        ModuleTestHost fits = hostFor(QStringLiteral("extender"));
        fits.addTargetModule(fixturePath(QStringLiteral("good")));
        QStringList problems = fits.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));

        ModuleTestHost misfits = hostFor(QStringLiteral("badextender"));
        misfits.addTargetModule(fixturePath(QStringLiteral("good")));
        problems = misfits.run();
        QStringList refused;
        for (const QString& problem : std::as_const(problems)) {
            QVERIFY2(problem.startsWith(QStringLiteral("extension "))
                         && problem.contains(QStringLiteral(" is refused: ")),
                     qPrintable(problem));
            refused.append(problem.section(QLatin1Char(' '), 1, 1));
        }
        std::sort(refused.begin(), refused.end());
        QCOMPARE(refused,
                 (QStringList{QStringLiteral("badextender:new-anchors"), QStringLiteral("badextender:no-anchor"),
                              QStringLiteral("badextender:no-page"), QStringLiteral("badextender:no-point"),
                              QStringLiteral("badextender:old-panel")}));

        // A target's own refused extensions are its author's business.
        ModuleTestHost target = hostFor(QStringLiteral("good"));
        target.addTargetModule(fixturePath(QStringLiteral("badextender")));
        problems = target.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));
    }

    void checksAnchorsAgainstThePreviousRelease_data()
    {
        QTest::addColumn<QByteArray>("json");
        // Each must be in one of the problems, and there are exactly as many
        // problems.
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("unchanged") << QByteArray(R"({ "Module": "good", "Pages": {
            "general": { "Version": 1, "Anchors": ["speed"], "ExtensionPoints": { "panel": 2 } } } })")
                                   << QStringList();
        QTest::newRow("an anchor added at the same version")
            << QByteArray(R"({ "Module": "good", "Pages": { "general": { "Version": 1, "Anchors": [] } } })")
            << QStringList();
        // A page can never go, whatever its version. An anchor can go with a
        // bump of its page's anchors version.
        QTest::newRow("a page gone, and an anchor gone with no version bump")
            << QByteArray(R"({ "Module": "good", "Pages": {
            "general": { "Version": 1, "Anchors": ["speed", "gone"] }, "removed": { "Version": 1, "Anchors": [] } } })")
            << QStringList{QStringLiteral("anchor gone of page general is gone"),
                           QStringLiteral("page removed of the previous "
                                          "release is gone")};
        // Its anchors are not compared: the version says they are not the
        // same anchors.
        QTest::newRow("the anchors version went down")
            << QByteArray(R"({ "Module": "good", "Pages": { "general": { "Version": 3, "Anchors": ["gone"] } } })")
            << QStringList{QStringLiteral("page general's anchors version went down from 3 to 1")};
        QTest::newRow("a named point gone and one whose version went down")
            << QByteArray(R"({ "Module": "good", "Pages": {
            "general": { "Version": 1, "Anchors": [], "ExtensionPoints": { "panel": 3, "vanished": 1 } } } })")
            << QStringList{QStringLiteral("extension point panel of page general went down from version 3 to 2"),
                           QStringLiteral("extension point vanished of page general is gone")};
        QTest::newRow("a named point whose version went up") << QByteArray(R"({ "Module": "good", "Pages": {
            "general": { "Version": 1, "Anchors": [], "ExtensionPoints": { "panel": 1 } } } })")
                                                             << QStringList();
        QTest::newRow("another module's file") << QByteArray(R"({ "Module": "other", "Pages": {} })")
                                               << QStringList{QStringLiteral("are for module other, not good")};
        QTest::newRow("malformed JSON") << QByteArray(R"({ "Module": )")
                                        << QStringList{QStringLiteral("are not valid JSON")};
        QTest::newRow("not an object") << QByteArray("[]") << QStringList{QStringLiteral("are not a JSON object")};
    }

    void checksAnchorsAgainstThePreviousRelease()
    {
        QFETCH(QByteArray, json);
        QFETCH(QStringList, expected);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("anchors.json"));
        QVERIFY(writeFile(path, json));
        ModuleTestHost host = hostFor(QStringLiteral("good"));
        host.setPreviousAnchors(path);
        const QStringList problems = host.run();
        QCOMPARE(problems.size(), expected.size());
        for (const QString& problem : std::as_const(expected)) {
            QVERIFY2(joined(problems).contains(problem), qPrintable(joined(problems)));
        }
    }

    void reportsPreviousAnchorsItCannotRead()
    {
        ModuleTestHost host = hostFor(QStringLiteral("good"));
        host.setPreviousAnchors(QStringLiteral("/nonexistent/anchors.json"));
        const QStringList problems = host.run();
        QCOMPARE(problems.size(), 1);
        QVERIFY2(
            problems.first().startsWith(QStringLiteral("cannot read the previous anchors /nonexistent/anchors.json")),
            qPrintable(problems.first()));
    }

    void checksAReleaseAgainstTheAnchorsTheToolWrote()
    {
        // The anchors.json phosphor_add_control_module_metadata generated for
        // good and for its next release, goodnext, which renames an anchor
        // and raises the page's anchors version for it.
        ModuleTestHost same = hostFor(QStringLiteral("good"));
        same.setPreviousAnchors(QStringLiteral(PHOSPHOR_CONTROL_GOOD_ANCHORS));
        QStringList problems = same.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));

        ModuleTestHost next = hostFor(QStringLiteral("goodnext"));
        next.setPreviousAnchors(QStringLiteral(PHOSPHOR_CONTROL_GOOD_ANCHORS));
        problems = next.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));

        ModuleTestHost back = hostFor(QStringLiteral("good"));
        back.setPreviousAnchors(QStringLiteral(PHOSPHOR_CONTROL_GOODNEXT_ANCHORS));
        problems = back.run();
        QCOMPARE(problems, QStringList{QStringLiteral("page general's anchors version went down from 2 to 1")});
    }

    void reportsEveryDomainThatAnswersWrongly_data()
    {
        QTest::addColumn<QString>("variant");
        QTest::addColumn<int>("timeoutMs");
        QTest::addColumn<QString>("expected");
        QTest::newRow("apply answered twice")
            << QStringLiteral("doubleresult") << 0 << QStringLiteral("applyResult 2 times");
        QTest::newRow("discard answered twice")
            << QStringLiteral("doublediscard") << 0 << QStringLiteral("discardResult 2 times");
        // The second applyResult arrives on a later event-loop turn, which
        // only waiting in an event loop can see.
        QTest::newRow("a second answer from the event loop")
            << QStringLiteral("lateresult") << 0 << QStringLiteral("applyResult 2 times");
        QTest::newRow("no answer") << QStringLiteral("silent") << 100 << QStringLiteral("applyResult 0 times");
    }

    void reportsEveryDomainThatAnswersWrongly()
    {
        QFETCH(QString, variant);
        QFETCH(int, timeoutMs);
        QFETCH(QString, expected);
        ModuleTestHost host = hostFor(variant);
        if (timeoutMs > 0) {
            host.setResultTimeoutMs(timeoutMs);
        }
        const QStringList problems = host.run();
        // Every page and the headless domain.
        QCOMPARE(problems.size(), 4);
        for (const QString& problem : problems) {
            QVERIFY2(problem.contains(expected), qPrintable(problem));
        }
    }

    void reportsAFailedApply()
    {
        ModuleTestHost host = hostFor(QStringLiteral("failingapply"));
        const QString problems = joined(host.run());
        QVERIFY2(problems.contains(QStringLiteral("reported a failed apply(): disk full")), qPrintable(problems));
    }

    void reportsADomainThatDeletesItself()
    {
        ModuleTestHost host = hostFor(QStringLiteral("selfdelete"));
        const QStringList problems = host.run();
        // Every page and the headless domain.
        QCOMPARE(problems.size(), 4);
        for (const QString& problem : problems) {
            QVERIFY2(problem.endsWith(QStringLiteral(" was deleted by apply()")), qPrintable(problem));
        }
        QVERIFY2(problems.contains(QStringLiteral("page selfdelete:general was deleted by apply()")),
                 qPrintable(joined(problems)));
        // A headless domain is named by its objectName.
        QVERIFY2(problems.contains(QStringLiteral("FakeDomain was deleted by apply()")), qPrintable(joined(problems)));
    }

    void reportsADomainDirtyAtLoad()
    {
        ModuleTestHost host = hostFor(QStringLiteral("dirtyatload"));
        const QStringList problems = host.run();
        QCOMPARE(problems.size(), 4);
        for (const QString& problem : problems) {
            QVERIFY2(problem.endsWith(QStringLiteral(" is dirty right after loading")), qPrintable(problem));
        }
        QVERIFY2(problems.contains(QStringLiteral("page dirtyatload:general is dirty right after loading")),
                 qPrintable(joined(problems)));
        QVERIFY2(problems.contains(QStringLiteral("FakeDomain is dirty right after loading")),
                 qPrintable(joined(problems)));
    }

    void reportsABrokenDiscard()
    {
        // Every page and the headless domain report a failed discard() and
        // delete themselves in it. The headless domain has no objectName, so
        // it goes by its class and its place.
        ModuleTestHost host = hostFor(QStringLiteral("discardbroken"));
        const QStringList problems = host.run();
        QCOMPARE(problems.size(), 8);
        for (const QString& name :
             {QStringLiteral("page discardbroken:general"), QStringLiteral("page discardbroken:general/advanced"),
              QStringLiteral("page discardbroken:about"), QStringLiteral("FakeDomain (headless domain 1)")}) {
            QVERIFY2(problems.contains(name + QStringLiteral(" reported a failed discard(): locked")),
                     qPrintable(joined(problems)));
            QVERIFY2(problems.contains(name + QStringLiteral(" was deleted by discard()")),
                     qPrintable(joined(problems)));
        }
    }

    void seesWhatLoadingDeletesLater()
    {
        // The first page's controller and the first section's domain are
        // deleted with deleteLater() while the module loads, and the
        // headless domain's apply() deletes the last section's domain.
        ModuleTestHost host = hostFor(QStringLiteral("deleteslater"));
        const QStringList problems = host.run();
        QCOMPARE(
            problems,
            (QStringList{
                QStringLiteral("page deleteslater:general's controller was deleted after it was bound"),
                QStringLiteral("extension deleteslater:first's domain was deleted while the module loaded"),
                QStringLiteral("extension deleteslater:second's domain was deleted before its apply() was called")}));
    }

    void reportsAHeadlessDomainDeletedWhileLoading()
    {
        // The second headless domain is deleted while the host registers
        // them, and the third with deleteLater() once registered. The host
        // never applies either, so both are reported, by their place.
        ModuleTestHost host = hostFor(QStringLiteral("headlessgone"));
        QCOMPARE(host.run(),
                 (QStringList{QStringLiteral("headless domain 2 was deleted while the module loaded, or the "
                                             "application refused it"),
                              QStringLiteral("headless domain 3 was deleted while the module loaded, or the "
                                             "application refused it")}));
    }

    void reportsARefusedModule()
    {
        ModuleTestHost host = hostFor(QStringLiteral("wrongapi"));
        const QStringList problems = host.run();
        QCOMPARE(problems.size(), 1);
        QVERIFY2(problems.first().startsWith(fixturePath(QStringLiteral("wrongapi")) + QStringLiteral(": refused: "))
                     && problems.first().contains(QStringLiteral("ApiVersion 2")),
                 qPrintable(problems.first()));
    }

    void reportsAMissingFile()
    {
        ModuleTestHost host(QStringLiteral("/nonexistent/module.so"));
        QCOMPARE(host.run(), QStringList{QStringLiteral("no module file at /nonexistent/module.so")});

        ModuleTestHost target = hostFor(QStringLiteral("good"));
        target.addTargetModule(QStringLiteral("/nonexistent/target.so"));
        QCOMPARE(target.run(), QStringList{QStringLiteral("no target module file at /nonexistent/target.so")});
    }

    void reportsAFileThatIsNotAPlugin_data()
    {
        QTest::addColumn<bool>("soFile");
        // Staged as a .so file like any other, so the catalog reads it.
        QTest::newRow("an executable") << false;
        QTest::newRow("a .so file") << true;
    }

    void reportsAFileThatIsNotAPlugin()
    {
        QFETCH(bool, soFile);
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        QString path = QCoreApplication::applicationFilePath();
        if (soFile) {
            path = scratch.filePath(QStringLiteral("junk.so"));
            QVERIFY(writeFile(path, QByteArrayLiteral("not a library")));
        }
        ModuleTestHost host(path);
        host.setStagingDirectory(stagingDirectory());
        QCOMPARE(host.run(), QStringList{path + QStringLiteral(": refused: not a Qt plugin")});
    }

    void reportsARefusedTargetByItsPath()
    {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString junk = scratch.filePath(QStringLiteral("junk.so"));
        QVERIFY(writeFile(junk, QByteArrayLiteral("not a library")));
        const QString wrongApi = fixturePath(QStringLiteral("wrongapi"));

        // The module passes, and each refused target is named.
        ModuleTestHost extender = hostFor(QStringLiteral("extender"));
        extender.addTargetModule(junk);
        extender.addTargetModule(wrongApi);
        QStringList problems = extender.run();
        QCOMPARE(problems.size(), 2);
        QVERIFY2(problems.contains(junk + QStringLiteral(": refused: not a Qt plugin")), qPrintable(joined(problems)));
        QVERIFY2(anyStartsWith(problems, wrongApi + QStringLiteral(": refused: ")), qPrintable(joined(problems)));

        // A refused target does not hide that the module was refused too.
        ModuleTestHost refused(junk);
        refused.setStagingDirectory(stagingDirectory());
        refused.addTargetModule(wrongApi);
        problems = refused.run();
        QCOMPARE(problems.size(), 2);
        QVERIFY2(problems.contains(junk + QStringLiteral(": refused: not a Qt plugin")), qPrintable(joined(problems)));
        QVERIFY2(anyStartsWith(problems, wrongApi + QStringLiteral(": refused: ")), qPrintable(joined(problems)));
    }

    void refusesATargetThatSharesAnId()
    {
        const QString good = fixturePath(QStringLiteral("good"));
        const QString goodNext = fixturePath(QStringLiteral("goodnext"));

        // goodnext is a build of good: it would stand in for the module
        // under test.
        ModuleTestHost self = hostFor(QStringLiteral("good"));
        self.addTargetModule(goodNext);
        QCOMPARE(self.run(),
                 QStringList{goodNext
                             + QStringLiteral(": not used as a target: it has the id good of the module "
                                              "under test")});

        ModuleTestHost twins = hostFor(QStringLiteral("extender"));
        twins.addTargetModule(good);
        twins.addTargetModule(goodNext);
        QCOMPARE(
            twins.run(),
            QStringList{goodNext + QStringLiteral(": not used as a target: target %1 has its id good too").arg(good)});
    }

    void stagesTargetsThatShareAFileName()
    {
        // Another module in a file named like the first target's.
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString renamed = scratch.filePath(QStringLiteral("good.so"));
        QVERIFY(QFile::copy(fixturePath(QStringLiteral("extender2")), renamed));
        ModuleTestHost host = hostFor(QStringLiteral("extender"));
        host.addTargetModule(fixturePath(QStringLiteral("good")));
        host.addTargetModule(renamed);
        const QStringList problems = host.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));
    }

    void acceptsRelativePaths()
    {
        const QString current = QDir::currentPath();
        const auto restore = qScopeGuard([&current]() {
            QDir::setCurrent(current);
        });
        QVERIFY(QDir::setCurrent(QStringLiteral(PHOSPHOR_CONTROL_FIXTURE_DIR)));
        ModuleTestHost host(QStringLiteral("good.so"));
        host.setStagingDirectory(QDir::current().relativeFilePath(stagingDirectory()));
        host.addTargetModule(QStringLiteral("extender.so"));
        const QStringList problems = host.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));
    }

    void reportsAStagingDirectoryItCannotMake()
    {
        // Under a file, where no directory can be made.
        const QString under = QCoreApplication::applicationFilePath() + QStringLiteral("/staging");
        ModuleTestHost host = hostFor(QStringLiteral("good"));
        host.setStagingDirectory(under);
        QCOMPARE(host.run(), QStringList{QStringLiteral("cannot create a temporary directory under %1").arg(under)});
    }

    void waitsForAResultThatComesLater_data()
    {
        QTest::addColumn<int>("timeoutMs");
        QTest::newRow("the default timeout") << ModuleTestHost::DefaultResultTimeoutMs;
        // Restores the default rather than waiting for nothing.
        QTest::newRow("zero") << 0;
        QTest::newRow("negative") << -1;
    }

    void waitsForAResultThatComesLater()
    {
        // Each domain answers once, from a timer, long after apply()
        // returned: a pass, not "0 times".
        QFETCH(int, timeoutMs);
        ModuleTestHost host = hostFor(QStringLiteral("slowresult"));
        host.setResultTimeoutMs(timeoutMs);
        const QStringList problems = host.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));
    }
};

QTEST_GUILESS_MAIN(TestModuleTestHost)
#include "test_module_test_host.moc"
