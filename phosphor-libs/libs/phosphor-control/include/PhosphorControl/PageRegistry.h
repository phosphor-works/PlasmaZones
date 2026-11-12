// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "phosphorcontrol_export.h"

namespace PhosphorControl {

class PageController;

/**
 * Holds the catalogue of pages an application has registered.
 *
 * Pages are organised as a tree: each entry has an optional parentId.
 * Top-level pages (parentId empty) are the entries the sidebar root
 * displays; child pages are listed under their parent for drill-down.
 *
 * The registry stores QPointer<PageController> weak references — ownership
 * lives with whoever constructed the controller (typically the
 * ApplicationController owning a parent QObject chain), and lookups via
 * controller(id) return nullptr if the controller has been destroyed
 * out-of-order. The catalogue only ever GROWS: entries are never removed
 * at runtime, but dynamic page registration (post-startup `registerPage`
 * calls) is supported and fires `pageRegistered`. An entry also changes in
 * place, keeping its place in the tree: setPageVisibility restamps its
 * simple/advanced tier (visibleSetChanged); bindPage replaces its controller
 * and QML source and sets its passesContext flag (pageBound, plus
 * visibleSetChanged when the entry gains or loses its QML); setPassesContext
 * sets that flag alone, and announces nothing. None of them fires
 * pageRegistered. Dynamic removal is intentionally NOT supported — apps that
 * need plugin-style hot-unload should rebuild the controller.
 */
class PHOSPHORCONTROL_EXPORT PageRegistry : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(PageRegistry)
    QML_UNCREATABLE("PageRegistry is owned by ApplicationController.")
    /// Master toggle for the two-tier "simple vs advanced" sidebar. When
    /// false, entries marked AdvancedOnly are filtered out of the tree
    /// accessors (visibleChildPages / firstVisibleLeafId /
    /// topLevelPagesData / childPagesData) and any virtual
    /// category left with no visible descendant vanishes with them; when
    /// true, SimpleOnly entries are hidden instead. Defaults to true so an
    /// app that never marks a page keeps the classic "show everything"
    /// behaviour. The value is app UI state — the app drives it (persisted
    /// however the app likes); the registry only consumes it for filtering.
    Q_PROPERTY(bool showAdvanced READ showAdvanced WRITE setShowAdvanced NOTIFY showAdvancedChanged)
    /// The upward-walk depth bound, readable from QML so a consumer running
    /// its own parent walk cannot drift from the C++ cap. CONSTANT because
    /// the getter returns a static constexpr.
    Q_PROPERTY(int maxParentChainHops READ maxParentChainHops CONSTANT)

public:
    /// Per-entry visibility tier for the simple/advanced split. `Always`
    /// (the default) shows the page in both modes; `AdvancedOnly` hides it
    /// while `showAdvanced` is false; `SimpleOnly` hides it while
    /// `showAdvanced` is true (for purpose-built simplified pages that a
    /// richer advanced page supersedes). Filtering happens only in the
    /// QML-facing tree accessors — `pageData` / `allPagesData` /
    /// `controller` stay unfiltered so a currently-shown or dirty page
    /// always resolves regardless of mode.
    enum class PageVisibility {
        Always,
        AdvancedOnly,
        SimpleOnly,
    };
    Q_ENUM(PageVisibility)

    /// The most ancestors a page may have. registerPage refuses a page any
    /// deeper, and refuses a parent that is not registered yet, so the
    /// catalogue is a forest no deeper than this, and every upward walk
    /// (allowedInMode, ApplicationController::parentChainFor, Breadcrumbs)
    /// reaches a root within this many parent links. Declared here rather
    /// than per walk: they have to agree with the registration bound, and a
    /// comment asking a human to keep constants in step is not a mechanism.
    static constexpr int MaxParentChainHops = 32;

    /// The same bound, readable from QML. Breadcrumbs runs its own upward walk
    /// with a seen-set cycle guard and needs the identical cap: a QML copy of
    /// the literal is exactly the "two constants a human must keep in step"
    /// the comment above rejects, one language further out.
    int maxParentChainHops() const
    {
        return MaxParentChainHops;
    }

