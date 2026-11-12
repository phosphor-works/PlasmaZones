// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorConfig/HostDirectory.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <optional>

namespace PhosphorConfig::tests {

namespace {

HostDescriptor descriptor(const QString& id, const QStringList& desktops)
{
    HostDescriptor d;
    d.id = id;
    d.name = id;
    d.busName = QStringLiteral("org.phosphor_works.Placement");
    d.backend = QStringLiteral("json");
    d.configPath = QStringLiteral("$XDG_CONFIG_HOME/%1/config.json").arg(id);
    d.desktops = desktops;
    return d;
}

HostRecord record(const QString& host, qint64 pid)
{
    return HostRecord{host, QStringLiteral("/recorded/%1.json").arg(host), pid, QDateTime::currentDateTimeUtc()};
}

/// Sets an environment variable, or unsets it for nullopt, and puts back what
/// was there when it goes out of scope.
class ScopedEnv
{
public:
    ScopedEnv(const char* name, const std::optional<QByteArray>& value)
        : m_name(name)
    {
        if (qEnvironmentVariableIsSet(name)) {
            m_previous = qgetenv(name);
        }
        apply(value);
    }
    ~ScopedEnv()
    {
        apply(m_previous);
    }
    Q_DISABLE_COPY_MOVE(ScopedEnv)

private:
    void apply(const std::optional<QByteArray>& value) const
    {
        if (value) {
            qputenv(m_name, *value);
        } else {
            qunsetenv(m_name);
        }
    }

