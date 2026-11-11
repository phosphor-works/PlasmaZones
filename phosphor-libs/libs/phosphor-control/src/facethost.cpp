// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorControl/FacetHost.h"

#include "PhosphorControl/ApplicationController.h"
#include "PhosphorControl/FacetContext.h"
#include "PhosphorControl/FacetModule.h"
#include "PhosphorControl/PageController.h"
#include "PhosphorControl/PageRegistry.h"
#include "PhosphorControl/StagingDomain.h"

#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QVariantMap>

#include <algorithm>
#include <memory>
#include <utility>

namespace PhosphorControl {

namespace {
Q_LOGGING_CATEGORY(lcFacetHost, "phosphorcontrol.facethost")

const QString CategoryPrefix = QStringLiteral("@category/");
const QString DefaultCategory = QStringLiteral("apps");

// The reason for a module one of whose placeholders could not be
// registered. Placeholders are placed once, so loading it cannot work until
// Facet restarts.
const QString PageNotPlaced = QStringLiteral("a page of it could not take its place in the sidebar");

const QLatin1String RecordLoadingKey("loading");
const QLatin1String RecordPidKey("pid");
const QLatin1String RecordProcessKey("process");
const QLatin1String RecordDisabledKey("disabled");

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

// What tells one process apart from a later one that reuses its pid: the
// boot it runs in and the time it started, in clock ticks since boot (field
// 22 of /proc/<pid>/stat). Empty when it cannot be read, which includes a
// process that no longer exists. Wayland-only project, so /proc is there.
QString processIdentity(qint64 pid)
{
    if (pid <= 0) {
        return QString();
    }
    QFile bootFile(QStringLiteral("/proc/sys/kernel/random/boot_id"));
    QFile statFile(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!bootFile.open(QIODevice::ReadOnly) || !statFile.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QByteArray boot = bootFile.readAll().trimmed();
    const QByteArray stat = statFile.readAll();
    // The command name in field 2 may hold spaces and parentheses, so count
    // fields from the last ')'. Field 3 is the first one after it.
    const qsizetype close = stat.lastIndexOf(')');
    if (boot.isEmpty() || close < 0) {
        return QString();
    }
    const QList<QByteArray> fields = stat.mid(close + 1).simplified().split(' ');
    constexpr qsizetype StartTimeIndex = 22 - 3;
    if (fields.size() <= StartTimeIndex) {
        return QString();
    }
    return QString::fromLatin1(boot + '/' + fields.at(StartTimeIndex));
}

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
    /// The category the module's node went under, empty when it is nested
    /// under another module.
    QString category;
    /// The module node it is nested under, empty for its category.
    QString nodeParent;
    bool nodeRegistered = false;
    QStringList boundPages;
    QList<QPointer<StagingDomain>> domains;
    struct Extension
    {
        QPointer<StagingDomain> domain;
        QUrl qmlSource;
    };
    /// The sections it registered while loading, by local extension id.
    QHash<QString, Extension> extensions;
};

struct FacetHost::ActiveExtension
{
    QString moduleId;
    QString localId;
    QString targetPage;
    QString slot;
    QString name;
    int order = 0;
};

struct FacetHost::LoadRecord
{
    QString loading;
    qint64 pid = 0;
    /// processIdentity() of the loading process, so a later process that
    /// reuses its pid is not mistaken for it.
    QString process;
    QStringList disabled;
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
{
    if (!m_controller) {
        qFatal("FacetHost: controller must not be null");
    }
    // An empty state location would put the record at the filesystem root.
    // No location means no crash memory, said once at start().
    const QString stateDir = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    if (!stateDir.isEmpty()) {
        m_sentinelPath = stateDir + QStringLiteral("/phosphor-works/facet-loading");
    }
}

FacetHost::~FacetHost()
{
    if (m_controller) {
        m_controller->clearNavigationHook(this);
        m_controller->setExtensionProvider({});
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
    m_sentinelPath = path;
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

void FacetHost::start()
{
    if (m_started) {
        qCWarning(lcFacetHost) << "FacetHost: start() called twice";
        return;
    }
    m_started = true;
    m_starting = true;
    m_collator = QCollator(m_locale);

    for (const BaseCategory& base : BaseCategories) {
        const QString id = QString::fromLatin1(base.id);
        m_categories.insert(id,
                            FacetCategoryMetadata{id,
                                                  QCoreApplication::translate("PhosphorControl::FacetHost", base.name),
                                                  QString::fromLatin1(base.icon), base.order});
    }

    // A module left in the record's "loading" field was loading when a Facet
    // process ended without finishing it. Unless that process is still
    // running (a second instance mid-load), the module is disabled, and the
    // record keeps it disabled across starts until it is retried.
    if (m_sentinelPath.isEmpty()) {
        qCInfo(lcFacetHost) << "FacetHost: no state directory, so a module that crashes Facet is not remembered";
    }
    LoadRecord record = readLoadRecord();
    if (!record.loading.isEmpty()) {
        if (record.pid != QCoreApplication::applicationPid() && !record.process.isEmpty()
            && processIdentity(record.pid) == record.process) {
            qCInfo(lcFacetHost).noquote()
                << "FacetHost: process" << record.pid << "is loading module" << record.loading;
        } else {
            const QString crashed = record.loading;
            record.loading.clear();
            record.pid = 0;
            record.process.clear();
            if (!record.disabled.contains(crashed)) {
                record.disabled.append(crashed);
            }
            if (writeLoadRecord(record)) {
                qCWarning(lcFacetHost).noquote()
                    << "FacetHost: module" << crashed
                    << "was loading when Facet last stopped unexpectedly; it stays disabled until it is retried";
            } else {
                // Unable to clear the loading field, the record would name the
                // module again at every start. Ignore it rather than disable
                // the module forever.
                record.disabled.removeAll(crashed);
                qCWarning(lcFacetHost).noquote()
                    << "FacetHost: module" << crashed << "was loading when Facet last stopped, but" << m_sentinelPath
                    << "cannot be written, so it is not disabled";
            }
        }
    }
    m_disabled = QSet<QString>(record.disabled.cbegin(), record.disabled.cend());

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
        if (module->state == ModuleState::Available && m_disabled.contains(module->metadata.id)) {
            disableAfterCrash(*module);
        }
    }

    resolveExtensions();
    m_controller->setExtensionProvider([this](const QString& pageId, const QString& slot) {
        return extensionsFor(pageId, slot);
    });

    connect(m_catalog.get(), &PhosphorRegistry::PluginCatalog::pluginAdded, this, &FacetHost::onPluginAdded);
    connect(m_catalog.get(), &PhosphorRegistry::PluginCatalog::pluginRemoved, this, &FacetHost::onPluginRemoved);
    if (!m_controller->setNavigationHook(this, [this](const QString& pageId) {
            onNavigation(pageId);
        })) {
        qCWarning(lcFacetHost) << "FacetHost: another component holds the navigation hook, so opening a module's page "
                                  "does not load it; call loadModule() instead";
    }

    // Announce what start() found, now that every placeholder and the hook
    // exist, so a slot reacting to it sees a complete sidebar.
    m_starting = false;
    for (const QString& id : std::as_const(found)) {
        const std::shared_ptr<Module> module = m_modules.value(id);
        if (!module || module->state == ModuleState::Available) {
            continue;
        }
        Q_EMIT moduleStateChanged(id, module->state);
        if (module->state == ModuleState::Failed) {
            Q_EMIT moduleFailed(id, module->reason);
        }
    }
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
    module->metadata = FacetModuleMetadata::parse(plugin->metaData, m_locale, &errors, &warnings);
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

void FacetHost::addModule(const QString& moduleId)
{
    const std::shared_ptr<Module> module = parseModule(moduleId);
    if (!module) {
        return;
    }
    m_modules.insert(moduleId, module);
    if (module->state == ModuleState::Failed) {
        Q_EMIT moduleStateChanged(moduleId, module->state);
        Q_EMIT moduleFailed(moduleId, module->reason);
        return;
    }
    placeModules({module});
    if (module->state == ModuleState::Available && m_disabled.contains(moduleId)) {
        disableAfterCrash(*module);
    }

    // The new module can be the target that makes another module's extension
    // active, or extend pages itself.
    const QStringList before = m_extensionsByPage.keys();
    resolveExtensions();
    QSet<QString> touched(before.cbegin(), before.cend());
    for (auto it = m_extensionsByPage.cbegin(); it != m_extensionsByPage.cend(); ++it) {
        touched.insert(it.key());
    }
    // A module loaded before its target arrived attaches its section now.
    for (const std::shared_ptr<Module>& other : std::as_const(m_modules)) {
        if (other->state == ModuleState::Loaded) {
            attachExtensions(*other);
        }
    }
    for (const QString& page : std::as_const(touched)) {
        m_controller->notifyExtensionsChanged(page);
    }
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
    if (existing->state != ModuleState::Failed) {
        return;
    }
    if (!existing->nodeRegistered) {
        // Its metadata was invalid, so it has no placeholders yet. Read it
        // again and place it if it is valid now.
        m_modules.remove(moduleId);
        addModule(moduleId);
        return;
    }
    // Offered again: its file changed after a failed load, or it was removed
    // and installed again. Its placeholders are already in the sidebar and
    // cannot move, so it can be tried again only when its metadata still
    // matches them and all of them are there.
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
        if (m_disabled.contains(moduleId)) {
            // Removing and reinstalling a module that crashed Facet does not
            // turn it back on; only a retry does.
            disableAfterCrash(*existing);
        } else {
            transition(*existing, ModuleState::Available, QString());
        }
    } else {
        transition(*existing, ModuleState::Failed,
                   QStringLiteral("its metadata changed on disk after Facet started, so it can be used only after a "
                                  "restart"),
                   false);
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
    transition(*module, ModuleState::Failed, QStringLiteral("its file was removed or is no longer accepted"), false);
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
            for (const ForeignRoot& root : std::as_const(waiting)) {
                const FacetPageMetadata& page = root.module->metadata.pages.at(root.pageIndex);
                qCInfo(lcFacetHost).noquote()
                    << "FacetHost: page" << pageId(root.module->metadata.id, page.id) << "goes under" << page.parent
                    << "in a cycle of pages placed under each other; it stays under its module";
                placeForeign(root, root.module->metadata.id);
            }
            break;
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

void FacetHost::onNavigation(const QString& pageId)
{
    // The page's own module, then every module that adds a section to it, so
    // the page shows complete on its first paint.
    QStringList toLoad;
    const QString owner = owningModule(pageId);
    if (!owner.isEmpty()) {
        toLoad.append(owner);
    }
    const QList<std::shared_ptr<ActiveExtension>> extensions = m_extensionsByPage.value(pageId);
    for (const auto& extension : extensions) {
        if (!toLoad.contains(extension->moduleId)) {
            toLoad.append(extension->moduleId);
        }
    }
    for (const QString& moduleId : std::as_const(toLoad)) {
        const std::shared_ptr<Module> module = m_modules.value(moduleId);
        if (!module || module->state != ModuleState::Available) {
            // A module page's placeholder says why and offers a retry, and a
            // section's FacetSlot shows the module's error.
            continue;
        }
        QString error;
        if (!loadModule(moduleId, &error)) {
            qCWarning(lcFacetHost).noquote()
                << "FacetHost: opening" << pageId << "could not load module" << moduleId << "-" << error;
        }
    }
}

bool FacetHost::loadModule(const QString& moduleId, QString* errorString)
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
    if (!m_loadingModule.isEmpty()) {
        // Reached from inside another module's load (a slot that navigated).
        // Running this one now would overwrite the load record of the one
        // still in registerInto, or run registerInto twice for the same
        // module.
        return fail(QStringLiteral("module %1 is still loading").arg(m_loadingModule));
    }
    m_loadingModule = moduleId;
    const auto loading = qScopeGuard([this]() {
        m_loadingModule.clear();
    });

    recordLoading(moduleId);
    QString loadError;
    QObject* const root = m_catalog->load(moduleId, &loadError);
    if (!root) {
        clearLoading();
        transition(*module, ModuleState::Failed, loadError);
        return fail(loadError);
    }
    auto* const facetModule = qobject_cast<IFacetModule*>(root);
    if (!facetModule) {
        clearLoading();
        const QString reason = QStringLiteral("its root object does not implement IFacetModule");
        transition(*module, ModuleState::Failed, reason, false);
        return fail(reason);
    }
    // The placeholders were built from the metadata read at start(). Code
    // loaded from a file that changed since then expects other pages, and
    // binding it would half-match them.
    const std::optional<PhosphorRegistry::PluginMetadata> offered = m_catalog->plugin(moduleId);
    if (!offered || offered->metaData != module->rawMetadata) {
        clearLoading();
        const QString reason =
            QStringLiteral("its metadata changed on disk after Facet started, so it can be used only after a restart");
        transition(*module, ModuleState::Failed, reason, false);
        return fail(reason);
    }

    FacetContext context(module->metadata);
    facetModule->registerInto(context);
    clearLoading();

    // Check the whole registration before anything takes effect.
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
    // A missing placeholder never comes back, so that failure is not one a
    // retry can fix.
    bool placeholderMissing = false;
    for (const FacetPageMetadata& page : std::as_const(module->metadata.pages)) {
        if (!m_controller->registry()->hasPage(pageId(moduleId, page.id))) {
            errors.append(QStringLiteral("page %1 has no place in the sidebar").arg(page.id));
            placeholderMissing = true;
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
    if (!errors.isEmpty()) {
        const QString reason = errors.join(QStringLiteral("; "));
        transition(*module, ModuleState::Failed, reason, !placeholderMissing);
        return fail(reason);
    }

    for (const auto& page : std::as_const(context.m_pages)) {
        const QString scoped = pageId(moduleId, page.localId);
        if (m_controller->bindPage(scoped, page.controller.data(), page.qmlSource)) {
            module->boundPages.append(scoped);
        } else {
            // Unreachable after the checks above, which cover every reason
            // the registry refuses a bind. Say so loudly if it ever is,
            // since what was bound before it stays bound.
            qCCritical(lcFacetHost).noquote() << "FacetHost: module" << moduleId << "page" << scoped
                                              << "passed every check and was still refused by the registry";
        }
    }
    for (const QPointer<StagingDomain>& domain : std::as_const(context.m_domains)) {
        if (m_controller->registerDomain(domain.data())) {
            module->domains.append(domain);
        }
    }
    for (const auto& extension : std::as_const(context.m_extensions)) {
        module->extensions.insert(extension.localId, Module::Extension{extension.domain, extension.qmlSource});
    }
    // Its sections join the pages they are added to: dirty mark and reset.
    // transition() then tells the pages' FacetSlots.
    attachExtensions(*module);
    transition(*module, ModuleState::Loaded, QString());
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
    if (module->state == ModuleState::DisabledAfterCrash) {
        m_disabled.remove(moduleId);
        persistDisabled();
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
    if (state == ModuleState::Failed) {
        qCWarning(lcFacetHost).noquote() << "FacetHost: module" << module.metadata.id << "failed:" << reason;
    }
    if (m_starting) {
        return;
    }
    Q_EMIT moduleStateChanged(module.metadata.id, state);
    if (state == ModuleState::Failed) {
        Q_EMIT moduleFailed(module.metadata.id, reason);
    } else if (state == ModuleState::Loaded) {
        Q_EMIT moduleLoaded(module.metadata.id);
    }
    // Its sections on other pages show it loaded, or its error card.
    notifyExtendedPages(module.metadata.id);
}

void FacetHost::attachExtensions(const Module& module)
{
    for (auto it = m_extensionsByPage.cbegin(); it != m_extensionsByPage.cend(); ++it) {
        for (const auto& extension : it.value()) {
            if (extension->moduleId != module.metadata.id) {
                continue;
            }
            const auto registered = module.extensions.constFind(extension->localId);
            if (registered != module.extensions.cend() && registered->domain) {
                m_controller->attachDomainToPage(registered->domain.data(), it.key());
            }
        }
    }
}

void FacetHost::notifyExtendedPages(const QString& moduleId)
{
    if (!m_controller) {
        return;
    }
    // Collected before notifying, since a slot handler may navigate and so
    // change the extension map.
    QStringList touched;
    for (auto it = m_extensionsByPage.cbegin(); it != m_extensionsByPage.cend(); ++it) {
        const bool extendsPage = std::any_of(it->cbegin(), it->cend(), [&moduleId](const auto& extension) {
            return extension->moduleId == moduleId;
        });
        if (extendsPage) {
            touched.append(it.key());
        }
    }
    for (const QString& page : std::as_const(touched)) {
        m_controller->notifyExtensionsChanged(page);
    }
}

void FacetHost::resolveExtensions()
{
    m_extensionsByPage.clear();
    m_refusedExtensions.clear();
    for (auto moduleIt = m_modules.cbegin(); moduleIt != m_modules.cend(); ++moduleIt) {
        const Module& module = *moduleIt.value();
        if (!module.nodeRegistered) {
            continue; // invalid metadata or not placed: nothing of it is shown
        }
        for (const FacetExtensionMetadata& extension : module.metadata.extensions) {
            const QString scoped = pageId(module.metadata.id, extension.id);
            const QString targetModuleId = extension.target.section(QLatin1Char(':'), 0, 0);
            const QString targetLocalPage = extension.target.section(QLatin1Char(':'), 1);
            const std::shared_ptr<Module> target = m_modules.value(targetModuleId);
            if (!target || !target->nodeRegistered) {
                qCInfo(lcFacetHost).noquote() << "FacetHost: extension" << scoped << "targets" << extension.target
                                              << "whose module is not installed; it stays inactive";
                continue;
            }
            QString reason;
            if (!target->metadata.page(targetLocalPage)) {
                reason = QStringLiteral("its target page %1 does not exist").arg(extension.target);
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
            auto active = std::make_shared<ActiveExtension>();
            active->moduleId = module.metadata.id;
            active->localId = extension.id;
            active->targetPage = extension.target;
            active->slot = extension.slot;
            active->name = extension.name;
            active->order = extension.order;
            m_extensionsByPage[extension.target].append(active);
        }
    }
    for (auto it = m_extensionsByPage.begin(); it != m_extensionsByPage.end(); ++it) {
        std::sort(it->begin(), it->end(), [](const auto& a, const auto& b) {
            if (a->order != b->order) {
                return a->order < b->order;
            }
            if (a->moduleId != b->moduleId) {
                return a->moduleId < b->moduleId;
            }
            return a->localId < b->localId;
        });
    }
    std::sort(m_refusedExtensions.begin(), m_refusedExtensions.end(),
              [](const RefusedExtension& a, const RefusedExtension& b) {
                  return a.id < b.id;
              });
    // Resolution runs again whenever a module arrives, so warn once per
    // refused extension, not once per pass.
    for (const RefusedExtension& refused : std::as_const(m_refusedExtensions)) {
        if (!m_warnedExtensions.contains(refused.id)) {
            m_warnedExtensions.insert(refused.id);
            qCWarning(lcFacetHost).noquote() << "FacetHost: skipping extension" << refused.id << "-" << refused.reason;
        }
    }
}

QVariantList FacetHost::extensionsFor(const QString& pageId, const QString& slot) const
{
    QVariantList list;
    const QList<std::shared_ptr<ActiveExtension>> extensions = m_extensionsByPage.value(pageId);
    for (const auto& extension : extensions) {
        if (extension->slot != slot) {
            continue;
        }
        const std::shared_ptr<Module> module = m_modules.value(extension->moduleId);
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), FacetHost::pageId(extension->moduleId, extension->localId));
        entry.insert(QStringLiteral("moduleId"), extension->moduleId);
        entry.insert(QStringLiteral("moduleName"), module->metadata.name);
        entry.insert(QStringLiteral("name"), extension->name);
        switch (module->state) {
        case ModuleState::Available:
            continue; // not loaded yet: opening the page loads it first
        case ModuleState::Loaded: {
            const auto registered = module->extensions.constFind(extension->localId);
            if (registered == module->extensions.cend()) {
                continue;
            }
            entry.insert(QStringLiteral("qmlSource"), registered->qmlSource);
            entry.insert(QStringLiteral("domain"), QVariant::fromValue<QObject*>(registered->domain.data()));
            break;
        }
        case ModuleState::Failed:
        case ModuleState::DisabledAfterCrash:
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
    const QList<std::shared_ptr<ActiveExtension>> extensions = m_extensionsByPage.value(pageId);
    for (const auto& extension : extensions) {
        ids.append(FacetHost::pageId(extension->moduleId, extension->localId));
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
            if (domain) {
                domains.append(domain.data());
            }
        }
    }
    return domains;
}

QStringList FacetHost::boundPages(const QString& moduleId) const
{
    const std::shared_ptr<Module> module = m_modules.value(moduleId);
    return module ? module->boundPages : QStringList();
}

FacetHost::LoadRecord FacetHost::readLoadRecord() const
{
    LoadRecord record;
    if (m_sentinelPath.isEmpty()) {
        return record;
    }
    QFile file(m_sentinelPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return record;
    }
    const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
    record.loading = json.value(RecordLoadingKey).toString();
    record.pid = json.value(RecordPidKey).toInteger();
    record.process = json.value(RecordProcessKey).toString();
    for (const QJsonValue& id : json.value(RecordDisabledKey).toArray()) {
        if (id.isString() && !id.toString().isEmpty() && !record.disabled.contains(id.toString())) {
            record.disabled.append(id.toString());
        }
    }
    return record;
}

bool FacetHost::writeLoadRecord(const LoadRecord& record) const
{
    if (m_sentinelPath.isEmpty()) {
        return false;
    }
    QJsonObject json;
    if (!record.loading.isEmpty()) {
        json.insert(RecordLoadingKey, record.loading);
        json.insert(RecordPidKey, record.pid);
        json.insert(RecordProcessKey, record.process);
    }
    if (!record.disabled.isEmpty()) {
        json.insert(RecordDisabledKey, QJsonArray::fromStringList(record.disabled));
    }
    QDir().mkpath(QFileInfo(m_sentinelPath).absolutePath());
    // Atomic replace, because the file also holds the disabled list, which a
    // write cut short by a crash must not lose.
    QSaveFile file(m_sentinelPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(json).toJson(QJsonDocument::Compact)) < 0
        || !file.commit()) {
        qCWarning(lcFacetHost).noquote() << "FacetHost: could not write the load record" << m_sentinelPath;
        return false;
    }
    return true;
}

bool FacetHost::recordLoading(const QString& moduleId)
{
    if (m_sentinelPath.isEmpty()) {
        return false;
    }
    LoadRecord record;
    record.loading = moduleId;
    record.pid = QCoreApplication::applicationPid();
    record.process = processIdentity(record.pid);
    record.disabled = QStringList(m_disabled.cbegin(), m_disabled.cend());
    std::sort(record.disabled.begin(), record.disabled.end());
    return writeLoadRecord(record);
}

void FacetHost::clearLoading()
{
    if (m_sentinelPath.isEmpty()) {
        return;
    }
    // A record left naming the module would make the next start disable a
    // module that loaded fine, so try once more before giving up.
    if (!persistDisabled() && !persistDisabled()) {
        qCWarning(lcFacetHost).noquote() << "FacetHost: the load record" << m_sentinelPath << "still names module"
                                         << m_loadingModule
                                         << "as loading; the next start will treat it as having crashed";
    }
}

bool FacetHost::persistDisabled()
{
    if (m_sentinelPath.isEmpty()) {
        return false;
    }
    LoadRecord record;
    record.disabled = QStringList(m_disabled.cbegin(), m_disabled.cend());
    std::sort(record.disabled.begin(), record.disabled.end());
    return writeLoadRecord(record);
}

} // namespace PhosphorControl

#include "facethost.moc"
