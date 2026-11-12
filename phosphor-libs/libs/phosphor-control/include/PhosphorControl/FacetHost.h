// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "PhosphorControl/FacetModuleMetadata.h"
#include "phosphorcontrol_export.h"

#include <PhosphorRegistry/PluginCatalog.h>

#include <QCollator>
#include <QHash>
#include <QList>
#include <QLocale>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

#include <functional>
#include <memory>
#include <optional>

namespace PhosphorConfig {
class IBackend;
}

namespace PhosphorControl {

class ApplicationController;
class FacetConfigBackends;
class FacetContext;
class FacetLoadRecord;
class ISearchProvider;
class SearchController;
class StagingDomain;

/// Hosts Facet modules in an ApplicationController.
///
/// start() reads every module's metadata through a PluginCatalog over the
/// `facet` seam, without loading any module, and fills the page registry with
/// placeholders: one node per category in use, one per module, and one page
/// per module page, all with their real titles and icons. A module page's
/// placeholder shows placeholderPageSource(), so it is an ordinary page in
/// the sidebar, search and keyboard navigation. Opening a module's page
/// (or navigating to its node) loads the module synchronously, through
/// ApplicationController's navigation hook: its registerInto registers into
/// a FacetContext, and the host then binds its real controllers into the
/// placeholders, all before the page change is announced. A search loads
/// modules too: those whose metadata says `SearchProviders`
/// (setSearchController). A module that does not load leaves its
/// placeholder up, and the placeholder page shows why, and offers a retry
/// when loading again can help.
///
/// Siblings follow `Order`, then the name: categories at the top level, and
/// modules under their category or parent module. A module's own pages
/// follow their declaration order and come before the modules nested under
/// it; a page that falls back under its module (its target is missing)
/// comes after the others. A module added while Facet runs has its node
/// slotted in the same way. The pages it places outside itself, in a category
/// or under another module's page, go after the entries already there. Pages
/// that modules place under each other's in a cycle cannot all go where they
/// ask: the one that sorts first stays under its own module, and the others
/// go under their targets.
///
/// Page ids: a category node is `@category/<id>`, a module node is the module
/// id, and a module page is `<module>:<page>` (FacetContext::pageId). These
/// forms cannot collide with each other, because module ids contain neither
/// `@` nor `:`. A module whose id is already the id of one of the host
/// application's own pages fails instead of being placed.
///
/// Extensions: a module's metadata can add sections to another module's
/// page. start(), and every module added or removed while Facet runs,
/// resolves each against its target page's slots and their versions, and
/// lists the ones that do not fit in refusedExtensions(). An extension aimed
/// at a module that is not installed is skipped quietly. Opening a page also
/// loads every module that extends it, once the page's own module has
/// loaded, and an extension that becomes active while its page is open loads
/// its module then. Only one module loads at a time, so a page that a slot
/// opens while a module is loading gets the modules that extend it when
/// that load ends. Nothing loads while a module that arrived takes its
/// place in the sidebar either, so a page that a slot of that (pageRegistered)
/// opens gets its own module and the modules that extend it once the module
/// is placed. All of this needs the navigation hook (start() says when
/// another component holds it). The host answers
/// ApplicationController::extensionsFor() for the page's FacetSlots. A
/// section is shown once its extension is active and its page's own module
/// has loaded, and only then does its staging domain join Apply and the
/// page's dirty mark and reset. Neither condition is ever undone, so a section
/// that joins stays shown.
///
/// A crash while a module loads is remembered. The module's id is written to
/// the load record (setSentinelPath) before its library is loaded, and
/// removed once its registerInto has returned, the host has checked what it
/// registered and, with a search controller set, has asked each of its
/// search providers for its entries once. A load record left behind at the
/// next start disables that module (DisabledAfterCrash), and it stays
/// disabled across starts until retryModule(). A crash later, in a page's
/// or a section's QML, in a domain, or in a search provider asked again, is
/// not remembered. Since opening a page loads every module that extends it,
/// a module whose section crashes takes that page down each time it opens,
/// until the module is removed.
///
/// The host also keeps the config backends modules share, one for each file,
/// and hands them out through FacetContext::configBackend. They belong to
/// the ApplicationController, in an object the host parents to it when the
/// host is constructed. So they outlive the host, and a domain can still be
/// applied through its backend then. They are destroyed with the controller,
/// before the page controllers and domains it adopted from modules (see
/// FacetContext::configBackend for what that asks of a module). A second
/// host on the same controller keeps its own backends, apart from the
/// first's, and the first's stay alive with the controller. Two hosts whose
/// modules open one file therefore hold two documents for it, and each
/// write drops the keys only the other holds.
///
/// GUI thread only. The ApplicationController must outlive the host.
class PHOSPHORCONTROL_EXPORT FacetHost : public QObject
{
    Q_OBJECT
public:
    enum class ModuleState : quint8 {
        /// Found and on the sidebar, not loaded yet.
        Available,
        Loaded,
        /// Its metadata is invalid, its id or a page id is already taken,
        /// its node or a page could not be placed, its library did not load,
        /// its root object is not an IFacetModule, its registerInto did not
        /// match its metadata, its file changed or went away, or its file
        /// came back after its code had been loaded. See stateReason().
        Failed,
        /// It was loading when Facet last stopped unexpectedly.
        DisabledAfterCrash,
    };
    Q_ENUM(ModuleState)

