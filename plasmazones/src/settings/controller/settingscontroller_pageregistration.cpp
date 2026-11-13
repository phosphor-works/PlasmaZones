// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Page-registration methods for SettingsController:
//   * buildApplicationController() — wires the PhosphorControl
//     PageRegistry with PlasmaZones' settings pages and sidebar categories
//     (the navigable leaf pages are enumerated in validPageNames()).
//
// What's New state lives in the sibling settingscontroller_whatsnew.cpp.
//
// The static sidebar topology accessors (pageGroupChildren,
// pageOwnedConfigKeys, validPageNames) live in the sibling
// settingscontroller_pagetopology.cpp.
//
// All methods here are members of PlasmaZones::SettingsController. Same class
// as settingscontroller.cpp, separate translation unit, no API change.

#include "settingscontroller.h"

#include "core/platform/logging.h"
#include "phosphor_i18n.h"
#include "pageadapter.h"
#include "settings/services/settingsstagingdomain.h"

#include <QUrl>

namespace PlasmaZones {

void SettingsController::buildApplicationController()
{
    // No QObject parent: m_app is owned by its unique_ptr member. Where that
    // member sits relative to the page controllers, and so which of them
    // outlive it at teardown, is spelled out at its declaration in
    // settingscontroller.h.
    m_app = std::make_unique<PhosphorControl::ApplicationController>();

    const QString qmlPrefix = QStringLiteral("qrc:/qt/qml/org/plasmazones/settings/qml/");

    // Simple/advanced classification happens HERE, at registration — each
    // page's tier (and, for mode-swapped pages, its other-mode counterpart)
    // is declared inline on its registration call, making the registry the
    // single source of truth per page. Unmarked pages default to Always
    // (both modes); virtual category parents stay Always and auto-hide once
    // every descendant filters out. `AdvancedOnly` and `SimpleOnly` below
    // are the shorthands.
    using PV = PhosphorControl::PageRegistry::PageVisibility;
    constexpr PV AdvancedOnly = PV::AdvancedOnly;
    constexpr PV SimpleOnly = PV::SimpleOnly;
    // The one registration path both helpers below go through. Returns whether
    // the registry accepted @p page. ApplicationController::registerPage
    // refuses with a warning (duplicate id, unknown parent, cross-thread page)
    // and reports nothing back, so acceptance is read off the registry: it
    // holds this controller under the page's id only if the entry went in.
    // `passesContext` marks a page whose QML root takes `controller` and
    // `settingsApp` as required properties, which PageHost then sets before
    // the page's bindings run (PageRegistry::Entry::passesContext).
    const auto registerEntry = [this, &qmlPrefix](PhosphorControl::PageController* page, const QString& parentId,
                                                  const QString& title, const QString& qmlFile, const QString& icon,
                                                  bool collapsible, bool divider, PV visibility,
                                                  const QString& counterpartId, bool passesContext) {
        const QUrl source = qmlFile.isEmpty() ? QUrl() : QUrl(qmlPrefix + qmlFile);
        m_app->registerPage(page, parentId, title, source, icon, collapsible, divider, visibility, counterpartId);
        if (m_app->registry()->controller(page->id()) != page) {
            return false;
        }
        if (passesContext) {
            m_app->registry()->setPassesContext(page->id(), true);
        }
        return true;
    };
    // The helpers every registration below calls:
    //   regPage:     the page has its own PageController subclass (the
    //                m_xxxPage controllers). Only these can pass context,
    //                since the context they pass is that controller.
    //   regVirtual:  no concrete controller — either a drill-down parent /
    //                inline-collapsible category header, or a leaf whose QML
    //                page binds to a Settings property directly (no per-page
    //                controller). Uses PageAdapter as the framework-facing
    //                identity so the registry still gets a stable
    //                PageController*.
    const auto regPage = [&registerEntry](PhosphorControl::PageController* page, const QString& parentId,
                                          const QString& title, const QString& qmlFile, const QString& icon,
                                          bool collapsible = false, bool divider = false, PV visibility = PV::Always,
                                          const QString& counterpartId = QString(), bool passesContext = false) {
        registerEntry(page, parentId, title, qmlFile, icon, collapsible, divider, visibility, counterpartId,
                      passesContext);
    };
    const auto regVirtual = [&registerEntry](const QString& id, const QString& parentId, const QString& title,
                                             const QString& qmlFile, const QString& icon, bool collapsible = false,
                                             bool divider = false, PV visibility = PV::Always,
                                             const QString& counterpartId = QString()) {
        // Parentless until accepted: registerPage adopts a parentless page into
        // m_app on success, and a refused adapter is freed here instead of
        // lingering as an m_app child with no registry entry.
        auto adapter = std::make_unique<PageAdapter>(id);
        if (registerEntry(adapter.get(), parentId, title, qmlFile, icon, collapsible, divider, visibility,
                          counterpartId, /*passesContext=*/false)) {
            static_cast<void>(adapter.release()); // owned by m_app now
        }
    };

    // Top-level entries. The rail reads top → bottom as three blocks split by
    // two dividers: (1) top/global — Overview (status dashboard), Profiles
    // (which frames everything below it) + General (global settings);
    // (2) per-feature configuration — Virtual Screens, Placement, Appearance
    // (which parents Decorations, Animations and Overlays), Rules;
    // (3) tools & meta — Editor, About.
    // `divider` flags mark only those two seams (after General, after Rules)
    // plus one inside the feature block (after Placement, to set the
    // placement categories apart from Appearance/Rules).

    // ── Block 1: top / global ──
    regVirtual(QStringLiteral("overview"), QString(), PhosphorI18n::tr("Overview", "@title settings sidebar page"),
               QStringLiteral("pages/screens/MonitorStatePage.qml"), QStringLiteral("monitor"));
    // Profiles sits in the top/global block, directly under the switcher in the
    // sidebar header: the active profile frames every page below it, so it
    // belongs here rather than among the tools. regPage trackDomain()s the
    // controller so its staged active-profile pointer joins the Save/Discard
    // transaction; the applied config rides the Settings staging path.
    regPage(m_profilesPage.get(), QString(), PhosphorI18n::tr("Profiles", "@title settings sidebar page"),
            QStringLiteral("pages/profiles/ProfilesPage.qml"), QStringLiteral("bookmarks"), /*collapsible=*/false,
            /*divider=*/false, AdvancedOnly, /*counterpartId=*/QString(), /*passesContext=*/true);
    // General leads near the top (mirrors the Animations section leading with
    // its own "General" child). Divider after it closes the top/global block.
    regPage(m_generalPage, QString(), PhosphorI18n::tr("General", "@title settings sidebar page"),
            QStringLiteral("GeneralPage.qml"), QStringLiteral("configure"), /*collapsible=*/false, /*divider=*/true,
            PV::Always, /*counterpartId=*/QString(), /*passesContext=*/true);

    // ── Block 2: per-feature configuration ──
    // Virtual Screens is a top-level leaf. It used to sit under a "Display"
    // category, but once the layout library moved into the placement modes
    // that category held this single child, and a one-child dropdown is pure
    // friction.
    regVirtual(QStringLiteral("virtualscreens"), QString(),
               PhosphorI18n::tr("Virtual Screens", "@title settings sidebar page"),
               QStringLiteral("pages/screens/VirtualScreensPage.qml"), QStringLiteral("virtual-desktops"),
               /*collapsible=*/false,
               /*divider=*/false, AdvancedOnly);
    // Placement groups the three placement modes (Snapping / Tiling / Scrolling) as an
    // inline-collapsible category. Divider after it (i.e.
    // above Appearance) sets the placement categories apart from the
    // Appearance / Rules pages that follow.
    regVirtual(QStringLiteral("placement"), QString(), PhosphorI18n::tr("Placement", "@title settings sidebar page"),
               QString(), QStringLiteral("preferences-system-windows"), /*collapsible=*/true, /*divider=*/true);
    // Appearance groups the visual surfaces — the Decoration tree (which owns
    // the window border / title-bar / gap page), the Animations tree and the
    // Overlays tree — as an inline-collapsible category (matching Placement).
    // No QML of its own; redirects to its first leaf.
    regVirtual(QStringLiteral("appearance"), QString(), PhosphorI18n::tr("Appearance", "@title settings sidebar page"),
               QString(), QStringLiteral("preferences-desktop-theme"), /*collapsible=*/true);
    // Decoration — a no-QML drill-down parent UNDER Appearance, alongside
    // Animations and Overlays (all visual-surface pages live in that group).
    // Registered BEFORE animations so the flat simple rail (registration order) reads
    // Appearance, then Animations.
    // PER-SURFACE scope: per-surface CHAINS of decoration shader packs,
    // resolved through a DecorationProfileTree (walk-up inheritance). Each
    // surface family (window / osd / popup / shell) is its own root card, whose toggle
    // engages or clears that family's override; children diverge from it through
    // the walk-up. The nav handle ("decorations") redirects to its General page
    // (the config-backed window-appearance page, registered below with the
    // decoration children). The DecorationPageController is wired in as a
    // headless staging domain below; it has no per-page staged state —
    // dirty tracking rides the global decorationProfileTreeChanged NOTIFY loop.
    regVirtual(QStringLiteral("decorations"), QStringLiteral("appearance"),
               PhosphorI18n::tr("Decorations", "@title settings sidebar page"), QString(),
               QStringLiteral("preferences-desktop-theme"));
    // Headless staging domain — trackDomain() connects dirtyChanged + appends
    // to m_domains so applyAllAsync walks it, exactly as registerPage would,
    // but without claiming a sidebar/registry id of its own.
    m_app->registerDomain(m_decorationPage);
    // "animations" is a no-QML drill-down parent under Appearance — register it as
    // a virtual navigation node (like placement / snapping / tiling),
    // NOT as m_animationsPage's own id. The AnimationsPageController is the
    // staging controller for animation edits; it carries the distinct id
    // "animations-staging" and is wired into the framework's dirty/apply
    // machinery as a headless domain below. Splitting the two means the nav
    // handle ("animations", resolved to its first visible leaf) and the
    // staging controller are independently addressable by QML / D-Bus.
    regVirtual(QStringLiteral("animations"), QStringLiteral("appearance"),
               PhosphorI18n::tr("Animations", "@title settings sidebar page"), QString(),
               QStringLiteral("media-playback-start"));
    // Headless staging domain, wired the same way as m_decorationPage above.
    m_app->registerDomain(m_animationsPage);
    // Rules is a top-level leaf (its old "Rules" parent retired after
    // the v4 fold left a single rule surface). Divider after it closes the
    // feature block and opens the tools-and-meta block below.
    regPage(m_rulesPage, QString(), PhosphorI18n::tr("Rules", "@title settings sidebar page"),
            QStringLiteral("pages/rules/RulesPage.qml"), QStringLiteral("view-list-details"), /*collapsible=*/false,
            /*divider=*/true);

    // ── Block 3: tools & meta ──
    // Editor holds layout-editor PREFERENCES (shortcuts, grid snapping,
    // fill-on-drop, background opacity), not the editor itself — authoring custom zone layouts
    // is a power activity, so the whole page is advanced.
    regPage(m_editorPage, QString(), PhosphorI18n::tr("Editor", "@title settings sidebar page"),
            QStringLiteral("EditorPage.qml"), QStringLiteral("document-edit"), /*collapsible=*/false, /*divider=*/false,
            AdvancedOnly, /*counterpartId=*/QString(), /*passesContext=*/true);
    regVirtual(QStringLiteral("about"), QString(), PhosphorI18n::tr("About", "@title settings sidebar page"),
               QStringLiteral("AboutPage.qml"), QStringLiteral("help-about"));

    // Placement children — the three placement modes. They keep their own
    // drill-down behaviour (collapsible=false) and their inline enable
    // toggles. In simple mode the flat rail keeps each mode as an expandable
    // header row of its own (a multi-leaf drill parent no longer dissolves —
    // see SidebarRows' flat walk), so Main.qml's trailing delegate keys the
    // toggles off exactly these three ids in both modes.
    regVirtual(QStringLiteral("snapping"), QStringLiteral("placement"),
               PhosphorI18n::tr("Snapping", "@title settings sidebar page"), QString(),
               QStringLiteral("view-split-left-right"));
    regVirtual(QStringLiteral("tiling"), QStringLiteral("placement"),
               PhosphorI18n::tr("Tiling", "@title settings sidebar page"), QString(),
               QStringLiteral("window-duplicate"));
    // Not view-list-details, which is the Rules page's icon.
    regVirtual(QStringLiteral("scrolling"), QStringLiteral("placement"),
               PhosphorI18n::tr("Scrolling", "@title settings sidebar page"), QString(),
               QStringLiteral("distribute-horizontal-equal"));

    // Snapping children — organised by SUBJECT, one leaf each for the drag
    // Overlay's behaviour, the edge Zone Selector popup and the snapped Window,
    // plus a shared Configuration category. Every leaf is QML-only
    // (regVirtual). The subject leaves bind an existing per-feature
    // controller: the simple page, Overlay and Window bind
    // snappingBehaviorPage, and Zone Selector binds snappingZoneSelectorPage.
    // Layouts, Priority and Quick Shortcuts bind none. The overlay's look lives under
    // Appearance → Overlays (its Appearance page binds snappingZonesPage) and
    // the window border / title bar under Appearance → Decorations → General.
    // Those controllers stay exposed as Q_PROPERTY bridges on
    // SettingsController but are not registered as pages of their own.
    // Per-page dirtiness keys off the ACTIVE page at edit time (see
    // setNeedsSave), so sharing a controller across pages is fine.
    // The simple-mode snapping surface leads: a SimpleOnly leaf condensing
    // the everyday decisions (drag triggers + Snap Assist). In advanced mode
    // it is filtered out and the full per-subject tree shows instead; it and
    // Overlay → Behavior are counterparts so mode flips and deep links land
    // sensibly. Titled "General": the flat rail nests it under the mode's
    // own expandable header row (which carries the mode name and the enable
    // toggle), the same lead-page shape as animations-general.
    regVirtual(QStringLiteral("snapping-simple"), QStringLiteral("snapping"),
               PhosphorI18n::tr("General", "@title settings sidebar page"),
               QStringLiteral("pages/snapping/SnappingSimplePage.qml"), QStringLiteral("view-split-left-right"),
               /*collapsible=*/false, /*divider=*/true, SimpleOnly, QStringLiteral("snapping-overlay-behavior"));
    // The snapping layout library — the browser formerly tabbed into
    // Display → Layouts, now this mode's own leaf. Leads the advanced section
    // (it is the mode's primary artifact; the config tree follows) and in
    // simple mode follows the condensed General page. It stays visible in
    // BOTH modes: it is the only surface where layouts are created, imported
    // and deleted, and that is everyday activity, not power depth. Divider
    // closes the content block before the Overlay leaf.
    regVirtual(QStringLiteral("snapping-layouts"), QStringLiteral("snapping"),
               PhosphorI18n::tr("Layouts", "@title settings sidebar page"),
               QStringLiteral("pages/snapping/SnappingLayoutsPage.qml"), QStringLiteral("view-grid"),
               /*collapsible=*/false, /*divider=*/true);
    // The overlay's BEHAVIOR — when it appears while you drag. A standalone top
    // leaf rather than a category: its Appearance sibling became the Overlays
    // entry under Appearance, so a category here would have one child.
    // Advanced-only, its simple face being the condensed SnappingSimplePage
    // (its declared counterpart).
    regVirtual(QStringLiteral("snapping-overlay-behavior"), QStringLiteral("snapping"),
               PhosphorI18n::tr("Overlay", "@title settings sidebar page"),
               QStringLiteral("pages/snapping/SnappingOverlayBehaviorPage.qml"),
               QStringLiteral("preferences-desktop-color"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly, QStringLiteral("snapping-simple"));

    // Zone Selector is a single top leaf under Snapping (not split into
    // Behavior/Appearance): its behaviour is just the enable toggle + trigger
    // distance, too small to warrant its own page. Structurally parallel to
    // Tiling's Algorithm leaf.
    regVirtual(QStringLiteral("snapping-zoneselector"), QStringLiteral("snapping"),
               PhosphorI18n::tr("Zone Selector", "@title settings sidebar page"),
               QStringLiteral("pages/snapping/SnappingZoneSelectorPage.qml"), QStringLiteral("view-choose"),
               /*collapsible=*/false,
               /*divider=*/true, AdvancedOnly);

    // Snapping → Window holds just the per-mode Behavior page now. The window
    // border / title-bar appearance moved to the shared Appearance →
    // Decorations → General page (config-backed, mode-neutral).
    regVirtual(QStringLiteral("snapping-window-behavior"), QStringLiteral("snapping"),
               PhosphorI18n::tr("Window", "@title settings sidebar page"),
               QStringLiteral("pages/snapping/SnappingWindowBehaviorPage.qml"),
               QStringLiteral("preferences-system-windows"),
               /*collapsible=*/false, /*divider=*/true, AdvancedOnly);

    regVirtual(QStringLiteral("snapping-config-cat"), QStringLiteral("snapping"),
               PhosphorI18n::tr("Configuration", "@title settings sidebar page"), QString(),
               QStringLiteral("configure"), /*collapsible=*/true);
    regVirtual(QStringLiteral("snapping-ordering"), QStringLiteral("snapping-config-cat"),
               PhosphorI18n::tr("Priority", "@title settings sidebar page"),
               QStringLiteral("pages/snapping/SnappingOrderingPage.qml"), QStringLiteral("view-sort"),
               /*collapsible=*/false,
               /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("snapping-shortcuts"), QStringLiteral("snapping-config-cat"),
               PhosphorI18n::tr("Quick Shortcuts", "@title settings sidebar page"),
               QStringLiteral("pages/snapping/SnappingQuickShortcutsPage.qml"), QStringLiteral("bookmark"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    // (The overlay shader pages are registered under Appearance → Overlays.)
    // Tiling children — organised by subject (Window / Algorithm / Configuration)
    // to match the snapping reorg. Tiling has no drag-overlay or selector popup,
    // so its only interaction surface (the drag-insert indicator) folds into
    // the Window leaf; the layout algorithm is its own top leaf, and gaps live
    // on the shared Appearance → Decorations → General page with the rest of
    // the unified gap model. The Window leaf keeps its historical
    // "tiling-behavior" id (the controller's), so dirty tracking, the per-page
    // reset manifest and deep links stay stable.
    // The simple-mode tiling surface leads: a SimpleOnly leaf condensing the
    // everyday decisions (algorithm picker + preview, master ratio, max
    // windows). In advanced mode it is filtered out and the full per-page tree
    // shows instead; it and the Algorithm page are counterparts so mode
    // flips and deep links land sensibly.
    regVirtual(QStringLiteral("tiling-simple"), QStringLiteral("tiling"),
               PhosphorI18n::tr("General", "@title settings sidebar page"),
               QStringLiteral("pages/tiling/TilingSimplePage.qml"), QStringLiteral("window-duplicate"),
               /*collapsible=*/false,
               /*divider=*/true, SimpleOnly, QStringLiteral("tiling-algorithm"));
    // The tiling-algorithm library — the browser formerly tabbed into
    // Display → Layouts, now this mode's own leaf. Manages the INSTALLED
    // algorithms (import/create/delete/hide); the Algorithm page below
    // configures the ACTIVE one. "Library" rather than "Algorithms" so the
    // two rows don't read as near-duplicates, matching the
    // Animations/Decorations Library precedent — and folder-open rather than
    // view-grid, which the Algorithm page below carries. folder-open is what
    // the Animations/Decorations Library rows use. Leads the advanced section
    // (in simple mode it follows the condensed General page) and stays
    // visible in both modes, mirroring Snapping → Layouts.
    regVirtual(QStringLiteral("tiling-library"), QStringLiteral("tiling"),
               PhosphorI18n::tr("Library", "@title settings sidebar page"),
               QStringLiteral("pages/tiling/TilingLibraryPage.qml"), QStringLiteral("folder-open"),
               /*collapsible=*/false, /*divider=*/true);
    // Tiling → Window holds just the per-mode Behavior page now. The window
    // border / title-bar appearance moved to the shared Appearance →
    // Decorations → General page (config-backed, mode-neutral). Advanced-only:
    // its simple face used to be the Triggers card, but drag-reinsert
    // vocabulary is itself power depth; simple mode gets TilingSimplePage.
    regPage(m_tilingBehaviorPage, QStringLiteral("tiling"), PhosphorI18n::tr("Window", "@title settings sidebar page"),
            QStringLiteral("pages/tiling/TilingBehaviorPage.qml"), QStringLiteral("preferences-system-windows"),
            /*collapsible=*/false, /*divider=*/true, AdvancedOnly, /*counterpartId=*/QString(),
            /*passesContext=*/true);

    // Algorithm is a top-level leaf under Tiling (no snapping peer — snapping
    // has no active-algorithm concept, only the layout library above). Divider after it sets the
    // mode-specific algorithm apart from the shared Configuration block below.
    regPage(m_tilingAlgorithmPage.get(), QStringLiteral("tiling"),
            PhosphorI18n::tr("Algorithm", "@title settings sidebar page"),
            QStringLiteral("pages/tiling/TilingAlgorithmPage.qml"), QStringLiteral("view-grid"), /*collapsible=*/false,
            /*divider=*/true, AdvancedOnly, QStringLiteral("tiling-simple"), /*passesContext=*/true);

    regVirtual(QStringLiteral("tiling-config-cat"), QStringLiteral("tiling"),
               PhosphorI18n::tr("Configuration", "@title settings sidebar page"), QString(),
               QStringLiteral("configure"), /*collapsible=*/true);
    regVirtual(QStringLiteral("tiling-ordering"), QStringLiteral("tiling-config-cat"),
               PhosphorI18n::tr("Priority", "@title settings sidebar page"),
               QStringLiteral("pages/tiling/TilingOrderingPage.qml"), QStringLiteral("view-sort"),
               /*collapsible=*/false,
               /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("tiling-shortcuts"), QStringLiteral("tiling-config-cat"),
               PhosphorI18n::tr("Quick Shortcuts", "@title settings sidebar page"),
               QStringLiteral("pages/tiling/TilingQuickShortcutsPage.qml"), QStringLiteral("bookmark"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);

    // Scrolling children — the third placement mode's own section, the peer
    // of Snapping and Tiling above (its sidebar row carries the same inline
    // enable toggle). The SimpleOnly condensed page leads, mirroring its
    // siblings; its advanced counterpart is the Columns page, the first of the
    // advanced leaves described below.
    regVirtual(QStringLiteral("scrolling-simple"), QStringLiteral("scrolling"),
               PhosphorI18n::tr("General", "@title settings sidebar page"),
               QStringLiteral("pages/scrolling/ScrollingSimplePage.qml"), QStringLiteral("distribute-horizontal-equal"),
               /*collapsible=*/false,
               /*divider=*/true, SimpleOnly, QStringLiteral("scrolling-columns"));
    // The scrolling-template library — the browser formerly tabbed into
    // Display → Layouts, now this mode's own leaf. Leads the advanced section
    // (in simple mode it follows the condensed General page) and stays
    // visible in both modes, mirroring Snapping → Layouts and Tiling → Library.
    regVirtual(QStringLiteral("scrolling-templates"), QStringLiteral("scrolling"),
               PhosphorI18n::tr("Templates", "@title settings sidebar page"),
               QStringLiteral("pages/scrolling/ScrollingTemplatesPage.qml"), QStringLiteral("view-grid"),
               /*collapsible=*/false, /*divider=*/true);
    // The advanced tree mirrors Tiling's per-concern split: Columns
    // (fresh-column/tile defaults + presets), Tabs (the tab indicator's own
    // settings family), Window (window handling plus the Focus and view
    // card, which carries viewport centering and the scroll-key trigger
    // rows — those follow focus, so they sit with it rather than on a View
    // leaf of their own), Strip Selector (the drag popup), then the shared
    // Configuration category (Priority + Quick Shortcuts) registered further
    // down. Columns is the simple page's counterpart, like tiling-simple ↔
    // tiling-algorithm.
    regVirtual(QStringLiteral("scrolling-columns"), QStringLiteral("scrolling"),
               PhosphorI18n::tr("Columns", "@title settings sidebar page"),
               QStringLiteral("pages/scrolling/ScrollingColumnsPage.qml"), QStringLiteral("view-file-columns"),
               /*collapsible=*/false,
               /*divider=*/false, AdvancedOnly, QStringLiteral("scrolling-simple"));
    // Ordered between Columns and Window because that is the order they
    // read in: which columns are tabbed (Columns), how a tabbed one is
    // marked (Tabs), how the strip treats windows generally (Window) — with
    // Strip Selector trailing as the interaction leaf, and the Configuration
    // category (Priority + Quick Shortcuts) after it.
    regVirtual(QStringLiteral("scrolling-tabs"), QStringLiteral("scrolling"),
               PhosphorI18n::tr("Tabs", "@title settings sidebar page"),
               QStringLiteral("pages/scrolling/ScrollingTabsPage.qml"), QStringLiteral("tab-detach"),
               /*collapsible=*/false,
               /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("scrolling-window"), QStringLiteral("scrolling"),
               PhosphorI18n::tr("Window", "@title settings sidebar page"),
               QStringLiteral("pages/scrolling/ScrollingWindowPage.qml"), QStringLiteral("preferences-system-windows"),
               /*collapsible=*/false,
               /*divider=*/false, AdvancedOnly);
    // Strip Selector — the drag popup's scrolling twin, a single top leaf
    // like Snapping's Zone Selector (enable + trigger + position + preview
    // size; no arrangement page, the strip popup is one row along the strip).
    // Divider after it closes the mode-specific block before the shared
    // Configuration category, the seam Snapping's Window leaf carries there
    // (Snapping's Zone Selector is followed by that Window leaf instead).
    regVirtual(QStringLiteral("scrolling-zoneselector"), QStringLiteral("scrolling"),
               PhosphorI18n::tr("Strip Selector", "@title settings sidebar page"),
               QStringLiteral("pages/scrolling/ScrollingZoneSelectorPage.qml"), QStringLiteral("view-choose"),
               /*collapsible=*/false,
               /*divider=*/true, AdvancedOnly);

    // Configuration — the shared trailing category, mirroring the snapping and
    // tiling sections: Priority (the template order used by cycling and the
    // pickers) and Quick Shortcuts.
    regVirtual(QStringLiteral("scrolling-config-cat"), QStringLiteral("scrolling"),
               PhosphorI18n::tr("Configuration", "@title settings sidebar page"), QString(),
               QStringLiteral("configure"), /*collapsible=*/true);
    regVirtual(QStringLiteral("scrolling-ordering"), QStringLiteral("scrolling-config-cat"),
               PhosphorI18n::tr("Priority", "@title settings sidebar page"),
               QStringLiteral("pages/scrolling/ScrollingOrderingPage.qml"), QStringLiteral("view-sort"),
               /*collapsible=*/false,
               /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("scrolling-shortcuts"), QStringLiteral("scrolling-config-cat"),
               PhosphorI18n::tr("Quick Shortcuts", "@title settings sidebar page"),
               QStringLiteral("pages/scrolling/ScrollingQuickShortcutsPage.qml"), QStringLiteral("bookmark"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);

    // Animations children — Transitions / Motion / Library categories expand
    // inline (collapsible).
    // The simple-mode surface leads: a SimpleOnly leaf that replaces the whole
    // per-event tree when the user is in simple mode. In advanced mode it is
    // filtered out and the General page leads instead. The two are declared as
    // each other's counterparts so a mode flip (or a deep link landing on the
    // hidden one) redirects between them instead of falling back to Overview.
    regVirtual(QStringLiteral("animations-simple"), QStringLiteral("animations"),
               PhosphorI18n::tr("Animations", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsSimplePage.qml"), QStringLiteral("media-playback-start"),
               /*collapsible=*/false, /*divider=*/true, SimpleOnly, QStringLiteral("animations-general"));
    regVirtual(QStringLiteral("animations-general"), QStringLiteral("animations"),
               PhosphorI18n::tr("General", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsGeneralPage.qml"), QStringLiteral("configure"),
               /*collapsible=*/false,
               /*divider=*/true, AdvancedOnly, QStringLiteral("animations-simple"));
    // Transitions — shader-driven appearance and reveal events (window
    // open/close/minimize, OSDs, overlays, desktop switch, shell surfaces),
    // grouped so each child page carries ONE shader contract. This keeps the
    // per-row shader picker coherent: every effect it offers actually applies.
    regVirtual(QStringLiteral("animations-transitions"), QStringLiteral("animations"),
               PhosphorI18n::tr("Transitions", "@title settings sidebar page"), QString(),
               QStringLiteral("preferences-desktop-multimedia"),
               /*collapsible=*/true);
    // Motion — movement and geometry events. Window motion (maximize / snap /
    // layout switch) carries the geometry shader contract, so its page still
    // shows a shader picker. Some children carry an opt-in class of their own:
    // the held drag is the `move` class on the Window Dragging page, and the
    // strip's view spring is the `strip` class on the Scrolling page. Side
    // panels, widgets and the editor are timing/curve only.
    regVirtual(QStringLiteral("animations-motion"), QStringLiteral("animations"),
               PhosphorI18n::tr("Motion", "@title settings sidebar page"), QString(), QStringLiteral("chronometer"),
               /*collapsible=*/true);
    regVirtual(QStringLiteral("animations-library"), QStringLiteral("animations"),
               PhosphorI18n::tr("Library", "@title settings sidebar page"), QString(), QStringLiteral("folder-open"),
               /*collapsible=*/true);

    regVirtual(QStringLiteral("animations-windows"), QStringLiteral("animations-transitions"),
               PhosphorI18n::tr("Windows", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsWindowsPage.qml"), QStringLiteral("window-new"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("animations-osds"), QStringLiteral("animations-transitions"),
               PhosphorI18n::tr("OSDs", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsOsdsPage.qml"), QStringLiteral("dialog-information"),
               /*collapsible=*/false,
               /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("animations-overlays"), QStringLiteral("animations-transitions"),
               PhosphorI18n::tr("Overlays", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsOverlaysPage.qml"), QStringLiteral("view-presentation"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("animations-desktops"), QStringLiteral("animations-transitions"),
               PhosphorI18n::tr("Desktop", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsDesktopsPage.qml"), QStringLiteral("virtual-desktops"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    // Last among Transitions' children, and the icon matches Decoration →
    // Shell: the two pages carry the same family of foreign surfaces, one for
    // what they wear and one for how they arrive.
    regVirtual(QStringLiteral("animations-shell"), QStringLiteral("animations-transitions"),
               PhosphorI18n::tr("Shell", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsShellPage.qml"), QStringLiteral("computer"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);

    // First among Motion's children because the strip's view is the coarsest
    // subject here — one leg moves every window on the screen — and the rows
    // below it narrow from there to a single window's motion.
    //
    // Icon matches the Placement → Scrolling row, because this page configures
    // that section's motion.
    regVirtual(QStringLiteral("animations-scrolling"), QStringLiteral("animations-motion"),
               PhosphorI18n::tr("Scrolling", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsScrollingPage.qml"),
               QStringLiteral("distribute-horizontal-equal"), /*collapsible=*/false, /*divider=*/false, AdvancedOnly);

    regVirtual(QStringLiteral("animations-window-motion"), QStringLiteral("animations-motion"),
               PhosphorI18n::tr("Window Motion", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsWindowMotionPage.qml"), QStringLiteral("window-new"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    // Window Dragging is deliberately its own page, not a row under Window
    // Motion: the drag event is its own opt-in shader class (`move`) that
    // takes no inherited shader, so parking it under the "All Windows"
    // cascade parent would misrepresent the inheritance.
    regVirtual(QStringLiteral("animations-window-dragging"), QStringLiteral("animations-motion"),
               PhosphorI18n::tr("Window Dragging", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsWindowDraggingPage.qml"), QStringLiteral("transform-move"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("animations-side-panels"), QStringLiteral("animations-motion"),
               PhosphorI18n::tr("Side Panels", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsSidePanelsPage.qml"),
               QStringLiteral("sidebar-collapse-symbolic"), /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("animations-widgets"), QStringLiteral("animations-motion"),
               PhosphorI18n::tr("Widgets", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsWidgetsPage.qml"),
               QStringLiteral("preferences-desktop-theme"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("animations-editor"), QStringLiteral("animations-motion"),
               PhosphorI18n::tr("Layout Editor", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsEditorPage.qml"), QStringLiteral("document-edit"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);

    regVirtual(QStringLiteral("animations-presets"), QStringLiteral("animations-library"),
               PhosphorI18n::tr("Presets", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsPresetsPage.qml"), QStringLiteral("bookmarks"),
               /*collapsible=*/false,
               /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("animations-motionsets"), QStringLiteral("animations-library"),
               PhosphorI18n::tr("Sets", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsMotionSetsPage.qml"), QStringLiteral("color-palette"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("animations-shaders"), QStringLiteral("animations-library"),
               PhosphorI18n::tr("Shaders", "@title settings sidebar page"),
               QStringLiteral("pages/animations/AnimationsShadersPage.qml"),
               QStringLiteral("preferences-desktop-display"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);

    // Decoration children — Surfaces / Library categories expand inline
    // (collapsible), the bucket shape animations uses. Decoration has no
    // meaningful global PACK default (borders and title bars are window-only; daemon surfaces
    // default to no decoration), so Surfaces fans straight out to the
    // per-surface override pages and Library holds the installed-pack browser.
    // Decoration → General: the shared, mode-neutral window-appearance page
    // (config-backed window border / title bar Windows.* + the unified gap
    // model Gaps.*). Lives under Decoration as its General page — the same
    // slot animations-general occupies — keeping its historical
    // "window-appearance" id so dirty tracking, the per-page reset manifest,
    // and deep links stay stable.
    regPage(m_windowAppearancePage, QStringLiteral("decorations"),
            PhosphorI18n::tr("General", "@title settings sidebar page"),
            QStringLiteral("pages/decoration/WindowAppearancePage.qml"), QStringLiteral("configure"),
            /*collapsible=*/false, /*divider=*/true, PV::Always, /*counterpartId=*/QString(),
            /*passesContext=*/true);

    regVirtual(QStringLiteral("decorations-surfaces"), QStringLiteral("decorations"),
               PhosphorI18n::tr("Surfaces", "@title settings sidebar page"), QString(),
               QStringLiteral("preferences-desktop-multimedia"), /*collapsible=*/true);
    regVirtual(QStringLiteral("decorations-library"), QStringLiteral("decorations"),
               PhosphorI18n::tr("Library", "@title settings sidebar page"), QString(), QStringLiteral("folder-open"),
               /*collapsible=*/true);

    regVirtual(QStringLiteral("decorations-windows"), QStringLiteral("decorations-surfaces"),
               PhosphorI18n::tr("Windows", "@title settings sidebar page"),
               QStringLiteral("pages/decoration/DecorationWindowsPage.qml"), QStringLiteral("window-new"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("decorations-osds"), QStringLiteral("decorations-surfaces"),
               PhosphorI18n::tr("OSDs", "@title settings sidebar page"),
               QStringLiteral("pages/decoration/DecorationOsdsPage.qml"), QStringLiteral("dialog-information"),
               /*collapsible=*/false,
               /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("decorations-popups"), QStringLiteral("decorations-surfaces"),
               PhosphorI18n::tr("Popups", "@title settings sidebar page"),
               QStringLiteral("pages/decoration/DecorationPopupsPage.qml"), QStringLiteral("view-presentation"),
               /*collapsible=*/false,
               /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("decorations-shell"), QStringLiteral("decorations-surfaces"),
               PhosphorI18n::tr("Shell", "@title settings sidebar page"),
               QStringLiteral("pages/decoration/DecorationShellPage.qml"), QStringLiteral("computer"),
               /*collapsible=*/false,
               /*divider=*/false, AdvancedOnly);
    // The mouse pointer is a decoration surface like any other: its chain
    // lives at the `pointer` path in the same DecorationProfileTree, so it
    // rides the same card, the same sets and the same per-page reset.
    regVirtual(QStringLiteral("decorations-pointer"), QStringLiteral("decorations-surfaces"),
               PhosphorI18n::tr("Pointer", "@title settings sidebar page"),
               QStringLiteral("pages/decoration/DecorationPointerPage.qml"), QStringLiteral("input-mouse"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);

    regVirtual(QStringLiteral("decorations-sets"), QStringLiteral("decorations-library"),
               PhosphorI18n::tr("Sets", "@title settings sidebar page"),
               QStringLiteral("pages/decoration/DecorationSetsPage.qml"), QStringLiteral("color-palette"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    regVirtual(QStringLiteral("decorations-shaders"), QStringLiteral("decorations-library"),
               PhosphorI18n::tr("Shaders", "@title settings sidebar page"),
               QStringLiteral("pages/decoration/DecorationShadersPage.qml"),
               QStringLiteral("preferences-desktop-display"), /*collapsible=*/false, /*divider=*/false, AdvancedOnly);

    // Overlays — an Appearance drill-down beside Animations and Decorations,
    // and shaped like Decorations: the config leaves first, then a
    // collapsible Library bucket holding the saved sets and the pack browser.
    // Everything about the zone overlay's LOOK lives here, including which
    // shader each layout draws. Only when the overlay appears while you drag
    // stayed under Snapping, because that is an activation decision.
    // Below the tree parent, icons follow the sibling trees rather than being
    // picked per page: the lead config page is `configure`, the Library bucket
    // is `folder-open`, its sets page is `color-palette`, and its pack browser
    // is `preferences-desktop-display`. A reader who has learnt one of these
    // trees can then read the others. The tree parents carry their own
    // domain's icon: Decorations preferences-desktop-theme (the same glyph as
    // the Appearance category above it), Animations media-playback-start, and
    // Overlays preferences-desktop-effects. Overlays does not take
    // preferences-desktop-color, which is Snapping → Overlay's icon.
    regVirtual(QStringLiteral("overlays"), QStringLiteral("appearance"),
               PhosphorI18n::tr("Overlays", "@title settings sidebar page"), QString(),
               QStringLiteral("preferences-desktop-effects"));
    // Appearance leads the config leaves and Layouts follows it, carrying the
    // divider that closes the config block before the Library bucket. Unlike
    // Decorations → General, the lead page here is titled for its subject and
    // is advanced-only.
    regVirtual(QStringLiteral("overlays-appearance"), QStringLiteral("overlays"),
               PhosphorI18n::tr("Appearance", "@title settings sidebar page"),
               QStringLiteral("pages/overlays/OverlaysAppearancePage.qml"), QStringLiteral("configure"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    // "Layouts", not "Shaders": Library → Shaders below is the pack browser,
    // named the way the decoration tree names its own, and two entries reading
    // "Shaders" in one drill-down would be a coin flip. The name says what the
    // page is keyed on, and the layouts icon follows from it —
    // `preferences-desktop-display` belongs to the pack browser in every
    // Appearance tree, so it stays off this entry.
    //
    // The page id stays overlays-assignments: it is what the page DOES, the
    // label is where it does it, and the id is not user-visible.
    regVirtual(QStringLiteral("overlays-assignments"), QStringLiteral("overlays"),
               PhosphorI18n::tr("Layouts", "@title settings sidebar page"),
               QStringLiteral("pages/overlays/OverlaysAssignmentsPage.qml"), QStringLiteral("view-grid"),
               /*collapsible=*/false, /*divider=*/true, AdvancedOnly);

    regVirtual(QStringLiteral("overlays-library"), QStringLiteral("overlays"),
               PhosphorI18n::tr("Library", "@title settings sidebar page"), QString(), QStringLiteral("folder-open"),
               /*collapsible=*/true);
    regVirtual(QStringLiteral("overlays-sets"), QStringLiteral("overlays-library"),
               PhosphorI18n::tr("Sets", "@title settings sidebar page"),
               QStringLiteral("pages/overlays/OverlaySetsPage.qml"), QStringLiteral("color-palette"),
               /*collapsible=*/false, /*divider=*/false, AdvancedOnly);
    regPage(m_overlaysPage.get(), QStringLiteral("overlays-library"),
            PhosphorI18n::tr("Shaders", "@title settings sidebar page"),
            QStringLiteral("pages/overlays/OverlaysLibraryPage.qml"), QStringLiteral("preferences-desktop-display"),
            /*collapsible=*/false, /*divider=*/false, AdvancedOnly, /*counterpartId=*/QString(),
            /*passesContext=*/true);

    // Every page declared its simple/advanced tier at registration above.
    // Seed the registry's mode from m_advancedMode (default simple) so the
    // very first sidebar build is already filtered — the registry's own
    // default is show-everything. The QML QtCore.Settings block in Main.qml
    // pushes the user's remembered choice in over this at startup.
    m_app->registry()->setShowAdvanced(m_advancedMode);

    // Counterparts are stored unvalidated at registration (the target may be
    // declared later), and nothing downstream checks them: a typo silently
    // sends every affected mode flip and deep link to the fallback page and
    // looks exactly like correct operation. Check once now that the whole
    // catalogue is in, so the mistake surfaces as a startup warning.
    if (!m_app->registry()->validateCounterparts()) {
        qCWarning(lcCore) << "Page counterpart declarations are inconsistent — mode flips and deep links from the "
                             "affected pages will fall back instead of redirecting. See the warnings above.";
    }

    // Bridge SettingsController.save/load to the framework's Apply/Cancel
    // (and to the global dirty flag QML chrome binds to).
    m_app->registerDomain(new SettingsStagingDomain(this, m_app.get()));

    // Sync activePage ↔ ApplicationController.currentPageId. Both directions
    // are guarded against re-entrancy by comparing the incoming value to the
    // already-stored one before propagating.
    m_app->setCurrentPageId(m_activePage);
    // `m_app.get()` is the first connection's context object and the second
    // connection's sender, so Qt drops both when `m_app` is destroyed — no
    // manual null-guard on `m_app` inside the lambdas is needed.
    connect(this, &SettingsController::activePageChanged, m_app.get(), [this]() {
        if (m_app->currentPageId() != m_activePage) {
            m_app->setCurrentPageId(m_activePage);
        }
    });
    connect(m_app.get(), &PhosphorControl::ApplicationController::currentPageIdChanged, this, [this]() {
        const QString id = m_app->currentPageId();
        if (id.isEmpty() || id == m_activePage) {
            return;
        }
        const QString previousActive = m_activePage;
        setActivePage(id);
        // m_app only ever holds a REGISTERED id (its setter refuses unknown
        // ones), but setActivePage can still leave m_activePage at
        // previousActive for one it has already accepted:
        //   * a registered id missing from validPageNames() is rejected with
        //     a DEBUG-level trace (the reject site explains why not a
        //     warning). Its only symptom is the sidebar row doing nothing;
        //     the registration/validPageNames parity test guards it;
        //   * the mode gate redirects a page the current mode hides (reached
        //     by a back/forward history step, for one) onto its counterpart or
        //     nearest visible ancestor leaf, and that can be the page already
        //     shown;
        //   * a re-entrant call is refused outright.
        // In each case m_activePage does not change, activePageChanged never
        // fires, and the first connection above never pulls m_app back, so
        // without this snap-back the two would diverge silently. Restore the
        // authoritative controller-side id.
        if (m_activePage == previousActive) {
            m_app->setCurrentPageId(m_activePage);
        }
    });
}

} // namespace PlasmaZones
