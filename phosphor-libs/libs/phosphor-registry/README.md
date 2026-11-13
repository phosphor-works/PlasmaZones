<!-- SPDX-FileCopyrightText: 2026 fuddlesworth
     SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-registry

> Generic factory-by-name registry for the five UI extension seams the
> phosphor-shell exposes (bar widgets, control center tiles, launcher
> providers, OSDs and desktop widgets), the plugin catalog every plugin
> seam of the suite shares, and the bar-widget plugin loader built on it.

## Responsibility

Phosphor used to ship several hand-rolled registries
(`phosphor-layout-api/ILayoutSourceFactory`, `phosphor-tiles/AlgorithmRegistry`,
`phosphor-shaders/ShaderRegistry`, `phosphor-animation/AnimationShaderRegistry`),
each designed on its own for its domain. `phosphor-registry` generalises the
pattern into a single template. All four now store their entries in a
`Registry<T>`, and the shell instantiates the same template once per UI seam.

- **One `Registry<T>` template, five interface specialisations.** The shell
  owns one `Registry<IBarWidgetFactory>`, one `Registry<IControlCenterTileFactory>`,
  and so on. Each is independent. Tests and the demos instantiate their
  own local registries with fakes.
- **Built-ins register explicitly.** The shell's composition root calls
  `registry.registerFactory(...)` for every built-in. No static-init magic,
  no global pending list. Same pattern for plugins (the `PluginLoader`
  calls the same `registerFactory`).
- **One plugin mechanism for the suite.** `PluginCatalog` finds the
  plugins of one seam (a kind of plugin, such as bar widgets),
  reads each one's metadata without loading it, and loads one only when
  asked. A plugin is a Qt plugin (`qt_add_plugin` + `Q_PLUGIN_METADATA`)
  whose IID is the seam's and whose metadata carries `"Id"` and
  `"ApiVersion"`. `PluginLoader` is the bar-widget seam on top of it.