    /// The `facet` seam: `<root>/facet/`, PhosphorFacetModule_iid,
    /// FacetModuleApiVersion.
    [[nodiscard]] static PhosphorRegistry::PluginSeam seam();

    /// The scoped page id of @p localPageId in @p moduleId.
    [[nodiscard]] static QString pageId(const QString& moduleId, const QString& localPageId);
    /// The page id of category @p categoryId's node.
    [[nodiscard]] static QString categoryNodeId(const QString& categoryId);
    /// The QML page a module page shows while its module is not loaded. It
    /// ships in the org.phosphor.control QML module, which the host
    /// application's chrome already imports.
    [[nodiscard]] static QUrl placeholderPageSource();

    explicit FacetHost(ApplicationController* controller, QObject* parent = nullptr);
    ~FacetHost() override;

    /// Directories to search for modules, highest priority first. Before
    /// start(), ignored with a warning after. Default:
    /// PluginCatalog::defaultSearchPaths("facet").
    void setSearchPaths(const QList<PhosphorRegistry::PluginSearchPath>& paths);
    /// The load record: the file that holds the module being loaded and the
    /// modules disabled after a crash. Before start(), ignored with a warning
    /// after. Default: `$XDG_STATE_HOME/phosphor-works/facet-loading`. A
    /// relative path is made absolute against the working directory of this
    /// call. When no state directory is available, or the path is set empty,
    /// crashes are not remembered. One FacetHost per load record: a second
    /// host, in this process or another, needs a path of its own, or each
    /// writes its disabled list over the other's.
    void setSentinelPath(const QString& path);
    /// Make every config file a module opens (FacetContext::configBackend)
    /// under @p directory instead of where it is, so the backends modules
    /// get do not read or write the real files, as long as each module's
    /// factory opens the path it is handed: for FacetTestHost and other
    /// tests.
    /// The module's factory is handed `<directory>/<path>`, where `<path>` is
    /// the file's absolute path with its symbolic links, `.` and `..`
    /// resolved, and the host creates that file's directory. Two spellings of
    /// one file still share one backend, two files stay apart, and
    /// configBackend() goes on answering for the path the module named.
    /// Before start(), ignored with a warning after. A relative path is made
    /// absolute against the working directory of this call. The filesystem
    /// root is refused with a warning, since it would redirect nothing.
    /// Default: empty, which redirects nothing.
    void setConfigRedirect(const QString& directory);
    /// The locale module metadata is read and sorted in. Before start(),
    /// ignored with a warning after. Default: QLocale() as it is when start()
    /// runs, so a default locale the application sets after making the host
    /// still counts. The names of the built-in categories come from the
    /// application's installed translators instead, when start() runs.
    void setLocale(const QLocale& locale);
    /// Turn the catalog's live reload off, for tests. Before start().
    void setLiveReload(bool enabled);
    /// Make @p search find every module's pages and `Search` entries, and
    /// each active extension as a section of the page it is on, before any
    /// module loads. Before start(), ignored with a warning after. Default:
    /// none, and nothing is searched.
    ///
    /// The host keeps one provider in @p search for all of it, listed in the
    /// order of each entry's page in the sidebar, and adds the entries of
    /// each loaded module's own search providers. It leaves out the entries
    /// of a page whose module failed or was turned off after a crash, and
    /// the sections when another component answers extensionsFor. Whenever
    /// the query is not blank, every module whose metadata says
    /// `SearchProviders` and that has not loaded yet loads, so its entries
    /// are found too: at once, once a load that is running is over, or once
    /// a module that arrived has taken its place in the sidebar. That
    /// covers a query typed before start() and a module that arrives while
    /// the query stands. The host takes its provider out of @p search when
    /// it is destroyed, so the modules' providers are asked only while the
    /// host lives. @p search may go first.
    void setSearchController(SearchController* search);

