// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorControl/FacetHost.h"

#include "facetconfigbackends_p.h"
#include "facetloadrecord_p.h"

#include "PhosphorControl/ApplicationController.h"
#include "PhosphorControl/FacetContext.h"
#include "PhosphorControl/FacetModule.h"
#include "PhosphorControl/ISearchProvider.h"
#include "PhosphorControl/PageController.h"
#include "PhosphorControl/PageRegistry.h"
#include "PhosphorControl/SearchController.h"
#include "PhosphorControl/SearchEntry.h"
#include "PhosphorControl/StagingDomain.h"

#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QThread>
#include <QVariantMap>

#include <algorithm>
#include <memory>
#include <utility>

namespace PhosphorControl {

// Declared in facetloadrecord_p.h, which logs under it too.
Q_LOGGING_CATEGORY(lcFacetHost, "phosphorcontrol.facethost")

namespace {
const QString CategoryPrefix = QStringLiteral("@category/");
const QString DefaultCategory = QStringLiteral("apps");

// The reason for a module one of whose placeholders could not be
// registered. Placeholders are placed once, so loading it cannot work until
// Facet restarts.
const QString PageNotPlaced = QStringLiteral("a page of it could not take its place in the sidebar");

// The reason for a module the catalog stopped offering (withdrawModule).
const QString FileRemoved = QStringLiteral("its file was removed or is no longer accepted");

// The reason for a module whose file now carries other metadata than its
// placeholders were built from.
const QString MetadataChanged =
    QStringLiteral("its metadata changed on disk after Facet started, so it can be used only after a restart");

// The categories every Facet has. Their names are marked for extraction here
// and translated when start() builds the sidebar, through whatever
// translators the application has installed by then.
struct BaseCategory
{
    const char* id;
    const char* name;
    const char* icon;
    int order;
};
constexpr BaseCategory BaseCategories[] = {
    {"window-management", QT_TRANSLATE_NOOP("PhosphorControl::FacetHost", "Window Management"),
     "preferences-system-windows", 10},
    {"appearance", QT_TRANSLATE_NOOP("PhosphorControl::FacetHost", "Appearance"), "preferences-desktop-theme-global",
     20},
    {"desktop", QT_TRANSLATE_NOOP("PhosphorControl::FacetHost", "Desktop"), "preferences-desktop", 30},
    {"input", QT_TRANSLATE_NOOP("PhosphorControl::FacetHost", "Input"), "preferences-desktop-peripherals", 40},
    {"apps", QT_TRANSLATE_NOOP("PhosphorControl::FacetHost", "Apps"), "preferences-desktop-default-applications", 50},
    {"system", QT_TRANSLATE_NOOP("PhosphorControl::FacetHost", "System"), "preferences-system", 60},
};

// Stands in for a page whose module has not loaded, and for the structural
// category and module nodes. Never dirty, and Apply and Discard succeed at
// once, so it never holds up a batch (and the batches skip clean domains
// anyway). The module page placeholders show FacetModulePage.qml, which reads
// the properties below to say why the module is not there and to offer a
// retry.
class FacetPlaceholderPage final : public PageController
{
    Q_OBJECT
    Q_PROPERTY(QString moduleName READ moduleName CONSTANT)
    Q_PROPERTY(bool failed READ failed NOTIFY moduleStateChanged)
    Q_PROPERTY(bool disabledAfterCrash READ disabledAfterCrash NOTIFY moduleStateChanged)
    Q_PROPERTY(QString reason READ reason NOTIFY moduleStateChanged)
    Q_PROPERTY(bool canRetry READ canRetry NOTIFY moduleStateChanged)

public:
    FacetPlaceholderPage(const QString& id, FacetHost* host, QString moduleId, QString moduleName, QObject* parent)
        : PageController(id, parent)
        , m_host(host)
        , m_moduleId(std::move(moduleId))
        , m_moduleName(std::move(moduleName))
    {
        if (m_host && !m_moduleId.isEmpty()) {
            connect(m_host, &FacetHost::moduleStateChanged, this, [this](const QString& changed) {
                if (changed == m_moduleId) {
                    Q_EMIT moduleStateChanged();
                }
            });
            connect(m_host, &QObject::destroyed, this, &FacetPlaceholderPage::moduleStateChanged);
        }
    }

    bool isDirty() const override
    {
        return false;
    }
    void apply() override
    {
        Q_EMIT applyResult(true, QString());
    }
    void discard() override
    {
        Q_EMIT discardResult(true, QString());
    }

    QString moduleName() const
    {
        return m_moduleName;
    }
    bool failed() const
    {
        return m_host && m_host->state(m_moduleId) == FacetHost::ModuleState::Failed;
    }
    bool disabledAfterCrash() const
    {
        return m_host && m_host->state(m_moduleId) == FacetHost::ModuleState::DisabledAfterCrash;
    }
    QString reason() const
    {
        return m_host ? m_host->stateReason(m_moduleId) : QString();
    }
    // Also true for a module that is still Available: it is on screen
    // without having loaded when the navigation hook could not load it (the
    // hook is held by another component, or another module was loading), and
    // retry() loads it.
    bool canRetry() const
    {
        return m_host
            && (m_host->canRetry(m_moduleId) || m_host->state(m_moduleId) == FacetHost::ModuleState::Available);
    }

    Q_INVOKABLE void retry()
    {
        if (m_host) {
            m_host->retryModule(m_moduleId);
        }
    }

Q_SIGNALS:
    void moduleStateChanged();

private:
    QPointer<FacetHost> m_host;
    QString m_moduleId;
    QString m_moduleName;
};

} // namespace

struct FacetHost::Module
{
    /// As parsed. Where the host puts the module never changes it.
    FacetModuleMetadata metadata;
    /// The metadata as scanned, to notice a module that changed on disk
    /// between start() and its first load.
    QJsonObject rawMetadata;
    ModuleState state = ModuleState::Available;
    QString reason;
    /// Whether a Failed state may be retried (canRetry).
    bool retryable = true;
    /// Counts its transitions, so the code announcing one notices when a slot
    /// of that announcement made another.
    quint64 transitions = 0;
    /// The category the module's node went under, empty when it is nested
    /// under another module.
    QString category;
    /// The module node it is nested under, empty for its category.
    QString nodeParent;
    bool nodeRegistered = false;
    /// False once the catalog stopped offering a module that never loaded.
    /// Its extensions are then inactive, as if it were not installed.
    bool installed = true;
    QStringList boundPages;
    /// Its headless domains, in the order it registered them. One deleted
    /// while it loaded, or refused by the application, is a null entry.
    QList<QPointer<StagingDomain>> domains;
    /// Its search providers, kept when it loaded. One that is a QObject and
    /// was deleted since is skipped.
    QList<FacetContext::SearchProviderRegistration> searchProviders;
    struct Extension
    {
        QPointer<StagingDomain> domain;
        QUrl qmlSource;
        /// Registered with the controller, which happens once the extension
        /// is active and so has a page to count toward.
        bool registered = false;
    };
    /// The sections it registered while loading, by local extension id.
    QHash<QString, Extension> extensions;
};

// The one provider the host keeps in its SearchController: every placed
// module's static search entries, each active extension as a section of the
// page it is on, at an anchor that is its scoped id, and the entries of the
// loaded modules' own providers. One provider rather than static entries and a
// provider per module, because SearchController can drop no static entry, and
// lists entries in the order it was handed them, which breaks score ties and
// decides what a limit cuts. Here every entry comes in the order of its page in
// the sidebar: the page's own entries, then its sections, then what providers
// list for it, a module's before those of the modules after it in the sidebar.
// Actions, which have no page, come last.
//
// A page whose module failed or was turned off after a crash shows that
// module's placeholder, so nothing on it is listed, a section included. A
// section of a module that failed or was turned off stays: its slot shows the
// module's error card in the section's place, at the same anchor.
//
// The host owns it and takes it out of the controller as it goes, so nothing
// here runs once the host is gone.
class FacetHost::ModuleSearch final : public ISearchProvider
{
public:
    explicit ModuleSearch(const FacetHost& host)
        : m_host(host)
    {
    }

