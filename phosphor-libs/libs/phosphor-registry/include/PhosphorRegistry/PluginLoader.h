// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <PhosphorRegistry/IBarWidgetFactory.h>
#include <PhosphorRegistry/PluginCatalog.h>
#include <PhosphorRegistry/Registry.h>
#include <PhosphorRegistry/phosphorregistry_export.h>

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtCore/qtclasshelpermacros.h>

#include <memory>

namespace PhosphorRegistry {

/// Loads every bar-widget plugin into a Registry<IBarWidgetFactory> and keeps
/// it in step with the plugins on disk.
///
/// Discovery, vetting, overrides and live reload come from a PluginCatalog
/// over the `bar-widget` seam (see barWidgetSeam). A plugin's root object
/// implements IBarWidgetPlugin. Every bar-widget plugin is loaded as soon as
/// it comes on offer, because the bar needs every factory to lay itself out.
/// A plugin whose id the registry already holds is not loaded at all.
///
/// A plugin the catalog withdraws (its file is gone or now refused) is
/// unregistered and pluginUnloaded is emitted. Its library stays mapped
/// (PluginCatalog never unloads), so widgets it already created keep working
/// until their parents delete them. A plugin whose library failed to load is
/// tried again once its file changes. One that loaded but handed out no
/// usable factory keeps its loaded code until the next start.
///
/// The Registry must outlive the loader. GUI thread only.
class PHOSPHORREGISTRY_EXPORT PluginLoader : public QObject
{
    Q_OBJECT
public:
    /// The bar-widget seam: plugins in `<root>/bar-widget/`, IID
    /// PhosphorRegistryBarWidgetPlugin_iid, ApiVersion 1.
    [[nodiscard]] static PluginSeam barWidgetSeam();

    /// @p searchPaths highest priority first. Empty means
    /// PluginCatalog::defaultSearchPaths for the bar-widget seam.
    explicit PluginLoader(Registry<IBarWidgetFactory>* registry,
                          const QList<PluginSearchPath>& searchPaths = QList<PluginSearchPath>(),
                          QObject* parent = nullptr);
    ~PluginLoader() override;
    Q_DISABLE_COPY_MOVE(PluginLoader)

    /// The first scan: find, load and register every plugin, and arm the
    /// watch. It runs synchronously, so connect to the registry first to see
    /// these registrations. A second call rescans, like rescanNow.
    void scanAndLoad();
    /// Rescan now instead of waiting for the watch. Called from a slot that
    /// a running scan reached, the rescan runs once that scan has finished.
    void rescanNow();

    /// The ids of the plugins currently registered, sorted.
    [[nodiscard]] QStringList loadedPluginIds() const;

    [[nodiscard]] PluginCatalog& catalog();
    [[nodiscard]] const PluginCatalog& catalog() const;

Q_SIGNALS:
    /// After every scan, whether or not anything changed.
    void rescanCompleted();
    /// A plugin's factory was registered.
    void pluginLoaded(const QString& id);
    /// The catalog withdrew a plugin this loader had registered. Its factory
    /// is unregistered, unless another owner has since replaced it.
    void pluginUnloaded(const QString& id);

private:
    void loadPlugin(const QString& id);
    void unloadPlugin(const QString& id);
    void unregisterIfOurs(const QString& id, const std::shared_ptr<IBarWidgetFactory>& ours);

    /// Non-owning, never null after construction.
    Registry<IBarWidgetFactory>* m_registry = nullptr;
    PluginCatalog m_catalog;
    /// The factories this loader registered, by plugin id.
    QHash<QString, std::shared_ptr<IBarWidgetFactory>> m_factories;
};

} // namespace PhosphorRegistry
