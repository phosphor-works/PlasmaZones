// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test_plugin_helpers.h"

#include <PhosphorRegistry/IBarWidgetPlugin.h>
#include <PhosphorRegistry/PluginCatalog.h>
#include <PhosphorRegistry/PluginLoader.h>

#include <QCoreApplication>
#include <QFile>
#include <QLibraryInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVersionNumber>

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

using namespace PhosphorRegistry;
using namespace PhosphorRegistryTestHelpers;

class TestPluginCatalog : public QObject
{
    Q_OBJECT

private:
    // A catalog over the bar-widget seam with the given search paths, and
    // live reload off so every scan is an explicit call.
    static std::unique_ptr<PluginCatalog> makeCatalog(const QList<PluginSearchPath>& paths)
    {
        auto catalog = std::make_unique<PluginCatalog>(PluginLoader::barWidgetSeam());
        catalog->setSearchPaths(paths);
        catalog->setLiveReload(false);
        return catalog;
    }

    static QString reasonFor(const PluginCatalog& catalog, const QString& filePath)
    {
        const QList<RefusedPlugin> refused = catalog.refused();
        for (const RefusedPlugin& entry : refused) {
            if (entry.filePath == filePath) {
                return entry.reason;
            }
        }
        return QString();
    }

    static QString canonical(const QString& path)
    {
        return QFileInfo(path).canonicalFilePath();
    }