    const char* m_name;
    std::optional<QByteArray> m_previous;
};

QStringList* s_capturedMessages = nullptr;

void captureMessage(QtMsgType, const QMessageLogContext&, const QString& message)
{
    if (s_capturedMessages) {
        s_capturedMessages->append(message);
    }
}

/// A category filter that turns the host directory's debug output on,
/// whatever QT_LOGGING_RULES says.
void enableHostDirectoryDebug(QLoggingCategory* category)
{
    if (qstrcmp(category->categoryName(), "phosphorconfig.hostdirectory") == 0) {
        category->setEnabled(QtDebugMsg, true);
    }
}

} // namespace

class TestHostDirectory : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        QVERIFY(m_root.emplace().isValid());
        const auto setDir = [this](const char* variable, const QString& name) {
            const QString path = m_root->filePath(name);
            QDir().mkpath(path);
            qputenv(variable, path.toUtf8());
        };
        setDir("XDG_CONFIG_HOME", QStringLiteral("config"));
        setDir("XDG_DATA_HOME", QStringLiteral("data"));
        setDir("XDG_STATE_HOME", QStringLiteral("state"));
        setDir("XDG_DATA_DIRS", QStringLiteral("system"));
    }

    void parsesADescriptorAndResolvesItsPath()
    {
        QString error;
        const std::optional<HostDescriptor> parsed =
            HostDescriptor::parse(QJsonDocument::fromJson(R"({ "Id": "plasmazones", "Name": "PlasmaZones",
                "BusName": "org.phosphor_works.Placement", "Backend": "json",
                "ConfigPath": "$XDG_CONFIG_HOME/plasmazones/config.json", "Desktops": ["KDE"] })")
                                      .object(),
                                  &error);
        QVERIFY2(parsed, qPrintable(error));
        QCOMPARE(parsed->desktops, QStringList{QStringLiteral("KDE")});
        QCOMPARE(parsed->resolvedConfigPath(), m_root->filePath(QStringLiteral("config/plasmazones/config.json")));
    }

    void resolvesEveryVariable_data()
    {
        QTest::addColumn<QString>("configPath");
        QTest::addColumn<QString>("expected");
        QTest::newRow("config") << QStringLiteral("$XDG_CONFIG_HOME/a/x.json") << QStringLiteral("config/a/x.json");
        QTest::newRow("data") << QStringLiteral("$XDG_DATA_HOME/a/x.json") << QStringLiteral("data/a/x.json");
        QTest::newRow("state") << QStringLiteral("$XDG_STATE_HOME/a/x.json") << QStringLiteral("state/a/x.json");
        QTest::newRow("home") << QStringLiteral("$HOME/a/x.json") << QStringLiteral("home/a/x.json");
        // U+0020 is the first character past the control range, and it stays legal.
        QTest::newRow("space in a segment") << QStringLiteral("$HOME/a b/x.json") << QStringLiteral("home/a b/x.json");
    }

    void resolvesEveryVariable()
    {
        QFETCH(QString, configPath);
        QFETCH(QString, expected);
        const ScopedEnv home("HOME", m_root->filePath(QStringLiteral("home")).toUtf8());
        HostDescriptor host = descriptor(QStringLiteral("a"), {});
        host.configPath = configPath;
        QCOMPARE(host.resolvedConfigPath(), m_root->filePath(expected));
    }

    void refusesABadDescriptor_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("expected");
        const QByteArray good = R"("Name": "N", "BusName": "b", "Backend": "json", "ConfigPath": "$HOME/x.json")";
        QTest::newRow("bad id") << (R"({ "Id": "Bad Id", )" + good + " }") << QStringLiteral("\"Id\"");
        QTest::newRow("id with a trailing newline") << (R"({ "Id": "a\n", )" + good + " }") << QStringLiteral("\"Id\"");
        QTest::newRow("no name") << QByteArray(R"({ "Id": "a", "BusName": "b", "Backend": "json",
            "ConfigPath": "$HOME/x.json" })")
                                 << QStringLiteral("\"Name\"");
        QTest::newRow("no bus name") << QByteArray(R"({ "Id": "a", "Name": "N", "Backend": "json",
            "ConfigPath": "$HOME/x.json" })")
                                     << QStringLiteral("\"BusName\"");
        QTest::newRow("unknown backend") << QByteArray(R"({ "Id": "a", "Name": "N", "BusName": "b",
            "Backend": "ini", "ConfigPath": "$HOME/x.json" })")
                                         << QStringLiteral("ini is not a known backend");
        QTest::newRow("absolute path") << QByteArray(R"({ "Id": "a", "Name": "N", "BusName": "b",
            "Backend": "json", "ConfigPath": "/etc/x.json" })")
                                       << QStringLiteral("\"ConfigPath\"");
        QTest::newRow("parent segment") << QByteArray(R"({ "Id": "a", "Name": "N", "BusName": "b",
            "Backend": "json", "ConfigPath": "$HOME/../x.json" })")
                                        << QStringLiteral("\"ConfigPath\"");
        QTest::newRow("variable prefix only") << QByteArray(R"({ "Id": "a", "Name": "N", "BusName": "b",
            "Backend": "json", "ConfigPath": "$HOMEX/x.json" })")
                                              << QStringLiteral("\"ConfigPath\"");
        QTest::newRow("variable alone") << QByteArray(R"({ "Id": "a", "Name": "N", "BusName": "b",
            "Backend": "json", "ConfigPath": "$HOME" })")
                                        << QStringLiteral("\"ConfigPath\"");
        QTest::newRow("trailing slash") << QByteArray(R"({ "Id": "a", "Name": "N", "BusName": "b",
            "Backend": "json", "ConfigPath": "$XDG_CONFIG_HOME/" })")
                                        << QStringLiteral("\"ConfigPath\"");
        QTest::newRow("dot segment") << QByteArray(R"({ "Id": "a", "Name": "N", "BusName": "b",
            "Backend": "json", "ConfigPath": "$HOME/." })")
                                     << QStringLiteral("\"ConfigPath\"");
        QTest::newRow("dot segment inside") << QByteArray(R"({ "Id": "a", "Name": "N", "BusName": "b",
            "Backend": "json", "ConfigPath": "$HOME/./x.json" })")
                                            << QStringLiteral("\"ConfigPath\"");
        // Not a raw string: moc takes the doubled slash inside one for the
        // start of a comment and then finds no class in this file.
        QTest::newRow("empty segment") << QByteArray(
            "{ \"Id\": \"a\", \"Name\": \"N\", \"BusName\": \"b\", "
            "\"Backend\": \"json\", \"ConfigPath\": \"$HOME//x.json\" }")
                                       << QStringLiteral("\"ConfigPath\"");
        QTest::newRow("line feed in the path") << QByteArray(R"({ "Id": "a", "Name": "N", "BusName": "b",
            "Backend": "json", "ConfigPath": "$HOME/a\nb.json" })")
                                               << QStringLiteral("\"ConfigPath\"");
        QTest::newRow("last C0 control in the path") << QByteArray(R"({ "Id": "a", "Name": "N", "BusName": "b",
            "Backend": "json", "ConfigPath": "$HOME/a\u001fb.json" })")
                                                     << QStringLiteral("\"ConfigPath\"");
        QTest::newRow("delete in the path") << QByteArray(R"({ "Id": "a", "Name": "N", "BusName": "b",
            "Backend": "json", "ConfigPath": "$HOME/a\u007fb.json" })")
                                            << QStringLiteral("\"ConfigPath\"");
        QTest::newRow("desktops is not an array")
            << (R"({ "Id": "a", "Desktops": "KDE", )" + good + " }") << QStringLiteral("\"Desktops\"");
        QTest::newRow("desktops is null")
            << (R"({ "Id": "a", "Desktops": null, )" + good + " }") << QStringLiteral("\"Desktops\"");
        QTest::newRow("a desktop is not a string")
            << (R"({ "Id": "a", "Desktops": ["KDE", 3], )" + good + " }") << QStringLiteral("\"Desktops\"");
        QTest::newRow("a desktop is empty")
            << (R"({ "Id": "a", "Desktops": ["KDE", ""], )" + good + " }") << QStringLiteral("\"Desktops\"");
    }

    void refusesABadDescriptor()
    {
        QFETCH(QByteArray, json);
        QFETCH(QString, expected);
        // The row has to reach parse() as the object it spells. A typo in it
        // would otherwise parse to an empty object and fail on "Id" instead.
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
        QVERIFY2(document.isObject(), qPrintable(parseError.errorString()));
        QString error;
        QVERIFY(!HostDescriptor::parse(document.object(), &error));
        QVERIFY2(error.contains(expected), qPrintable(error));
    }

    void namesTheVariableThatIsNotAbsolute()
    {
        // A relative HOME is the environment's fault, and the error has to say
        // so rather than blame the descriptor's text.
        const ScopedEnv home("HOME", QByteArrayLiteral("relhome"));
        HostDescriptor host = descriptor(QStringLiteral("a"), {});
        host.configPath = QStringLiteral("$HOME/x.json");
        QVERIFY(host.resolvedConfigPath().isEmpty());

        QString error;
        QVERIFY(!HostDescriptor::parse(QJsonDocument::fromJson(R"({ "Id": "a", "Name": "N", "BusName": "b",
                "Backend": "json", "ConfigPath": "$HOME/x.json" })")
                                           .object(),
                                       &error));
        QVERIFY2(error.contains(QStringLiteral("$HOME expands to relhome, which is not absolute")), qPrintable(error));
    }

    void findsDescriptorsAndLetsTheUserOverride()
    {
        QVERIFY(install(QStringLiteral("system"), QStringLiteral("plasmazones.json"),
                        R"({ "Id": "plasmazones", "Name": "System", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/pz.json" })"));
        QVERIFY(install(QStringLiteral("system"), QStringLiteral("phosphor.json"),
                        R"({ "Id": "phosphor", "Name": "Phosphor", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/ph.json" })"));
        QVERIFY(install(QStringLiteral("system"), QStringLiteral("broken.json"), "{ not json"));
        QVERIFY(install(QStringLiteral("data"), QStringLiteral("plasmazones.json"),
                        R"({ "Id": "plasmazones", "Name": "User", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/pz.json" })"));

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("skipping .*broken.json")));
        const QList<HostDescriptor> found = HostDirectory(QStringLiteral("placement")).descriptors();
        QCOMPARE(found.size(), 2);
        QCOMPARE(found.at(0).id, QStringLiteral("phosphor"));
        QCOMPARE(found.at(1).id, QStringLiteral("plasmazones"));
        QCOMPARE(found.at(1).name, QStringLiteral("User"));
        QVERIFY(HostDirectory(QStringLiteral("other")).descriptors().isEmpty());
    }

    void keepsTheFirstOfTwoDescriptorsInOneDirectory()
    {
        QVERIFY(install(QStringLiteral("system"), QStringLiteral("a.json"),
                        R"({ "Id": "phosphor", "Name": "First", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/ph.json" })"));
        QVERIFY(install(QStringLiteral("system"), QStringLiteral("b.json"),
                        R"({ "Id": "phosphor", "Name": "Second", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/ph.json" })"));
        const QList<HostDescriptor> found = HostDirectory(QStringLiteral("placement")).descriptors();
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).name, QStringLiteral("First"));
    }

    void readsOnlyLowerCaseJsonFiles()
    {
        QVERIFY(install(QStringLiteral("system"), QStringLiteral("UPPER.JSON"),
                        R"({ "Id": "upper", "Name": "Upper", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/up.json" })"));
        QVERIFY(install(QStringLiteral("system"), QStringLiteral("lower.json"),
                        R"({ "Id": "lower", "Name": "Lower", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/lo.json" })"));
        const QList<HostDescriptor> found = HostDirectory(QStringLiteral("placement")).descriptors();
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).id, QStringLiteral("lower"));
    }

    void warnsAboutAnUnreadableDirectory()
    {
        QVERIFY(install(QStringLiteral("system"), QStringLiteral("hidden.json"),
                        R"({ "Id": "hidden", "Name": "Hidden", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/hidden.json" })"));
        const QString directory = m_root->filePath(QStringLiteral("system/phosphor-works/placement-hosts"));
        QFile dir(directory);
        const QFileDevice::Permissions permissions = dir.permissions();
        QVERIFY(dir.setPermissions(QFileDevice::Permissions()));
        const auto restore = qScopeGuard([&dir, permissions] {
            dir.setPermissions(permissions);
        });
        if (QFileInfo(directory).isReadable()) {
            QSKIP("this user can read a directory with no permissions (root)");
        }
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("skipping the unreadable directory")));
        QVERIFY(HostDirectory(QStringLiteral("placement")).descriptors().isEmpty());
    }

    void skipsAnOversizedDescriptor()
    {
        // Valid JSON and a valid descriptor, padded past the size limit with
        // white space, so only the limit can be what refuses it.
        QByteArray oversized = R"({ "Id": "oversized", "Name": "N", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/big.json" })";
        const QJsonDocument padded = QJsonDocument::fromJson(oversized + QByteArray(64, ' '));
        QVERIFY(padded.isObject());
        oversized.append(QByteArray(1024 * 1024, ' '));
        QVERIFY(install(QStringLiteral("system"), QStringLiteral("oversized.json"), oversized));

        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral("skipping .*oversized.json.*larger than \\d+ bytes")));
        QVERIFY(HostDirectory(QStringLiteral("placement")).descriptors().isEmpty());
    }

    void readsADataDirListedTwiceOnce()
    {
        // XDG_DATA_HOME repeated inside XDG_DATA_DIRS is the shape that lists
        // one directory twice. QStandardPaths drops a repeat within
        // XDG_DATA_DIRS itself.
        const ScopedEnv dataDirs(
            "XDG_DATA_DIRS",
            (m_root->filePath(QStringLiteral("system")) + QLatin1Char(':') + m_root->filePath(QStringLiteral("data")))
                .toUtf8());
        QVERIFY(install(QStringLiteral("data"), QStringLiteral("plasmazones.json"),
                        R"({ "Id": "plasmazones", "Name": "User", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/pz.json" })"));
        QVERIFY(install(QStringLiteral("system"), QStringLiteral("plasmazones.json"),
                        R"({ "Id": "plasmazones", "Name": "System", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/pz.json" })"));

        QStringList messages;
        s_capturedMessages = &messages;
        const QtMessageHandler previous = qInstallMessageHandler(captureMessage);
        const QLoggingCategory::CategoryFilter previousFilter =
            QLoggingCategory::installFilter(enableHostDirectoryDebug);
        const QList<HostDescriptor> found = HostDirectory(QStringLiteral("placement")).descriptors();
        QLoggingCategory::installFilter(previousFilter);
        qInstallMessageHandler(previous);
        s_capturedMessages = nullptr;

        QCOMPARE(found.size(), 1);
        QCOMPARE(found.at(0).name, QStringLiteral("User"));
        // The system file is overridden. The user's file, reached a second
        // time through XDG_DATA_DIRS, is not reported as overriding itself.
        const QStringList overridden = messages.filter(QStringLiteral("is overridden by an earlier descriptor"));
        QCOMPARE(overridden.size(), 1);
        QVERIFY2(overridden.at(0).contains(QStringLiteral("system/phosphor-works/placement-hosts/plasmazones.json")),
                 qPrintable(overridden.at(0)));
    }

    void refusesAnApiNameThatIsNotAnId()
    {
        // `../placement` would make the descriptor directory
        // `<datadir>/phosphor-works/../placement-hosts` and the record
        // `<statedir>/phosphor-works/../placement-host.json`. A descriptor
        // waits where the first would land.
        QVERIFY(QDir().mkpath(m_root->filePath(QStringLiteral("data/phosphor-works"))));
        QVERIFY(QDir().mkpath(m_root->filePath(QStringLiteral("state/phosphor-works"))));
        QVERIFY(writeFile(m_root->filePath(QStringLiteral("data/placement-hosts/plasmazones.json")),
                          R"({ "Id": "plasmazones", "Name": "Escaped", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/pz.json" })"));

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("ignoring the invalid API name")));
        const HostDirectory directory(QStringLiteral("../placement"));
        QVERIFY(directory.descriptors().isEmpty());
        QVERIFY(directory.recordPath().isEmpty());
        QVERIFY(!directory.record());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("refusing to record host")));
        QVERIFY(!directory.writeRecord(QStringLiteral("plasmazones"), QStringLiteral("/a.json"), 1));
        QVERIFY(!QFileInfo::exists(m_root->filePath(QStringLiteral("state/placement-host.json"))));
    }

    void hasNoRecordUnderARelativeStateDirectory()
    {
        // With no XDG_STATE_HOME the state directory is under HOME, and a
        // relative HOME makes it relative to wherever the process runs.
        const ScopedEnv state("XDG_STATE_HOME", std::nullopt);
        const ScopedEnv home("HOME", QByteArrayLiteral("relhome"));
        const HostDirectory directory(QStringLiteral("placement"));
        QVERIFY(directory.recordPath().isEmpty());
        QVERIFY(!directory.record());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("refusing to record host")));
        QVERIFY(!directory.writeRecord(QStringLiteral("plasmazones"), QStringLiteral("/a.json"), 1));
        QVERIFY(!QFileInfo::exists(QStringLiteral("relhome")));
    }

    void writesAndReadsTheRecord()
    {
        const HostDirectory directory(QStringLiteral("placement"));
        QCOMPARE(directory.recordPath(), m_root->filePath(QStringLiteral("state/phosphor-works/placement-host.json")));
        QVERIFY(!directory.record());
        const HostDescriptor host = descriptor(QStringLiteral("plasmazones"), {});
        QVERIFY(directory.writeRecord(host.id, host.resolvedConfigPath(), 4242));
        const std::optional<HostRecord> read = directory.record();
        QVERIFY(read);
        QCOMPARE(read->host, QStringLiteral("plasmazones"));
        QCOMPARE(read->configPath, m_root->filePath(QStringLiteral("config/plasmazones/config.json")));
        QCOMPARE(read->pid, 4242);
        QVERIFY(read->written.isValid());

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("refusing to record host")));
        QVERIFY(!directory.writeRecord(QStringLiteral("plasmazones"), QStringLiteral("relative.json"), 1));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("refusing to record host")));
        QVERIFY(!directory.writeRecord(QStringLiteral("../x"), QStringLiteral("/a.json"), 1));
        const std::optional<HostRecord> kept = directory.record();
        QVERIFY(kept);
        QCOMPARE(kept->pid, 4242);
    }

    void refusesToWriteABadRecord_data()
    {
        QTest::addColumn<QString>("hostId");
        QTest::addColumn<QString>("configPath");
        QTest::addColumn<qint64>("pid");
        QTest::newRow("id with a trailing newline")
            << QStringLiteral("plasmazones\n") << QStringLiteral("/a.json") << qint64(1);
        QTest::newRow("parent segment") << QStringLiteral("plasmazones") << QStringLiteral("/a/../b.json") << qint64(1);
        QTest::newRow("resource path") << QStringLiteral("plasmazones") << QStringLiteral(":/a.json") << qint64(1);
        QTest::newRow("negative pid") << QStringLiteral("plasmazones") << QStringLiteral("/a.json") << qint64(-1);
        QTest::newRow("line feed in the path")
            << QStringLiteral("plasmazones") << QStringLiteral("/a\nb.json") << qint64(1);
        QTest::newRow("last C0 control in the path")
            << QStringLiteral("plasmazones") << QStringLiteral("/a\u001Fb.json") << qint64(1);
        QTest::newRow("delete in the path")
            << QStringLiteral("plasmazones") << QStringLiteral("/a\u007Fb.json") << qint64(1);
    }

    void refusesToWriteABadRecord()
    {
        QFETCH(QString, hostId);
        QFETCH(QString, configPath);
        QFETCH(qint64, pid);
        const HostDirectory directory(QStringLiteral("placement"));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("refusing to record host")));
        QVERIFY(!directory.writeRecord(hostId, configPath, pid));
        QVERIFY(!QFileInfo::exists(directory.recordPath()));
    }

    void recordsACleanedPath()
    {
        const HostDirectory directory(QStringLiteral("placement"));
        QVERIFY(directory.writeRecord(QStringLiteral("plasmazones"), QStringLiteral("/a//b/./c.json"), 0));
        const std::optional<HostRecord> read = directory.record();
        QVERIFY(read);
        QCOMPARE(read->configPath, QStringLiteral("/a/b/c.json"));
        QCOMPARE(read->pid, qint64(0));
    }

    void refusesABadRecord_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("reason");
        // Each row after `not an object` is a JSON object with one thing
        // wrong, so it is refused by its own check and the reason says which.
        const QByteArray syntaxError = "{ not json";
        QJsonParseError parseError;
        QJsonDocument::fromJson(syntaxError, &parseError);
        QVERIFY(parseError.error != QJsonParseError::NoError);
        QTest::newRow("syntax error") << syntaxError << QRegularExpression::escape(parseError.errorString());
        QTest::newRow("not an object") << QByteArray("[]") << QStringLiteral("not a JSON object");
        QTest::newRow("host is not an id") << QByteArray(R"({ "Host": "../x", "ConfigPath": "/a.json", "Pid": 1,
            "Written": "2026-01-02T03:04:05Z" })")
                                           << QStringLiteral("Host must match");
        QTest::newRow("host with a trailing newline")
            << QByteArray(R"({ "Host": "a\n", "ConfigPath": "/a.json", "Pid": 1,
            "Written": "2026-01-02T03:04:05Z" })")
            << QStringLiteral("Host must match");
        QTest::newRow("relative path") << QByteArray(R"({ "Host": "a", "ConfigPath": "a.json", "Pid": 1,
            "Written": "2026-01-02T03:04:05Z" })")
                                       << QStringLiteral("ConfigPath must be an absolute path");
        QTest::newRow("resource path") << QByteArray(R"({ "Host": "a", "ConfigPath": ":/a.json", "Pid": 1,
            "Written": "2026-01-02T03:04:05Z" })")
                                       << QStringLiteral("ConfigPath must be an absolute path");
        QTest::newRow("parent segment") << QByteArray(R"({ "Host": "a", "ConfigPath": "/a/../b.json", "Pid": 1,
            "Written": "2026-01-02T03:04:05Z" })")
                                        << QStringLiteral("ConfigPath must be an absolute path");
        QTest::newRow("line feed in the path") << QByteArray(R"({ "Host": "a", "ConfigPath": "/a\nb.json", "Pid": 1,
            "Written": "2026-01-02T03:04:05Z" })")
                                               << QStringLiteral("ConfigPath must be an absolute path");
        QTest::newRow("delete in the path") << QByteArray(R"({ "Host": "a", "ConfigPath": "/a\u007fb.json", "Pid": 1,
            "Written": "2026-01-02T03:04:05Z" })")
                                            << QStringLiteral("ConfigPath must be an absolute path");
        QTest::newRow("negative pid") << QByteArray(R"({ "Host": "a", "ConfigPath": "/a.json", "Pid": -1,
            "Written": "2026-01-02T03:04:05Z" })")
                                      << QStringLiteral("Pid must be a non-negative integer");
        QTest::newRow("fractional pid") << QByteArray(R"({ "Host": "a", "ConfigPath": "/a.json", "Pid": 1.5,
            "Written": "2026-01-02T03:04:05Z" })")
                                        << QStringLiteral("Pid must be a non-negative integer");
        QTest::newRow("pid is a string") << QByteArray(R"({ "Host": "a", "ConfigPath": "/a.json", "Pid": "12",
            "Written": "2026-01-02T03:04:05Z" })")
                                         << QStringLiteral("Pid must be a non-negative integer");
        QTest::newRow("no pid") << QByteArray(R"({ "Host": "a", "ConfigPath": "/a.json",
            "Written": "2026-01-02T03:04:05Z" })")
                                << QStringLiteral("Pid must be a non-negative integer");
        QTest::newRow("no written") << QByteArray(R"({ "Host": "a", "ConfigPath": "/a.json", "Pid": 1 })")
                                    << QStringLiteral("Written must be an ISO date");
        QTest::newRow("written is not a date") << QByteArray(R"({ "Host": "a", "ConfigPath": "/a.json", "Pid": 1,
            "Written": "yesterday" })") << QStringLiteral("Written must be an ISO date");
    }

    void refusesABadRecord()
    {
        QFETCH(QByteArray, json);
        QFETCH(QString, reason);
        const HostDirectory directory(QStringLiteral("placement"));
        QVERIFY(writeFile(directory.recordPath(), json));
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral("ignoring the unreadable host record .*") + reason));
        QVERIFY(!directory.record());
    }

    void readsAHandWrittenRecord()
    {
        // The record the bad rows of refusesABadRecord start from, with
        // nothing wrong: it has to read, or those rows prove nothing.
        const HostDirectory directory(QStringLiteral("placement"));
        QVERIFY(writeFile(directory.recordPath(), R"({ "Host": "a", "ConfigPath": "/a.json", "Pid": 1,
            "Written": "2026-01-02T03:04:05Z" })"));
        const std::optional<HostRecord> read = directory.record();
        QVERIFY(read);
        QCOMPARE(read->host, QStringLiteral("a"));
        QCOMPARE(read->configPath, QStringLiteral("/a.json"));
        QCOMPARE(read->pid, qint64(1));
        QCOMPARE(read->written, QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5), QTimeZone::UTC));
    }

    void resolvesInOrder()
    {
        const HostDescriptor kde = descriptor(QStringLiteral("plasmazones"), {QStringLiteral("KDE")});
        const HostDescriptor phosphor = descriptor(QStringLiteral("phosphor"), {QStringLiteral("Phosphor")});
        const HostDescriptor alsoKde = descriptor(QStringLiteral("other"), {QStringLiteral("kde")});
        const QList<HostDescriptor> both = {kde, phosphor};
        const QList<HostDescriptor> twoOnKde = {kde, alsoKde, phosphor};
        const QStringList onKde = {QStringLiteral("KDE")};

        // 1. The running host's own record: written by the process that owns
        // the name now. It wins although its host serves another desktop.
        HostResolution r = HostDirectory::resolve(both, record(QStringLiteral("phosphor"), 7), 7, onKde);
        QVERIFY(r.host);
        QCOMPARE(r.host->id, QStringLiteral("phosphor"));
        QCOMPARE(r.configPath, QStringLiteral("/recorded/phosphor.json"));
        QVERIFY(r.running);

        // 2. A user's choice (pid 0) between two hosts of this desktop, while
        // another process owns the name.
        r = HostDirectory::resolve(twoOnKde, record(QStringLiteral("other"), 0), 9, onKde);
        QVERIFY(r.host);
        QCOMPARE(r.host->id, QStringLiteral("other"));
        QCOMPARE(r.configPath, QStringLiteral("/recorded/other.json"));
        QVERIFY(r.candidates.isEmpty());
        QVERIFY(r.running);

        // 3. A host's record, when nothing owns the name. Its path is the one
        // to edit, not the descriptor's.
        r = HostDirectory::resolve(twoOnKde, record(QStringLiteral("other"), 7), 0, onKde);
        QVERIFY(r.host);
        QCOMPARE(r.host->id, QStringLiteral("other"));
        QCOMPARE(r.configPath, QStringLiteral("/recorded/other.json"));
        QVERIFY(!r.running);

        // The same record while another process owns the name is stale, so
        // the desktop decides, and here it has two hosts to offer.
        r = HostDirectory::resolve(twoOnKde, record(QStringLiteral("other"), 7), 9, onKde);
        QVERIFY(!r.host);
        QCOMPARE(r.candidates.size(), 2);
        QVERIFY(r.running);

        // With one host for the desktop, a stale record leaves it chosen.
        r = HostDirectory::resolve(both, record(QStringLiteral("phosphor"), 7), 9, onKde);
        QVERIFY(r.host);
        QCOMPARE(r.host->id, QStringLiteral("plasmazones"));
        QCOMPARE(r.configPath, kde.resolvedConfigPath());
        QVERIFY(r.running);

        // A record for a host that is no longer installed is ignored.
        r = HostDirectory::resolve(both, record(QStringLiteral("gone"), 0), 0, onKde);
        QVERIFY(r.host);
        QCOMPARE(r.host->id, QStringLiteral("plasmazones"));

        // 4. The one descriptor for this desktop, ignoring case.
        r = HostDirectory::resolve(both, std::nullopt, 0, {QStringLiteral("phosphor")});
        QVERIFY(r.host);
        QCOMPARE(r.host->id, QStringLiteral("phosphor"));
        QVERIFY(r.candidates.isEmpty());

        // 5. Several match: nothing is chosen, and the user is asked.
        r = HostDirectory::resolve(twoOnKde, std::nullopt, 0, onKde);
        QVERIFY(!r.host);
        QVERIFY(r.configPath.isEmpty());
        QCOMPARE(r.candidates.size(), 2);

        // None match: no host, and nothing to offer.
        r = HostDirectory::resolve(both, std::nullopt, 0, {QStringLiteral("GNOME")});
        QVERIFY(!r.host);
        QVERIFY(r.candidates.isEmpty());
    }

    void keepsARecordToTheDesktopsItsHostServes()
    {
        const HostDescriptor kde = descriptor(QStringLiteral("plasmazones"), {QStringLiteral("KDE")});
        const HostDescriptor phosphor = descriptor(QStringLiteral("phosphor"), {QStringLiteral("Phosphor")});
        const HostDescriptor anywhere = descriptor(QStringLiteral("anywhere"), {});
        const QList<HostDescriptor> both = {kde, phosphor};
        const QStringList onKde = {QStringLiteral("KDE")};

        // A record left by the Phosphor host is skipped in a KDE session,
        // with nothing owning the name: the desktop decides.
        HostResolution r = HostDirectory::resolve(both, record(QStringLiteral("phosphor"), 7), 0, onKde);
        QVERIFY(r.host);
        QCOMPARE(r.host->id, QStringLiteral("plasmazones"));
        QCOMPARE(r.configPath, kde.resolvedConfigPath());
        QVERIFY(!r.running);

        // So is a user's choice of it, with and without an owner.
        r = HostDirectory::resolve(both, record(QStringLiteral("phosphor"), 0), 0, onKde);
        QVERIFY(r.host);
        QCOMPARE(r.host->id, QStringLiteral("plasmazones"));
        QCOMPARE(r.configPath, kde.resolvedConfigPath());
        r = HostDirectory::resolve(both, record(QStringLiteral("phosphor"), 0), 9, onKde);
        QVERIFY(r.host);
        QCOMPARE(r.host->id, QStringLiteral("plasmazones"));
        QCOMPARE(r.configPath, kde.resolvedConfigPath());

        // In its own session the same record wins, whatever the case of the
        // desktop's name.
        r = HostDirectory::resolve(both, record(QStringLiteral("phosphor"), 7), 0, {QStringLiteral("PHOSPHOR")});
        QVERIFY(r.host);
        QCOMPARE(r.host->id, QStringLiteral("phosphor"));
        QCOMPARE(r.configPath, QStringLiteral("/recorded/phosphor.json"));

        // A session that names no desktop skips no record.
        r = HostDirectory::resolve(both, record(QStringLiteral("phosphor"), 7), 0, {});
        QVERIFY(r.host);
        QCOMPARE(r.host->id, QStringLiteral("phosphor"));
        QCOMPARE(r.configPath, QStringLiteral("/recorded/phosphor.json"));

        // Nor is the record of a host that declares no desktops skipped. The
        // desktop match alone would have chosen the KDE host here.
        r = HostDirectory::resolve({kde, anywhere}, record(QStringLiteral("anywhere"), 7), 0, onKde);
        QVERIFY(r.host);
        QCOMPARE(r.host->id, QStringLiteral("anywhere"));
        QCOMPARE(r.configPath, QStringLiteral("/recorded/anywhere.json"));

        // Without a record that host is neither chosen nor offered.
        r = HostDirectory::resolve({anywhere}, std::nullopt, 0, onKde);
        QVERIFY(!r.host);
        QVERIFY(r.candidates.isEmpty());
    }

    void aUsersChoiceEndsTheAsking()
    {
        // The documented flow, through the record on disk: resolve() offers
        // candidates, the module records the user's answer with pid 0, and
        // the next resolve() returns that host whether or not a host runs.
        QVERIFY(install(QStringLiteral("system"), QStringLiteral("plasmazones.json"),
                        R"({ "Id": "plasmazones", "Name": "PlasmaZones", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/pz.json", "Desktops": ["KDE"] })"));
        QVERIFY(install(QStringLiteral("system"), QStringLiteral("other.json"),
                        R"({ "Id": "other", "Name": "Other", "BusName": "b", "Backend": "json",
                     "ConfigPath": "$XDG_CONFIG_HOME/other.json", "Desktops": ["KDE"] })"));
        const HostDirectory directory(QStringLiteral("placement"));
        const QList<HostDescriptor> descriptors = directory.descriptors();
        const QStringList onKde = {QStringLiteral("KDE")};

        const HostResolution asked = HostDirectory::resolve(descriptors, directory.record(), 9, onKde);
        QVERIFY(!asked.host);
        QCOMPARE(asked.candidates.size(), 2);

        const HostDescriptor answer = asked.candidates.at(0);
        QCOMPARE(answer.id, QStringLiteral("other"));
        QVERIFY(directory.writeRecord(answer.id, answer.resolvedConfigPath(), 0));

        for (const qint64 owner : {qint64(9), qint64(0)}) {
            const HostResolution chosen = HostDirectory::resolve(descriptors, directory.record(), owner, onKde);
            QVERIFY(chosen.host);
            QCOMPARE(chosen.host->id, QStringLiteral("other"));
            QCOMPARE(chosen.configPath, m_root->filePath(QStringLiteral("config/other.json")));
            QVERIFY(chosen.candidates.isEmpty());
        }
    }

    void readsTheCurrentDesktops()
    {
        const ScopedEnv desktop("XDG_CURRENT_DESKTOP", QByteArrayLiteral("KDE:Plasma::"));
        QCOMPARE(HostDirectory::currentDesktops(), (QStringList{QStringLiteral("KDE"), QStringLiteral("Plasma")}));
    }

private:
    /// Write @p bytes to @p path, creating its directory. False when any
    /// step fails, for the caller to QVERIFY: a QVERIFY in here would only
    /// leave this function.
    static bool writeFile(const QString& path, const QByteArray& bytes)
    {
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            return false;
        }
        QFile out(path);
        return out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size() && out.flush();
    }

    /// Install @p json as @p file in the placement descriptor directory of
    /// the data dir @p root (`data` for the user's, `system`).
    bool install(const QString& root, const QString& file, const QByteArray& json) const
    {
        return writeFile(m_root->filePath(root + QStringLiteral("/phosphor-works/placement-hosts/") + file), json);
    }

    std::optional<QTemporaryDir> m_root;
};

} // namespace PhosphorConfig::tests

QTEST_GUILESS_MAIN(PhosphorConfig::tests::TestHostDirectory)
#include "test_host_directory.moc"
