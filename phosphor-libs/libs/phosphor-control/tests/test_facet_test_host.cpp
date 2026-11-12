// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorControl/FacetTestHost.h>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

#ifndef PHOSPHOR_CONTROL_FIXTURE_DIR
#error "PHOSPHOR_CONTROL_FIXTURE_DIR must be defined by tests/CMakeLists.txt"
#endif
#ifndef PHOSPHOR_CONTROL_STAGING_DIR
#error "PHOSPHOR_CONTROL_STAGING_DIR must be defined by tests/CMakeLists.txt"
#endif

using namespace PhosphorControl;

namespace {

// A FacetTestHost for fixture @p variant, staging under the build tree.
FacetTestHost hostFor(const char* variant)
{
    FacetTestHost host(QStringLiteral(PHOSPHOR_CONTROL_FIXTURE_DIR) + QLatin1Char('/') + QLatin1String(variant)
                       + QStringLiteral(".so"));
    host.setStagingDirectory(QStringLiteral(PHOSPHOR_CONTROL_STAGING_DIR));
    return host;
}

QString joined(const QStringList& problems)
{
    return problems.join(QLatin1Char('\n'));
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

class TestFacetTestHost : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aWellBehavedModulePasses()
    {
        FacetTestHost host = hostFor("good");
        const QStringList problems = host.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));
    }

    void reportsAContractBreach_data()
    {
        QTest::addColumn<QByteArray>("variant");
        QTest::addColumn<QString>("expected");
        QTest::newRow("skips a page") << QByteArray("missingpage") << QStringLiteral("did not register page about");
        QTest::newRow("no QML") << QByteArray("noqml") << QStringLiteral("no valid QML source");
        QTest::newRow("relative QML") << QByteArray("relativeqml") << QStringLiteral("not an absolute URL");
        QTest::newRow("missing QML file") << QByteArray("missingqml") << QStringLiteral("does not exist");
        QTest::newRow("invalid metadata") << QByteArray("badmetadata") << QStringLiteral("invalid metadata");
        QTest::newRow("not a module") << QByteArray("notamodule") << QStringLiteral("IFacetModule");
        QTest::newRow("skips an extension")
            << QByteArray("missingextension") << QStringLiteral("did not register extension second");
        QTest::newRow("missing extension QML file")
            << QByteArray("extensionmissingqml")
            << QStringLiteral(
                   "extension extensionmissingqml:first's QML source qrc:/fake/MissingSection.qml does not "
                   "exist");
        QTest::newRow("page controller deleted while loading")
            << QByteArray("deletesduringload")
            << QStringLiteral("page deletesduringload:general was not bound, so Facet keeps its placeholder");
        QTest::newRow("section domain deleted while loading")
            << QByteArray("deletesduringload")
            << QStringLiteral("extension deletesduringload:first's domain was deleted while the module loaded");
    }

    void reportsAContractBreach()
    {
        QFETCH(QByteArray, variant);
        QFETCH(QString, expected);
        FacetTestHost host = hostFor(variant.constData());
        const QString problems = joined(host.run());
        QVERIFY2(problems.contains(expected), qPrintable(problems));
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
        FacetTestHost host = hostFor("sharera");
        const QStringList problems = host.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));
        // Opened, and somewhere else.
        const QStringList opened = qApp->property("fakeConfigFactoryPaths").toStringList();
        QCOMPARE(opened.size(), 1);
        QVERIFY2(opened.first() != path, qPrintable(opened.first()));
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(scratch.filePath(QStringLiteral("app"))));
    }

    void reportsAConfigFileItCannotOpen_data()
    {
        QTest::addColumn<QByteArray>("variant");
        // "@dir" stands for a scratch directory, in both columns.
        QTest::addColumn<QString>("configPath");
        QTest::addColumn<QString>("expected");
        QTest::newRow("a factory that makes no backend")
            << QByteArray("confignull") << QStringLiteral("@dir/config.json")
            << QStringLiteral("could not open config file @dir/config.json");
        QTest::newRow("a relative path") << QByteArray("relativeconfig") << QString()
                                         << QStringLiteral("relative.json, which is not an absolute path");
        QTest::newRow("a Qt resource") << QByteArray("sharera") << QStringLiteral(":/fake/config.json")
                                       << QStringLiteral(":/fake/config.json, which is a Qt resource path");
        QTest::newRow("a directory") << QByteArray("sharera") << QStringLiteral("@dir/config/")
                                     << QStringLiteral("@dir/config/, which has no file name");
        QTest::newRow("a directory named by .") << QByteArray("sharera") << QStringLiteral("@dir/config/.")
                                                << QStringLiteral("@dir/config/., which has no file name");
        QTest::newRow("a directory named by ..") << QByteArray("sharera") << QStringLiteral("@dir/config/..")
                                                 << QStringLiteral("@dir/config/.., which has no file name");
    }

    void reportsAConfigFileItCannotOpen()
    {
        QFETCH(QByteArray, variant);
        QFETCH(QString, configPath);
        QFETCH(QString, expected);
        clearConfigFixtureState();
        const auto cleanup = qScopeGuard(&clearConfigFixtureState);
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        if (!configPath.isEmpty()) {
            qputenv("FAKE_CONFIG_PATH", QFile::encodeName(configPath.replace(QLatin1String("@dir"), scratch.path())));
        }
        FacetTestHost host = hostFor(variant.constData());
        const QString problems = joined(host.run());
        const QString reason = expected.replace(QLatin1String("@dir"), scratch.path());
        QVERIFY2(problems.contains(QStringLiteral("did not load")) && problems.contains(reason), qPrintable(problems));
    }

    void anExtenderPassesWithoutItsTargets()
    {
        // None of its targets is installed here, and its sections' QML and
        // domains are checked all the same.
        FacetTestHost host = hostFor("extender");
        const QStringList problems = host.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));
    }

    void checksExtensionDomainsLikeAnyOther()
    {
        // One page, the headless domain and two sections, each answering
        // apply() twice, each named so its author can find it.
        FacetTestHost host = hostFor("extensiondouble");
        const QStringList problems = host.run();
        QCOMPARE(problems.size(), 4);
        const QString all = joined(problems);
        QVERIFY2(all.contains(QStringLiteral("extension extensiondouble:first's domain emitted applyResult 2 times")),
                 qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("extension extensiondouble:second's domain emitted applyResult 2 times")),
                 qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("FakeDomain emitted applyResult 2 times")), qPrintable(all));
    }

    void reportsADomainThatAnswersApplyTwice()
    {
        FacetTestHost host = hostFor("doubleresult");
        const QStringList problems = host.run();
        // Every page and the headless domain answer apply() twice.
        QCOMPARE(problems.size(), 4);
        for (const QString& problem : problems) {
            QVERIFY2(problem.contains(QStringLiteral("applyResult 2 times")), qPrintable(problem));
        }
    }

    void reportsADomainThatAnswersDiscardTwice()
    {
        FacetTestHost host = hostFor("doublediscard");
        const QStringList problems = host.run();
        QCOMPARE(problems.size(), 4);
        for (const QString& problem : problems) {
            QVERIFY2(problem.contains(QStringLiteral("discardResult 2 times")), qPrintable(problem));
        }
    }

    void catchesASecondResultSentFromTheEventLoop()
    {
        // The second applyResult arrives on a later event-loop turn, which
        // only waiting in an event loop can see.
        FacetTestHost host = hostFor("lateresult");
        const QStringList problems = host.run();
        QCOMPARE(problems.size(), 4);
        for (const QString& problem : problems) {
            QVERIFY2(problem.contains(QStringLiteral("applyResult 2 times")), qPrintable(problem));
        }
    }

    void reportsAFailedApply()
    {
        FacetTestHost host = hostFor("failingapply");
        const QString problems = joined(host.run());
        QVERIFY2(problems.contains(QStringLiteral("reported a failed apply(): disk full")), qPrintable(problems));
    }

    void reportsADomainThatDeletesItself()
    {
        FacetTestHost host = hostFor("selfdelete");
        const QString problems = joined(host.run());
        QVERIFY2(problems.contains(QStringLiteral("was deleted by apply()")), qPrintable(problems));
        // A headless domain is named by its objectName.
        QVERIFY2(problems.contains(QStringLiteral("FakeDomain was deleted by apply()")), qPrintable(problems));
    }

    void reportsADomainDirtyAtLoad()
    {
        FacetTestHost host = hostFor("dirtyatload");
        const QString problems = joined(host.run());
        QVERIFY2(problems.contains(QStringLiteral("FakeDomain is dirty right after loading")), qPrintable(problems));
    }

    void reportsADomainThatNeverAnswers()
    {
        FacetTestHost host = hostFor("silent");
        host.setResultTimeoutMs(100);
        const QStringList problems = host.run();
        QCOMPARE(problems.size(), 4);
        for (const QString& problem : problems) {
            QVERIFY2(problem.contains(QStringLiteral("applyResult 0 times")), qPrintable(problem));
        }
    }

    void reportsARefusedModule()
    {
        FacetTestHost host = hostFor("wrongapi");
        const QString problems = joined(host.run());
        QVERIFY2(problems.contains(QStringLiteral("refused")) && problems.contains(QStringLiteral("ApiVersion 2")),
                 qPrintable(problems));
    }

    void reportsAMissingFile()
    {
        FacetTestHost host(QStringLiteral("/nonexistent/module.so"));
        QCOMPARE(host.run(), QStringList{QStringLiteral("no module file at /nonexistent/module.so")});
    }

    void reportsAFileThatIsNotAPlugin()
    {
        // The test executable is a file, but no Facet module.
        FacetTestHost host(QCoreApplication::applicationFilePath());
        host.setStagingDirectory(QStringLiteral(PHOSPHOR_CONTROL_STAGING_DIR));
        const QString problems = joined(host.run());
        QVERIFY2(problems.contains(QStringLiteral("no Facet module found")), qPrintable(problems));
    }

    void waitsForAResultThatComesLater()
    {
        // Each domain answers once, from a timer, long after apply()
        // returned: a pass, not "0 times".
        FacetTestHost host = hostFor("slowresult");
        const QStringList problems = host.run();
        QVERIFY2(problems.isEmpty(), qPrintable(joined(problems)));
    }
};

QTEST_GUILESS_MAIN(TestFacetTestHost)
#include "test_facet_test_host.moc"
