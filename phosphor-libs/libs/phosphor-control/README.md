<!-- SPDX-FileCopyrightText: 2026 fuddlesworth
     SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-control

> Reusable Qt6/QML/Kirigami settings-app framework: the window chrome
> (sidebar, breadcrumbs, footer, page host) plus the orchestration
> (`PageController`, `PageRegistry`, `StagingDomain`, `ApplicationController`)
> that any Kirigami settings app builds on, so each consumer writes only its
> own page controllers and QML pages.

## Responsibility

A settings app on this framework declares a set of pages, each backed by a
staging domain that holds the user's pending edits, and the shell handles the
rest: sidebar navigation, breadcrumbs, a global dirty flag, batched
Apply/Discard across every domain, deep-link reveal, and global search. The
library provides:

- **Staging domains.** `StagingDomain` is the unit of staged work:
  `isDirty()`, `apply()`, `discard()`, `resetToDefaults()`, plus async
  completion signals. `PageController` is a domain that also has a QML page and
  a sidebar row. Headless domains hold cross-cutting state (e.g. per-screen
  assignment maps) with no sidebar entry.
- **A page catalogue.** `PageRegistry` holds the page tree (top-level entries
  plus drill-down or collapsible children), keyed by stable page id, and drives
  the sidebar via serialized `Entry` views.
- **Top-level orchestration.** `ApplicationController` owns the registry and the
  registered domains, recomputes the global dirty flag, drives `applyAll()` /
  `discardAll()` as one transaction (sync or async), dispatches per-page
  `resetToDefaults()`, and latches deep-link anchors for `PageHost` to reveal.
- **Global search.** `SearchController` builds a ranked result index from the
  page registry (Page entries), authored anchors (`addEntry()`), and dynamic
  content (`ISearchProvider`). `SearchRanker` is the pure, headless,
  typo-tolerant scorer behind it.
- **The QML chrome.** `org.phosphor.control` ships `SettingsAppWindow`,
  `Sidebar`, `Breadcrumbs`, `UnsavedChangesFooter`, `PageHost`,
  `DiscardChangesDialog`, `AboutPageShell`, `FacetModulePage`, `FacetSlot` and
  `FacetExtensionFrame`, wired to the controllers above.
- **Glue.** `DBusBridge` for talking to a daemon, and `LocalizedContext` for
  bare-word `i18n()` in QML on Qt-only (non-KF6) builds.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorControl::ApplicationController` | Top-level orchestrator: owns the registry + domains, global dirty flag, batched `applyAllAsync` / `discardAllAsync`, current-page selection, deep-link anchor latch, `bindPage` for pages whose controller arrives late, a single owned navigation hook (`setNavigationHook`) a plugin host loads pages through, and the sections other modules add to a page: a single owned extension provider (`setExtensionProvider`) behind `extensionsFor` / `extensionsChanged`, and `attachDomainToPage` to count a section's domain toward its page's dirty mark and reset |
| `PhosphorControl::StagingDomain`         | Abstract unit of staged changes: `isDirty()`, `apply()`, `discard()`, `resetToDefaults()`, with `applyResult` / `discardResult` async signals |
| `PhosphorControl::PageController`         | A `StagingDomain` that also has a QML page and a sidebar row, with a stable, globally-unique page id |
| `PhosphorControl::PageRegistry`           | Catalogue of registered pages as a tree (`Entry` per page). It grows at runtime and never shrinks: `registerPage` adds (optionally at a sibling slot), `bindPage` swaps a page's controller and QML in place, `setPageVisibility` restamps a tier |
| `PhosphorControl::SearchController`       | Global settings search: ranked results from page registry, authored anchors, and registered providers |
| `PhosphorControl::SearchRanker`           | Pure, stateless, typo-tolerant scoring + ranking; headless and Qt-free beyond `QString` |
| `PhosphorControl::ISearchProvider`        | App-implemented source of dynamic search entries (rules, shaders, layouts, …) |
| `PhosphorControl::SearchEntry`            | One searchable item (Page / Section / Setting / Entity), addressed as `"pageId"` or `"pageId#anchor"` |
| `PhosphorControl::DBusBridge`             | Configured endpoint(s) for daemon calls: sync `call()` / async `asyncCall()`, with explicit-interface variants |
| `PhosphorControl::LocalizedContext`       | QML i18n context routing `i18n()`/`i18nc()`/`i18np()` through Qt translation, for Qt-only consumers without KF6 |
| `PhosphorControl::IFacetModule`           | The interface a Facet module's root object implements (`registerInto(FacetContext&)`), IID `org.phosphor_works.Facet.Module/1` |
| `PhosphorControl::FacetModuleMetadata`    | A module's embedded metadata (id, name, icon, category, parent module, pages and their anchors, extension points, extensions), parsed and validated per locale |
| `PhosphorControl::FacetContext`           | What a module registers into: its page controllers, by local page id, its headless staging domains, and the domain and QML of each section it adds to another module's page. It also hands out one config backend per file, shared by every module that writes it |
| `PhosphorControl::FacetHost`              | Finds Facet modules through a `PhosphorRegistry::PluginCatalog`, fills the page registry with placeholders, resolves extensions against their target pages, and loads a module the first time one of its pages, or a page it extends, is opened |
| `PhosphorControl::FacetSlotModel`         | The sections in one slot of one page, for `FacetSlot`: reads `extensionsFor` again on `extensionsChanged` and keeps the row of a section that stays |
| `PhosphorControl::FacetTestHost`          | Loads one module headless and checks it against the module contract, for the module's own CI. Lives in the separate `PhosphorControl::PhosphorControlTest` library |

## Typical use

A consumer subclasses `ApplicationController` to declare its pages, subclasses
`PageController` for each page, and points the QML `SettingsAppWindow` at the
controller. Titles are translated by the caller, because the library
deliberately provides no translation context for app strings.

```cpp
#include <PhosphorControl/ApplicationController.h>
#include <PhosphorControl/PageController.h>