- **Search order.** Highest priority first: each directory in
  `PHOSPHOR_WORKS_PLUGIN_PATH` (development, a build tree's plugin
  output) plus `/<seam>`, then `$XDG_DATA_HOME/phosphor-works/plugins/<seam>`,
  then each Qt plugin directory (`QT_PLUGIN_PATH`, then Qt's own) plus
  `/phosphor-works/<seam>` (where packages install). Relative entries are
  skipped. An earlier plugin overrides a later one with the same id, and
  the override is logged.
- **Live reload on disk change.** A `WatchedDirectorySet` (from
  phosphor-fsloader) watches the search paths with a debounced rescan.
  A new plugin is offered and a removed one is withdrawn. A search path
  that does not exist yet is picked up when it appears, unless its
  nearest existing ancestor is a directory the watcher refuses to watch,
  such as `~/.local/share` on a fresh account or `/tmp`. Then the next
  rescan picks it up.
- **Nothing is unloaded.** A withdrawn plugin's library stays mapped so
  objects it created keep working. A loaded plugin keeps running the code
  it was loaded from until the process restarts, and Qt keeps reporting
  that code's metadata for its path. Replace a loaded plugin by rename
  (`install`, `mv`), never by writing over it in place, which changes the
  code the process is running.
- **A plugin that is not loaded is offered again when its file changes.**
  A plugin whose library failed to load, or that was never loaded, stays on
  offer, and `pluginAdded` announces it again once its file is rewritten,
  renamed over, shadowed by a higher-priority copy, or removed while another
  copy takes its place. Rebuilding it is enough. A loaded plugin is never offered again.
- **Capabilities are advisory for now.** Each factory declares a
  `capabilities()` list. An enforcement layer is a future addition.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorRegistry::Registry<T>` | Template registry. Stores factories of type `T` keyed by `T::id()`. Instance-per-composition-root. Emits `factoryRegistered` / `factoryUnregistered` via its `RegistryNotifier`. |
| `PhosphorRegistry::RegistryNotifier` | QObject signal carrier the template owns. The template itself can't be Q_OBJECT, so signals route through this. Reached via `registry.notifier()`. |
| `PhosphorRegistry::IFactoryBase` | Common base for the five interface families. Declares `id()`, `displayName()`, `capabilities()`. |
| `PhosphorRegistry::IBarWidgetFactory` | Factory for one top-bar widget (clock, workspaces, tray, etc.). Adds `createWidget(QQmlEngine*, QObject*)`. |
| `PhosphorRegistry::IControlCenterTileFactory` | Factory for one tile inside the Control Center popout. Adds `createTile(QQmlEngine*, QObject*)`. Consumed by `phosphor-shell-libs/libs/phosphor-shell-control-center`, over the registry `phosphor-shell/src/ControlCenterController` populates. The returned item must be handed to `JavaScriptOwnership`, because the surface destroys the tiles it built. |
| `PhosphorRegistry::ILauncherProvider` | The launcher's query contract, implemented by each provider. Declares `setQuery()`, `results()`, `resultsChanged()` and `activate(id, Primary\|Alternate)`. Returning false from `activate` leaves the surface open. |
| `PhosphorRegistry::ILauncherProviderFactory` | Factory for a launcher query provider (apps, calculator, etc.). Adds `createProvider(QObject*) -> ILauncherProvider*`. Consumed by `phosphor-shell-libs/libs/phosphor-shell-launcher`, over the registry `phosphor-shell/src/LauncherController` populates. |
| `PhosphorRegistry::IOSDFactory` | Factory for an on-screen display (volume / mic / brightness). Adds `createOSD(QQmlEngine*, QObject*)`. **No consumer yet**, reserved for a future OSD surface. |
| `PhosphorRegistry::IDesktopWidgetFactory` | Factory for a desktop card / widget. Adds `createWidget(QQmlEngine*, QObject*)`. **No consumer yet**, reserved for a future desktop-widget surface. |
| `PhosphorRegistry::PluginCatalog` | Finds the plugins of one `PluginSeam` across the search paths, offers their `PluginMetadata` without loading them, records `RefusedPlugin`s with the reason, and loads one on `load(id)`. Hot-reloads on filesystem change. |
| `PhosphorRegistry::IBarWidgetPlugin` | The interface a bar-widget plugin's root object implements: `createFactory() -> std::unique_ptr<IBarWidgetFactory>`. |
| `PhosphorRegistry::PluginLoader` | The bar-widget seam (`barWidgetSeam()`): loads every bar-widget plugin as soon as it is found and registers its factory in a `Registry<IBarWidgetFactory>`, unregistering it when the catalog withdraws the plugin. |

## Typical use

**C++ shell composition root**: register built-ins explicitly, then arm the
plugin loader for any third-party plugins.

```cpp
#include <PhosphorRegistry/PhosphorRegistry.h>

using namespace PhosphorRegistry;

Registry<IBarWidgetFactory> barRegistry;

// Built-ins: explicit register
barRegistry.registerFactory(std::make_shared<ClockFactory>());
barRegistry.registerFactory(std::make_shared<WorkspacesFactory>());
barRegistry.registerFactory(std::make_shared<MediaFactory>());

// Connect first: the first scan loads every plugin before it returns.
QObject::connect(barRegistry.notifier(), &RegistryNotifier::factoryRegistered,
                 [&](const QString& id) { qDebug() << "registered" << id; });

// Plugins, into the same registry:
PluginLoader loader(&barRegistry);  // searches PluginCatalog::defaultSearchPaths("bar-widget")
loader.scanAndLoad();
```

**Glue layer**: the shell wraps the registry in a `QObject` controller
that exposes the factory ids as a `Q_PROPERTY(QStringList)` and the
widget construction as a `Q_INVOKABLE`. The library stays QML-free so
consumers own their own facade. This controller pattern is what both
phosphor-registry demos use. The snippets below name it `BarController`
to match the production-shell shape (a top-bar widget controller).
`phosphor-shell-libs/examples/phosphor-registry-demo/Main.qml` wires the same pattern under
the context-property name `demoController` to keep the demo's QML
self-describing.

```cpp
// barcontroller.h
class BarController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList factoryIds READ factoryIds NOTIFY factoryIdsChanged)
public:
    explicit BarController(QObject* parent = nullptr);
    void setEngine(QQmlEngine* engine); // call once from main()
    Q_INVOKABLE QQuickItem* createWidgetFor(const QString& id, QQuickItem* parent);
    [[nodiscard]] QStringList factoryIds() const;