    /**
     * Public-by-value view of a registry entry. Consumers receive copies
     * via topLevelPages() / childPages() / allPages() / entry(); they
     * may NOT mutate the underlying controller pointer to detach it
     * from the registry. The QPointer surface is read-only by
     * convention — the registry is the only writer. allPagesRef() hands
     * out the live list by const reference instead of a copy, for
     * read-only whole-list walks; see its invalidation note (the
     * reference does not survive the next registry mutation).
     *
     * Code that needs to act on the controller should prefer the
     * explicit `controller(id)` accessor (which performs the live-pointer
     * null-check) over Entry.controller. The field is public because
     * Entry is a plain value type, and C++ walks such as
     * ApplicationController::dirtyPageIds read it directly to avoid a
     * second lookup.
     */
    struct Entry
    {
        QString id;
        QString parentId; // empty == top-level
        QString title; // already translated by caller
        QString iconSource; // freedesktop icon name or QML asset URL; optional
        QUrl qmlSource; // page QML file URL
        // QPointer guards against dangling lookups if a consumer destroys
        // the controller out-of-order relative to the registry. Symmetric
        // with ApplicationController::m_domains.
        QPointer<PageController> controller;
        /// When true, the Sidebar renders this entry as an inline-expandable
        /// category header rather than a drill-down target — its children
        /// appear indented under it (toggleable) instead of replacing the
        /// list. Useful for shallow grouping ("Display", "Rules", `*-cat`
        /// buckets) inside an otherwise drill-down sidebar.
        bool isCollapsible = false;
        /// When true, the Sidebar draws a horizontal divider line in the
        /// row slot immediately after this entry. Used to break up the
        /// vertical rhythm of long sidebars without grouping items under
        /// a category header. Suppressed while a search filter is active
        /// — dividers are navigation ornament, not match metadata.
        bool hasDividerAfter = false;
        /// Simple/advanced tier for this page. Declared at registration
        /// (set it on the Entry passed to registerPage); defaults to
        /// Always so unclassified pages show in both modes. See
        /// PageVisibility. setPageVisibility() remains for late
        /// reclassification but registration-time declaration is the
        /// canonical path — it has NO in-tree caller and is retained for
        /// plugin-style registration, where a host registers a page before
        /// it knows the tier. Do not delete it as dead code: its
        /// visibleSetChanged emit is the only reason that signal exists
        /// separately from showAdvancedChanged.
        PageVisibility visibility = PageVisibility::Always;
        /// Optional id of this page's other-mode counterpart. When a mode
        /// flip (or a deep link) hides this page, the app's navigation
        /// gate should land on the counterpart instead of a generic
        /// fallback — e.g. a purpose-built SimpleOnly page and the
        /// AdvancedOnly page it condenses point at each other. Empty
        /// means "no counterpart; use the app fallback". The registry
        /// stores the mapping; it does not act on it.
        /// Braced default, so an aggregate initialization that stops before
        /// this field does not trip -Wmissing-field-initializers.
        QString counterpartId{};
        /// The page's QML root declares `controller` and `settingsApp`, and
        /// PageHost sets them when it creates the page, before any binding
        /// runs, so the page may declare them `required`. A page without the
        /// flag gets them assigned after creation, if it declares them. Every
        /// page bound with bindPage() has it: it is part of the Facet module
        /// contract.
        ///
        /// PageHost reads the flag only when it creates the page, and
        /// nothing announces a change of it. So changing it for a page
        /// already built does nothing to that page; it counts the next time
        /// PageHost creates the page: its first visit, or a bind that gives
        /// it other QML. Clearing it for a page whose QML declares the
        /// properties `required` (every Facet page) makes that creation fail.
        bool passesContext = false;
    };

    explicit PageRegistry(QObject* parent = nullptr);
    ~PageRegistry() override;

    /** Register a page. Emits `pageRegistered(id)` and returns `true` on
     *  success. Warns and returns `false` on any of, in the order the code
     *  checks them: empty id, duplicate id, unknown parentId, a parent so deep
     *  that the page would have more than MaxParentChainHops ancestors, null
     *  controller, or the same controller already registered under another
     *  id. The intent is to surface
     *  programmer errors in the log without aborting startup; the
     *  PageRegistry's tree just won't contain the misconfigured
     *  entry — downstream Sidebar / Breadcrumbs / page-router lookups
     *  for it will return empty.
     *
     *  Callers that perform additional bookkeeping per registered page
     *  (e.g. ApplicationController tracking the page as a staging domain)
     *  must gate that bookkeeping on the returned bool — otherwise a
     *  rejected page leaks into half-registered state where downstream
     *  systems mutate it but the UI cannot see it. */
    bool registerPage(Entry entry);
    /** registerPage, placing the entry at @p siblingPosition among its
     *  parent's children instead of after them. A negative or out-of-range
     *  position appends. For a host that registers late but wants the entry
     *  where a registration at startup would have put it. */
    bool registerPage(Entry entry, qsizetype siblingPosition);