    /// Find the modules and build the placeholder sidebar. Call once.
    void start();

    /// Load @p moduleId now. True when it is loaded, at once or already.
    /// False, with the reason in @p errorString, for a module that failed or
    /// was turned off after a crash, and for one that cannot load now:
    /// another module is loading, modules are still taking their place in
    /// the sidebar (a pageRegistered slot, in start() or as a module
    /// arrives), or the call is off the GUI thread. A module the load record
    /// keeps turned off after a crash that is still Available, as it is for a
    /// slot that runs once its pages are placed but before addModule has
    /// disabled it (a search results update, say), becomes DisabledAfterCrash
    /// instead of loading.
    bool loadModule(const QString& moduleId, QString* errorString = nullptr);
    /// Clear DisabledAfterCrash, or a Failed state that canRetry() allows,
    /// for @p moduleId and load it again. Any other module is handed to
    /// loadModule(): an Available one loads, a Loaded one answers true, and
    /// a Failed one canRetry() refuses answers false with its reason. When
    /// the load record cannot be written, a module that was disabled after a
    /// crash is retried all the same, with a warning, and the next start may
    /// disable it again. A retry that cannot run now, for the reasons
    /// loadModule() gives, changes nothing: the module keeps its state, and
    /// one turned off after a crash stays off in the load record.
    bool retryModule(const QString& moduleId, QString* errorString = nullptr);
    /// True when retryModule() would try to load @p moduleId again: it is
    /// DisabledAfterCrash, or it failed while loading and its file has not
    /// changed since. False for a module that is Available, which
    /// loadModule() and retryModule() both load (its placeholder page offers
    /// that as a retry too), and for a Loaded one. A failure that loading
    /// again cannot fix (invalid metadata, a taken id, a node or page that
    /// could not be placed, a root object that is not an IFacetModule or was
    /// deleted after its library loaded, a file that changed or went away)
    /// is not retryable. The catalog offers a module's file again when
    /// it is installed again after a removal, and when it changes on disk
    /// (rewritten, or replaced by a rename over it) while its library is not
    /// loaded. Then a module whose metadata is unchanged, whose pages are all
    /// placed and whose library was never loaded can be loaded again, and one
    /// that was never placed (invalid metadata, a taken id, no node) is read
    /// and placed afresh, so fixing its metadata on disk is enough. A module
    /// that had not failed, Available or DisabledAfterCrash, whose file comes
    /// back with other metadata fails then, without its library being
    /// loaded.
    /// Anything else needs a restart. That includes a module that failed
    /// with its library loaded: the process keeps running that code, so the
    /// file offered again cannot be tried.
    /// True does not promise the retry runs now: while another module loads,
    /// or modules take their place in the sidebar, retryModule() refuses it
    /// and changes nothing.
    [[nodiscard]] bool canRetry(const QString& moduleId) const;

