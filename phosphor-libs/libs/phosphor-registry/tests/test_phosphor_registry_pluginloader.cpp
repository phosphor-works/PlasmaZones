// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "test_plugin_helpers.h"

#include <PhosphorRegistry/IBarWidgetFactory.h>
#include <PhosphorRegistry/IBarWidgetPlugin.h>
#include <PhosphorRegistry/PluginLoader.h>
#include <PhosphorRegistry/Registry.h>

#include <QFile>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

using namespace PhosphorRegistry;
using namespace PhosphorRegistryTestHelpers;

namespace {

class BuiltinFactory : public IBarWidgetFactory
{
public:
    explicit BuiltinFactory(QString id)
        : m_id(std::move(id))
    {
    }
    [[nodiscard]] QString id() const override
    {
        return m_id;
    }
    [[nodiscard]] QString displayName() const override
    {
        return m_id;
    }
    [[nodiscard]] QQuickItem* createWidget(QQmlEngine*, QObject* parent) override
    {
        return new QQuickItem(qobject_cast<QQuickItem*>(parent));
    }

private:
    QString m_id;
};

} // namespace

class TestPluginLoader : public QObject
{
    Q_OBJECT

private:
    // A loader over one development directory, with live reload off so every
    // scan is an explicit call.
    static std::unique_ptr<PluginLoader> makeLoader(Registry<IBarWidgetFactory>* registry, const QString& directory)
    {
        auto loader =
            std::make_unique<PluginLoader>(registry, QList<PluginSearchPath>{{directory, PluginOrigin::Development}});
        loader->catalog().setLiveReload(false);
        return loader;
    }

private Q_SLOTS:
    void barWidgetSeamNamesItsDirectoryIidAndVersion()
    {
        const PluginSeam seam = PluginLoader::barWidgetSeam();
        QCOMPARE(seam.name, QStringLiteral("bar-widget"));
        QCOMPARE(seam.iid, QStringLiteral("org.phosphor_works.Lambent.BarWidget/1"));
        QCOMPARE(seam.apiVersion, 1);
    }