    /// Replace the controller and QML source of the registered page @p id,
    /// keeping its place in the tree, title, icon, tier and counterpart. This
    /// is how a page registered with a placeholder controller (a Facet module
    /// that has not loaded yet) gets its real one. Refuses an unknown id, a
    /// null controller, a controller whose id() is not @p id, or a controller
    /// already registered for another page. Emits pageBound when the
    /// controller or QML source changes, and visibleSetChanged as well when
    /// the page gains or loses its QML source. Rebinding the same controller
    /// and source is accepted silently.
    ///
    /// Every accepted bind, the silent one included, also sets the entry's
    /// passesContext: a bound page is created with `controller` and
    /// `settingsApp` already set, as the Facet module contract has it. That
    /// is announced by nothing of its own; PageHost reads the flag when a
    /// pageBound for a new QML source makes it create the page.
    bool bindPage(const QString& id, PageController* controller, const QUrl& qmlSource);

    /// Mark page @p id as one whose QML is created with `controller` and
    /// `settingsApp` already set (Entry::passesContext). Warns for an unknown
    /// id. Emits nothing: PageHost reads the flag only when it creates the
    /// page, so set it before the page is first shown. Set later, or cleared
    /// on a page bound with bindPage(), it does nothing to a page already
    /// built, and clearing it breaks the next creation of a page that
    /// declares the properties `required` (see Entry::passesContext).
    void setPassesContext(const QString& id, bool passesContext);

    /// Restamp a page's simple/advanced tier after it has been registered.
    /// No-op (with a warning) for an unknown id, and a no-op when the tier is
    /// already what you are setting. Declaring the tier at registration
    /// (Entry::visibility) is the canonical path; this is for a host that
    /// registers a page before it knows the tier. A real change emits
    /// visibleSetChanged() so consumers that cache a tier-filtered view stay
    /// correct whenever it is called. It does NOT emit showAdvancedChanged:
    /// the mode itself is untouched, only this one entry's tier.
    void setPageVisibility(const QString& id, PageVisibility visibility);

    bool showAdvanced() const;
    void setShowAdvanced(bool showAdvanced);

    Q_INVOKABLE bool hasPage(const QString& id) const;
    /** True iff @p id is a registered page that carries its own QML body, i.e.
     *  a navigable destination. False for an unknown id AND for a registered
     *  virtual CATEGORY (empty qmlSource), which has no page body to land on.
     *  Folds the "registered and navigable" test into one index lookup so a
     *  caller filtering by reachability need not copy a whole Entry to read
     *  two isEmpty() bits. */
    bool hasNavigablePage(const QString& id) const;
    /** True iff the page is reachable under the current showAdvanced mode:
     *  its own visibility tier passes AND so does every ancestor's. The
     *  ancestor walk matters because hiding a category hides its whole
     *  subtree in the rail, so a page whose own tier passes under a filtered
     *  parent has no row and no drill path — answering "yes" for it would
     *  send search, keyboard next/prev and the mode gate somewhere the user
     *  cannot navigate back from. Unknown ids return true: this is a tier
     *  filter, not an existence check; validate with hasPage() first. It
     *  does NOT apply the empty-category descendant rule the tree accessors
     *  use — that asks "is this category worth drawing", not "may the user
     *  navigate here".
     *
     *  The walk always reaches a root: registerPage refuses an unknown parent
     *  and a page more than MaxParentChainHops levels down. */
    bool pageAllowedInCurrentMode(const QString& id) const;
    /** pageAllowedInCurrentMode against an EXPLICIT mode rather than the
     *  current one. Same ancestor walk and same unknown-id semantics. Public
     *  so SearchController can classify an entry's reachability in BOTH modes
     *  (its per-result simple/advanced tag) without flipping the live mode
     *  back and forth. */
    bool allowedInMode(const QString& id, bool advanced) const;
    /** Log a warning for every entry whose `counterpartId` is broken. Listed
     *  in the order the warnings are emitted, which the tests assert:
     *  the counterpart is this entry ITSELF; it names a page that does not
     *  exist; it does not name this entry back (a one-way declaration
     *  dead-ends the RETURN flip on the app fallback); it sits in the SAME
     *  tier (so a mode flip would redirect to something equally hidden); it is
     *  not navigable (a category with no QML of its own, so the redirect would
     *  land on an empty page body); or it is itself unreachable in the mode
     *  that hides this entry (an ancestor category filters it out, so the
     *  redirect falls back anyway).
     *
     *  Reciprocity is checked BEFORE the tier fault and does not share its
     *  skip, because the two are independent: a pair that is both same-tier
     *  and one-way has two faults and must report both.
     *
     *  NOT exhaustive for the runtime gate: the app additionally requires the
     *  counterpart to be in its own valid-page set, which this cannot see.
     *  Counterparts are stored unvalidated at registration because the target
     *  may be registered later, and nothing else ever checks them: a typo
     *  silently degrades every affected mode flip and deep link to the
     *  fallback page, and looks exactly like correct operation. Call once
     *  after registration completes. Returns true when every counterpart
     *  resolves. */
    bool validateCounterparts() const;
    /** validateCounterparts for the entries @p ids alone, with the same
     *  checks and warnings. An unknown id, or an entry with no counterpart,
     *  passes. For a host that registers pages after the application checked
     *  its own (FacetHost, as modules take their place), so the application's
     *  pages are not warned about again. */
    bool validateCounterparts(const QStringList& ids) const;
    /** Depth-first search (sibling order) below `parentId` for the
     *  first navigable page visible under the current mode; empty string
     *  when nothing below the parent survives filtering. Pass an empty
     *  parentId to search from the root. This is the mode-aware
     *  replacement for a static "parent redirects to its first leaf"
     *  table. */
    /// Q_INVOKABLE: Breadcrumbs.qml routes a non-navigable ancestor crumb
    /// through this to avoid navigating into an empty page body.
    Q_INVOKABLE QString firstVisibleLeafId(const QString& parentId) const;
    Q_INVOKABLE PhosphorControl::PageController* controller(const QString& id) const;
    /** Look up an Entry by id. Returns a default-constructed (empty) Entry
     *  when the id is unknown — callers that need to distinguish "no such
     *  page" from "page exists with empty optional fields" should call
     *  hasPage() first. */
    Entry entry(const QString& id) const;
    /** The parentId of @p id, or an empty string when the id is unknown or the
     *  entry is top-level. Exists so an upward parent walk
     *  (ApplicationController::parentChainFor) can read one field per hop
     *  instead of copying a whole Entry to throw all but one field away. */
    QString parentIdOf(const QString& id) const;