    static QString filePathOf(const PluginCatalog& catalog, const QString& id)
    {
        const auto metadata = catalog.plugin(id);
        return metadata ? metadata->filePath : QString();
    }

private Q_SLOTS:
    void offersAPluginFromItsMetadataWithoutLoadingIt()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.filePath(QStringLiteral("dev")), QStringLiteral("fake-plugin"));
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.filePath(QStringLiteral("dev")), PluginOrigin::Development}});

        QSignalSpy added(catalog.get(), &PluginCatalog::pluginAdded);
        catalog->scan();

        QCOMPARE(added.count(), 1);
        QCOMPARE(added.first().first().toString(), QStringLiteral("fake-plugin"));
        const auto metadata = catalog->plugin(QStringLiteral("fake-plugin"));
        QVERIFY(metadata.has_value());
        QCOMPARE(metadata->filePath, path);
        QCOMPARE(metadata->origin, PluginOrigin::Development);
        QCOMPARE(metadata->metaData.value(QLatin1String("Id")).toString(), QStringLiteral("fake-plugin"));
        QVERIFY(!catalog->isLoaded(QStringLiteral("fake-plugin")));
        QVERIFY(!isMapped(canonical(path)));
        QVERIFY(catalog->refused().isEmpty());
    }

    void loadReturnsTheRootObjectOnceAndCachesIt()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), QStringLiteral("fake-plugin"));
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        catalog->scan();

        QStringList warnings;
        QStringList infos;
        QString error;
        QObject* first = nullptr;
        QObject* second = nullptr;
        {
            MessageCapture capture(warnings, &infos);
            first = catalog->load(QStringLiteral("fake-plugin"), &error);
            second = catalog->load(QStringLiteral("fake-plugin"));
        }
        QVERIFY2(first, qPrintable(error));
        QCOMPARE(second, first);
        QVERIFY(catalog->isLoaded(QStringLiteral("fake-plugin")));
        QVERIFY(isMapped(canonical(path)));
        // The library is mapped once: the second call is served from the cache.
        QCOMPARE(countContaining(infos, QStringLiteral("loaded")), 1);
    }

    void loadOfAnUnknownIdFails()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        catalog->scan();

        QString error;
        QCOMPARE(catalog->load(QStringLiteral("nothing-here"), &error), nullptr);
        QVERIFY(error.contains(QStringLiteral("nothing-here")));
    }

    void anEarlierSearchPathOverridesALaterOneAndSaysSo()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString dev = root.filePath(QStringLiteral("dev"));
        const QString user = root.filePath(QStringLiteral("user"));
        const QString devCopy = stagePlugin(dev, QStringLiteral("fake-plugin"));
        const QString userCopy = stagePlugin(user, QStringLiteral("fake-plugin"));
        QVERIFY(!devCopy.isEmpty() && !userCopy.isEmpty());
        auto catalog = makeCatalog({{dev, PluginOrigin::Development}, {user, PluginOrigin::User}});

        QStringList warnings;
        QStringList infos;
        {
            MessageCapture capture(warnings, &infos);
            catalog->scan();
        }

        QCOMPARE(catalog->plugins().size(), 1);
        const auto winner = catalog->plugin(QStringLiteral("fake-plugin"));
        QVERIFY(winner.has_value());
        QCOMPARE(winner->filePath, devCopy);
        QCOMPARE(winner->origin, PluginOrigin::Development);
        QCOMPARE(countContaining(infos, QStringLiteral("overrides")), 1);
        QCOMPARE(countContaining(infos, userCopy), 1);
        QVERIFY(catalog->refused().isEmpty());
    }

    void refusesAPluginWithoutTheSeamsApiVersion_data()
    {
        QTest::addColumn<QString>("variant");
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("expected");
        QTest::newRow("another version") << QStringLiteral("fake-plugin-wrongapi")
                                         << QStringLiteral("fake-plugin-wrongapi")
                                         << QStringLiteral("built for ApiVersion 2, this build loads 1");
        QTest::newRow("missing") << QStringLiteral("fake-plugin-noapi") << QStringLiteral("fake-plugin-noapi")
                                 << QStringLiteral("no integer \"ApiVersion\"");
        QTest::newRow("a string") << QStringLiteral("fake-plugin-stringapi") << QStringLiteral("fake-plugin-stringapi")
                                  << QStringLiteral("no integer \"ApiVersion\"");
    }

    void refusesAPluginWithoutTheSeamsApiVersion()
    {
        QFETCH(QString, variant);
        QFETCH(QString, id);
        QFETCH(QString, expected);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), variant);
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }

        QVERIFY(catalog->plugins().isEmpty());
        const QString reason = reasonFor(*catalog, path);
        QVERIFY2(reason.contains(expected), qPrintable(reason));
        QCOMPARE(catalog->refused().size(), 1);
        QCOMPARE(catalog->refused().first().id, id);
        QCOMPARE(countContaining(warnings, path), 1);
        // Refused from its metadata alone: the library was never mapped.
        QVERIFY(!isMapped(canonical(path)));
    }

    void refusesAPluginOfAnotherSeam()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), QStringLiteral("fake-plugin-wrongiid"));
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }

        QVERIFY(catalog->plugins().isEmpty());
        QVERIFY(reasonFor(*catalog, path).contains(QStringLiteral("org.phosphor_works.Test.NotABarWidget/1")));
    }

    void refusesAPluginBuiltAgainstAnIncompatibleQt_data()
    {
        QTest::addColumn<int>("major");
        QTest::addColumn<int>("minor");
        QTest::addColumn<bool>("offered");
        // The catalog compares with the Qt this process runs, which can be
        // newer than the one these tests were built against.
        const QVersionNumber running = QLibraryInfo::version();
        QTest::newRow("a newer minor version") << running.majorVersion() << running.minorVersion() + 1 << false;
        QTest::newRow("another major version") << running.majorVersion() + 1 << 0 << false;
        if (running.minorVersion() > 0) {
            QTest::newRow("an older minor version") << running.majorVersion() << running.minorVersion() - 1 << true;
        }
    }

    void refusesAPluginBuiltAgainstAnIncompatibleQt()
    {
        QFETCH(int, major);
        QFETCH(int, minor);
        QFETCH(bool, offered);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePluginBuiltAgainstQt(root.path(), QStringLiteral("fake-plugin"), major, minor);
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }

        QCOMPARE(catalog->plugin(QStringLiteral("fake-plugin")).has_value(), offered);
        if (!offered) {
            const QString reason = reasonFor(*catalog, path);
            QVERIFY2(reason.contains(QStringLiteral("built against Qt %1.%2").arg(major).arg(minor)),
                     qPrintable(reason));
        }
    }

    void refusesMetadataWithoutAValidId_data()
    {
        QTest::addColumn<QString>("variant");
        QTest::newRow("missing") << QStringLiteral("fake-plugin-noid");
        QTest::newRow("path characters") << QStringLiteral("fake-plugin-badid");
        QTest::newRow("trailing newline") << QStringLiteral("fake-plugin-newline");
    }

    void refusesMetadataWithoutAValidId()
    {
        QFETCH(QString, variant);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), variant);
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }

        QVERIFY(catalog->plugins().isEmpty());
        QVERIFY(reasonFor(*catalog, path).contains(QStringLiteral("no valid \"Id\"")));
    }

    void refusesALibraryThatIsNotAQtPlugin_data()
    {
        QTest::addColumn<bool>("garbage");
        QTest::newRow("plain shared library") << false;
        QTest::newRow("not a library at all") << true;
    }

    void refusesALibraryThatIsNotAQtPlugin()
    {
        QFETCH(bool, garbage);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(makeOwnerWritable(root.path()));
        const QString path = garbage
            ? writePlugin(root.filePath(QStringLiteral("garbage.so")), QByteArrayLiteral("this is not an ELF file"))
            : stagePlugin(root.path(), QStringLiteral("plain"));
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }

        QVERIFY(catalog->plugins().isEmpty());
        QCOMPARE(reasonFor(*catalog, path), QStringLiteral("not a Qt plugin"));
        QVERIFY(!isMapped(canonical(path)));
    }

    void refusesAGroupOrWorldWritablePlugin_data()
    {
        QTest::addColumn<bool>("loosenDirectory");
        QTest::addColumn<int>("bit");
        QTest::newRow("group-writable file") << false << int(QFileDevice::WriteGroup);
        QTest::newRow("world-writable file") << false << int(QFileDevice::WriteOther);
        QTest::newRow("group-writable directory") << true << int(QFileDevice::WriteGroup);
        QTest::newRow("world-writable directory") << true << int(QFileDevice::WriteOther);
    }

    void refusesAGroupOrWorldWritablePlugin()
    {
        QFETCH(bool, loosenDirectory);
        QFETCH(int, bit);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString dir = root.filePath(QStringLiteral("plugins"));
        const QString path = stagePlugin(dir, QStringLiteral("fake-plugin"));
        QVERIFY(!path.isEmpty());
        const QString target = loosenDirectory ? dir : path;
        QVERIFY(QFile::setPermissions(target, QFile::permissions(target) | QFileDevice::Permission(bit)));
        auto catalog = makeCatalog({{dir, PluginOrigin::User}});
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }

        QVERIFY(catalog->plugins().isEmpty());
        QVERIFY(reasonFor(*catalog, path).contains(QStringLiteral("writable")));
    }

    void refusesAPluginSomeoneElseOwns_data()
    {
        QTest::addColumn<bool>("directory");
        QTest::newRow("file") << false;
        QTest::newRow("directory") << true;
    }

    void refusesAPluginSomeoneElseOwns()
    {
        QFETCH(bool, directory);
        if (::geteuid() != 0) {
            QSKIP("handing a file to another user needs root");
        }
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString dir = root.filePath(QStringLiteral("plugins"));
        const QString path = stagePlugin(dir, QStringLiteral("fake-plugin"));
        QVERIFY(!path.isEmpty());
        if (::chown(QFile::encodeName(directory ? dir : path).constData(), 65534, 65534) != 0) {
            const int error = errno;
            QSKIP(qPrintable(QStringLiteral("cannot hand the file to uid 65534: %1")
                                 .arg(QString::fromLocal8Bit(std::strerror(error)))));
        }
        auto catalog = makeCatalog({{dir, PluginOrigin::User}});
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }

        QVERIFY(catalog->plugins().isEmpty());
        QVERIFY(reasonFor(*catalog, path).contains(QStringLiteral("owned by uid 65534")));
    }

    void refusesAnUnreadablePlugin()
    {
        if (::geteuid() == 0) {
            QSKIP("root reads any file");
        }
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), QStringLiteral("fake-plugin"));
        QVERIFY(!path.isEmpty());
        QVERIFY(QFile::setPermissions(path, QFileDevice::WriteOwner));
        // Live reload on: the watcher must not be asked to watch a file it
        // cannot read, which would add a misleading warning of its own.
        PluginCatalog catalog(PluginLoader::barWidgetSeam());
        catalog.setSearchPaths({{root.path(), PluginOrigin::User}});
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog.scan();
        }

        QVERIFY(catalog.plugins().isEmpty());
        QCOMPARE(reasonFor(catalog, path), QStringLiteral("it is not readable"));
        QCOMPARE(countContaining(warnings, QStringLiteral("failed to add watch")), 0);
    }

    void refusesASymlinkedPlugin()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString outside = stagePlugin(root.filePath(QStringLiteral("outside")), QStringLiteral("fake-plugin"));
        QVERIFY(!outside.isEmpty());
        const QString dir = root.filePath(QStringLiteral("plugins"));
        QVERIFY(QDir().mkpath(dir));
        QVERIFY(makeOwnerWritable(dir));
        const QString link = dir + QStringLiteral("/linked.so");
        QVERIFY(QFile::link(outside, link));
        auto catalog = makeCatalog({{dir, PluginOrigin::User}});
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }

        QVERIFY(catalog->plugins().isEmpty());
        QVERIFY(reasonFor(*catalog, link).contains(QStringLiteral("symlink")));
        QVERIFY(!isMapped(canonical(outside)));
    }

    void refusesAPluginWithAnArchVariantBesideIt_data()
    {
        QTest::addColumn<bool>("symlink");
        QTest::newRow("a file") << false;
        QTest::newRow("a dangling symlink") << true;
    }

    void refusesAPluginWithAnArchVariantBesideIt()
    {
        QFETCH(bool, symlink);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), QStringLiteral("fake-plugin"));
        QVERIFY(!path.isEmpty());
        const QString variant = path + QStringLiteral(".avx2");
        if (symlink) {
            QVERIFY(QFile::link(root.filePath(QStringLiteral("nowhere.so")), variant));
        } else {
            QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin-secondary"),
                                 QStringLiteral("fake-plugin.so.avx2"))
                         .isEmpty());
        }
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }

        QVERIFY(catalog->plugins().isEmpty());
        QVERIFY(reasonFor(*catalog, path).contains(QStringLiteral("fake-plugin.so.avx2")));
    }

    void refusesASecondPluginWithTheSameIdInOneDirectory()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString first = stagePlugin(root.path(), QStringLiteral("fake-plugin"), QStringLiteral("a.so"));
        const QString second = stagePlugin(root.path(), QStringLiteral("fake-plugin"), QStringLiteral("b.so"));
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }

        QCOMPARE(filePathOf(*catalog, QStringLiteral("fake-plugin")), first);
        QVERIFY(reasonFor(*catalog, second).contains(QStringLiteral("a.so")));
    }

    void aRescanOffersNewPluginsAndWithdrawsRemovedOnes()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        QSignalSpy added(catalog.get(), &PluginCatalog::pluginAdded);
        QSignalSpy removed(catalog.get(), &PluginCatalog::pluginRemoved);
        QSignalSpy completed(catalog.get(), &PluginCatalog::scanCompleted);
        catalog->scan();
        QCOMPARE(completed.count(), 1);
        QVERIFY(catalog->plugins().isEmpty());

        const QString path = stagePlugin(root.path(), QStringLiteral("fake-plugin"));
        QVERIFY(!path.isEmpty());
        catalog->scan();
        QCOMPARE(added.count(), 1);
        QCOMPARE(completed.count(), 2);

        QVERIFY(QFile::remove(path));
        catalog->scan();
        QCOMPARE(removed.count(), 1);
        QCOMPARE(removed.first().first().toString(), QStringLiteral("fake-plugin"));
        QVERIFY(catalog->plugins().isEmpty());
        QCOMPARE(completed.count(), 3);
    }

    void liveReloadOffersAPluginWhenItAppears()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(makeOwnerWritable(root.path()));
        PluginCatalog catalog(PluginLoader::barWidgetSeam());
        catalog.setSearchPaths({{root.path(), PluginOrigin::User}});
        QSignalSpy added(&catalog, &PluginCatalog::pluginAdded);
        catalog.scan();
        QCOMPARE(added.count(), 0);

        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin")).isEmpty());
        QTRY_COMPARE(added.count(), 1);
        QCOMPARE(added.first().first().toString(), QStringLiteral("fake-plugin"));
    }

    void loadVetsTheFileAgain_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<QString>("expected");
        QTest::newRow("another plugin in its place")
            << QStringLiteral("swap") << QStringLiteral("now carries plugin fake-plugin-secondary");
        QTest::newRow("built against a newer Qt") << QStringLiteral("qt") << QStringLiteral("built against Qt");
        QTest::newRow("made group-writable") << QStringLiteral("chmod") << QStringLiteral("writable");
    }

    void loadVetsTheFileAgain()
    {
        QFETCH(QString, change);
        QFETCH(QString, expected);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), QStringLiteral("fake-plugin"));
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        catalog->scan();
        QVERIFY(catalog->plugin(QStringLiteral("fake-plugin")).has_value());

        // The file changes between the scan and the load.
        if (change == QLatin1String("swap")) {
            QCOMPARE(
                stagePlugin(root.path(), QStringLiteral("fake-plugin-secondary"), QStringLiteral("fake-plugin.so")),
                path);
        } else if (change == QLatin1String("qt")) {
            QCOMPARE(stagePluginBuiltAgainstQt(root.path(), QStringLiteral("fake-plugin"),
                                               QLibraryInfo::version().majorVersion(),
                                               QLibraryInfo::version().minorVersion() + 1),
                     path);
        } else {
            QVERIFY(QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::WriteGroup));
        }

        QString error;
        QCOMPARE(catalog->load(QStringLiteral("fake-plugin"), &error), nullptr);
        QVERIFY2(error.contains(expected), qPrintable(error));
        QVERIFY(!catalog->isLoaded(QStringLiteral("fake-plugin")));
        QVERIFY(!isMapped(canonical(path)));
    }

    void loadRecordsTheMetadataItMapped()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), QStringLiteral("fake-plugin"));
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        catalog->scan();
        QCOMPARE(stagePlugin(root.path(), QStringLiteral("fake-plugin-extra"), QStringLiteral("fake-plugin.so")), path);

        QString error;
        QVERIFY2(catalog->load(QStringLiteral("fake-plugin"), &error), qPrintable(error));
        const auto metadata = catalog->plugin(QStringLiteral("fake-plugin"));
        QVERIFY(metadata.has_value());
        QVERIFY(metadata->metaData.value(QLatin1String("Extra")).toBool());
    }

    void loadResolvesEverySymbolFirst()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), QStringLiteral("fake-plugin-unresolved"));
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        catalog->scan();
        QVERIFY(catalog->plugin(QStringLiteral("fake-plugin-retry")).has_value());

        // With lazy binding the library would load here, and the process
        // would end when createFactory first called the missing function.
        QString error;
        QCOMPARE(catalog->load(QStringLiteral("fake-plugin-retry"), &error), nullptr);
        QVERIFY2(error.contains(QStringLiteral("phosphor_registry_test_missing_function")), qPrintable(error));
        QVERIFY(!catalog->isLoaded(QStringLiteral("fake-plugin-retry")));
    }

    void aPluginWhoseLoadFailedIsOfferedAgainWhenItsFileChanges()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path =
            stagePlugin(root.path(), QStringLiteral("fake-plugin-unresolved"), QStringLiteral("fake-plugin-retry.so"));
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        QSignalSpy added(catalog.get(), &PluginCatalog::pluginAdded);
        catalog->scan();
        QCOMPARE(added.count(), 1);
        QCOMPARE(catalog->load(QStringLiteral("fake-plugin-retry")), nullptr);

        // Nothing changed on disk: no second offer, and no second attempt.
        catalog->scan();
        QCOMPARE(added.count(), 1);

        QCOMPARE(stagePlugin(root.path(), QStringLiteral("fake-plugin-retry")), path);
        catalog->scan();
        QCOMPARE(added.count(), 2);
        QCOMPARE(added.last().first().toString(), QStringLiteral("fake-plugin-retry"));
        QString error;
        QVERIFY2(catalog->load(QStringLiteral("fake-plugin-retry"), &error), qPrintable(error));
    }

    void aWithdrawnPluginCannotBeLoadedUntilItIsBack()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), QStringLiteral("fake-plugin"));
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        catalog->scan();
        QObject* const instance = catalog->load(QStringLiteral("fake-plugin"));
        QVERIFY(instance);

        QVERIFY(QFile::remove(path));
        catalog->scan();
        QVERIFY(!catalog->plugin(QStringLiteral("fake-plugin")).has_value());
        QString error;
        QCOMPARE(catalog->load(QStringLiteral("fake-plugin"), &error), nullptr);
        QVERIFY2(error.contains(QStringLiteral("on offer")), qPrintable(error));
        QVERIFY(catalog->isLoaded(QStringLiteral("fake-plugin")));

        QCOMPARE(stagePlugin(root.path(), QStringLiteral("fake-plugin")), path);
        catalog->scan();
        QCOMPARE(catalog->load(QStringLiteral("fake-plugin")), instance);
    }

    void aLoadedPluginKeepsOfferingTheCodeThatRuns()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString user = root.filePath(QStringLiteral("user"));
        const QString dev = root.filePath(QStringLiteral("dev"));
        const QString loadedFrom = stagePlugin(user, QStringLiteral("fake-plugin"));
        QVERIFY(!loadedFrom.isEmpty());
        auto catalog = makeCatalog({{dev, PluginOrigin::Development}, {user, PluginOrigin::User}});
        catalog->scan();
        QVERIFY(catalog->load(QStringLiteral("fake-plugin")));

        // A higher-priority copy appears after the plugin was loaded. It
        // takes effect at the next start, so the offer still describes the
        // loaded file.
        QVERIFY(!stagePlugin(dev, QStringLiteral("fake-plugin")).isEmpty());
        QStringList warnings;
        QStringList infos;
        {
            MessageCapture capture(warnings, &infos);
            catalog->scan();
        }
        QCOMPARE(filePathOf(*catalog, QStringLiteral("fake-plugin")), loadedFrom);
        QCOMPARE(countContaining(infos, QStringLiteral("next start")), 1);
    }

    void aLoadedPluginWhoseFileIsRefusedIsWithdrawnEvenWithAnotherCopy()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString dev = root.filePath(QStringLiteral("dev"));
        const QString user = root.filePath(QStringLiteral("user"));
        const QString loadedFrom = stagePlugin(dev, QStringLiteral("fake-plugin"));
        QVERIFY(!loadedFrom.isEmpty());
        QVERIFY(!stagePlugin(user, QStringLiteral("fake-plugin")).isEmpty());
        auto catalog = makeCatalog({{dev, PluginOrigin::Development}, {user, PluginOrigin::User}});
        catalog->scan();
        QVERIFY(catalog->load(QStringLiteral("fake-plugin")));

        QVERIFY(QFile::setPermissions(loadedFrom, QFile::permissions(loadedFrom) | QFileDevice::WriteGroup));
        QSignalSpy removed(catalog.get(), &PluginCatalog::pluginRemoved);
        QStringList warnings;
        QStringList infos;
        {
            MessageCapture capture(warnings, &infos);
            catalog->scan();
        }

        QCOMPARE(removed.count(), 1);
        QVERIFY(catalog->plugins().isEmpty());
        QVERIFY(reasonFor(*catalog, loadedFrom).contains(QStringLiteral("writable")));
        QCOMPARE(countContaining(infos, QStringLiteral("withdrawn")), 1);
    }

    void aScanCutShortByTheCapKeepsPluginsItDidNotReach()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin")).isEmpty());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin-secondary")).isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        catalog->scan();
        QCOMPARE(catalog->plugins().size(), 2);

        catalog->setMaxFilesPerScanForTest(1);
        QSignalSpy removed(catalog.get(), &PluginCatalog::pluginRemoved);
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
            catalog->scan();
        }
        QCOMPARE(removed.count(), 0);
        QCOMPARE(catalog->plugins().size(), 2);
        // Logged once while the cap stays exceeded, not on every scan.
        QCOMPARE(countContaining(warnings, QStringLiteral("stopped after")), 1);
    }

    void aScanCutShortKeepsALoadedPluginItDidNotReach()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString dev = root.filePath(QStringLiteral("dev"));
        const QString user = root.filePath(QStringLiteral("user"));
        const QString loadedFrom = stagePlugin(user, QStringLiteral("fake-plugin"));
        QVERIFY(!loadedFrom.isEmpty());
        auto catalog = makeCatalog({{dev, PluginOrigin::Development}, {user, PluginOrigin::User}});
        catalog->scan();
        QVERIFY(catalog->load(QStringLiteral("fake-plugin")));
        QVERIFY(!stagePlugin(dev, QStringLiteral("fake-plugin")).isEmpty());
        catalog->scan();
        QCOMPARE(filePathOf(*catalog, QStringLiteral("fake-plugin")), loadedFrom);

        // The cap reaches the new copy in `dev` and not the loaded one in
        // `user`, which is still there: the plugin stays on offer.
        catalog->setMaxFilesPerScanForTest(1);
        QSignalSpy removed(catalog.get(), &PluginCatalog::pluginRemoved);
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }
        QCOMPARE(removed.count(), 0);
        QCOMPARE(filePathOf(*catalog, QStringLiteral("fake-plugin")), loadedFrom);
    }

    void aScanCutShortStillDropsWhatItSawGoAway()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString first = root.filePath(QStringLiteral("first"));
        const QString second = root.filePath(QStringLiteral("second"));
        const QString gone = stagePlugin(first, QStringLiteral("fake-plugin-retry"));
        // In name order fake-plugin-secondary.so comes before fake-plugin.so,
        // so a cap of one reaches only fake-plugin-secondary in `second`.
        const QString refusedLater = stagePlugin(second, QStringLiteral("fake-plugin-secondary"));
        QVERIFY(!gone.isEmpty() && !refusedLater.isEmpty());
        QVERIFY(!stagePlugin(second, QStringLiteral("fake-plugin")).isEmpty());
        auto catalog = makeCatalog({{first, PluginOrigin::Development}, {second, PluginOrigin::User}});
        catalog->scan();
        QCOMPARE(catalog->plugins().size(), 3);

        // `first` is listed to the end and comes back empty, so its plugin is
        // gone. fake-plugin-secondary is reached and refused. Only
        // fake-plugin is past the cap.
        QVERIFY(QFile::remove(gone));
        QVERIFY(QFile::setPermissions(refusedLater, QFile::permissions(refusedLater) | QFileDevice::WriteGroup));
        catalog->setMaxFilesPerScanForTest(1);
        QSignalSpy removed(catalog.get(), &PluginCatalog::pluginRemoved);
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }

        QCOMPARE(removed.count(), 2);
        QCOMPARE(catalog->plugins().size(), 1);
        QVERIFY(catalog->plugin(QStringLiteral("fake-plugin")).has_value());
        QVERIFY(reasonFor(*catalog, refusedLater).contains(QStringLiteral("writable")));
    }

    void aScanRequestedFromItsOwnSignalRunsAfterIt()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString first = stagePlugin(root.path(), QStringLiteral("fake-plugin"));
        const QString second = stagePlugin(root.path(), QStringLiteral("fake-plugin-secondary"));
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        catalog->scan();
        QCOMPARE(catalog->plugins().size(), 2);

        QSignalSpy removed(catalog.get(), &PluginCatalog::pluginRemoved);
        QSignalSpy completed(catalog.get(), &PluginCatalog::scanCompleted);
        PluginCatalog* const raw = catalog.get();
        connect(raw, &PluginCatalog::pluginRemoved, this, [raw] {
            raw->scan();
        });
        QVERIFY(QFile::remove(first));
        QVERIFY(QFile::remove(second));
        catalog->scan();

        // The requested scans ran after this one finished, not inside it, so
        // each removal was reported once.
        QCOMPARE(completed.count(), 1);
        QCOMPARE(removed.count(), 2);
        QTRY_COMPARE(completed.count(), 2);
        QCOMPARE(removed.count(), 2);
        QVERIFY(catalog->plugins().isEmpty());
    }

    // The helper the warning assertions rely on: a capture opened inside
    // another one hands each line to both, and returns.
    void nestedMessageCapturesBothSeeALine()
    {
        QStringList outer;
        QStringList inner;
        {
            MessageCapture outerCapture(outer);
            {
                MessageCapture innerCapture(inner);
                qWarning("nested capture probe");
            }
            qWarning("outer capture probe");
        }
        QCOMPARE(inner, QStringList{QStringLiteral("nested capture probe")});
        QCOMPARE(outer, (QStringList{QStringLiteral("nested capture probe"), QStringLiteral("outer capture probe")}));
    }

    void warnsOncePerProblemUntilItGoesAway()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), QStringLiteral("fake-plugin-wrongapi"));
        QVERIFY(!path.isEmpty());
        auto catalog = makeCatalog({{root.path(), PluginOrigin::User}});
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            catalog->scan();
            catalog->scan();
            catalog->scan();
        }
        QCOMPARE(countContaining(warnings, path), 1);

        // Once the problem is gone, its return warns again.
        QVERIFY(QFile::remove(path));
        catalog->scan();
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin-wrongapi")).isEmpty());
        warnings.clear();
        {
            MessageCapture capture(warnings);
            catalog->scan();
        }
        QCOMPARE(countContaining(warnings, path), 1);
    }

    void anInvalidSeamOffersNothing()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin")).isEmpty());
        QStringList warnings;
        std::unique_ptr<PluginCatalog> catalog;
        {
            MessageCapture capture(warnings);
            catalog = std::make_unique<PluginCatalog>(
                PluginSeam{QStringLiteral("../bar-widget"), QStringLiteral(PhosphorRegistryBarWidgetPlugin_iid), 1});
        }
        QCOMPARE(countContaining(warnings, QStringLiteral("invalid seam")), 1);
        catalog->setSearchPaths({{root.path(), PluginOrigin::User}});
        catalog->setLiveReload(false);
        QSignalSpy completed(catalog.get(), &PluginCatalog::scanCompleted);
        catalog->scan();
        QCOMPARE(completed.count(), 1);
        QVERIFY(catalog->plugins().isEmpty());

        // A scan asked for from scanCompleted runs after it, not inside it,
        // for an invalid seam as for any other.
        int requests = 0;
        PluginCatalog* const raw = catalog.get();
        connect(raw, &PluginCatalog::scanCompleted, this, [raw, &requests] {
            if (++requests < 3) {
                raw->scan();
            }
        });
        catalog->scan();
        QCOMPARE(requests, 1);
        QTRY_COMPARE(requests, 3);
    }

    void aRelativeSearchPathIsResolvedWhenSet()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString saved = QDir::currentPath();
        QVERIFY(QDir::setCurrent(root.path()));
        auto catalog = makeCatalog({{QStringLiteral("relative/dir"), PluginOrigin::Development}});
        QVERIFY(QDir::setCurrent(saved));

        QCOMPARE(catalog->searchPaths().size(), 1);
        QCOMPARE(catalog->searchPaths().first().directory, canonical(root.path()) + QStringLiteral("/relative/dir"));
    }

    void defaultSearchPathsPutDevelopmentFirstThenUserThenSystem()
    {
        const QByteArray saved = qgetenv(PluginCatalog::DevelopmentPathVariable);
        qputenv(PluginCatalog::DevelopmentPathVariable, QByteArrayLiteral("/build/one::relative:/build/two"));
        QStringList warnings;
        QList<PluginSearchPath> paths;
        {
            MessageCapture capture(warnings);
            paths = PluginCatalog::defaultSearchPaths(QStringLiteral("facet"));
        }
        if (saved.isNull()) {
            qunsetenv(PluginCatalog::DevelopmentPathVariable);
        } else {
            qputenv(PluginCatalog::DevelopmentPathVariable, saved);
        }

        // The relative entry is skipped, not resolved against whatever the
        // working directory happens to be.
        QCOMPARE(countContaining(warnings, QStringLiteral("relative/facet")), 1);
        QVERIFY(paths.size() > 3);
        QCOMPARE(paths.at(0).directory, QStringLiteral("/build/one/facet"));
        QCOMPARE(paths.at(0).origin, PluginOrigin::Development);
        QCOMPARE(paths.at(1).directory, QStringLiteral("/build/two/facet"));
        QCOMPARE(paths.at(1).origin, PluginOrigin::Development);
        QCOMPARE(paths.at(2).origin, PluginOrigin::User);
        QCOMPARE(paths.at(2).directory,
                 QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                     + QStringLiteral("/phosphor-works/plugins/facet"));

        // Qt lists its plugin directories by their canonical paths.
        const QString qtPlugins =
            canonical(QLibraryInfo::path(QLibraryInfo::PluginsPath)) + QStringLiteral("/phosphor-works/facet");
        const QString appDir =
            QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/phosphor-works/facet"));
        bool sawQtPlugins = false;
        for (qsizetype i = 3; i < paths.size(); ++i) {
            QCOMPARE(paths.at(i).origin, PluginOrigin::System);
            QVERIFY(paths.at(i).directory.endsWith(QStringLiteral("/phosphor-works/facet")));
            QVERIFY(paths.at(i).directory != appDir);
            sawQtPlugins = sawQtPlugins || paths.at(i).directory == qtPlugins;
        }
        QVERIFY(sawQtPlugins);
    }
};

QTEST_MAIN(TestPluginCatalog)
#include "test_phosphor_registry_plugincatalog.moc"