    /// Every module the host knows, sorted: the ones on offer, and the ones
    /// the catalog stopped offering, which stay (Failed, or Loaded when they
    /// had loaded). The one exception is a module that was never placed (its
    /// metadata was invalid, an id it needs was taken, or its node could not
    /// be placed) and whose plugin is gone by the time it is read again:
    /// nothing is left of it to keep.
    [[nodiscard]] QStringList moduleIds() const;
    /// The state of @p moduleId, or nullopt for a module the host does not
    /// know.
    [[nodiscard]] std::optional<ModuleState> state(const QString& moduleId) const;
    [[nodiscard]] QString stateReason(const QString& moduleId) const;
    /// @p moduleId's metadata exactly as parsed. Where the host put the
    /// module (a fallback category, a broken nesting cycle) does not change
    /// it.
    [[nodiscard]] std::optional<FacetModuleMetadata> metadata(const QString& moduleId) const;
    /// Module files the catalog refused, with the reason.
    [[nodiscard]] QList<PhosphorRegistry::RefusedPlugin> refusedModules() const;

    /// An extension that will not be shown, and why: its target module is
    /// installed but could not be placed, its target page does not exist or
    /// has no place in the sidebar, the page has no such slot, or offers the
    /// slot at another version. An extension whose target module is not
    /// installed is not refused, only inactive.
    struct RefusedExtension
    {
        /// `<module>:<extension>`.
        QString id;
        QString reason;
    };
    /// Sorted by id. Changes as modules come and go
    /// (refusedExtensionsChanged).
    [[nodiscard]] QList<RefusedExtension> refusedExtensions() const;
    /// The scoped ids of the active extensions on @p pageId, across all its
    /// slots, sorted by `Order`, then module id, then extension id.
    [[nodiscard]] QStringList extensionsOn(const QString& pageId) const;

    /// A section a module registered while loading (FacetContext::
    /// registerExtension).
    struct ModuleExtension
    {
        /// The local extension id.
        QString id;
        QUrl qmlSource;
        StagingDomain* domain = nullptr;
    };
    /// The headless domains @p moduleId registered while loading, in the
    /// order it registered them. One that code the module ran while it
    /// loaded deleted, or that the application refused, reads null, and so
    /// does one deleted since.
    [[nodiscard]] QList<StagingDomain*> moduleDomains(const QString& moduleId) const;
    /// The sections @p moduleId registered while loading, in its metadata's
    /// order, whether or not they are active. A deleted domain reads null.
    [[nodiscard]] QList<ModuleExtension> moduleExtensions(const QString& moduleId) const;
    /// The page ids the module of @p moduleId registered while loading.
    [[nodiscard]] QStringList boundPages(const QString& moduleId) const;
    /// The search providers the module of @p moduleId registered while
    /// loading, but for one that is a QObject and was deleted since.
    [[nodiscard]] QList<ISearchProvider*> searchProviders(const QString& moduleId) const;

    /// The config backend modules share for the file at @p path, an absolute
    /// path in any spelling of the file (FacetContext::configBackend), or
    /// null when no module opened that file, @p path is relative or the
    /// ApplicationController is gone. With setConfigRedirect, @p path is
    /// still the path the module named, not where the file was made. For a
    /// host application, or a test, that wants to see which backend a file
    /// is shared under. A module gets its backend from its FacetContext.
    [[nodiscard]] PhosphorConfig::IBackend* configBackend(const QString& path) const;

Q_SIGNALS:
    void moduleLoaded(const QString& moduleId);
    void moduleFailed(const QString& moduleId, const QString& reason);
    /// Every change of a module's state or reason, including the ones
    /// start() finds (invalid metadata, disabled after a crash), which are
    /// announced once start() has finished building the sidebar.
    void moduleStateChanged(const QString& moduleId, PhosphorControl::FacetHost::ModuleState state);
    /// refusedExtensions() changed.
    void refusedExtensionsChanged();

private:
    // For sharedConfigBackend.
    friend class FacetContext;
    struct Module;
    class ModuleSearch;
    struct SearchBatch;
    /// An extension resolved onto its target page.
    struct ActiveExtension
    {
        QString moduleId;
        QString localId;
        QString slot;
        QString name;
        int order = 0;
    };

