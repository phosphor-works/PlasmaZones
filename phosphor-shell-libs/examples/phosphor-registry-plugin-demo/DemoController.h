// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <PhosphorRegistry/IBarWidgetFactory.h>
#include <PhosphorRegistry/PluginLoader.h>
#include <PhosphorRegistry/Registry.h>

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>
#include <QString>
#include <QStringList>

#include <memory>

namespace PhosphorRegistryPluginDemo {

// Mirrors the in-process demo's controller but adds a PluginLoader that
// searches a plugin root ahead of the standard plugin paths. The same
// Registry receives both built-in factories (registered explicitly) and
// plugin factories (registered by the loader when a plugin comes on offer).
class DemoController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList factoryIds READ factoryIds NOTIFY factoryIdsChanged)
    Q_PROPERTY(QStringList searchedDirectories READ searchedDirectories CONSTANT)
    Q_PROPERTY(QStringList refusedPlugins READ refusedPlugins NOTIFY refusedPluginsChanged)
public:
    // @p pluginRoot holds a bar-widget directory searched first. Empty
    // means the standard plugin paths alone.
    explicit DemoController(const QString& pluginRoot, QObject* parent = nullptr);
    ~DemoController() override;
    Q_DISABLE_COPY_MOVE(DemoController)

    // Set the QML engine the controller will hand to factories when
    // they need to compile QML. MUST be called exactly once, from
    // main() AFTER both this controller and the engine are
    // constructed, BEFORE setContextProperty / loadFromModule. See
    // the in-process registry demo's DemoController for the longer-
    // form rationale.
    void setEngine(QQmlEngine* engine);

    // Bar QML invokes this from rebuildBar() for each id in
    // factoryIds.
    Q_INVOKABLE QQuickItem* createWidgetFor(const QString& id, QQuickItem* parent);

    // QML "Reload" button hook. Rescans now instead of waiting for the
    // watcher's debounce, which picks up plugins added or removed since
    // the last scan. A rebuilt plugin that is already loaded keeps its old
    // code until the demo restarts.
    Q_INVOKABLE void reloadPlugins();

    [[nodiscard]] QStringList factoryIds() const;
    [[nodiscard]] QStringList searchedDirectories() const;
    // Each plugin file the catalog refused, with the reason.
    [[nodiscard]] QStringList refusedPlugins() const;

Q_SIGNALS:
    void factoryIdsChanged();
    void refusedPluginsChanged();

private:
    void registerBuiltins();
    void updateRefusedPlugins();

    QPointer<QQmlEngine> m_engine;
    QStringList m_searchedDirectories;
    QStringList m_refusedPlugins;
    std::unique_ptr<PhosphorRegistry::Registry<PhosphorRegistry::IBarWidgetFactory>> m_registry;
    std::unique_ptr<PhosphorRegistry::PluginLoader> m_loader;
};

} // namespace PhosphorRegistryPluginDemo
