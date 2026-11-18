// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_kwin_per_output_desktops.cpp
 * @brief The kwinrc reader and writer for KWin's PerOutputVirtualDesktops flag.
 *
 * Both processes ask kwinPerOutputVirtualDesktopsEnabled() the same question
 * (the daemon to gate dynamic workspaces, the settings app for the consent
 * banner), so the answer has to be KWin's own reading of kwinrc: the
 * XDG_CONFIG_DIRS cascade under the user's file, an immutable entry, and every
 * boolean spelling KConfig writes. enableKWinPerOutputVirtualDesktops() writes
 * the one key and leaves the rest of the file alone.
 *
 * Every case points XDG_CONFIG_HOME and XDG_CONFIG_DIRS at fresh temporary
 * directories, so neither the user's kwinrc nor the build machine's
 * /etc/xdg/kwinrc is ever read or written.
 */

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "common/kwinperoutputdesktops.h"

using namespace PlasmaZones;

namespace {

/// Points XDG_CONFIG_HOME and XDG_CONFIG_DIRS at two fresh directories for the
/// lifetime of the object and restores the previous values afterwards.
class KwinrcSandbox
{
public:
    KwinrcSandbox()
        : m_oldHome(qgetenv("XDG_CONFIG_HOME"))
        , m_oldDirs(qgetenv("XDG_CONFIG_DIRS"))
        , m_hadHome(qEnvironmentVariableIsSet("XDG_CONFIG_HOME"))
        , m_hadDirs(qEnvironmentVariableIsSet("XDG_CONFIG_DIRS"))
    {
        qputenv("XDG_CONFIG_HOME", m_home.path().toUtf8());
        qputenv("XDG_CONFIG_DIRS", m_system.path().toUtf8());
    }
    ~KwinrcSandbox()
    {
        if (m_hadHome) {
            qputenv("XDG_CONFIG_HOME", m_oldHome);
        } else {
            qunsetenv("XDG_CONFIG_HOME");
        }
        if (m_hadDirs) {
            qputenv("XDG_CONFIG_DIRS", m_oldDirs);
        } else {
            qunsetenv("XDG_CONFIG_DIRS");
        }
    }
    KwinrcSandbox(const KwinrcSandbox&) = delete;
    KwinrcSandbox& operator=(const KwinrcSandbox&) = delete;

    bool isValid() const
    {
        return m_home.isValid() && m_system.isValid();
    }
    QString userKwinrc() const
    {
        return m_home.filePath(kwinConfigFileName());
    }
    QString systemKwinrc() const
    {
        return m_system.filePath(kwinConfigFileName());
    }

private:
    QTemporaryDir m_home;
    QTemporaryDir m_system;
    QByteArray m_oldHome;
    QByteArray m_oldDirs;
    bool m_hadHome;
    bool m_hadDirs;
};

bool writeFile(const QString& path, const QByteArray& content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content) == content.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

} // namespace

class TestKwinPerOutputDesktops : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void sandboxIsTheConfigLocation()
    {
        KwinrcSandbox sandbox;
        QVERIFY(sandbox.isValid());
        // The reader resolves kwinrc through QStandardPaths. If the sandbox
        // did not redirect it, every case below would be reading the real
        // user's file.
        QCOMPARE(QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
                     .filePath(kwinConfigFileName()),
                 sandbox.userKwinrc());
    }

