// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorControl/FacetTestHost.h>

#include <QTest>

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
    }

    void reportsAContractBreach()
    {
        QFETCH(QByteArray, variant);
        QFETCH(QString, expected);
        FacetTestHost host = hostFor(variant.constData());
        const QString problems = joined(host.run());
        QVERIFY2(problems.contains(expected), qPrintable(problems));
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