    void registersTheFactoryOfEveryPluginItFinds()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin")).isEmpty());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin-secondary")).isEmpty());
        Registry<IBarWidgetFactory> registry;
        auto loader = makeLoader(&registry, root.path());
        QSignalSpy loaded(loader.get(), &PluginLoader::pluginLoaded);
        QSignalSpy completed(loader.get(), &PluginLoader::rescanCompleted);

        loader->scanAndLoad();

        QCOMPARE(loaded.count(), 2);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(loader->loadedPluginIds(),
                 (QStringList{QStringLiteral("fake-plugin"), QStringLiteral("fake-plugin-secondary")}));
        QVERIFY(registry.factory(QStringLiteral("fake-plugin")));
        QVERIFY(registry.factory(QStringLiteral("fake-plugin-secondary")));
    }

    void aPluginFactoryBuildsWidgets()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin")).isEmpty());
        Registry<IBarWidgetFactory> registry;
        auto loader = makeLoader(&registry, root.path());
        loader->scanAndLoad();

        const auto factory = registry.factory(QStringLiteral("fake-plugin"));
        QVERIFY(factory);
        QQmlEngine engine;
        QQuickItem parent;
        QQuickItem* const widget = factory->createWidget(&engine, &parent);
        QVERIFY(widget);
        QCOMPARE(widget->parentItem(), &parent);
    }

    void refusesAFactoryWhoseIdDiffersFromTheMetadata()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin-idmismatch")).isEmpty());
        Registry<IBarWidgetFactory> registry;
        auto loader = makeLoader(&registry, root.path());
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            loader->scanAndLoad();
        }

        QVERIFY(loader->loadedPluginIds().isEmpty());
        QVERIFY(registry.isEmpty());
        QCOMPARE(countContaining(warnings, QStringLiteral("fake-other")), 1);
    }

    void refusesAPluginThatReturnsNoFactory()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin-nullfactory")).isEmpty());
        Registry<IBarWidgetFactory> registry;
        auto loader = makeLoader(&registry, root.path());
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            loader->scanAndLoad();
        }

        QVERIFY(registry.isEmpty());
        QCOMPARE(countContaining(warnings, QStringLiteral("returned no factory")), 1);
    }

    void refusesAPluginWhoseRootObjectIsNotABarWidgetPlugin()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin-nointerface")).isEmpty());
        Registry<IBarWidgetFactory> registry;
        auto loader = makeLoader(&registry, root.path());
        QSignalSpy loaded(loader.get(), &PluginLoader::pluginLoaded);
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            loader->scanAndLoad();
        }

        QVERIFY(registry.isEmpty());
        QCOMPARE(loaded.count(), 0);
        QCOMPARE(countContaining(warnings, QStringLiteral("does not implement IBarWidgetPlugin")), 1);
    }

    void reportsAPluginThatCannotBeLoaded()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin-unresolved")).isEmpty());
        Registry<IBarWidgetFactory> registry;
        auto loader = makeLoader(&registry, root.path());
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            loader->scanAndLoad();
        }

        QVERIFY(registry.isEmpty());
        QCOMPARE(countContaining(warnings, QStringLiteral("did not load")), 1);
        QVERIFY(!loader->catalog().isLoaded(QStringLiteral("fake-plugin-retry")));
    }

    void aPluginThatFailedToLoadIsLoadedOnceItsFileIsFixed()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path =
            stagePlugin(root.path(), QStringLiteral("fake-plugin-unresolved"), QStringLiteral("fake-plugin-retry.so"));
        QVERIFY(!path.isEmpty());
        Registry<IBarWidgetFactory> registry;
        auto loader = makeLoader(&registry, root.path());
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            loader->scanAndLoad();
            loader->rescanNow();
        }
        QVERIFY(registry.isEmpty());
        // Tried once: an unchanged file is not loaded again on every scan.
        QCOMPARE(countContaining(warnings, QStringLiteral("did not load")), 1);

        QCOMPARE(stagePlugin(root.path(), QStringLiteral("fake-plugin-retry")), path);
        QSignalSpy loaded(loader.get(), &PluginLoader::pluginLoaded);
        loader->rescanNow();
        QCOMPARE(loaded.count(), 1);
        QVERIFY(registry.factory(QStringLiteral("fake-plugin-retry")));
    }

    void aBuiltinWithTheSameIdKeepsItsSlot()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin")).isEmpty());
        Registry<IBarWidgetFactory> registry;
        auto builtin = std::make_shared<BuiltinFactory>(QStringLiteral("fake-plugin"));
        QVERIFY(registry.registerFactory(builtin));
        auto loader = makeLoader(&registry, root.path());
        QStringList warnings;
        {
            MessageCapture capture(warnings);
            loader->scanAndLoad();
        }

        QCOMPARE(registry.factory(QStringLiteral("fake-plugin")), builtin);
        QVERIFY(loader->loadedPluginIds().isEmpty());
        QCOMPARE(countContaining(warnings, QStringLiteral("already has a factory with that id")), 1);
        // Not even mapped: the registry was asked first.
        QVERIFY(!loader->catalog().isLoaded(QStringLiteral("fake-plugin")));
    }

    void aRemovedPluginIsUnregisteredAndItsWidgetsSurvive()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), QStringLiteral("fake-plugin"));
        QVERIFY(!path.isEmpty());
        Registry<IBarWidgetFactory> registry;
        auto loader = makeLoader(&registry, root.path());
        loader->scanAndLoad();

        const auto factory = registry.factory(QStringLiteral("fake-plugin"));
        QVERIFY(factory);
        QQmlEngine engine;
        auto parent = std::make_unique<QQuickItem>();
        QPointer<QQuickItem> widget = factory->createWidget(&engine, parent.get());
        QVERIFY(widget);

        // Captured before the file goes: once it is unlinked, only its
        // canonical path still names the mapped library.
        const QString mappedPath = QFileInfo(path).canonicalFilePath();
        QSignalSpy unloaded(loader.get(), &PluginLoader::pluginUnloaded);
        QVERIFY(QFile::remove(path));
        loader->rescanNow();

        QCOMPARE(unloaded.count(), 1);
        QVERIFY(!registry.factory(QStringLiteral("fake-plugin")));
        QVERIFY(loader->loadedPluginIds().isEmpty());
        // The library stays mapped, so the widget, whose class is defined in
        // the plugin, is still a valid object and its parent can delete it.
        QVERIFY(loader->catalog().isLoaded(QStringLiteral("fake-plugin")));
        QVERIFY(isMapped(mappedPath));
        QVERIFY(widget);
        QCOMPARE(QString::fromLatin1(widget->metaObject()->className()), QStringLiteral("FakeWidget"));
        QCOMPARE(widget->parentItem(), parent.get());
        parent.reset();
        QVERIFY(!widget);
    }

    void aPluginAddedLaterIsLoadedOnRescan()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        Registry<IBarWidgetFactory> registry;
        auto loader = makeLoader(&registry, root.path());
        loader->scanAndLoad();
        QVERIFY(registry.isEmpty());

        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin")).isEmpty());
        QSignalSpy loaded(loader.get(), &PluginLoader::pluginLoaded);
        loader->rescanNow();
        QCOMPARE(loaded.count(), 1);
        QVERIFY(registry.factory(QStringLiteral("fake-plugin")));
    }

    void aRescanFromAnUnloadSlotRunsAfterTheCurrentOne()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString first = stagePlugin(root.path(), QStringLiteral("fake-plugin"));
        const QString second = stagePlugin(root.path(), QStringLiteral("fake-plugin-secondary"));
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        Registry<IBarWidgetFactory> registry;
        auto loader = makeLoader(&registry, root.path());
        loader->scanAndLoad();
        QCOMPARE(registry.size(), 2);

        int reentries = 0;
        connect(loader.get(), &PluginLoader::pluginUnloaded, this, [&loader, &reentries] {
            ++reentries;
            loader->rescanNow();
        });
        QSignalSpy withdrawn(&loader->catalog(), &PluginCatalog::pluginRemoved);
        QSignalSpy completed(loader.get(), &PluginLoader::rescanCompleted);
        QVERIFY(QFile::remove(first));
        QVERIFY(QFile::remove(second));
        loader->rescanNow();
        QTRY_COMPARE(completed.count(), 2);

        // Each plugin was withdrawn and unloaded exactly once.
        QCOMPARE(withdrawn.count(), 2);
        QCOMPARE(reentries, 2);
        QVERIFY(registry.isEmpty());
        QVERIFY(loader->loadedPluginIds().isEmpty());
    }

    void unloadingLeavesAFactoryAnotherOwnerPutInItsPlace()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString path = stagePlugin(root.path(), QStringLiteral("fake-plugin"));
        QVERIFY(!path.isEmpty());
        Registry<IBarWidgetFactory> registry;
        auto loader = makeLoader(&registry, root.path());
        loader->scanAndLoad();
        QVERIFY(registry.factory(QStringLiteral("fake-plugin")));

        auto replacement = std::make_shared<BuiltinFactory>(QStringLiteral("fake-plugin"));
        QVERIFY(registry.registerFactory(replacement, QString(), DuplicatePolicy::Replace));
        QSignalSpy unloaded(loader.get(), &PluginLoader::pluginUnloaded);
        QVERIFY(QFile::remove(path));
        loader->rescanNow();

        QCOMPARE(unloaded.count(), 1);
        QCOMPARE(registry.factory(QStringLiteral("fake-plugin")), replacement);
    }

    void destroyingTheLoaderUnregistersItsFactoriesOnly()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin")).isEmpty());
        QVERIFY(!stagePlugin(root.path(), QStringLiteral("fake-plugin-secondary")).isEmpty());
        Registry<IBarWidgetFactory> registry;
        auto builtin = std::make_shared<BuiltinFactory>(QStringLiteral("clock"));
        QVERIFY(registry.registerFactory(builtin));
        auto loader = makeLoader(&registry, root.path());
        loader->scanAndLoad();
        QCOMPARE(registry.size(), 3);

        // Another owner replaced one of the loader's factories.
        auto replacement = std::make_shared<BuiltinFactory>(QStringLiteral("fake-plugin-secondary"));
        QVERIFY(registry.registerFactory(replacement, QString(), DuplicatePolicy::Replace));

        loader.reset();
        QCOMPARE(registry.size(), 2);
        QCOMPARE(registry.factory(QStringLiteral("clock")), builtin);
        QCOMPARE(registry.factory(QStringLiteral("fake-plugin-secondary")), replacement);
    }
};

QTEST_MAIN(TestPluginLoader)
#include "test_phosphor_registry_pluginloader.moc"
