// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "PhosphorControl/FacetModuleMetadata.h"
#include "phosphorcontrol_export.h"

#include <PhosphorRegistry/PluginCatalog.h>

#include <QCollator>
#include <QHash>
#include <QJsonObject>
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
class FacetContext;
class PageController;
class PageRegistry;
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
/// placeholders, all before the page change is announced. A module that does
/// not load leaves its placeholder up, and the placeholder page shows why,
/// and offers a retry when loading again can help.
///
/// Siblings follow `Order`, then the name: categories at the top level, and
/// modules under their category or parent module. A module's own pages
/// follow their declaration order and come before the modules nested under
/// it; a page that falls back under its module (its target is missing)
/// comes after the others. A module added while Facet runs is slotted in the
/// same way.
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
/// its module then. Both need the navigation hook (start() says when another
/// component holds it). The host answers
/// ApplicationController::extensionsFor() for the page's FacetSlots. A
/// section is shown once its extension is active and its page's own module
/// has loaded, and only then does its staging domain join Apply and the
/// page's dirty mark and reset. Neither condition is ever undone, so a section
/// that joins stays shown.
///
/// A crash while a module loads is remembered. The module's id is written to
/// the load record (setSentinelPath) before its library is loaded, and
/// removed once its registerInto has returned. A load record left behind at
/// the next start disables that module (DisabledAfterCrash), and it stays
/// disabled across starts until retryModule(). A crash later, in a page's
/// or a section's QML or in a domain, is not remembered. Since opening a
/// page loads every module that extends it, a module whose section crashes
/// takes that page down each time it opens, until the module is removed.
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
        /// match its metadata, or its file changed or went away. See
        /// stateReason().
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
    /// after. Default: `$XDG_STATE_HOME/phosphor-works/facet-loading`. When
    /// no state directory is available, or the path is set empty, crashes
    /// are not remembered. One Facet process per load record.
    void setSentinelPath(const QString& path);
    /// The locale module metadata is read and sorted in. Before start(),
    /// ignored with a warning after. Default: QLocale(). The names of the
    /// built-in categories come from the application's installed translators
    /// instead, when start() runs.
    void setLocale(const QLocale& locale);
    /// Turn the catalog's live reload off, for tests. Before start().
    void setLiveReload(bool enabled);

    /// Find the modules and build the placeholder sidebar. Call once.
    void start();

    /// Load @p moduleId now. True when it is loaded, at once or already.
    bool loadModule(const QString& moduleId, QString* errorString = nullptr);
    /// Clear DisabledAfterCrash, or a Failed state that canRetry() allows,
    /// for @p moduleId and load it again.
    bool retryModule(const QString& moduleId, QString* errorString = nullptr);
    /// True when retryModule() would try to load @p moduleId again: it is
    /// DisabledAfterCrash, or it failed while loading and its file has not
    /// changed since. A failure that loading again cannot fix (invalid
    /// metadata, a taken id, a node or page that could not be placed, a
    /// root object that is not an IFacetModule, a file that changed or went
    /// away) is not retryable. When the catalog offers the file again, a
    /// module whose metadata is unchanged and whose pages are all placed can
    /// be loaded again, and one that was never placed (invalid metadata, no
    /// node) is read and placed afresh. Anything else needs a restart.
    [[nodiscard]] bool canRetry(const QString& moduleId) const;

    /// The modules on offer, sorted.
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
    /// The headless domains @p moduleId registered while loading.
    [[nodiscard]] QList<StagingDomain*> moduleDomains(const QString& moduleId) const;
    /// The sections @p moduleId registered while loading, in its metadata's
    /// order, whether or not they are active. A deleted domain reads null.
    [[nodiscard]] QList<ModuleExtension> moduleExtensions(const QString& moduleId) const;
    /// The page ids the module of @p moduleId registered while loading.
    [[nodiscard]] QStringList boundPages(const QString& moduleId) const;

    /// The config backend modules share for the file at @p path, or null
    /// when no module opened it (FacetContext::configBackend).
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
    struct LoadRecord;
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
    void addModule(const QString& moduleId);
    void onPluginAdded(const QString& moduleId);
    void onPluginRemoved(const QString& moduleId);
    void placeModules(QList<std::shared_ptr<Module>> modules);
    bool registerPlaceholder(const QString& id, const QString& parentId, const QString& title, const QString& icon,
                             const QUrl& qmlSource, bool isCollapsible, const Module* module,
                             const FacetPageMetadata* page, qsizetype pageSlot = -1);
    /// The backend modules share for @p path (FacetContext::configBackend),
    /// created with @p create the first time any module opens the file.
    PhosphorConfig::IBackend*
    sharedConfigBackend(const QString& path,
                        const std::function<std::unique_ptr<PhosphorConfig::IBackend>(const QString&)>& create);
    void ensureCategoryNode(const QString& categoryId);
    [[nodiscard]] qsizetype siblingSlot(const QString& parentId,
                                        const std::function<bool(const QString& siblingId)>& sortsAfter) const;
    [[nodiscard]] bool moduleSortsBefore(const Module& a, const Module& b) const;
    [[nodiscard]] bool categorySortsBefore(const FacetCategoryMetadata& a, const FacetCategoryMetadata& b) const;
    void onNavigation(const QString& pageId);
    /// Load the modules that extend @p pageId, once its own module is loaded.
    /// Stops when a newer navigation, which a slot reacting to a load can
    /// start, took the current page anywhere but @p pageId: away from
    /// @p landing, or through another page and back (m_navigations moved on
    /// from @p navigations).
    void loadExtenders(const QString& pageId, const QString& landing, quint64 navigations);
    /// Whether a navigation newer than the one that saw @p landing and
    /// @p navigations left the current page on one other than @p pageId.
    [[nodiscard]] bool navigatedElsewhere(const QString& landing, quint64 navigations, const QString& pageId) const;
    /// @p owner just loaded: join the loaded modules' sections to its pages,
    /// tell those pages' FacetSlots, and load what extends the open page.
    void showSectionsOn(const Module& owner);
    [[nodiscard]] QString owningModule(const QString& pageId) const;
    /// Whether @p moduleId may be loaded now. False, with the reason in
    /// @p errorString, while another load runs or off the controller's thread.
    bool canLoadNow(const QString& moduleId, QString* errorString) const;
    void transition(Module& module, ModuleState state, const QString& reason, bool retryable = true);
    void disableAfterCrash(Module& module);
    [[nodiscard]] LoadRecord readLoadRecord() const;
    bool writeLoadRecord(const LoadRecord& record) const;
    /// Write @p moduleId into the load record as loading. False when it was
    /// not written (no record, or the write failed).
    bool recordLoading(const QString& moduleId);
    /// Take the loading module back out of the record, when recordLoading
    /// put it there (@p recorded).
    void clearLoading(bool recorded);
    bool persistDisabled();
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
    std::unique_ptr<PhosphorRegistry::PluginCatalog> m_catalog;
    QString m_sentinelPath;
    QLocale m_locale;
    /// Sorts names in m_locale. Built once start() has fixed the locale.
    QCollator m_collator;
    bool m_started = false;
    /// True while start() builds the sidebar: transitions are recorded but
    /// announced only once it has finished.
    bool m_starting = false;
    /// Whether start() got the navigation hook, so the host loads modules as
    /// their pages open.
    bool m_holdsNavigationHook = false;
    /// The page a navigation is opening, while onNavigation runs.
    QString m_openingPage;
    /// Counts the navigations the hook saw, so one that went somewhere and
    /// back is still told apart from none.
    quint64 m_navigations = 0;
    /// The module being loaded, from before its library loads until just
    /// before its outcome is announced, so a nested load (a navigation from
    /// inside registerInto or a pageBound slot) is refused instead of running
    /// registerInto twice or overwriting the load record.
    QString m_loadingModule;
    QSet<QString> m_disabled;
    QHash<QString, std::shared_ptr<Module>> m_modules;
    QHash<QString, FacetCategoryMetadata> m_categories;
    QSet<QString> m_registeredCategoryNodes;
    /// Target page id → its active extensions, sorted as extensionsOn() says.
    QHash<QString, QList<ActiveExtension>> m_extensionsByPage;
    QList<RefusedExtension> m_refusedExtensions;
    QSet<QString> m_warnedExtensions;
    // Shared, not unique: QHash values must be copyable. Each backend writes
    // its unsaved changes when it is destroyed.
    QHash<QString, std::shared_ptr<PhosphorConfig::IBackend>> m_configBackends;
};

} // namespace PhosphorControl