    QVector<SearchEntry> searchEntries() const override
    {
        QVector<SearchEntry> entries;
        if (!m_host.m_controller) {
            return entries;
        }
        const QList<const PageRegistry::Entry*> pages = m_host.m_controller->registry()->pagesInTreeOrder();
        QHash<QString, qsizetype> position;
        position.reserve(pages.size());
        for (qsizetype i = 0; i < pages.size(); ++i) {
            position.insert(pages.at(i)->id, i);
        }
        QList<QVector<SearchEntry>> byPage(pages.size());
        // Actions, and an entry whose page is not in the sidebar, which
        // SearchController drops with a warning.
        QVector<SearchEntry> rest;
        const auto place = [&position, &byPage, &rest](SearchEntry&& entry) {
            const auto at =
                entry.kind == SearchEntry::Kind::Action ? position.cend() : position.constFind(entry.pageId);
            if (at == position.cend()) {
                rest.append(std::move(entry));
            } else {
                byPage[*at].append(std::move(entry));
            }
        };
        const auto shown = [this](const QString& pageId) {
            const std::shared_ptr<Module> owner = m_host.m_modules.value(m_host.owningModule(pageId));
            return owner && (owner->state == ModuleState::Loaded || owner->state == ModuleState::Available);
        };

        // The placed modules in sidebar order, by their node.
        QList<const Module*> modules;
        for (auto it = m_host.m_modules.cbegin(); it != m_host.m_modules.cend(); ++it) {
            if (it.value()->nodeRegistered) {
                modules.append(it.value().get());
            }
        }
        std::sort(modules.begin(), modules.end(), [&position](const Module* a, const Module* b) {
            return position.value(a->metadata.id, -1) < position.value(b->metadata.id, -1);
        });

        for (const Module* module : std::as_const(modules)) {
            for (const FacetSearchEntryMetadata& item : module->metadata.search) {
                SearchEntry entry;
                entry.kind = item.section ? SearchEntry::Kind::Section : SearchEntry::Kind::Setting;
                entry.pageId = pageId(module->metadata.id, item.page);
                entry.anchor = item.anchor;
                entry.title = item.title;
                entry.keywords = item.keywords;
                entry.advancedOnly = item.advancedOnly;
                if (shown(entry.pageId)) {
                    place(std::move(entry));
                }
            }
        }

        // Only when this host's sections are the ones FacetSlots show.
        if (m_host.m_holdsExtensionProvider) {
            for (auto it = m_host.m_extensionsByPage.cbegin(); it != m_host.m_extensionsByPage.cend(); ++it) {
                if (!shown(it.key())) {
                    continue;
                }
                for (const ActiveExtension& active : it.value()) {
                    const std::shared_ptr<Module> module = m_host.m_modules.value(active.moduleId);
                    const FacetExtensionMetadata* const declared =
                        module ? module->metadata.extension(active.localId) : nullptr;
                    SearchEntry entry;
                    entry.kind = SearchEntry::Kind::Section;
                    entry.pageId = it.key();
                    entry.anchor = pageId(active.moduleId, active.localId);
                    entry.title = active.name;
                    entry.keywords = declared ? declared->keywords : QStringList();
                    place(std::move(entry));
                }
            }
        }

        for (const Module* module : std::as_const(modules)) {
            if (module->state != ModuleState::Loaded || module->searchProviders.isEmpty()) {
                continue;
            }
            QSet<QString> extended;
            for (const FacetExtensionMetadata& extension : module->metadata.extensions) {
                extended.insert(extension.target);
            }
            for (const FacetContext::SearchProviderRegistration& registration : module->searchProviders) {
                if (registration.isObject && !registration.object) {
                    continue;
                }
                const QVector<SearchEntry> provided = registration.provider->searchEntries();
                for (SearchEntry entry : provided) {
                    if (entry.kind == SearchEntry::Kind::Action) {
                        place(std::move(entry));
                        continue;
                    }
                    if (m_host.owningModule(entry.pageId) != module->metadata.id && !extended.contains(entry.pageId)) {
                        // Another module's page, or the host's: once per page,
                        // not once per rebuild.
                        const QString key = module->metadata.id + QLatin1Char('\x1f') + entry.pageId;
                        if (!m_warned.contains(key)) {
                            m_warned.insert(key);
                            qCWarning(lcFacetHost).noquote()
                                << "FacetHost: module" << module->metadata.id << "lists search entries on page"
                                << entry.pageId << "which is neither its own nor one it extends; they are left out";
                        }
                        continue;
                    }
                    // A page it extends whose module is not installed is not
                    // in the sidebar: nothing to find there, and not an error.
                    if (!position.contains(entry.pageId) || !shown(entry.pageId)) {
                        continue;
                    }
                    place(std::move(entry));
                }
            }
        }

        for (QVector<SearchEntry>& onPage : byPage) {
            entries.append(std::move(onPage));
        }
        entries.append(std::move(rest));
        return entries;
    }

private:
    const FacetHost& m_host;
    /// The module and page pairs already warned about.
    mutable QSet<QString> m_warned;
};

// Holds back the host's invalidateSearch() calls while it runs, and makes one
// at its end when any was held back, so a run of loads rebuilds the index
// once.
struct FacetHost::SearchBatch
{
    explicit SearchBatch(FacetHost& host)
        : m_host(host)
    {
        ++m_host.m_searchBatches;
    }
    ~SearchBatch()
    {
        if (--m_host.m_searchBatches == 0 && std::exchange(m_host.m_searchStale, false)) {
            m_host.invalidateSearch();
        }
    }
    Q_DISABLE_COPY_MOVE(SearchBatch)

private:
    FacetHost& m_host;
};

PhosphorRegistry::PluginSeam FacetHost::seam()
{
    return PhosphorRegistry::PluginSeam{QStringLiteral("facet"), QStringLiteral(PhosphorFacetModule_iid),
                                        FacetModuleApiVersion};
}

QString FacetHost::pageId(const QString& moduleId, const QString& localPageId)
{
    return moduleId + QLatin1Char(':') + localPageId;
}

QString FacetHost::categoryNodeId(const QString& categoryId)
{
    return CategoryPrefix + categoryId;
}

QUrl FacetHost::placeholderPageSource()
{
    return QUrl(QStringLiteral("qrc:/qt/qml/org/phosphor/control/qml/FacetModulePage.qml"));
}

FacetHost::FacetHost(ApplicationController* controller, QObject* parent)
    : QObject(parent)
    , m_controller(controller)
    , m_catalog(std::make_unique<PhosphorRegistry::PluginCatalog>(seam()))
    , m_loadRecord(std::make_unique<FacetLoadRecord>())
{
    if (!m_controller) {
        qFatal("FacetHost: controller must not be null");
    }
    // First, so the backends are the controller's child ahead of everything
    // it adopts from a module (FacetConfigBackends says why).
    m_configBackends = new FacetConfigBackends(m_controller);
}

FacetHost::~FacetHost()
{
    // First, so a live query's results drop this host's entries at once and
    // the controller never asks a provider of a module this host loaded once
    // the host is gone.
    if (m_search && m_searchProvider) {
        m_search->unregisterProvider(m_searchProvider.get());
    }
    if (!m_controller) {
        return;
    }
    m_controller->clearNavigationHook(this);
    m_controller->clearExtensionProvider(this);
    // The slots on those pages read the now empty provider. The sections'
    // domains stay registered and attached, so what they stage is still
    // applied and still marks its page. The config backends they write
    // through stay too: m_configBackends belongs to the controller.
    const QStringList pages = m_extensionsByPage.keys();
    m_extensionsByPage.clear();
    for (const QString& page : pages) {
        m_controller->notifyExtensionsChanged(page);
    }
}

void FacetHost::setSearchPaths(const QList<PhosphorRegistry::PluginSearchPath>& paths)
{
    if (m_started) {
        qCWarning(lcFacetHost) << "FacetHost: setSearchPaths after start() is ignored";
        return;
    }
    m_catalog->setSearchPaths(paths);
}

void FacetHost::setSentinelPath(const QString& path)
{
    if (m_started) {
        qCWarning(lcFacetHost) << "FacetHost: setSentinelPath after start() is ignored";
        return;
    }
    m_loadRecord->setPath(path);
}

void FacetHost::setConfigRedirect(const QString& directory)
{
    if (m_started) {
        qCWarning(lcFacetHost) << "FacetHost: setConfigRedirect after start() is ignored";
        return;
    }
    // Absolute, like the load record's path. Empty stays empty: no redirect.
    const QString absolute = directory.isEmpty() ? QString() : QDir(directory).absolutePath();
    if (!absolute.isEmpty() && QDir(absolute).isRoot()) {
        // Every file would land back where it already is.
        qCWarning(lcFacetHost) << "FacetHost: setConfigRedirect to the filesystem root would redirect nothing, ignored";
        return;
    }
    if (m_configBackends) {
        m_configBackends->setRedirect(absolute);
    }
}

void FacetHost::setLocale(const QLocale& locale)
{
    if (m_started) {
        qCWarning(lcFacetHost) << "FacetHost: setLocale after start() is ignored";
        return;
    }
    m_locale = locale;
}

void FacetHost::setLiveReload(bool enabled)
{
    if (m_started) {
        qCWarning(lcFacetHost) << "FacetHost: setLiveReload after start() is ignored";
        return;
    }
    m_catalog->setLiveReload(enabled);
}

void FacetHost::setSearchController(SearchController* search)
{
    if (m_started) {
        qCWarning(lcFacetHost) << "FacetHost: setSearchController after start() is ignored";
        return;
    }
    m_search = search;
}

void FacetHost::start()
{
    if (m_started) {
        qCWarning(lcFacetHost) << "FacetHost: start() called twice";
        return;
    }
    if (!m_controller) {
        qCWarning(lcFacetHost) << "FacetHost: start() called after the ApplicationController was destroyed";
        return;
    }
    m_started = true;
    m_starting = true;
    // QLocale() as it is now, not as it was when the host was made: the
    // application may set the default locale in between.
    if (!m_locale) {
        m_locale = QLocale();
    }
    m_collator = QCollator(*m_locale);

    for (const BaseCategory& base : BaseCategories) {
        const QString id = QString::fromLatin1(base.id);
        m_categories.insert(id,
                            FacetCategoryMetadata{id,
                                                  QCoreApplication::translate("PhosphorControl::FacetHost", base.name),
                                                  QString::fromLatin1(base.icon), base.order});
    }

    m_loadRecord->restore();

    QStringList found;
    const QMetaObject::Connection collect =
        connect(m_catalog.get(), &PhosphorRegistry::PluginCatalog::pluginAdded, this, [&found](const QString& id) {
            found.append(id);
        });
    m_catalog->scan();
    disconnect(collect);
    std::sort(found.begin(), found.end());

    QList<std::shared_ptr<Module>> parsed;
    for (const QString& id : std::as_const(found)) {
        const std::shared_ptr<Module> module = parseModule(id);
        if (!module) {
            continue;
        }
        m_modules.insert(id, module);
        if (module->state == ModuleState::Available) {
            parsed.append(module);
        }
    }

    // A disabled module that is not installed now stays on the list, so it
    // comes back disabled if it is installed again (at the next start or
    // while Facet runs). The list grows only by modules that crashed Facet.

    placeModules(parsed);
    for (const std::shared_ptr<Module>& module : std::as_const(parsed)) {
        if (module->state == ModuleState::Available && m_loadRecord->isDisabled(module->metadata.id)) {
            disableAfterCrash(*module);
        }
    }

    resolveExtensions();
    m_holdsExtensionProvider =
        m_controller->setExtensionProvider(this, [this](const QString& pageId, const QString& slot) {
            return extensionsFor(pageId, slot);
        });
    if (!m_holdsExtensionProvider) {
        qCWarning(lcFacetHost) << "FacetHost: another component answers extensionsFor, so no FacetSlot shows this "
                                  "host's sections, and search does not list them";
    }
    if (m_search) {
        for (const std::shared_ptr<Module>& module : std::as_const(parsed)) {
            indexModule(*module);
        }
        m_searchProvider = std::make_unique<ModuleSearch>(*this);
        m_search->registerProvider(m_searchProvider.get());
        connect(m_search.data(), &SearchController::queryChanged, this, &FacetHost::loadOwedSearchProviders);
    }

    connect(m_catalog.get(), &PhosphorRegistry::PluginCatalog::pluginAdded, this, &FacetHost::onPluginAdded);
    connect(m_catalog.get(), &PhosphorRegistry::PluginCatalog::pluginRemoved, this, &FacetHost::onPluginRemoved);
    m_holdsNavigationHook = m_controller->setNavigationHook(this, [this](const QString& pageId) {
        onNavigation(pageId);
    });
    if (!m_holdsNavigationHook) {
        qCWarning(lcFacetHost) << "FacetHost: another component holds the navigation hook, so opening a page loads "
                                  "neither its module nor the modules that extend it; call loadModule() instead";
    }

    // Announce what start() found, now that every placeholder and the hook
    // exist, so a slot reacting to it sees a complete sidebar. A slot can
    // move a module on (a retry), and transition() has then announced that
    // module itself. So what start() found is taken down first, and each
    // module is announced only while it is still as found.
    m_starting = false;
    struct Finding
    {
        QString id;
        std::shared_ptr<Module> module;
        quint64 transitions;
    };
    QList<Finding> findings;
    for (const QString& id : std::as_const(found)) {
        const std::shared_ptr<Module> module = m_modules.value(id);
        if (module && module->state != ModuleState::Available) {
            findings.append(Finding{id, module, module->transitions});
        }
    }
    const auto asFound = [this](const Finding& finding) {
        return m_modules.value(finding.id) == finding.module && finding.module->transitions == finding.transitions;
    };
    for (const Finding& finding : std::as_const(findings)) {
        if (!asFound(finding)) {
            continue;
        }
        const ModuleState state = finding.module->state;
        const QString reason = finding.module->reason;
        Q_EMIT moduleStateChanged(finding.id, state);
        if (state == ModuleState::Failed && asFound(finding)) {
            Q_EMIT moduleFailed(finding.id, reason);
        }
    }
    // A query typed before start(): its modules with providers load now that
    // the sidebar is complete and announced.
    loadOwedSearchProviders();
}

std::shared_ptr<FacetHost::Module> FacetHost::parseModule(const QString& moduleId)
{
    const std::optional<PhosphorRegistry::PluginMetadata> plugin = m_catalog->plugin(moduleId);
    if (!plugin) {
        return nullptr;
    }
    auto module = std::make_shared<Module>();
    module->rawMetadata = plugin->metaData;
    QStringList errors;
    QStringList warnings;
    // Only from start() on, which has fixed the locale.
    module->metadata = FacetModuleMetadata::parse(plugin->metaData, *m_locale, &errors, &warnings);
    for (const QString& warning : std::as_const(warnings)) {
        qCInfo(lcFacetHost).noquote() << "FacetHost: module" << moduleId << "metadata:" << warning;
    }
    if (!errors.isEmpty()) {
        // Recorded on the module before it is in m_modules, so transition()
        // would not find a listener-visible module yet. Set it directly; the
        // callers announce it.
        module->state = ModuleState::Failed;
        module->reason = QStringLiteral("invalid metadata: %1").arg(errors.join(QStringLiteral("; ")));
        module->retryable = false;
        qCWarning(lcFacetHost).noquote() << "FacetHost: module" << moduleId << "failed:" << module->reason;
        return module;
    }
    mergeCategories(*module);
    return module;
}

void FacetHost::mergeCategories(const Module& module)
{
    for (const FacetCategoryMetadata& category : module.metadata.categories) {
        const auto existing = m_categories.constFind(category.id);
        if (existing == m_categories.cend()) {
            m_categories.insert(category.id, category);
            continue;
        }
        // The first definition stands: the built-in categories, then modules
        // in id order. Say so when a later one disagrees.
        if (existing->name != category.name || existing->icon != category.icon || existing->order != category.order) {
            qCInfo(lcFacetHost).noquote() << "FacetHost: module" << module.metadata.id << "defines category"
                                          << category.id << "differently from an earlier definition, which stands";
        }
    }
}

void FacetHost::addModule(const QString& moduleId, const Module* previous)
{
    const std::shared_ptr<Module> module = parseModule(moduleId);
    if (!module) {
        return;
    }
    m_modules.insert(moduleId, module);
    if (module->state == ModuleState::Failed) {
        // Offered again and broken the same way: nothing changed, so nothing
        // is announced.
        const bool unchanged = previous && previous->state == module->state && previous->reason == module->reason
            && previous->retryable == module->retryable;
        if (!unchanged) {
            Q_EMIT moduleStateChanged(moduleId, module->state);
            Q_EMIT moduleFailed(moduleId, module->reason);
        }
        // Installed, though not placed: an extension aimed at it is refused
        // from now on, where one aimed at a module that is not installed is
        // only inactive.
        updateExtensions();
        return;
    }
    // A new module starts Available, which is not announced. One offered
    // again stays as it was last announced until placing it says otherwise,
    // so transition() announces only a real change: failing to place it for
    // the same reason as before says nothing.
    if (previous) {
        module->state = previous->state;
        module->reason = previous->reason;
        module->retryable = previous->retryable;
    }
    const quint64 transitions = module->transitions;
    placeModules({module});
    indexModule(*module);
    // Its static search entries join the index (ModuleSearch), before anyone
    // hears of it.
    invalidateSearch();
    // Placed: its node is up, and nothing failed it on the way.
    const bool placed = module->nodeRegistered && module->transitions == transitions;
    if (placed && m_loadRecord->isDisabled(moduleId)) {
        disableAfterCrash(*module);
    } else if (placed && previous) {
        transition(*module, ModuleState::Available, QString());
    }

    // The new module can be the target that makes another module's extension
    // active, or extend pages itself.
    updateExtensions();
    // With a query standing, a module with search providers loads at once, so
    // its entries are found.
    loadOwedSearchProviders();
}

void FacetHost::onPluginAdded(const QString& moduleId)
{
    if (!m_controller) {
        return;
    }
    const std::shared_ptr<Module> existing = m_modules.value(moduleId);
    if (!existing) {
        addModule(moduleId);
        return;
    }
    if (existing->state == ModuleState::Available || existing->state == ModuleState::DisabledAfterCrash) {
        // Its file changed on disk while its library was not loaded. With the
        // same metadata the next load maps the new file, which fits the
        // placeholders. With other metadata it never can, so it fails now,
        // without mapping a library whose pages do not match the sidebar. A
        // load in progress checks the file it maps itself.
        const std::optional<PhosphorRegistry::PluginMetadata> plugin = m_catalog->plugin(moduleId);
        if (moduleId != m_loadingModule && plugin && plugin->metaData != existing->rawMetadata) {
            transition(*existing, ModuleState::Failed, MetadataChanged, false);
        }
        return;
    }
    // A loaded plugin is never offered again (PluginCatalog keeps its code).
    if (existing->state != ModuleState::Failed) {
        return;
    }
    // Offered again, its extensions count once more, whatever state the
    // checks below leave it in. Resolved again in every case, also for a
    // module that was never removed (its library did not load, and its file
    // was replaced): going back to Available takes its error card off the
    // pages it extends, and only this loads it for the one that is open.
    existing->installed = true;
    // Declared first, so it runs last: back to Available with a query
    // standing, it loads for its search providers like a module that just
    // arrived (addModule), once its extensions are resolved again.
    const auto loadOnceSettled = qScopeGuard([this] {
        loadOwedSearchProviders();
    });
    auto updateOnceSettled = qScopeGuard([this] {
        updateExtensions();
    });
    if (!existing->nodeRegistered) {
        // It was never placed (its metadata was invalid, an id it needs was
        // taken, or its node could not be placed), so it has no placeholders
        // yet. Read it again and place it if it can be now. addModule
        // resolves the extensions itself once it has kept the module. When
        // the catalog no longer has the plugin it keeps nothing, and the
        // guard resolves them instead, for the extensions that aimed at the
        // module.
        m_modules.remove(moduleId);
        addModule(moduleId, existing.get());
        if (m_modules.contains(moduleId)) {
            updateOnceSettled.dismiss();
        }
        return;
    }
    if (m_catalog->isLoaded(moduleId)) {
        // It failed after its library was mapped (its registerInto broke the
        // contract, say), and PluginCatalog::load hands out that same code
        // for as long as the process runs, whatever file is on disk now. A
        // retry would run the old code and fail the same way. A library that
        // never loaded is not mapped, and goes on to be tried again below.
        transition(*existing, ModuleState::Failed,
                   QStringLiteral("its file was offered again after its code had been loaded, so the file now on "
                                  "disk can be used only after a restart"),
                   false);
        return;
    }
    // Offered again: its file changed while its library was not loaded, or it
    // was removed and installed again. Its placeholders are already in the
    // sidebar and cannot move, so it can be tried again only when its
    // metadata still matches them and all of them are there.
    const std::optional<PhosphorRegistry::PluginMetadata> plugin = m_catalog->plugin(moduleId);
    const auto allPlaced = [this, &existing] {
        for (const FacetPageMetadata& page : existing->metadata.pages) {
            if (!m_controller->registry()->hasPage(pageId(existing->metadata.id, page.id))) {
                return false;
            }
        }
        return true;
    };
    if (plugin && plugin->metaData == existing->rawMetadata && !allPlaced()) {
        // Back to the reason it failed in the first place (a removal in
        // between replaced it).
        transition(*existing, ModuleState::Failed, PageNotPlaced, false);
        return;
    }
    if (plugin && plugin->metaData == existing->rawMetadata) {
        if (m_loadRecord->isDisabled(moduleId)) {
            // Removing and reinstalling a module that crashed Facet does not
            // turn it back on; only a retry does.
            disableAfterCrash(*existing);
        } else {
            transition(*existing, ModuleState::Available, QString());
        }
    } else {
        transition(*existing, ModuleState::Failed, MetadataChanged, false);
    }
}

void FacetHost::onPluginRemoved(const QString& moduleId)
{
    if (!m_controller) {
        return;
    }
    const std::shared_ptr<Module> module = m_modules.value(moduleId);
    if (!module) {
        return;
    }
    if (module->state == ModuleState::Loaded) {
        qCInfo(lcFacetHost).noquote() << "FacetHost: module" << moduleId
                                      << "was removed; it keeps running the code it loaded until Facet restarts";
        return;
    }
    if (moduleId == m_loadingModule) {
        // A rescan reached while the module's code runs in its load (a
        // nested event loop in registerInto). Its library is mapped, and the
        // load settles its state: failing it here would be overwritten by a
        // load that succeeds, which would then run with its extensions
        // inactive. A load that fails leaves it Failed, and a retry finds it
        // withdrawn (loadModule).
        qCInfo(lcFacetHost).noquote() << "FacetHost: module" << moduleId
                                      << "was removed while it loaded; the load goes on, and what it loads keeps "
                                         "running until Facet restarts";
        return;
    }
    withdrawModule(*module);
}

void FacetHost::withdrawModule(Module& module)
{
    // Its sections leave the pages they extend, and the extensions aimed at
    // its pages go quiet, as for a module that was never installed. Done
    // before the transition, so the pages are not told about an error card
    // for a section that is about to go.
    if (module.installed) {
        module.installed = false;
        updateExtensions();
    }
    transition(module, ModuleState::Failed, FileRemoved, false);
}

bool FacetHost::moduleSortsBefore(const Module& a, const Module& b) const
{
    if (a.metadata.order != b.metadata.order) {
        return a.metadata.order < b.metadata.order;
    }
    const int byName = m_collator.compare(a.metadata.name, b.metadata.name);
    if (byName != 0) {
        return byName < 0;
    }
    return a.metadata.id < b.metadata.id;
}

bool FacetHost::categorySortsBefore(const FacetCategoryMetadata& a, const FacetCategoryMetadata& b) const
{
    if (a.order != b.order) {
        return a.order < b.order;
    }
    const int byName = m_collator.compare(a.name, b.name);
    if (byName != 0) {
        return byName < 0;
    }
    return a.id < b.id;
}

qsizetype FacetHost::siblingSlot(const QString& parentId,
                                 const std::function<bool(const QString& siblingId)>& sortsAfter) const
{
    const QList<PageRegistry::Entry> siblings = m_controller->registry()->childPages(parentId);
    for (qsizetype i = 0; i < siblings.size(); ++i) {
        if (sortsAfter(siblings.at(i).id)) {
            return i;
        }
    }
    return -1;
}

void FacetHost::ensureCategoryNode(const QString& categoryId)
{
    if (m_registeredCategoryNodes.contains(categoryId)) {
        return;
    }
    const FacetCategoryMetadata category = m_categories.value(categoryId);
    // Among the top-level entries, before the first category that sorts
    // after this one. The host application's own top-level pages are not
    // categories and keep their place.
    const qsizetype slot = siblingSlot(QString(), [this, &category](const QString& siblingId) {
        if (!siblingId.startsWith(CategoryPrefix)) {
            return false;
        }
        const auto other = m_categories.constFind(siblingId.mid(CategoryPrefix.size()));
        return other != m_categories.cend() && categorySortsBefore(category, *other);
    });
    PageRegistry::Entry entry;
    entry.id = categoryNodeId(categoryId);
    entry.title = category.name;
    entry.iconSource = category.icon;
    entry.isCollapsible = true;
    auto placeholder = std::make_unique<FacetPlaceholderPage>(entry.id, this, QString(), QString(), nullptr);
    entry.controller = placeholder.get();
    if (!m_controller->registry()->registerPage(std::move(entry), slot)) {
        return;
    }
    // Parented to the ApplicationController, whose registry keeps pointing
    // at it for as long as it lives, even if this host goes first.
    placeholder.release()->setParent(m_controller.data());
    m_registeredCategoryNodes.insert(categoryId);
}

bool FacetHost::registerPlaceholder(const QString& id, const QString& parentId, const QString& title,
                                    const QString& icon, const QUrl& qmlSource, bool isCollapsible,
                                    const Module* module, const FacetPageMetadata* page, qsizetype pageSlot)
{
    PageRegistry::Entry entry;
    entry.id = id;
    entry.parentId = parentId;
    entry.title = title;
    entry.iconSource = icon;
    entry.qmlSource = qmlSource;
    entry.isCollapsible = isCollapsible;
    if (page) {
        entry.visibility = page->visibility;
        entry.hasDividerAfter = page->dividerAfter;
        if (!page->counterpart.isEmpty()) {
            entry.counterpartId = page->counterpart.contains(QLatin1Char(':'))
                ? page->counterpart
                : pageId(module->metadata.id, page->counterpart);
        }
    }
    // Not tracked as a staging domain: it is never dirty.
    auto placeholder = std::make_unique<FacetPlaceholderPage>(id, this, module ? module->metadata.id : QString(),
                                                              module ? module->metadata.name : QString(), nullptr);
    entry.controller = placeholder.get();
    qsizetype slot = pageSlot;
    if (module && !page) {
        // A module node: before the first sibling module that sorts after
        // it. Under a category, entries that are not module nodes are pages
        // placed in the category directly, which come after the modules.
        // Under a module node, they are that module's own pages, which come
        // before its nested modules.
        const bool underCategory = parentId.startsWith(CategoryPrefix);
        slot = siblingSlot(parentId, [this, module, underCategory](const QString& siblingId) {
            const std::shared_ptr<Module> other = m_modules.value(siblingId);
            if (!other || !other->nodeRegistered || other.get() == module) {
                return underCategory;
            }
            return moduleSortsBefore(*module, *other);
        });
    }
    if (!m_controller->registry()->registerPage(std::move(entry), slot)) {
        return false;
    }
    // Parented to the ApplicationController, whose registry keeps pointing
    // at it for as long as it lives, even if this host goes first.
    placeholder.release()->setParent(m_controller.data());
    return true;
}

void FacetHost::placeModules(QList<std::shared_ptr<Module>> modules)
{
    PageRegistry* const registry = m_controller->registry();
    std::stable_sort(modules.begin(), modules.end(), [this](const auto& a, const auto& b) {
        return moduleSortsBefore(*a, *b);
    });

    // A module whose id is already a page id (one of the host application's
    // own pages) would take over that page in owningModule.
    // The same for its pages: binding into a page id the host already uses
    // would take that page over.
    for (auto it = modules.begin(); it != modules.end();) {
        const FacetModuleMetadata& metadata = (*it)->metadata;
        QString taken = registry->hasPage(metadata.id) ? metadata.id : QString();
        for (const FacetPageMetadata& page : metadata.pages) {
            if (taken.isEmpty() && registry->hasPage(pageId(metadata.id, page.id))) {
                taken = pageId(metadata.id, page.id);
            }
        }
        if (!taken.isEmpty()) {
            transition(**it, ModuleState::Failed,
                       QStringLiteral("%1 is already the id of another page in the sidebar").arg(taken), false);
            it = modules.erase(it);
        } else {
            ++it;
        }
    }
    QHash<QString, Module*> batch;
    for (const auto& module : std::as_const(modules)) {
        batch.insert(module->metadata.id, module.get());
    }

    // Nesting: a module goes under the module it names as its parent when
    // that module is placed already or is placed now; otherwise under its
    // category.
    for (const auto& module : std::as_const(modules)) {
        const QString& parent = module->metadata.parent;
        module->nodeParent.clear();
        if (parent.isEmpty()) {
            continue;
        }
        const std::shared_ptr<Module> target = m_modules.value(parent);
        if (batch.contains(parent) || (target && target->nodeRegistered)) {
            module->nodeParent = parent;
        } else {
            qCInfo(lcFacetHost).noquote() << "FacetHost: module" << module->metadata.id << "nests under" << parent
                                          << "which is not available; it goes under its category";
        }
    }
    // Break nesting cycles, and only them. In each cycle, the member that
    // sorts first goes under its category, and the rest stay nested.
    {
        QHash<const Module*, int> rank;
        for (qsizetype i = 0; i < modules.size(); ++i) {
            rank.insert(modules.at(i).get(), int(i));
        }
        enum Mark {
            Unvisited,
            OnPath,
            Done
        };
        QHash<const Module*, Mark> mark;
        for (const auto& start : std::as_const(modules)) {
            QList<Module*> path;
            Module* cursor = start.get();
            while (cursor && mark.value(cursor, Unvisited) == Unvisited) {
                mark.insert(cursor, OnPath);
                path.append(cursor);
                cursor = batch.value(cursor->nodeParent, nullptr);
            }
            if (cursor && mark.value(cursor) == OnPath) {
                const QList<Module*> cycle = path.mid(path.indexOf(cursor));
                Module* const breaker = *std::min_element(cycle.cbegin(), cycle.cend(), [&rank](auto* a, auto* b) {
                    return rank.value(a) < rank.value(b);
                });
                qCInfo(lcFacetHost).noquote() << "FacetHost: module" << breaker->metadata.id
                                              << "is in a nesting cycle; it goes under its category";
                breaker->nodeParent.clear();
            }
            for (Module* visited : std::as_const(path)) {
                mark.insert(visited, Done);
            }
        }
    }

    // Categories: a module under a category uses it, and so does a page
    // placed in a category directly.
    struct ForeignRoot
    {
        Module* module;
        qsizetype pageIndex;
    };
    QList<ForeignRoot> foreignRoots;
    QHash<const Module*, QSet<QString>> foreignPages;
    QSet<QString> usedCategories;
    for (const auto& module : std::as_const(modules)) {
        module->category.clear();
        if (module->nodeParent.isEmpty()) {
            QString category = module->metadata.category.isEmpty() ? DefaultCategory : module->metadata.category;
            if (!m_categories.contains(category)) {
                qCInfo(lcFacetHost).noquote() << "FacetHost: module" << module->metadata.id << "names unknown category"
                                              << category << "- it goes under" << DefaultCategory;
                category = DefaultCategory;
            }
            module->category = category;
            usedCategories.insert(category);
        }
        // A page is foreign when it is placed outside its module: in a
        // category, or under another module's page. Its local descendants
        // go with it. The rest are registered under the module node.
        QSet<QString>& foreign = foreignPages[module.get()];
        const QList<FacetPageMetadata>& pages = module->metadata.pages;
        for (qsizetype i = 0; i < pages.size(); ++i) {
            const FacetPageMetadata& page = pages.at(i);
            const bool crossModule = page.parent.contains(QLatin1Char(':'));
            bool inCategory = !page.category.isEmpty();
            if (inCategory && !m_categories.contains(page.category)) {
                qCInfo(lcFacetHost).noquote()
                    << "FacetHost: page" << pageId(module->metadata.id, page.id) << "names unknown category"
                    << page.category << "- it stays under its module";
                inCategory = false;
            }
            if (inCategory || crossModule) {
                foreign.insert(page.id);
                foreignRoots.append(ForeignRoot{module.get(), i});
                if (inCategory) {
                    usedCategories.insert(page.category);
                }
            } else if (!page.parent.isEmpty() && foreign.contains(page.parent)) {
                foreign.insert(page.id);
            }
        }
    }
    QList<FacetCategoryMetadata> categories;
    for (const QString& id : std::as_const(usedCategories)) {
        categories.append(m_categories.value(id));
    }
    std::sort(categories.begin(), categories.end(), [this](const auto& a, const auto& b) {
        return categorySortsBefore(a, b);
    });
    for (const FacetCategoryMetadata& category : std::as_const(categories)) {
        ensureCategoryNode(category.id);
    }

    // Module nodes and their own pages, pre-order: a module's node, its
    // pages in declaration order, then the modules nested under it. Every
    // parent therefore exists before its children, and siblings keep their
    // sort order.
    // A module whose node or one of whose pages could not be registered never
    // loads: binding it would bind into placeholders that are missing. The
    // modules nested under a node that is missing go with it.
    const std::function<void(Module&, const QString&)> failTree = [&](Module& module, const QString& reason) {
        transition(module, ModuleState::Failed, reason, false);
        for (const auto& child : std::as_const(modules)) {
            if (child->nodeParent == module.metadata.id && !child->nodeRegistered) {
                failTree(*child, QStringLiteral("its parent module %1 could not be placed").arg(module.metadata.id));
            }
        }
    };
    const std::function<void(Module&)> placeTree = [&](Module& module) {
        const QString parent = module.nodeParent.isEmpty() ? categoryNodeId(module.category) : module.nodeParent;
        if (!registerPlaceholder(module.metadata.id, parent, module.metadata.name, module.metadata.icon, QUrl(), true,
                                 &module, nullptr)) {
            failTree(module, QStringLiteral("its node could not take its place in the sidebar"));
            return;
        }
        module.nodeRegistered = true;
        const QSet<QString>& foreign = foreignPages.value(&module);
        for (const FacetPageMetadata& page : std::as_const(module.metadata.pages)) {
            if (foreign.contains(page.id)) {
                continue;
            }
            const QString pageParent =
                page.parent.isEmpty() ? module.metadata.id : pageId(module.metadata.id, page.parent);
            if (!registerPlaceholder(pageId(module.metadata.id, page.id), pageParent, page.name, page.icon,
                                     placeholderPageSource(), false, &module, &page)) {
                transition(module, ModuleState::Failed, PageNotPlaced, false);
            }
        }
        for (const auto& child : std::as_const(modules)) {
            if (child->nodeParent == module.metadata.id) {
                placeTree(*child);
            }
        }
    };
    for (const auto& module : std::as_const(modules)) {
        if (!batch.contains(module->nodeParent)) {
            placeTree(*module);
        }
    }

    // Pages placed outside their module, now that every module's own pages
    // exist. A page under another module's page waits for that page when it
    // is itself placed outside its module, so the order modules were found
    // in does not decide where a page lands. A target that never appears
    // (its module is not installed, failed, or does not declare it) leaves
    // the page under its own module.
    // The scoped ids of foreign pages already tried, placed or not, so a page
    // waiting on one of them stops waiting once it has been tried.
    QSet<QString> attempted;
    // The index in @p waiting of the page to break a cycle at, when every
    // page there waits on the page of another there: the first, in sort
    // order, of the pages on the cycle that following those waits from the
    // first page runs into. A page that only waits on the cycle is never the
    // one cut loose.
    const auto cycleBreaker = [](const QList<ForeignRoot>& waiting) -> qsizetype {
        // Each page waited on, by scoped id, to the root whose subtree
        // holds it: the root, and its local descendants that follow it.
        QHash<QString, qsizetype> holder;
        for (qsizetype i = 0; i < waiting.size(); ++i) {
            const ForeignRoot& root = waiting.at(i);
            const QList<FacetPageMetadata>& pages = root.module->metadata.pages;
            QSet<QString> subtree{pages.at(root.pageIndex).id};
            holder.insert(pageId(root.module->metadata.id, pages.at(root.pageIndex).id), i);
            for (qsizetype p = root.pageIndex + 1; p < pages.size() && subtree.contains(pages.at(p).parent); ++p) {
                subtree.insert(pages.at(p).id);
                holder.insert(pageId(root.module->metadata.id, pages.at(p).id), i);
            }
        }
        QList<qsizetype> path;
        qsizetype at = 0;
        while (!path.contains(at)) {
            path.append(at);
            const ForeignRoot& root = waiting.at(at);
            const qsizetype next = holder.value(root.module->metadata.pages.at(root.pageIndex).parent, -1);
            if (next < 0) {
                // Unreachable while every page waits on one held here.
                return at;
            }
            at = next;
        }
        const QList<qsizetype> cycle = path.mid(path.indexOf(at));
        return *std::min_element(cycle.cbegin(), cycle.cend());
    };
    const auto placeForeign = [&](const ForeignRoot& root, const QString& parent) {
        Module& module = *root.module;
        const QList<FacetPageMetadata>& pages = module.metadata.pages;
        const FacetPageMetadata& rootPage = pages.at(root.pageIndex);
        // A page that falls back under its own module goes after the
        // module's other pages and before the modules nested under it, like
        // the module's own pages.
        qsizetype slot = -1;
        if (parent == module.metadata.id) {
            slot = siblingSlot(parent, [this](const QString& siblingId) {
                const std::shared_ptr<Module> other = m_modules.value(siblingId);
                return other && other->nodeRegistered;
            });
        }
        attempted.insert(pageId(module.metadata.id, rootPage.id));
        bool placed = registerPlaceholder(pageId(module.metadata.id, rootPage.id), parent, rootPage.name, rootPage.icon,
                                          placeholderPageSource(), false, &module, &rootPage, slot);
        // The root's local descendants follow it in the pre-ordered list.
        QSet<QString> subtree{rootPage.id};
        for (qsizetype i = root.pageIndex + 1; i < pages.size(); ++i) {
            const FacetPageMetadata& page = pages.at(i);
            if (!subtree.contains(page.parent)) {
                break;
            }
            subtree.insert(page.id);
            attempted.insert(pageId(module.metadata.id, page.id));
            placed = registerPlaceholder(pageId(module.metadata.id, page.id), pageId(module.metadata.id, page.parent),
                                         page.name, page.icon, placeholderPageSource(), false, &module, &page)
                && placed;
        }
        if (!placed) {
            transition(module, ModuleState::Failed, PageNotPlaced, false);
        }
    };
    QList<ForeignRoot> pending;
    for (const ForeignRoot& root : std::as_const(foreignRoots)) {
        if (root.module->nodeRegistered) {
            pending.append(root);
        }
    }
    while (!pending.isEmpty()) {
        QList<ForeignRoot> waiting;
        for (const ForeignRoot& root : std::as_const(pending)) {
            const FacetPageMetadata& page = root.module->metadata.pages.at(root.pageIndex);
            const QString scoped = pageId(root.module->metadata.id, page.id);
            if (page.parent.isEmpty()) {
                placeForeign(root, categoryNodeId(page.category));
                continue;
            }
            if (registry->hasPage(page.parent)) {
                placeForeign(root, page.parent);
                continue;
            }
            const qsizetype colon = page.parent.indexOf(QLatin1Char(':'));
            const QString targetModuleId = page.parent.left(colon);
            const QString targetPage = page.parent.mid(colon + 1);
            Module* const target = batch.value(targetModuleId, nullptr);
            // Worth waiting for only while the target page is one placed
            // outside its own module and not tried yet. A page of the
            // target's own that is missing failed to register already.
            if (target && target->nodeRegistered && foreignPages.value(target).contains(targetPage)
                && !attempted.contains(page.parent)) {
                waiting.append(root);
                continue;
            }
            qCInfo(lcFacetHost).noquote() << "FacetHost: page" << scoped << "goes under" << page.parent
                                          << "which is not available; it stays under its module";
            placeForeign(root, root.module->metadata.id);
        }
        if (waiting.size() == pending.size()) {
            // Nothing was placed, so every page left waits on a page of
            // another that waits too, and following those waits goes round
            // a cycle. The page that sorts first in that cycle stays under
            // its module, and the next round places the rest of the cycle,
            // and the pages that wait on it from outside, under their
            // targets.
            const qsizetype breaker = cycleBreaker(waiting);
            const ForeignRoot root = waiting.takeAt(breaker);
            const FacetPageMetadata& page = root.module->metadata.pages.at(root.pageIndex);
            qCInfo(lcFacetHost).noquote()
                << "FacetHost: page" << pageId(root.module->metadata.id, page.id) << "goes under" << page.parent
                << "in a cycle of pages placed under each other; it stays under its module";
            placeForeign(root, root.module->metadata.id);
        }
        pending = waiting;
    }
}

QString FacetHost::owningModule(const QString& pageId) const
{
    if (pageId.startsWith(CategoryPrefix)) {
        return QString();
    }
    // The module's node, or one of the pages its metadata declares. A host
    // page that merely starts with `<module>:` is not the module's, and
    // opening it must not load the module.
    const qsizetype colon = pageId.indexOf(QLatin1Char(':'));
    const QString candidate = colon > 0 ? pageId.left(colon) : pageId;
    const std::shared_ptr<Module> module = m_modules.value(candidate);
    if (!module) {
        return QString();
    }
    if (colon > 0 && !module->metadata.page(pageId.mid(colon + 1))) {
        return QString();
    }
    return candidate;
}

bool FacetHost::navigatedElsewhere(const QString& landing, quint64 navigations, const QString& pageId) const
{
    // Away and back to the page being left still counts: the controller drops
    // the outer navigation either way.
    const QString now = m_controller->currentPageId();
    return now != pageId && (now != landing || m_navigations != navigations);
}

void FacetHost::onNavigation(const QString& pageId)
{
    // The hook runs before the controller moves, so the current page is still
    // the one being left. A slot reacting to a load that navigates elsewhere
    // on its own moves it, and that newer navigation wins: nothing more is
    // loaded for this one. One that lands on this same page lets it go on.
    const QString landing = m_controller->currentPageId();
    const quint64 navigations = ++m_navigations;
    // Loads below finish their own pages through this navigation, not on
    // their own (showSectionsOn).
    const QString outerOpening = std::exchange(m_openingPage, pageId);
    const auto restore = qScopeGuard([this, &outerOpening] {
        m_openingPage = outerOpening;
    });
    // The page's own module, then every module that adds a section to it, so
    // the page shows complete on its first paint.
    const QString owner = owningModule(pageId);
    if (const std::shared_ptr<Module> module = m_modules.value(owner);
        module && module->state == ModuleState::Available) {
        QString error;
        if (!loadModule(owner, &error)) {
            // The placeholder page says why and offers a retry.
            qCWarning(lcFacetHost).noquote()
                << "FacetHost: opening" << pageId << "could not load module" << owner << "-" << error;
        }
        if (navigatedElsewhere(landing, navigations, pageId)) {
            return;
        }
    }
    loadExtenders(pageId, landing, navigations);
}

void FacetHost::loadExtenders(const QString& pageId, const QString& landing, quint64 navigations)
{
    // While a module loads, canLoadNow refuses every other load, so nothing
    // can be loaded for the page now. It is owed instead, and the load that
    // is running pays once it is over (loadOwedExtenders).
    if (!m_loadingModule.isEmpty()) {
        m_extendersOwed = true;
        return;
    }
    // A page whose own module did not load shows its placeholder, which has
    // no slots, so running the extenders' code would show nothing.
    const QString owner = owningModule(pageId);
    if (const std::shared_ptr<Module> module = m_modules.value(owner);
        !owner.isEmpty() && (!module || module->state != ModuleState::Loaded)) {
        return;
    }
    QStringList toLoad;
    const QList<ActiveExtension> extensions = m_extensionsByPage.value(pageId);
    for (const ActiveExtension& extension : extensions) {
        if (!toLoad.contains(extension.moduleId)) {
            toLoad.append(extension.moduleId);
        }
    }
    for (const QString& moduleId : std::as_const(toLoad)) {
        const std::shared_ptr<Module> module = m_modules.value(moduleId);
        if (!module || module->state != ModuleState::Available) {
            // A section of a module that did not load shows its error.
            continue;
        }
        if (!m_catalog->plugin(moduleId)) {
            // Withdrawn in the scan being announced: its pluginRemoved is
            // still on its way, and loading it now could only fail.
            continue;
        }
        QString error;
        if (!loadModule(moduleId, &error)) {
            qCWarning(lcFacetHost).noquote()
                << "FacetHost: opening" << pageId << "could not load module" << moduleId << "-" << error;
        }
        if (navigatedElsewhere(landing, navigations, pageId)) {
            return;
        }
    }
}

void FacetHost::loadOwedExtenders()
{
    // Every load comes through here once its outcome is settled, whatever
    // started it. A page that a slot opened while it ran, or whose
    // extensions changed then, could load nothing (loadExtenders). The page
    // on screen now gets what extends it, as opening it would have given
    // it. A page whose own module was refused gets nothing: loadExtenders
    // skips it, and its placeholder offers to load the module
    // (FacetPlaceholderPage::canRetry). The loads made here come through
    // here in turn, so a page one of their slots opens is paid the same way.
    if (!m_extendersOwed || !m_controller) {
        return;
    }
    m_extendersOwed = false;
    const QString shown = m_controller->currentPageId();
    loadExtenders(shown, shown, m_navigations);
}

void FacetHost::showSectionsOn(const Module& owner)
{
    // Collected first, so no slot runs while the maps are being walked. Only
    // pages with a section to list: one from a module that is not Available
    // (extensionsFor skips those, and each is announced when its module
    // loads, by transition()).
    QStringList pages;
    for (auto it = m_extensionsByPage.cbegin(); it != m_extensionsByPage.cend(); ++it) {
        if (owningModule(it.key()) != owner.metadata.id) {
            continue;
        }
        const bool listsSomething = std::any_of(it->cbegin(), it->cend(), [this](const ActiveExtension& extension) {
            const std::shared_ptr<Module> module = m_modules.value(extension.moduleId);
            return module && module->state != ModuleState::Available;
        });
        if (listsSomething) {
            pages.append(it.key());
        }
    }
    const QList<std::shared_ptr<Module>> modules = m_modules.values();
    for (const std::shared_ptr<Module>& other : modules) {
        if (other.get() != &owner && other->state == ModuleState::Loaded) {
            attachExtensions(*other);
        }
    }
    for (const QString& page : std::as_const(pages)) {
        m_controller->notifyExtensionsChanged(page);
    }
    // A retry on the open page: load what extends it, as opening it would
    // have. A navigation in progress does that itself, once it is done.
    if (m_holdsNavigationHook && m_openingPage.isEmpty()) {
        const QString current = m_controller->currentPageId();
        if (owningModule(current) == owner.metadata.id) {
            loadExtenders(current, current, m_navigations);
        }
    }
}

bool FacetHost::canLoadNow(const QString& moduleId, QString* errorString) const
{
    const auto fail = [errorString](const QString& message) {
        if (errorString) {
            *errorString = message;
        }
        return false;
    };
    if (!m_controller) {
        return fail(QStringLiteral("the ApplicationController is gone"));
    }
    // Every registration below is refused off the controller's thread, which
    // would leave a module marked loaded with nothing bound.
    if (QThread::currentThread() != m_controller->thread()) {
        return fail(QStringLiteral("module %1 can only be loaded on the GUI thread").arg(moduleId));
    }
    if (!m_loadingModule.isEmpty()) {
        // Reached from inside another module's load (a slot that navigated).
        // Running this one now would overwrite the load record of the one
        // still in registerInto, or run registerInto twice for the same
        // module.
        return fail(QStringLiteral("module %1 is still loading").arg(m_loadingModule));
    }
    return true;
}

bool FacetHost::loadModule(const QString& moduleId, QString* errorString)
{
    const auto fail = [errorString](const QString& message) {
        if (errorString) {
            *errorString = message;
        }
        return false;
    };
    const std::shared_ptr<Module> module = m_modules.value(moduleId);
    if (!module) {
        return fail(QStringLiteral("no module %1").arg(moduleId));
    }
    switch (module->state) {
    case ModuleState::Loaded:
        return true;
    case ModuleState::Failed:
    case ModuleState::DisabledAfterCrash:
        return fail(module->reason);
    case ModuleState::Available:
        break;
    }
    if (!canLoadNow(moduleId, errorString)) {
        return false;
    }
    // Not on offer: withdrawn in the scan being announced, whose
    // pluginRemoved for it is still on its way (a slot of that scan's earlier
    // announcements asked for it), or removed while its last load ran, which
    // onPluginRemoved leaves to the load. It fails as the removal does, so
    // that removal, when it comes, finds it so and announces nothing more.
    if (!m_catalog->plugin(moduleId)) {
        withdrawModule(*module);
        return fail(FileRemoved);
    }
    // Held until the module's state is settled, so a slot reached while its
    // code runs or its pages bind (pageBound, a navigation) cannot run its
    // registerInto again. Released just before the transition that announces
    // the outcome, so a slot of that may load the next module.
    m_loadingModule = moduleId;
    const bool recorded = m_loadRecord->recordLoading(moduleId);
    const auto finishLoading = [this, recorded]() {
        m_loadRecord->clearLoading(m_loadingModule, recorded);
        m_loadingModule.clear();
    };
    // The way out of a failed load, after its transition. What the load
    // kept other pages from loading comes first (loadOwedExtenders), then the
    // search providers a query asked for meanwhile. A slot of the failure
    // transition, or of those loads, may have retried the module and loaded
    // it, which is then the outcome to report.
    const auto failedWith = [this, &module, &fail](const QString& reason) {
        loadOwedExtenders();
        if (m_searchProvidersOwed) {
            loadOwedSearchProviders();
        }
        return module->state == ModuleState::Loaded || fail(reason);
    };

    QString loadError;
    QObject* const root = m_catalog->load(moduleId, &loadError);
    if (!root) {
        finishLoading();
        // A library that did not load may load another time, once what it
        // needs is installed. One that is mapped already failed because the
        // root object it handed out was deleted since, and loading it again
        // hands out the same deleted object.
        transition(*module, ModuleState::Failed, loadError, !m_catalog->isLoaded(moduleId));
        return failedWith(loadError);
    }
    auto* const facetModule = qobject_cast<IFacetModule*>(root);
    if (!facetModule) {
        finishLoading();
        const QString reason = QStringLiteral("its root object does not implement IFacetModule");
        transition(*module, ModuleState::Failed, reason, false);
        return failedWith(reason);
    }
    // The placeholders were built from the metadata read at start(). Code
    // loaded from a file that changed since then expects other pages, and
    // binding it would half-match them. PluginCatalog::load offers what it
    // has just loaded, so the plugin is on offer here. Were it not, it fails
    // as its removal does, which then announces nothing more.
    const std::optional<PhosphorRegistry::PluginMetadata> offered = m_catalog->plugin(moduleId);
    if (!offered || offered->metaData != module->rawMetadata) {
        finishLoading();
        const QString reason = offered ? MetadataChanged : FileRemoved;
        transition(*module, ModuleState::Failed, reason, false);
        return failedWith(reason);
    }

    FacetContext context(this, module->metadata);
    facetModule->registerInto(context);

    // Check the whole registration before anything takes effect. The load
    // record still names the module, for the search providers asked below.
    QStringList errors = context.m_errors;
    for (const FacetPageMetadata& page : std::as_const(module->metadata.pages)) {
        if (!context.m_registeredPages.contains(page.id)) {
            errors.append(QStringLiteral("did not register page %1").arg(page.id));
        }
    }
    for (const FacetExtensionMetadata& extension : std::as_const(module->metadata.extensions)) {
        if (!context.m_registeredExtensions.contains(extension.id)) {
            errors.append(QStringLiteral("did not register extension %1").arg(extension.id));
        }
    }
    QSet<const PageController*> liveControllers;
    for (const PageRegistry::Entry& entry : m_controller->registry()->allPagesRef()) {
        if (entry.controller) {
            liveControllers.insert(entry.controller.data());
        }
    }
    for (const auto& page : std::as_const(context.m_pages)) {
        if (!page.controller) {
            errors.append(QStringLiteral("page %1's controller was deleted during registration").arg(page.localId));
        } else if (liveControllers.contains(page.controller.data())) {
            errors.append(QStringLiteral("page %1's controller is already another page's").arg(page.localId));
        }
    }
    for (const QPointer<StagingDomain>& domain : std::as_const(context.m_domains)) {
        if (!domain) {
            errors.append(QStringLiteral("a domain was deleted during registration"));
        } else if (m_controller->isTrackedDomain(domain.data())) {
            errors.append(QStringLiteral("registered a domain the application already tracks"));
        }
    }
    for (const FacetContext::SearchProviderRegistration& provider : std::as_const(context.m_searchProviders)) {
        if (provider.isObject && !provider.object) {
            errors.append(QStringLiteral("a search provider was deleted during registration"));
        }
    }
    if (!errors.isEmpty()) {
        const QString reason = errors.join(QStringLiteral("; "));
        finishLoading();
        transition(*module, ModuleState::Failed, reason);
        return failedWith(reason);
    }
    // context.m_domains holds the extensions' domains too, for the checks
    // above. The headless ones are told apart now, while the checks have
    // just found every one alive: module code run from here on can delete
    // one, and a deleted one could no longer be told from a section's.
    QSet<const StagingDomain*> extensionDomains;
    for (const auto& extension : std::as_const(context.m_extensions)) {
        extensionDomains.insert(extension.domain.data());
    }
    QList<QPointer<StagingDomain>> headless;
    for (const QPointer<StagingDomain>& domain : std::as_const(context.m_domains)) {
        if (!extensionDomains.contains(domain.data())) {
            headless.append(domain);
        }
    }
    // Each search provider is asked for its entries once now, while the load
    // record names the module, so one that crashes the first time it is asked
    // disables the module at the next start, as a crash in registerInto does.
    // Its later answers are not covered. Without a search controller nothing
    // ever asks.
    if (m_search) {
        for (const FacetContext::SearchProviderRegistration& provider : std::as_const(context.m_searchProviders)) {
            // One asked before it can delete another.
            if (!provider.isObject || provider.object) {
                provider.provider->searchEntries();
            }
        }
    }
    m_loadRecord->clearLoading(m_loadingModule, recorded);

    // From here on module code can run (a search provider asked above, an
    // isDirty override while a domain registers, a dirtyChanged or pageBound
    // slot), so a domain or controller the checks above found alive can be
    // gone by the time it is used. Each use checks again.
    //
    // The headless domains join Apply now. The extensions' domains join only
    // once their section is shown (attachExtensions), so a section that is
    // never shown never marks the application dirty. A headless domain that
    // is gone by now, or that the application refused, keeps its place as a
    // null entry, so moduleDomains() shows it went.
    for (const QPointer<StagingDomain>& domain : std::as_const(headless)) {
        if (domain && m_controller->registerDomain(domain.data())) {
            module->domains.append(domain);
        } else {
            module->domains.append(QPointer<StagingDomain>());
        }
    }
    for (const auto& extension : std::as_const(context.m_extensions)) {
        // Recorded even when its domain is gone, so the section shows that
        // error (extensionsFor) instead of vanishing.
        module->extensions.insert(extension.localId, Module::Extension{extension.domain, extension.qmlSource});
        if (!extension.domain) {
            continue;
        }
        // Adopted now, like a registered domain, since one that stays
        // inactive is never registered.
        if (!extension.domain->parent()) {
            extension.domain->setParent(m_controller);
        }
        // A section whose domain goes shows an error instead, which its page
        // has to be told about.
        connect(
            extension.domain.data(), &QObject::destroyed, this,
            [this, moduleId]() {
                notifyExtendedPages(moduleId);
            },
            Qt::QueuedConnection);
    }
    for (const auto& page : std::as_const(context.m_pages)) {
        const QString scoped = pageId(moduleId, page.localId);
        if (!page.controller) {
            // Deleted by module code that ran after the checks. The page
            // keeps its placeholder.
            qCWarning(lcFacetHost).noquote()
                << "FacetHost: module" << moduleId << "deleted the controller of page" << scoped << "while it loaded";
            continue;
        }
        if (m_controller->bindPage(scoped, page.controller.data(), page.qmlSource)) {
            module->boundPages.append(scoped);
        } else {
            // Unreachable for a live controller after the checks above and
            // canLoadNow's thread check, which cover every other reason the
            // registry refuses a bind. Say so loudly if it ever is, since
            // what was bound before it stays bound.
            qCCritical(lcFacetHost).noquote() << "FacetHost: module" << moduleId << "page" << scoped
                                              << "passed every check and was still refused by the registry";
        }
    }
    // Asked from now on (ModuleSearch), once the transition below has had the
    // index rebuilt.
    module->searchProviders = context.m_searchProviders;
    // Its sections shown on pages whose module is loaded join them: Apply,
    // dirty mark and reset. transition() then tells those pages' FacetSlots.
    attachExtensions(*module);
    m_loadingModule.clear();
    transition(*module, ModuleState::Loaded, QString());
    showSectionsOn(*module);
    loadOwedExtenders();
    if (m_searchProvidersOwed) {
        loadOwedSearchProviders();
    }
    qCInfo(lcFacetHost).noquote() << "FacetHost: loaded module" << moduleId;
    return true;
}

bool FacetHost::retryModule(const QString& moduleId, QString* errorString)
{
    const std::shared_ptr<Module> module = m_modules.value(moduleId);
    if (!module) {
        if (errorString) {
            *errorString = QStringLiteral("no module %1").arg(moduleId);
        }
        return false;
    }
    if (!canRetry(moduleId)) {
        return loadModule(moduleId, errorString);
    }
    // Checked before the crash memory and the state are cleared, so a retry
    // that cannot run now leaves the module as it was.
    if (!canLoadNow(moduleId, errorString)) {
        return false;
    }
    // Retried all the same when the record cannot be written: the user asked
    // for it, and the module is on screen. Only the next start may disable
    // it again, unless a later write of the record succeeds.
    if (module->state == ModuleState::DisabledAfterCrash && !m_loadRecord->enable(moduleId)) {
        qCWarning(lcFacetHost).noquote() << "FacetHost: module" << moduleId
                                         << "is retried, but the load record could not be written, so the next start "
                                            "may disable it again";
    }
    transition(*module, ModuleState::Available, QString());
    return loadModule(moduleId, errorString);
}

bool FacetHost::canRetry(const QString& moduleId) const
{
    const std::shared_ptr<Module> module = m_modules.value(moduleId);
    if (!module) {
        return false;
    }
    switch (module->state) {
    case ModuleState::DisabledAfterCrash:
        return true;
    case ModuleState::Failed:
        // A failed load keeps nothing it registered, so loading again is
        // safe whenever the module's placeholders are up.
        return module->retryable && module->nodeRegistered;
    case ModuleState::Available:
    case ModuleState::Loaded:
        break;
    }
    return false;
}

void FacetHost::transition(Module& module, ModuleState state, const QString& reason, bool retryable)
{
    if (module.state == state && module.reason == reason && module.retryable == retryable) {
        return;
    }
    module.state = state;
    module.reason = reason;
    module.retryable = retryable;
    const quint64 transitions = ++module.transitions;
    if (state == ModuleState::Failed) {
        qCWarning(lcFacetHost).noquote() << "FacetHost: module" << module.metadata.id << "failed:" << reason;
    }
    // What search lists for a module follows its state (ModuleSearch): its
    // providers once loaded, nothing on its pages once failed. Rebuilt
    // before anyone hears of the change.
    invalidateSearch();
    if (m_starting) {
        return;
    }
    Q_EMIT moduleStateChanged(module.metadata.id, state);
    // A slot of that can move the module on (a retry that loads it). The
    // transition it made announced itself and told the pages, so nothing is
    // left to say here.
    if (module.transitions != transitions) {
        return;
    }
    if (state == ModuleState::Failed) {
        Q_EMIT moduleFailed(module.metadata.id, reason);
    } else if (state == ModuleState::Loaded) {
        Q_EMIT moduleLoaded(module.metadata.id);
    }
    // Its sections on other pages show it loaded, or its error card.
    notifyExtendedPages(module.metadata.id);
}

void FacetHost::attachExtensions(Module& module)
{
    // A copy, so the map cannot change under the walk: registering a domain
    // runs module code (its isDirty, a dirtyChanged slot).
    const QHash<QString, QList<ActiveExtension>> extensionsByPage = m_extensionsByPage;
    for (auto it = extensionsByPage.cbegin(); it != extensionsByPage.cend(); ++it) {
        // Shown only once the page's own module has loaded (extensionsFor).
        // Both that and the extension being active last, so a section that
        // joins here never goes unseen again.
        const std::shared_ptr<Module> pageOwner = m_modules.value(owningModule(it.key()));
        if (!pageOwner || pageOwner->state != ModuleState::Loaded) {
            continue;
        }
        for (const ActiveExtension& extension : it.value()) {
            if (extension.moduleId != module.metadata.id) {
                continue;
            }
            const auto registered = module.extensions.find(extension.localId);
            if (registered == module.extensions.end() || !registered->domain) {
                continue;
            }
            if (!registered->registered) {
                if (!m_controller->registerDomain(registered->domain.data())) {
                    continue;
                }
                registered->registered = true;
            }
            m_controller->attachDomainToPage(registered->domain.data(), it.key());
        }
    }
}

void FacetHost::notifyExtendedPages(const QString& moduleId)
{
    if (!m_controller) {
        return;
    }
    // Collected first, so no slot runs while the map is being walked.
    QStringList touched;
    for (auto it = m_extensionsByPage.cbegin(); it != m_extensionsByPage.cend(); ++it) {
        const bool extendsPage = std::any_of(it->cbegin(), it->cend(), [&moduleId](const ActiveExtension& extension) {
            return extension.moduleId == moduleId;
        });
        if (extendsPage) {
            touched.append(it.key());
        }
    }
    for (const QString& page : std::as_const(touched)) {
        m_controller->notifyExtensionsChanged(page);
    }
}

void FacetHost::indexModule(const Module& module)
{
    // A module that was never placed may share a page id with a page of the
    // host application's, whose keywords are not this module's to set.
    if (!m_search || !module.nodeRegistered) {
        return;
    }
    // Only the pages that took their place in the sidebar: SearchController
    // drops, with a warning, an entry on a page it cannot navigate to. The
    // module's own keywords and its name find every one of its pages, a page
    // placed outside the module's node too, whose breadcrumb does not name
    // the module.
    const PageRegistry* const registry = m_controller->registry();
    QStringList moduleWords = module.metadata.keywords;
    if (!module.metadata.name.isEmpty()) {
        moduleWords.append(module.metadata.name);
    }
    for (const FacetPageMetadata& page : module.metadata.pages) {
        const QString scoped = pageId(module.metadata.id, page.id);
        if (registry->hasPage(scoped)) {
            m_search->setPageKeywords(scoped, page.keywords + moduleWords);
        }
    }
}

void FacetHost::invalidateSearch()
{
    if (!m_search) {
        return;
    }
    if (m_searchBatches > 0) {
        m_searchStale = true;
        return;
    }
    m_search->invalidate();
}

void FacetHost::loadOwedSearchProviders()
{
    if (!m_search || !m_controller || m_search->query().trimmed().isEmpty()) {
        return;
    }
    // One walk at a time, and none while a module loads, since every load is
    // refused then. A call meanwhile, from a slot of a load (a query typed, a
    // module arriving or offered again), is owed: the walk goes round again,
    // or the load that is running pays it once it is over.
    if (m_loadingSearchProviders || !m_loadingModule.isEmpty()) {
        m_searchProvidersOwed = true;
        return;
    }
    m_loadingSearchProviders = true;
    const auto done = qScopeGuard([this] {
        m_loadingSearchProviders = false;
    });
    // The loads' transitions rebuild the index once, at the end.
    const SearchBatch batch(*this);
    do {
        m_searchProvidersOwed = false;
        // Collected afresh each round, since a load can add modules to the
        // map or move them on, and in id order, so the loads run in the same
        // order every time. A module the catalog withdrew in the scan being
        // announced is left for its pluginRemoved, since loading it now
        // could only fail.
        QStringList owed;
        for (auto it = m_modules.cbegin(); it != m_modules.cend(); ++it) {
            const Module& module = *it.value();
            if (module.metadata.searchProviders && module.state == ModuleState::Available
                && m_catalog->plugin(it.key())) {
                owed.append(it.key());
            }
        }
        std::sort(owed.begin(), owed.end());
        for (const QString& moduleId : std::as_const(owed)) {
            QString error;
            if (!loadModule(moduleId, &error)) {
                qCWarning(lcFacetHost).noquote()
                    << "FacetHost: module" << moduleId << "has search providers but did not load -" << error;
            }
        }
    } while (m_searchProvidersOwed && m_controller && m_search);
}

void FacetHost::resolveExtensions()
{
    m_extensionsByPage.clear();
    m_refusedExtensions.clear();
    for (auto moduleIt = m_modules.cbegin(); moduleIt != m_modules.cend(); ++moduleIt) {
        const Module& module = *moduleIt.value();
        if (!module.nodeRegistered || !module.installed) {
            continue; // invalid metadata, not placed or removed: nothing of it is shown
        }
        for (const FacetExtensionMetadata& extension : module.metadata.extensions) {
            const QString scoped = pageId(module.metadata.id, extension.id);
            const QString targetModuleId = extension.target.section(QLatin1Char(':'), 0, 0);
            const QString targetLocalPage = extension.target.section(QLatin1Char(':'), 1);
            const std::shared_ptr<Module> target = m_modules.value(targetModuleId);
            if (!target || !target->installed) {
                continue; // not installed: inactive, and quietly so
            }
            QString reason;
            if (!target->nodeRegistered) {
                reason = QStringLiteral(
                             "its target module %1 is installed but has invalid metadata or could not be "
                             "placed in the sidebar")
                             .arg(targetModuleId);
            } else if (!target->metadata.page(targetLocalPage)) {
                reason = QStringLiteral("its target page %1 does not exist").arg(extension.target);
            } else if (!m_controller->registry()->hasPage(extension.target)) {
                reason = QStringLiteral("its target page %1 has no place in the sidebar").arg(extension.target);
            } else if (const int offered = target->metadata.slotVersion(targetLocalPage, extension.slot);
                       offered == 0) {
                reason = QStringLiteral("page %1 has no slot %2").arg(extension.target, extension.slot);
            } else if (offered != extension.targetVersion) {
                reason = QStringLiteral("it is built for version %1 of slot %2 on %3, which is at version %4")
                             .arg(QString::number(extension.targetVersion), extension.slot, extension.target,
                                  QString::number(offered));
            }
            if (!reason.isEmpty()) {
                m_refusedExtensions.append({scoped, reason});
                continue;
            }
            m_extensionsByPage[extension.target].append(ActiveExtension{
                module.metadata.id,
                extension.id,
                extension.slot,
                extension.name,
                extension.order,
            });
        }
    }
    for (auto it = m_extensionsByPage.begin(); it != m_extensionsByPage.end(); ++it) {
        std::sort(it->begin(), it->end(), [](const ActiveExtension& a, const ActiveExtension& b) {
            if (a.order != b.order) {
                return a.order < b.order;
            }
            if (a.moduleId != b.moduleId) {
                return a.moduleId < b.moduleId;
            }
            return a.localId < b.localId;
        });
    }
    std::sort(m_refusedExtensions.begin(), m_refusedExtensions.end(),
              [](const RefusedExtension& a, const RefusedExtension& b) {
                  return a.id < b.id;
              });
    // Resolution runs again whenever a module comes or goes, so warn once per
    // refused extension, not once per pass.
    for (const RefusedExtension& refused : std::as_const(m_refusedExtensions)) {
        if (!m_warnedExtensions.contains(refused.id)) {
            m_warnedExtensions.insert(refused.id);
            qCWarning(lcFacetHost).noquote() << "FacetHost: skipping extension" << refused.id << "-" << refused.reason;
        }
    }
}

QHash<QString, QStringList> FacetHost::extensionIdsByPage() const
{
    QHash<QString, QStringList> ids;
    for (auto it = m_extensionsByPage.cbegin(); it != m_extensionsByPage.cend(); ++it) {
        ids.insert(it.key(), extensionsOn(it.key()));
    }
    return ids;
}

void FacetHost::updateExtensions()
{
    if (!m_controller) {
        return;
    }
    const QHash<QString, QStringList> before = extensionIdsByPage();
    const QList<RefusedExtension> refusedBefore = m_refusedExtensions;
    resolveExtensions();
    // A module loaded before its target arrived joins that page now. Walked
    // as a copy, like showSectionsOn, since attaching runs module code.
    const QList<std::shared_ptr<Module>> modules = m_modules.values();
    for (const std::shared_ptr<Module>& module : modules) {
        if (module->state == ModuleState::Loaded) {
            attachExtensions(*module);
        }
    }
    const QHash<QString, QStringList> after = extensionIdsByPage();
    const bool refusedChanged = refusedBefore.size() != m_refusedExtensions.size()
        || !std::equal(refusedBefore.cbegin(), refusedBefore.cend(), m_refusedExtensions.cbegin(),
                       [](const RefusedExtension& a, const RefusedExtension& b) {
                           return a.id == b.id && a.reason == b.reason;
                       });
    // Only the pages whose sections changed: a FacetSlot rebuilds what it
    // shows on every notice.
    QSet<QString> pages(QSet<QString>(before.keyBegin(), before.keyEnd()));
    pages.unite(QSet<QString>(after.keyBegin(), after.keyEnd()));
    QStringList changed;
    for (const QString& page : std::as_const(pages)) {
        if (before.value(page) != after.value(page)) {
            changed.append(page);
        }
    }
    std::sort(changed.begin(), changed.end());
    // ModuleSearch lists the active extensions as sections, so a live query
    // shows the change before the pages are told.
    if (!changed.isEmpty()) {
        invalidateSearch();
    }
    if (refusedChanged) {
        Q_EMIT refusedExtensionsChanged();
    }
    for (const QString& page : std::as_const(changed)) {
        m_controller->notifyExtensionsChanged(page);
    }
    // An extension that became active on the open page loads its module now,
    // as opening the page would have.
    if (m_holdsNavigationHook) {
        const QString current = m_controller->currentPageId();
        loadExtenders(current, current, m_navigations);
    }
}

QString FacetHost::firstPlacedPage(const Module& module) const
{
    for (const FacetPageMetadata& page : module.metadata.pages) {
        const QString scoped = pageId(module.metadata.id, page.id);
        if (m_controller->registry()->hasPage(scoped)) {
            return scoped;
        }
    }
    return QString();
}

QVariantList FacetHost::extensionsFor(const QString& pageId, const QString& slot) const
{
    QVariantList list;
    // A page whose own module is not loaded shows its placeholder instead of
    // its QML, so no section is listed for it.
    const std::shared_ptr<Module> owner = m_modules.value(owningModule(pageId));
    if (!owner || owner->state != ModuleState::Loaded) {
        return list;
    }
    const QList<ActiveExtension> extensions = m_extensionsByPage.value(pageId);
    for (const ActiveExtension& extension : extensions) {
        if (extension.slot != slot) {
            continue;
        }
        const std::shared_ptr<Module> module = m_modules.value(extension.moduleId);
        if (!module) {
            continue;
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), FacetHost::pageId(extension.moduleId, extension.localId));
        entry.insert(QStringLiteral("moduleId"), extension.moduleId);
        entry.insert(QStringLiteral("moduleName"), module->metadata.name);
        entry.insert(QStringLiteral("name"), extension.name);
        entry.insert(QStringLiteral("modulePageId"), firstPlacedPage(*module));
        switch (module->state) {
        case ModuleState::Available:
            // Not loaded yet. Opening the page, or the extension turning
            // active while the page is open, loads it (loadExtenders).
            continue;
        case ModuleState::Loaded: {
            const auto registered = module->extensions.constFind(extension.localId);
            if (registered == module->extensions.cend()) {
                continue;
            }
            if (!registered->domain) {
                entry.insert(QStringLiteral("error"), QStringLiteral("the section's staging domain was deleted"));
                break;
            }
            entry.insert(QStringLiteral("qmlSource"), registered->qmlSource);
            entry.insert(QStringLiteral("domain"), QVariant::fromValue<QObject*>(registered->domain.data()));
            break;
        }
        case ModuleState::DisabledAfterCrash:
            entry.insert(QStringLiteral("moduleFailed"), true);
            entry.insert(QStringLiteral("disabledAfterCrash"), true);
            entry.insert(QStringLiteral("error"), module->reason);
            break;
        case ModuleState::Failed:
            entry.insert(QStringLiteral("moduleFailed"), true);
            entry.insert(QStringLiteral("error"), module->reason);
            break;
        }
        list.append(entry);
    }
    return list;
}