using namespace PhosphorControl;

// Each page stages its own edits.
class GeneralPage : public PageController
{
    Q_OBJECT
public:
    explicit GeneralPage(QObject* parent = nullptr)
        : PageController(QStringLiteral("general"), parent) {}

    bool isDirty() const override { return m_dirty; }
    void apply() override   { /* persist */ m_dirty = false; Q_EMIT dirtyChanged(); Q_EMIT applyResult(true, {}); }
    void discard() override { /* reload  */ m_dirty = false; Q_EMIT dirtyChanged(); Q_EMIT discardResult(true, {}); }

private:
    bool m_dirty = false;
};

// The app wires its pages up at construction.
class MySettings : public ApplicationController
{
    // Q_OBJECT gives MySettings its own tr() context, which is the one
    // lupdate extracts the string below under.
    Q_OBJECT
public:
    explicit MySettings(QObject* parent = nullptr)
        : ApplicationController(parent)
    {
        auto* general = new GeneralPage(this);
        registerPage(general, {}, MySettings::tr("General"),
                     QUrl(QStringLiteral("qrc:/qt/qml/MyApp/qml/GeneralPage.qml")));
    }
};
```

```qml
// Main.qml — the chrome binds to the controller exposed from C++.
import org.phosphor.control

SettingsAppWindow {
    controller: appController   // your ApplicationController subclass
}
```

The shell drives the dirty/Apply lifecycle from the domains' signals:
`UnsavedChangesFooter` binds to `ApplicationController::dirty` /
`applying` / `discarding`, so a synchronous page just emits
`applyResult(true, {})` at the end of `apply()` and the footer swaps
"Save" → "Saving…" automatically.

See `examples/minimal/` for a complete two-page app (General + About).

## Facet modules

Facet is the suite's one settings app, and every app adds its settings as a
module: a Qt plugin in the `facet` seam whose root object implements
`IFacetModule` and whose metadata (`FacetModuleMetadata`) lists its pages.
`FacetHost` reads every module's metadata without loading it and fills the
page registry before anything loads:

- a node per category in use (`@category/<id>`), a node per module
  (`<module>`), and a page per module page (`<module>:<page>`), ordered by
  `Order` and then name, with a module's own pages parents first and
  siblings in declaration order;
- each module page shows `FacetModulePage.qml` until its module loads, so it
  is an ordinary page in the sidebar, search and keyboard navigation;
- a page can sit under one of its module's pages, under another module's page
  (`<module>:<page>`), or directly in a category, and carries its tier,
  divider and counterpart from the metadata.

Opening one of a module's pages loads the module through
`ApplicationController`'s navigation hook, before the page change is
announced. Its `registerInto` registers into a `FacetContext`, which only
records. Facet then checks the whole registration and binds every page
(`ApplicationController::bindPage`) and domain at once, or, when anything is
wrong (a page or extension the metadata does not declare, a skipped page or
extension, one registered twice, a controller with the wrong id, a page or
section without QML), keeps none of it and fails the module with the
reason. The one call that does more than record is
`FacetContext::configBackend` (below): it shares the file's backend at once,
and the backend stays shared when the registration is not kept. A module that
did not load keeps its placeholder page, which says why, and offers a retry
when loading again can help.

A module that was loading when the process died is disabled, at the next start
and every start after, until `retryModule` (or the placeholder page's retry
button) loads it. A module that failed after its library was loaded stays
failed when its file is offered again: the process keeps running the code it
loaded, so the new file takes effect at the next start. `FacetTestHost` runs a
module through the same host headless, for the module's own CI. It makes the
config files the module opens under its temporary directory
(`FacetHost::setConfigRedirect`), so the module starts from its defaults and
the run does not write the real settings, as long as the module's factory
opens the path it is handed.

A module can also add sections to another module's page. Its metadata lists
each one under `Extensions` with a `Target` (`<module>:<page>`), a `Slot`,
the `TargetVersion` of the slot it was built for (1 by default) and a `Name`.
Every page offers `top`, `bottom`, and `before:<anchor>` / `after:<anchor>`
for each entry of its `Anchors` list, all at the version of the page's
reserved `anchors` extension point (1 when it declares none). A module adds
named slots with its top-level `ExtensionPoints` list, each entry a `Page`, an
`Id` and a `Version` (1 by default). `FacetHost` shows an extension only when
its target page offers that slot at that version. One aimed at a module that
is not installed stays inactive, and one whose target module could not be
placed, or whose page, slot or version does not match, is listed in
`refusedExtensions()` with the reason (`refusedExtensionsChanged` says when
that list changes). Opening a page loads every module that extends it once the
page's own module has loaded, and so does a section that becomes active while
its page is open, or the page's module loading while it is open (a retry). A
section is shown once its extension is active and its page's module has
loaded. From then on its staging domain joins Apply and counts toward that
page's dirty mark and reset, and neither condition is ever undone. Before
that, nothing shows it, so it joins nothing.

`PageHost` places the `top` and `bottom` slots around every page, outside the
page's own scrolling content. The page places the others itself with
`FacetSlot`, which lists them through `FacetSlotModel` and wraps each in a
`FacetExtensionFrame`. The frame names the module the section comes from,
creates the section's QML with its `domain` already set, which the section
must declare (it may make it `required`), and shows a card in the section's
place when the module did not load, the section's domain was deleted or its
QML failed, with a button to the module's own page when the module is what
failed:

```qml
FacetSlot {
    Layout.fillWidth: true
    settingsApp: page.settingsApp
    pageId: "placement:snapping/behavior"
    slot: "after:drag-modifier"
}
```

Modules that keep settings in one file share its backend:
`FacetContext::configBackend(path, create)` creates the backend for the first
module that opens the file and hands the same one to every later module, so
one Apply writes every module's keys into one document and no module
overwrites another's. Each module wraps the backend in its own
`PhosphorConfig::Store` or settings object. A module whose settings belong to
a host that may not be running finds the file with
`PhosphorConfig::HostDirectory`.

The path must be absolute. Paths that lead to one file through symbolic
links, `.` or `..` get one backend, whether or not the file or its directory
exists yet, and `create` is handed the resolved path. Sharing a backend asks
three things of the modules:

- Every `Store` on the backend must be given the same path resolver object
  (`Schema::pathResolver` is compared by pointer, so an equal copy counts as
  another) and the same version stamp. The first `Store`'s stand, and what a
  later one brings instead is ignored, with a warning. A `Store`'s migrations
  run on the whole document.
- A `JsonBackend` allows one live group at a time, whichever module holds it.
  A second one asserts in a debug build and is read-only in a release build,
  so its writes are lost. Open a group, use it and let it go without running
  other modules' code in between.
- `sync()`, `commit()` and `reparseConfiguration()` act on the whole document.
  A write also saves what the other modules have not saved yet, and a reparse
  drops it.

The backends belong to the `ApplicationController`: `FacetHost` keeps them in
an object it parents to the controller, so they outlive the host and a domain
can still be applied after it is gone. The controller destroys them before
the page controllers and domains it adopted from modules, and a `JsonBackend`
writes what was not saved then. A module must therefore not use its backend
from a destructor, nor keep one of its groups in a member. A second
`FacetHost` on the same controller keeps its own backends, and the first's
stay alive with the controller, so two hosts whose modules open one file hold
two documents for it and each write drops the keys only the other holds.

`FacetHost::configBackend(path)` returns the backend a file is shared under,
for a host application or a test that wants to look. Modules get theirs from
the context. `FacetHost::setConfigRedirect(directory)` makes every such file
under `directory` instead, at its own absolute path below it, for a test that
must not touch the real settings.

## Design notes

- **Staging is the contract, not persistence.** `ApplicationController` only
  invokes `apply()` / `discard()` on domains whose `isDirty()` is true, so
  implementations must not rely on side effects that need to run while clean
  (timestamp stamping, notifications). Every domain must emit `applyResult` /
  `discardResult` exactly once per batch. The async driver's pending counter
  parks the chrome's "Saving…" state until it lands or the 60 s timeout fires.
- **Boundary with phosphor-config and phosphor-shortcuts.** This library
  defines no configuration store and no shortcut backend. Consumers wire their
  own `ISettings` against [`phosphor-config`](../phosphor-config/README.md) and
  pull global shortcuts from [`phosphor-shortcuts`](../phosphor-shortcuts/README.md).
  It links `phosphor-config` for one thing: `FacetHost` keeps the `IBackend` of
  each config file Facet modules share, which the first module to open the
  file creates (`FacetContext::configBackend`).
- **Headless-friendly C++ core.** The `PhosphorControl` library avoids
  `Qt6::Quick`, so headless tests can construct `ApplicationController` /
  `PageController` / `StagingDomain` without a QML engine. The QML module is a
  separate target. `Qt6::Gui` is another matter: the library links it
  privately, but `phosphor-config` links it publicly, so it reaches every
  consumer through that dependency.
- **Search is layered.** `SearchRanker` is pure and unit-testable on its own
  (best-tier-per-field scoring, weighted title > keywords > subtitle, edit
  distance only as a "did you mean" fallback). `SearchController` composes it
  with the page registry and providers. The app supplies translated strings, and
  the index stores them verbatim.
- **STATIC QML module in-tree, SHARED on opt-in.** `org.phosphor.control` builds
  STATIC for in-tree consumers (linked directly). Out-of-tree
  `find_package(PhosphorControl)` consumers need the SHARED runtime plugin,
  gated behind `-DPHOSPHOR_CONTROL_QML_INSTALL=ON` (mirrors phosphor-animation).

## Dependencies

- `QtCore`, `QtDBus`, `QtQml`, and `QtGui`, which the library links privately
  and consumers get through `phosphor-config`. The QML module adds `QtQuick`,
  and its QML imports `QtQuick.Controls`, `QtQuick.Layouts` and KF6 Kirigami
  (`org.kde.kirigami`) at run time
- [`phosphor-animation`](../phosphor-animation/README.md) — the QML chrome
  imports `org.phosphor.animation` for the spring-physics motion profiles
  (panel fade and slide, accordion expand/collapse, widget fade, hover and
  tint)
- [`phosphor-registry`](../phosphor-registry/README.md) — `PluginCatalog`, the
  plugin loader `FacetHost` finds and loads Facet modules through
- [`phosphor-config`](../phosphor-config/README.md) — `IBackend`, the config
  backend `FacetContext::configBackend` hands Facet modules, one for each file

## See also

- [`phosphor-config`](../phosphor-config/README.md) — the configuration store an
  app's `ISettings` implementation is wired against, and the backends Facet
  modules share. This library does not duplicate that contract.
- [`phosphor-shortcuts`](../phosphor-shortcuts/README.md) — global-shortcut
  backends, kept out of this library on purpose.
