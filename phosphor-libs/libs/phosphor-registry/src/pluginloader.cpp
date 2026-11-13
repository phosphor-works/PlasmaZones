// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorRegistry/PluginLoader.h>

#include <PhosphorRegistry/IBarWidgetPlugin.h>

#include <QLoggingCategory>

#include <algorithm>
#include <string_view>

namespace PhosphorRegistry {

namespace {
Q_LOGGING_CATEGORY(lcPluginLoader, "phosphorregistry.pluginloader")

// The ApiVersion the bar-widget seam loads. The IID carries the same number
// after its last '/', so a plugin built for another version is refused by its
// IID before its ApiVersion is even read.
constexpr int BarWidgetApiVersion = 1;

constexpr int iidVersion(std::string_view iid)
{
    int version = 0;
    for (const char digit : iid.substr(iid.rfind('/') + 1)) {
        if (digit < '0' || digit > '9') {
            return -1;
        }
        version = version * 10 + (digit - '0');
    }
    return version;
}
static_assert(iidVersion(PhosphorRegistryBarWidgetPlugin_iid) == BarWidgetApiVersion,
              "PhosphorRegistryBarWidgetPlugin_iid must end in /<BarWidgetApiVersion>");
} // namespace

PluginSeam PluginLoader::barWidgetSeam()
{
    return PluginSeam{QStringLiteral("bar-widget"), QStringLiteral(PhosphorRegistryBarWidgetPlugin_iid),
                      BarWidgetApiVersion};
}

PluginLoader::PluginLoader(Registry<IBarWidgetFactory>* registry, const QList<PluginSearchPath>& searchPaths,
                           QObject* parent)
    : QObject(parent)
    , m_registry(registry)
    , m_catalog(barWidgetSeam())
{
    if (!m_registry) {
        qFatal("PluginLoader: registry must not be null");
    }
    if (!searchPaths.isEmpty()) {
        m_catalog.setSearchPaths(searchPaths);
    }
    connect(&m_catalog, &PluginCatalog::pluginAdded, this, &PluginLoader::loadPlugin);
    connect(&m_catalog, &PluginCatalog::pluginRemoved, this, &PluginLoader::unloadPlugin);
    connect(&m_catalog, &PluginCatalog::scanCompleted, this, &PluginLoader::rescanCompleted);
}

PluginLoader::~PluginLoader()
{
    // Take this loader's factories out of the registry, which outlives it.
    // The libraries stay mapped (PluginCatalog never unloads), so a factory
    // the registry's other users still hold remains valid.
    QStringList ids = m_factories.keys();
    std::sort(ids.begin(), ids.end());
    for (const QString& id : std::as_const(ids)) {
        unregisterIfOurs(id, m_factories.value(id));
    }
}

void PluginLoader::scanAndLoad()
{
    m_catalog.scan();
}

void PluginLoader::rescanNow()
{
    m_catalog.scan();
}

QStringList PluginLoader::loadedPluginIds() const
{
    QStringList ids = m_factories.keys();
    std::sort(ids.begin(), ids.end());
    return ids;
}

PluginCatalog& PluginLoader::catalog()
{
    return m_catalog;
}

const PluginCatalog& PluginLoader::catalog() const
{
    return m_catalog;
}

void PluginLoader::loadPlugin(const QString& id)
{
    if (m_factories.contains(id)) {
        return;
    }
    // Check before loading: a plugin whose id the registry already holds
    // would be mapped, and its static initialisers run, only to be dropped.
    if (m_registry->factory(id)) {
        qCWarning(lcPluginLoader) << "PluginLoader: bar-widget plugin" << id
                                  << "was not loaded because the registry already has a factory with that id";
        return;
    }
    QString error;
    QObject* const root = m_catalog.load(id, &error);
    if (!root) {
        qCWarning(lcPluginLoader) << "PluginLoader: bar-widget plugin" << id << "did not load:" << error;
        return;
    }
    auto* const plugin = qobject_cast<IBarWidgetPlugin*>(root);
    if (!plugin) {
        qCWarning(lcPluginLoader) << "PluginLoader: bar-widget plugin" << id
                                  << "has a root object that does not implement IBarWidgetPlugin";
        return;
    }
    std::shared_ptr<IBarWidgetFactory> factory = plugin->createFactory();
    if (!factory) {
        qCWarning(lcPluginLoader) << "PluginLoader: bar-widget plugin" << id << "returned no factory";
        return;
    }
    if (factory->id() != id) {
        qCWarning(lcPluginLoader) << "PluginLoader: bar-widget plugin" << id << "returned a factory with id"
                                  << factory->id() << "but the ids must match";
        return;
    }
    // Recorded before registering, so the loader already owns the id while
    // the registry's slots run.
    m_factories.insert(id, factory);
    if (!m_registry->registerFactory(factory)) {
        m_factories.remove(id);
        qCWarning(lcPluginLoader) << "PluginLoader: bar-widget plugin" << id << "was not registered";
        return;
    }
    Q_EMIT pluginLoaded(id);
}

void PluginLoader::unloadPlugin(const QString& id)
{
    const auto it = m_factories.find(id);
    if (it == m_factories.end()) {
        return;
    }
    const std::shared_ptr<IBarWidgetFactory> ours = it.value();
    m_factories.erase(it);
    unregisterIfOurs(id, ours);
    Q_EMIT pluginUnloaded(id);
}

void PluginLoader::unregisterIfOurs(const QString& id, const std::shared_ptr<IBarWidgetFactory>& ours)
{
    // The registry is shared. If another owner replaced this loader's
    // factory under the same id, that entry is not this loader's to remove.
    if (m_registry->factory(id) == ours) {
        m_registry->unregisterFactory(id);
    }
}

} // namespace PhosphorRegistry