    std::shared_ptr<Module> parseModule(const QString& moduleId);
    void mergeCategories(const Module& module);
    /// Read and place @p moduleId. @p previous is the module as the host knew
    /// it until the catalog offered it again (onPluginAdded): Failed, and
    /// never placed. It keeps the state it last announced until reading and
    /// placing it give another, so only a change is announced.
    void addModule(const QString& moduleId, const Module* previous = nullptr);
    void onPluginAdded(const QString& moduleId);
    void onPluginRemoved(const QString& moduleId);
    /// Fail @p module as one the catalog no longer offers: its extensions go
    /// inactive, as if it were not installed.
    void withdrawModule(Module& module);
    void placeModules(QList<std::shared_ptr<Module>> modules);
    bool registerPlaceholder(const QString& id, const QString& parentId, const QString& title, const QString& icon,
                             const QUrl& qmlSource, bool isCollapsible, const Module* module,
                             const FacetPageMetadata* page, qsizetype pageSlot = -1);
    /// Give @p module's placed pages their keywords in the search controller,
    /// once the module is placed. Its static search entries come from
    /// ModuleSearch.
    void indexModule(const Module& module);
    /// Load every module that declares search providers and has not loaded
    /// yet, while the query is not blank. Runs from queryChanged, at the end
    /// of start(), and when a module arrives or is offered again. Called
    /// while it runs, while another module loads, or while modules take their
    /// place, it is owed instead (m_searchProvidersOwed), and paid when that
    /// is over.
    void loadOwedSearchProviders();
    /// Have the search controller rebuild what ModuleSearch lists, now or,
    /// inside a SearchBatch, once the batch is over.
    void invalidateSearch();
    /// The backend modules share for @p path (FacetContext::configBackend),
    /// created with @p create the first time any module opens the file. Null
    /// when @p create made none, or the ApplicationController is gone.
    PhosphorConfig::IBackend*
    sharedConfigBackend(const QString& path,
                        const std::function<std::unique_ptr<PhosphorConfig::IBackend>(const QString&)>& create);
    void ensureCategoryNode(const QString& categoryId);
    [[nodiscard]] qsizetype siblingSlot(const QString& parentId,
                                        const std::function<bool(const QString& siblingId)>& sortsAfter) const;
    [[nodiscard]] bool moduleSortsBefore(const Module& a, const Module& b) const;
    [[nodiscard]] bool categorySortsBefore(const FacetCategoryMetadata& a, const FacetCategoryMetadata& b) const;
    void onNavigation(const QString& pageId);
    /// What a navigation to @p pageId loads: its own module, when it is
    /// Available, then the modules that extend it (loadExtenders). Stops, as
    /// loadExtenders does, when a newer navigation took the current page
    /// elsewhere.
    void openPage(const QString& pageId, const QString& landing, quint64 navigations);
    /// Open the page on screen, as a navigation would, when the navigation
    /// that reached it came while modules took their place and so loaded
    /// nothing (m_navigationOwed), and the page still belongs to the module
    /// it was owed for. Called once a module has taken its place (addModule).
    void loadOwedNavigation();
    /// Load the modules that extend @p pageId, once its own module is loaded.
    /// Stops when a newer navigation, which a slot reacting to a load can
    /// start, took the current page anywhere but @p pageId: away from
    /// @p landing, or through another page and back (m_navigations moved on
    /// from @p navigations). While a module is loading, or modules take their
    /// place, it loads nothing and notes that a page is owed its extenders
    /// (loadOwedExtenders).
    void loadExtenders(const QString& pageId, const QString& landing, quint64 navigations);
    /// Load what extends the page on screen, when loadExtenders could not
    /// while a module was loading or modules took their place
    /// (m_extendersOwed). Called as each load ends, and once a module has
    /// taken its place (addModule).
    void loadOwedExtenders();
    /// Whether a navigation newer than the one that saw @p landing and
    /// @p navigations left the current page on one other than @p pageId.
    [[nodiscard]] bool navigatedElsewhere(const QString& landing, quint64 navigations, const QString& pageId) const;
    /// @p owner just loaded: join the loaded modules' sections to its pages,
    /// tell those pages' FacetSlots, and load what extends the open page.
    void showSectionsOn(const Module& owner);
    [[nodiscard]] QString owningModule(const QString& pageId) const;
    /// Whether @p moduleId may be loaded now. False, with the reason in
    /// @p errorString, while another load runs, while modules take their
    /// place in the sidebar, or off the controller's thread.
    bool canLoadNow(const QString& moduleId, QString* errorString) const;
    void transition(Module& module, ModuleState state, const QString& reason, bool retryable = true);
    void disableAfterCrash(Module& module);
    void resolveExtensions();
    /// Resolve again after a module came or went: attach what became active,
    /// tell the pages whose sections changed, announce a changed refused
    /// list, and load what now extends the open page.
    void updateExtensions();
    /// Register and attach the domains of @p module's sections that are shown:
    /// active, on a page whose own module is loaded.
    void attachExtensions(Module& module);
    void notifyExtendedPages(const QString& moduleId);
    [[nodiscard]] QHash<QString, QStringList> extensionIdsByPage() const;
    /// The first of @p module's pages that is in the sidebar, or empty.
    [[nodiscard]] QString firstPlacedPage(const Module& module) const;
    [[nodiscard]] QVariantList extensionsFor(const QString& pageId, const QString& slot) const;