Q_SIGNALS:
    void factoryIdsChanged();
private:
    QPointer<QQmlEngine> m_engine;
    PhosphorRegistry::Registry<PhosphorRegistry::IBarWidgetFactory> m_registry;
};

// barcontroller.cpp: forward the registry's signals as one factoryIdsChanged
QObject::connect(m_registry.notifier(),
                 &PhosphorRegistry::RegistryNotifier::factoryRegistered,
                 this, &BarController::factoryIdsChanged);
QObject::connect(m_registry.notifier(),
                 &PhosphorRegistry::RegistryNotifier::factoryUnregistered,
                 this, &BarController::factoryIdsChanged);

QStringList BarController::factoryIds() const
{
    // Registry::ids() returns registration (insertion) order, which is
    // already deterministic. We sort here only to present an alphabetical
    // bar layout (a real shell would persist a user-configured order; the
    // demos sort alphabetically for simplicity).
    QStringList ids = m_registry.ids();
    ids.sort();
    return ids;
}

QQuickItem* BarController::createWidgetFor(const QString& id, QQuickItem* parent)
{
    if (!m_engine) return nullptr;
    auto factory = m_registry.factory(id);
    if (!factory) return nullptr;
    return factory->createWidget(m_engine.data(), parent);
}
```

**Bar QML**: a `Repeater` drives `factoryIds` and parents each widget
under its own delegate `Item`. Repeater destroys old delegates on
model change, and QObject's parent-cascade takes the C++-owned widgets
with them (no manual `child.destroy()`, which fails on C++-owned
QQuickItems with "indestructible object" errors).

```qml
Row {
    spacing: 8

    Repeater {
        model: barController ? barController.factoryIds : []

        delegate: Item {
            id: slot
            required property string modelData
            property Item widget: null

            implicitWidth: widget ? widget.implicitWidth : 0
            implicitHeight: widget ? widget.implicitHeight : 0

            Component.onCompleted: {
                slot.widget = barController.createWidgetFor(slot.modelData, slot)
            }
        }
    }
}
```

The bar refreshes automatically when plugins load or unload because
`factoryIdsChanged` re-evaluates the Repeater's `model` binding. See
`phosphor-shell-libs/examples/phosphor-registry-demo/` for the in-process variant and
`phosphor-shell-libs/examples/phosphor-registry-plugin-demo/` for the same pattern with a
third widget loaded from a separate plugin.

**Plugin author side**: a plugin is a Qt plugin whose root object
implements `IBarWidgetPlugin`, installed in a `bar-widget` directory on the
search path.

```cpp
// myplugin.cpp
#include <PhosphorRegistry/IBarWidgetPlugin.h>

#include <new>

class MyFactory : public PhosphorRegistry::IBarWidgetFactory { /* id() returns "my-plugin" */ };

class MyPlugin : public QObject, public PhosphorRegistry::IBarWidgetPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PhosphorRegistryBarWidgetPlugin_iid FILE "metadata.json")
    Q_INTERFACES(PhosphorRegistry::IBarWidgetPlugin)
public:
    std::unique_ptr<PhosphorRegistry::IBarWidgetFactory> createFactory() override
    {
        // createFactory must not throw, so allocate without throwing and
        // return nullptr when that fails.
        return std::unique_ptr<PhosphorRegistry::IBarWidgetFactory>(new (std::nothrow) MyFactory);
    }
};