    QList<Entry> topLevelPages() const;
    QList<Entry> childPages(const QString& parentId) const;
    /** Children of @p parentId that survive the current mode's filter,
     *  including the empty-category rule (a virtual node with no surviving
     *  descendant drops out). The Entry-returning twin of childPagesData();
     *  childPages() above is deliberately UNFILTERED, so anything building a
     *  navigable view wants this one. */
    QList<Entry> visibleChildPages(const QString& parentId) const;
    QList<Entry> allPages() const;
    /** The catalogue by reference, for whole-list walks that only READ,
     *  such as ApplicationController::dirtyPageIds. allPages() deep-copies
     *  every Entry, which such a walk would pay on every call for a list it
     *  only iterates. The list is in REGISTRATION order, not the order the
     *  sidebar draws; use pagesInTreeOrder() for anything the user sees in
     *  sequence. The reference stays valid until the next registry mutation
     *  that changes something (registerPage, bindPage, setPageVisibility,
     *  setPassesContext), any of which may reallocate or detach the list —
     *  do not hold it across one. */
    const QList<Entry>& allPagesRef() const;
    /** Every entry in the order the sidebar draws the tree: each page, then
     *  its children in their sibling order, depth first. This is the order
     *  keyboard next/previous and search walk, so they agree with the rail
     *  however the pages were registered. The pointers stay valid until the
     *  next registry mutation, like allPagesRef(). */
    QList<const Entry*> pagesInTreeOrder() const;

    // QML-facing accessors. Return dicts whose canonical key set lives in
    // entryToVariant() (pageregistry.cpp): id, parentId, title, iconSource,
    // qmlSource, isCollapsible, hasDividerAfter, hasQmlSource, passesContext.
    // PageHost reads passesContext to decide how it creates the page.
    // `visibility` and `counterpartId` are deliberately C++-only: tier
    // filtering is applied here, inside the tree accessors, so QML never needs
    // to re-derive it, and the counterpart is navigation policy the app owns.
    //
    // The rail moved to SidebarRows and Breadcrumbs uses pageData /
    // firstVisibleLeafId, so the list accessors here serve the app's own QML
    // (the settings app's drill restore asks childPagesData whether a page
    // has visible children) and out-of-tree consumers. Callers are QML, which
    // no build checks, so grep before treating one as unused. Covered by tests
    // so the serialization contract cannot rot.
    Q_INVOKABLE QVariantList topLevelPagesData() const;
    Q_INVOKABLE QVariantList childPagesData(const QString& parentId) const;
    Q_INVOKABLE QVariantMap pageData(const QString& id) const;
    /** Full flat list of every registered page, in registration order,
     *  unfiltered. */
    Q_INVOKABLE QVariantList allPagesData() const;