#ifdef USE_KDE_FRAMEWORKS
    void read_absentFileOrKeyIsOff()
    {
        KwinrcSandbox sandbox;
        QVERIFY(sandbox.isValid());
        QVERIFY(!kwinPerOutputVirtualDesktopsEnabled());

        QVERIFY(writeFile(sandbox.userKwinrc(), QByteArrayLiteral("[Windows]\nFocusPolicy=ClickToFocus\n")));
        QVERIFY(!kwinPerOutputVirtualDesktopsEnabled());
    }

    // KConfig's boolean spellings. A hand-rolled `== "true"` compare reads
    // every other spelling as off.
    void read_understandsEveryBooleanSpelling_data()
    {
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<bool>("expected");
        QTest::newRow("true") << QByteArrayLiteral("true") << true;
        QTest::newRow("on") << QByteArrayLiteral("on") << true;
        QTest::newRow("yes") << QByteArrayLiteral("yes") << true;
        QTest::newRow("1") << QByteArrayLiteral("1") << true;
        QTest::newRow("false") << QByteArrayLiteral("false") << false;
        QTest::newRow("off") << QByteArrayLiteral("off") << false;
        QTest::newRow("0") << QByteArrayLiteral("0") << false;
    }
    void read_understandsEveryBooleanSpelling()
    {
        QFETCH(QByteArray, value);
        QFETCH(bool, expected);
        KwinrcSandbox sandbox;
        QVERIFY(sandbox.isValid());
        QVERIFY(writeFile(sandbox.userKwinrc(),
                          QByteArrayLiteral("[Windows]\nPerOutputVirtualDesktops=") + value + QByteArrayLiteral("\n")));
        QCOMPARE(kwinPerOutputVirtualDesktopsEnabled(), expected);
    }

    // A value set only in the system kwinrc is what KWin runs with, so it is
    // what the reader must answer. The user's file still wins over it.
    void read_cascadesTheSystemFileUnderTheUsers()
    {
        KwinrcSandbox sandbox;
        QVERIFY(sandbox.isValid());
        QVERIFY(writeFile(sandbox.systemKwinrc(), QByteArrayLiteral("[Windows]\nPerOutputVirtualDesktops=true\n")));
        QVERIFY(kwinPerOutputVirtualDesktopsEnabled());

        QVERIFY(writeFile(sandbox.userKwinrc(), QByteArrayLiteral("[Windows]\nPerOutputVirtualDesktops=false\n")));
        QVERIFY(!kwinPerOutputVirtualDesktopsEnabled());
    }

    // The writer sets the key and keeps every other line of the file,
    // KConfig-only constructs included.
    void enable_writesTheKeyAndKeepsTheRest()
    {
        KwinrcSandbox sandbox;
        QVERIFY(sandbox.isValid());
        QVERIFY(writeFile(sandbox.userKwinrc(),
                          QByteArrayLiteral("[Desktops]\nNumber=3\n\n"
                                            "[Script-foo][Settings]\nkey=value\n\n"
                                            "[Windows]\nFocusPolicy=ClickToFocus\n")));
        QVERIFY(!kwinPerOutputVirtualDesktopsEnabled());

        QVERIFY(enableKWinPerOutputVirtualDesktops());
        QVERIFY(kwinPerOutputVirtualDesktopsEnabled());

        const QByteArray written = readFile(sandbox.userKwinrc());
        QVERIFY2(written.contains("PerOutputVirtualDesktops=true"), written.constData());
        QVERIFY2(written.contains("[Script-foo][Settings]"), written.constData());
        QVERIFY2(written.contains("key=value"), written.constData());
        QVERIFY2(written.contains("Number=3"), written.constData());
        QVERIFY2(written.contains("FocusPolicy=ClickToFocus"), written.constData());
    }

    // An administrator's immutable value is never overridden: nothing is
    // written to the user's file and the read keeps answering the pinned
    // value.
    void enable_refusesAnImmutableEntry()
    {
        KwinrcSandbox sandbox;
        QVERIFY(sandbox.isValid());
        QVERIFY(
            writeFile(sandbox.systemKwinrc(), QByteArrayLiteral("[Windows]\nPerOutputVirtualDesktops[$i]=false\n")));

        QVERIFY(!enableKWinPerOutputVirtualDesktops());
        QVERIFY(!kwinPerOutputVirtualDesktopsEnabled());
        QVERIFY(!readFile(sandbox.userKwinrc()).contains("PerOutputVirtualDesktops"));

        // The pinned value beats a user file that says otherwise.
        QVERIFY(writeFile(sandbox.userKwinrc(), QByteArrayLiteral("[Windows]\nPerOutputVirtualDesktops=true\n")));
        QVERIFY(!kwinPerOutputVirtualDesktopsEnabled());
    }
#else
    // The feature is compiled out: nothing is read and nothing is written,
    // whatever kwinrc says.
    void compiledOut_neitherReadsNorWrites()
    {
        KwinrcSandbox sandbox;
        QVERIFY(sandbox.isValid());
        const QByteArray content = QByteArrayLiteral("[Windows]\nPerOutputVirtualDesktops=true\n");
        QVERIFY(writeFile(sandbox.userKwinrc(), content));

        QVERIFY(!kwinPerOutputVirtualDesktopsEnabled());
        QVERIFY(!enableKWinPerOutputVirtualDesktops());
        QCOMPARE(readFile(sandbox.userKwinrc()), content);
    }
#endif
};

QTEST_GUILESS_MAIN(TestKwinPerOutputDesktops)
#include "test_kwin_per_output_desktops.moc"
