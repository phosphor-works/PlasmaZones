// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DemoController.h"

#include "QmlComponentBarWidgetFactory.h"

#include <PhosphorRegistry/RegistryNotifier.h>

#include <QQmlEngine>
#include <QUrl>

using namespace PhosphorRegistry;

namespace PhosphorRegistryPluginDemo {

DemoController::DemoController(const QString& pluginRoot, QObject* parent)
    : QObject(parent)
    , m_registry(std::make_unique<Registry<IBarWidgetFactory>>())
{
    QObject::connect(m_registry->notifier(), &RegistryNotifier::factoryRegistered, this,
                     &DemoController::factoryIdsChanged);
    QObject::connect(m_registry->notifier(), &RegistryNotifier::factoryUnregistered, this,
                     &DemoController::factoryIdsChanged);

    registerBuiltins();

    // A given root's bar-widget directory is searched first, as a
    // development root, then the standard paths, so the demo shows the real
    // search order and a plugin from PHOSPHOR_WORKS_PLUGIN_PATH loads too.
    // Without a root an empty list leaves the loader on the standard paths.
    const QString seamName = PluginLoader::barWidgetSeam().name;
    QList<PluginSearchPath> searchPaths;
    if (!pluginRoot.isEmpty()) {
        searchPaths.append({pluginRoot + QLatin1Char('/') + seamName, PluginOrigin::Development});
        searchPaths.append(PluginCatalog::defaultSearchPaths(seamName));
    }
    m_loader = std::make_unique<PluginLoader>(m_registry.get(), searchPaths);
    connect(m_loader.get(), &PluginLoader::rescanCompleted, this, &DemoController::updateRefusedPlugins);
    // scanAndLoad runs the first scan synchronously, so the bar has every
    // plugin widget on its first paint.
    m_loader->scanAndLoad();
    // The CONSTANT property holds because QML is wired up only after the
    // constructor returns, and the search paths are fixed by the first scan.
    const QList<PluginSearchPath> searched = m_loader->catalog().searchPaths();
    for (const PluginSearchPath& path : searched) {
        m_searchedDirectories.append(path.directory);
    }
}

DemoController::~DemoController() = default;

void DemoController::setEngine(QQmlEngine* engine)
{
    // Idempotent on same-engine repeat; qFatal on different-engine
    // after first set (same contract as the in-process demo's
    // DemoController — see that .cpp for the long-form rationale).
    if (m_engine && m_engine != engine) {
        Q_ASSERT_X(false, "DemoController::setEngine", "DemoController::setEngine called twice with different engines");
        qFatal("DemoController::setEngine called twice with different engines");
    }
    m_engine = engine;
}

void DemoController::registerBuiltins()
{
    m_registry->registerFactory(std::make_shared<QmlComponentBarWidgetFactory>(
        QStringLiteral("clock"), QStringLiteral("Clock"),
        QUrl(QStringLiteral("qrc:/qt/qml/Phosphor/RegistryPluginDemo/ClockWidget.qml")),
        QStringList{QStringLiteral("bar.widget")}));
    m_registry->registerFactory(std::make_shared<QmlComponentBarWidgetFactory>(
        QStringLiteral("colorsquare"), QStringLiteral("Color Square"),
        QUrl(QStringLiteral("qrc:/qt/qml/Phosphor/RegistryPluginDemo/ColorSquareWidget.qml")),
        QStringList{QStringLiteral("bar.widget")}));
}

QQuickItem* DemoController::createWidgetFor(const QString& id, QQuickItem* parent)
{
    if (!m_engine) {
        // Engine torn down — refuse the call rather than crash
        // inside the factory.
        return nullptr;
    }
    auto factory = m_registry->factory(id);
    if (!factory) {
        return nullptr;
    }
    return factory->createWidget(m_engine.data(), parent);
}

void DemoController::reloadPlugins()
{
    if (m_loader) {
        m_loader->rescanNow();
    }
}

QStringList DemoController::factoryIds() const
{
    QStringList ids = m_registry->ids();
    ids.sort();
    return ids;
}

QStringList DemoController::searchedDirectories() const
{
    return m_searchedDirectories;
}

QStringList DemoController::refusedPlugins() const
{
    return m_refusedPlugins;
}

void DemoController::updateRefusedPlugins()
{
    QStringList refused;
    const QList<RefusedPlugin> entries = m_loader->catalog().refused();
    for (const RefusedPlugin& entry : entries) {
        refused.append(QStringLiteral("%1: %2").arg(entry.filePath, entry.reason));
    }
    if (refused == m_refusedPlugins) {
        return;
    }
    m_refusedPlugins = std::move(refused);
    Q_EMIT refusedPluginsChanged();
}

} // namespace PhosphorRegistryPluginDemo