    QPointer<ApplicationController> m_controller;
    QPointer<SearchController> m_search;
    /// The one provider the host keeps in m_search, from start() on.
    std::unique_ptr<ModuleSearch> m_searchProvider;
    /// True while loadOwedSearchProviders runs.
    bool m_loadingSearchProviders = false;
    /// Set when loadOwedSearchProviders was called while it ran, while a
    /// module loaded, or while modules took their place, and so loaded
    /// nothing.
    bool m_searchProvidersOwed = false;
    /// True while placeModules runs.
    bool m_placingModules = false;
    /// How many SearchBatch scopes are open, and whether one of them held
    /// back an invalidateSearch().
    int m_searchBatches = 0;
    bool m_searchStale = false;
    std::unique_ptr<PhosphorRegistry::PluginCatalog> m_catalog;
    /// The load record (setSentinelPath) and the modules it keeps disabled
    /// after a crash.
    std::unique_ptr<FacetLoadRecord> m_loadRecord;
    /// setLocale(), or QLocale() once start() has run without it.
    std::optional<QLocale> m_locale;
    /// Sorts names in m_locale. Built once start() has fixed the locale.
    QCollator m_collator;
    bool m_started = false;
    /// True while start() builds the sidebar: transitions are recorded but
    /// announced only once it has finished.
    bool m_starting = false;
    /// Whether start() got the navigation hook, so the host loads modules as
    /// their pages open.
    bool m_holdsNavigationHook = false;
    /// Whether start() got the extension provider, so FacetSlots show this
    /// host's sections and search may list them.
    bool m_holdsExtensionProvider = false;
    /// The page openPage is opening, for a navigation or for one owed
    /// (loadOwedNavigation), while it runs.
    QString m_openingPage;
    /// Counts the navigations the hook saw, so one that went somewhere and
    /// back is still told apart from none.
    quint64 m_navigations = 0;
    /// The module being loaded, from before its library loads until just
    /// before its outcome is announced, so a nested load (a navigation from
    /// inside registerInto or a pageBound slot) is refused instead of running
    /// registerInto twice or overwriting the load record.
    QString m_loadingModule;
    /// Set when loadExtenders was called while a module was loading, and so
    /// loaded nothing. Set too while modules took their place. Cleared by
    /// loadOwedExtenders, at the end of that load or once the placing is
    /// over.
    bool m_extendersOwed = false;
    /// Set when the navigation hook ran while modules took their place, and
    /// so loaded nothing: the module that owns the page it opened, empty for
    /// a page no module owns. Paid and cleared by loadOwedNavigation, and
    /// cleared by any later navigation, which loads what it opens itself.
    std::optional<QString> m_navigationOwed;
    QHash<QString, std::shared_ptr<Module>> m_modules;
    QHash<QString, FacetCategoryMetadata> m_categories;
    QSet<QString> m_registeredCategoryNodes;
    /// Target page id → its active extensions, sorted as extensionsOn() says.
    QHash<QString, QList<ActiveExtension>> m_extensionsByPage;
    QList<RefusedExtension> m_refusedExtensions;
    QSet<QString> m_warnedExtensions;
    /// The config backends modules share, and where they are made
    /// (setConfigRedirect). A child of the ApplicationController, which
    /// destroys it, so this is null once the controller is gone and is
    /// checked before every use.
    QPointer<FacetConfigBackends> m_configBackends;
};

} // namespace PhosphorControl