    /// Key under which the page-data dicts above carry the title. Exposed so
    /// SidebarRows::flatPageData can override that field through the SAME
    /// constant the registry writes it with, rather than its own independent
    /// "title" literal — two spellings of one schema key means a rename here
    /// would silently ADD a second key over there instead of overriding, and
    /// the assertion on the literal would stay green while the UI read the
    /// un-overridden value.
    static QLatin1String titleKey();

Q_SIGNALS:
    void pageRegistered(const QString& id);
    /// A registered page got a new controller and QML source (bindPage).
    void pageBound(const QString& id);
    void showAdvancedChanged();
    /// The set of entries visible under the current mode may have changed —
    /// either the mode flipped or an entry was restamped. Consumers that
    /// cache a tier-filtered view (SearchController's index, any external
    /// tree mirror) should rebuild on this rather than on
    /// showAdvancedChanged, which fires only for a genuine mode flip.
    /// Kept distinct because showAdvancedChanged is the NOTIFY of the
    /// showAdvanced Q_PROPERTY: firing it when the property has not changed
    /// would re-evaluate every binding on it and mislead any consumer that
    /// reasonably reads it as "the user switched modes".
    void visibleSetChanged();

private:
    /// True iff `v` should be shown under the current `m_showAdvanced` mode
    /// (Always always; AdvancedOnly iff advanced; SimpleOnly iff simple).
    bool modeAllows(PageVisibility v) const;
    /// modeAllows against an EXPLICIT mode rather than the current one, so
    /// validateCounterparts can ask "would this be reachable in the mode that
    /// hides its partner" without mutating m_showAdvanced.
    static bool modeAllowsIn(PageVisibility v, bool advanced);
    /// Full visibility test for a sidebar tree accessor: the entry's own
    /// tier must match the mode AND, for a virtual node (no qmlSource), at
    /// least one descendant must itself be visible — so an emptied-out
    /// category header disappears with its children. Recursion is bounded
    /// by the tree depth; the registry graph is acyclic by construction
    /// (registerPage rejects a parentId that isn't already registered).
    /// Descends via m_childrenByParent, so the cost is the size of the
    /// subtree below @p entry rather than a full m_pages rescan per level.
    bool isEntryVisible(const Entry& entry) const;
    /// True iff @p controller is the live controller of some entry. The
    /// m_controllerSet lookup first, then a scan only on a hit, because the
    /// set can hold the address of a destroyed controller.
    bool isLiveRegisteredController(PageController* controller) const;
    /// The checks validateCounterparts makes, for one entry: warns about
    /// each fault and returns false when there is one.
    bool counterpartResolves(const Entry& e) const;

    QList<Entry> m_pages;
    /// Simple/advanced master mode. Defaults true so an app that never
    /// classifies a page sees no behavioural change.
    bool m_showAdvanced = true;
    // qsizetype matches QList::size()'s return type; avoids -Wnarrowing
    // when stricter build flags land. Settings registries are never
    // anywhere near 2^31 pages, but the narrowing is unnecessary.
    QHash<QString, qsizetype> m_indexById;
    // Child indices grouped by parentId (the empty key holds the top-level
    // entries), in sibling order: registration order, unless registerPage was
    // given a sibling position. Every tree accessor here descends through this
    // rather than rescanning m_pages, so finding a node's children costs the
    // size of that list. visibleChildPages(p) still costs the subtree below
    // p, since isEntryVisible descends a virtual child until it finds a
    // visible descendant: that is the empty-category rule, not a rescan.
    // Built in registerPage and never invalidated: the catalogue only grows,
    // and no in-place change moves an entry.
    QHash<QString, QList<qsizetype>> m_childrenByParent;
    // Controller-pointer dedup set — registerPage previously did an
    // O(N) linear scan of m_pages per registration to reject a
    // duplicate controller, which compounded to O(K²) for K page
    // registrations. The set lookup keeps registration O(1) for
    // catalogues that grow to dozens of pages.
    QSet<PageController*> m_controllerSet;
    /// How many ancestors each entry has, by its index in m_pages, so
    /// registerPage checks a new page against MaxParentChainHops from its
    /// parent's alone.
    QList<int> m_depths;
};

} // namespace PhosphorControl