#include "myplugin.moc"
```

```json
{
    "Id": "my-plugin",
    "ApiVersion": 1
}
```

```cmake
qt_add_plugin(my-plugin SHARED myplugin.cpp)
target_link_libraries(my-plugin PRIVATE PhosphorRegistry::PhosphorRegistry Qt6::Core Qt6::Quick)
install(TARGETS my-plugin LIBRARY DESTINATION ${KDE_INSTALL_PLUGINDIR}/phosphor-works/bar-widget)
```

The library and its directory must be writable by their owner alone. A
umask of 002, the default for user-private groups on several distributions,
leaves both group-writable, and the catalog then refuses the plugin. Run
`chmod go-w` on both, in a build tree too when it is on
`PHOSPHOR_WORKS_PLUGIN_PATH`. The linker writes a new file on every relink,
so a build tree needs that after each build.

## Arbitration / lifecycle

- **Duplicate id rejected.** `registerFactory` of an id already in the
  registry is a no-op + qWarning (the first registration wins). The
  plugin loader asks the registry first and does not load a plugin whose
  id is already registered.
- **Factory id must match the metadata's `"Id"`.** The loader
  cross-checks the factory's `id()` against the plugin's metadata, and a
  mismatch is logged and the factory dropped. This prevents a renamed
  plugin from quietly taking over another's slot. It is not listed in
  `PluginCatalog::refused()`, which holds what the catalog found wrong with
  the file itself.
- **Symlinked plugin files refused.** A plugin file that is a symlink is
  refused with a reason, never followed, so a link in a search path cannot
  point the loader at code outside it. A searched directory that is itself
  a symlink, or sits below one, is followed.
- **Untrusted files refused.** The catalog refuses a plugin file, or one in
  a directory, that someone other than this user or root owns or that is
  group- or world-writable: the StrictModes discipline OpenSSH and sudo
  apply to files they trust. Either would let another local user swap the
  code the shell is about to run. It also refuses a plugin with a
  `<file>.avx2` file beside it, which Qt loads in its place on a CPU with
  the Haswell feature set. The file is vetted again right before it is
  mapped, and every symbol must resolve at load time. Only the plugin file
  and its directory are vetted. Libraries it links (through its RUNPATH
  too) or opens are its own trust decision. Signature / origin
  verification is reserved for a future sandbox.
- **Wrong seam or version refused before loading.** A plugin whose IID is
  not the seam's, which is built against a Qt this process cannot load,
  whose metadata has no valid `"Id"`, or whose integer `"ApiVersion"` is
  not the seam's is refused from its metadata alone, logged once and
  listed in `PluginCatalog::refused()` with the reason. Its library is
  never mapped. Bump a seam's `apiVersion` and its IID's `/N` together for
  any change to its interface or metadata a plugin relies on. A plugin
  built for the old version then fails the IID check first.
- **Unload is registry-drop only.** When the catalog withdraws a plugin
  (its file is removed or now refused) the loader unregisters its factory,
  unless another owner has since replaced it. The library stays mapped for
  the process lifetime, so widgets the withdrawn plugin produced keep
  working until they destruct naturally. A future sandbox revisits this
  with refcounted safe-unload.
- **Capabilities are advisory.** The `capabilities()` list ships in
  every factory, but the loader does not enforce it yet. A future sandbox
  uses the same slot.

## Dependencies

- `QtCore`, `QtQml`. The library does not link `QtGui` or `QtQuick`.
  Consumers that build widgets do.
- `phosphor-fsloader` (PUBLIC link): `WatchedDirectorySet` + `IScanStrategy`
  back both the plugin catalog's live reload and the header-only
  `MetadataPackLoader<T>` template, which exposes fsloader types in its public
  interface, so the dependency is transitive (PUBLIC) for consumers.

## See also

- `phosphor-shell-libs/examples/phosphor-registry-demo/`: toy bar with two built-in widgets
  registered explicitly. Proves the registry seam.
- `phosphor-shell-libs/examples/phosphor-registry-plugin-demo/`: same toy bar plus a third
  widget loaded from a separate plugin. Proves the plugin seam and the
  live reload of plugins added or removed while it runs.
- `phosphor-layout-api/ILayoutSourceFactory`: the pre-existing
  registry pattern this library generalises.