QList<FacetHost::RefusedExtension> FacetHost::refusedExtensions() const
{
    return m_refusedExtensions;
}

QStringList FacetHost::extensionsOn(const QString& pageId) const
{
    QStringList ids;
    const QList<ActiveExtension> extensions = m_extensionsByPage.value(pageId);
    for (const ActiveExtension& extension : extensions) {
        ids.append(FacetHost::pageId(extension.moduleId, extension.localId));
    }
    return ids;
}

void FacetHost::disableAfterCrash(Module& module)
{
    transition(module, ModuleState::DisabledAfterCrash,
               QStringLiteral("it was loading when Facet last stopped unexpectedly"));
}

QStringList FacetHost::moduleIds() const
{
    QStringList ids = m_modules.keys();
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::optional<FacetHost::ModuleState> FacetHost::state(const QString& moduleId) const
{
    const std::shared_ptr<Module> module = m_modules.value(moduleId);
    if (!module) {
        return std::nullopt;
    }
    return module->state;
}

QString FacetHost::stateReason(const QString& moduleId) const
{
    const std::shared_ptr<Module> module = m_modules.value(moduleId);
    return module ? module->reason : QStringLiteral("no module %1").arg(moduleId);
}

std::optional<FacetModuleMetadata> FacetHost::metadata(const QString& moduleId) const
{
    const std::shared_ptr<Module> module = m_modules.value(moduleId);
    if (!module) {
        return std::nullopt;
    }
    return module->metadata;
}

QList<PhosphorRegistry::RefusedPlugin> FacetHost::refusedModules() const
{
    return m_catalog->refused();
}

QList<StagingDomain*> FacetHost::moduleDomains(const QString& moduleId) const
{
    QList<StagingDomain*> domains;
    const std::shared_ptr<Module> module = m_modules.value(moduleId);
    if (module) {
        for (const QPointer<StagingDomain>& domain : module->domains) {
            domains.append(domain.data());
        }
    }
    return domains;
}

QList<FacetHost::ModuleExtension> FacetHost::moduleExtensions(const QString& moduleId) const
{
    QList<ModuleExtension> extensions;
    const std::shared_ptr<Module> module = m_modules.value(moduleId);
    if (!module) {
        return extensions;
    }
    for (const FacetExtensionMetadata& declared : module->metadata.extensions) {
        const auto registered = module->extensions.constFind(declared.id);
        if (registered != module->extensions.cend()) {
            extensions.append(ModuleExtension{declared.id, registered->qmlSource, registered->domain.data()});
        }
    }
    return extensions;
}

PhosphorConfig::IBackend*
FacetHost::sharedConfigBackend(const QString& path,
                               const std::function<std::unique_ptr<PhosphorConfig::IBackend>(const QString&)>& create)
{
    // The backends went with the ApplicationController.
    if (!m_configBackends) {
        return nullptr;
    }
    return m_configBackends->shared(path, create);
}

PhosphorConfig::IBackend* FacetHost::configBackend(const QString& path) const
{
    // No backends once the ApplicationController is gone. And no module can
    // have opened a relative path (FacetContext::configBackend refuses one),
    // while resolving it against the working directory could name a file
    // one did open.
    if (!m_configBackends || !QDir::isAbsolutePath(path)) {
        return nullptr;
    }
    return m_configBackends->find(path);
}

QList<ISearchProvider*> FacetHost::searchProviders(const QString& moduleId) const
{
    QList<ISearchProvider*> providers;
    const std::shared_ptr<Module> module = m_modules.value(moduleId);
    if (module) {
        for (const FacetContext::SearchProviderRegistration& provider : module->searchProviders) {
            if (!provider.isObject || provider.object) {
                providers.append(provider.provider);
            }
        }
    }
    return providers;
}

QStringList FacetHost::boundPages(const QString& moduleId) const
{
    const std::shared_ptr<Module> module = m_modules.value(moduleId);
    return module ? module->boundPages : QStringList();
}

} // namespace PhosphorControl

#include "facethost.moc"
