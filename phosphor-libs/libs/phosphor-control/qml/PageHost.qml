// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.phosphor.animation
import org.phosphor.control
import "LoaderHelpers.js" as PhosphorLoaderHelpers

/**
 * Hosts the QML page for ApplicationController.currentPageId, KDE
 * System-Settings style: each visited page is built once and KEPT ALIVE
 * (hidden), so navigating back to it is an instant visibility flip rather
 * than a rebuild. This mirrors how `systemsettings` caches loaded KCMs.
 *
 * This Item only handles the instance cache. An app that wants a page's
 * first visit to pay for construction alone can compile its page QML ahead
 * of time, after startup.
 *
 * Each page gets `controller`, the registered PageController for its page
 * id, and `settingsApp`, the ApplicationController, for cross-page access.
 * A page whose entry passes context (PageRegistry::Entry::passesContext,
 * every control module page) is created with both already set, so it may
 * declare them required, and it is not created while its controller is
 * gone. Any other page gets them assigned once it is created, if it
 * declares them.
 *
 * When the current page cannot be shown (its QML did not load, or it passes
 * context and has no controller), the page area says so. When no page is
 * current, or the current entry has no QML of its own (a category), it asks
 * for a page instead.
 *
 * Above and below the current page sit its `top` and `bottom` ExtensionSlots,
 * where other modules' sections appear. They are outside the page's own
 * scrolling content, take no space while the page has no such section, and
 * follow the current page, so unlike the pages they are built again on each
 * visit. What a section stages lives in its domain and survives that.
 */
Item {
    id: root

    required property ApplicationController controller
    /** Edge gutter applied around every loaded page so page content
     *  (cards, rows, breadcrumb-aligned headers) has breathing room
     *  instead of running flush to the chrome separators / window
     *  edge. Defaults to largeSpacing to match the UnsavedChangesFooter
     *  content inset and the breadcrumb row. */
    property real contentMargins: Kirigami.Units.largeSpacing

    // Bumped by _refreshPage when a page is bound (PageRegistry::pageBound),
    // so _currentEntry and every Loader's _entry read pageData() again:
    // pageData() is a non-notifying invokable. Internal state, hence the `_`
    // prefix.
    property int _registryRevision: 0

    // The current page's registry entry, or null when no page is current.
    // ApplicationController::setCurrentPageId refuses an id the registry does
    // not hold, and the registry never removes an entry, so a current id
    // always has one.
    readonly property var _currentEntry: {
        // `void (…)` marks the read as a deliberate dependency registration
        // rather than a stray statement a cleanup would delete.
        void (root._registryRevision);
        if (root.controller.currentPageId === "")
            return null;
        return root.controller.registry.pageData(root.controller.currentPageId);
    }

    // ── Instance cache ───────────────────────────────────────────────
    // Ordered model of page ids that have a live (built, cached) Loader.
    // Appended to the first time each page becomes current; the Repeater
    // below maps it to one persistent Loader per id. Backed by a ListModel,
    // NOT a plain JS array: Repeater does not diff array models, so a
    // reassigned array would destroy and rebuild EVERY delegate on each
    // first visit, throwing away all cached page instances. ListModel
    // appends leave existing delegates untouched. Never reordered, so a
    // page's Loader index is its row here for the host's lifetime.
    ListModel {
        id: cacheModel
    }

    // Append `id` to the cache model if it is not already cached. Only ever
    // handed the current page id, which is registered (see _currentEntry).
    function _cachePage(id: string): void {
        if (id === "")
            return;
        for (let i = 0; i < cacheModel.count; ++i) {
            if (cacheModel.get(i).pageId === id)
                return;
        }
        cacheModel.append({
            pageId: id
        });
    }

    Component.onCompleted: root._cachePage(root.controller.currentPageId)

    // ── Loading and failure state ────────────────────────────────────
    // The page id whose Loader is current AND still incubating, or "".
    // Maintained imperatively by the delegates (see _reportIncubating):
    // only one delegate is current at a time, but on fast navigation the
    // new page can report loading BEFORE the old one reports done, so
    // the clear path only clears its OWN id.
    property string _loadingPageId: ""
    // The page id whose Loader is current and cannot show its page, or "".
    // Reported the same way, for the same reason.
    property string _unavailablePageId: ""

    function _reportIncubating(id: string, incubating: bool): void {
        if (incubating)
            root._loadingPageId = id;
        else if (root._loadingPageId === id)
            root._loadingPageId = "";
    }

    function _reportUnavailable(id: string, unavailable: bool): void {
        if (unavailable)
            root._unavailablePageId = id;
        else if (root._unavailablePageId === id)
            root._unavailablePageId = "";
    }

    // Cache the page the instant it becomes current (covers normal
    // navigation, restored-on-startup ids, programmatic switches).
    Connections {
        function onCurrentPageIdChanged() {
            root._cachePage(root.controller.currentPageId);
        }

        target: root.controller
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.contentMargins
        spacing: Kirigami.Units.largeSpacing

        ExtensionSlot {
            Layout.fillWidth: true
            settingsApp: root.controller
            pageId: root.controller.currentPageId
            slot: "top"
        }

        Item {
            id: pageArea

            Layout.fillWidth: true
            Layout.fillHeight: true

            Repeater {
                id: pageRepeater

                model: cacheModel

                delegate: Loader {
                    id: pageLoader

                    required property string pageId
                    readonly property bool isCurrent: root.controller.currentPageId === pageLoader.pageId
                    // Lazy-build latch: a page incubates the first time it becomes
                    // current and stays built (cached) thereafter. Without it, `active:
                    // true` built ALL pages at once on startup — an async-incubation
                    // storm that overlapped the daemon's initial D-Bus broadcasts
                    // (model resets / layout reloads re-entering mid-incubation), an
                    // observed source of incubation-time crashes. Building one page at
                    // a time, on navigation, keeps the async no-hitch behaviour while
                    // removing the storm.
                    property bool _everCurrent: false

                    // True while this page is the one being shown but its item is
                    // still incubating, the window where the viewport would otherwise
                    // be blank because the item does not exist yet. Reported up to
                    // the host, which shows one shared loading indicator over the
                    // page area.
                    readonly property bool isIncubating: pageLoader.isCurrent && pageLoader.status === Loader.Loading
                    onIsIncubatingChanged: root._reportIncubating(pageLoader.pageId, pageLoader.isIncubating)

                    // True while this page is the one being shown and cannot be:
                    // its QML did not load, or _load refused to create it without a
                    // controller. Reported up to the host, which says so.
                    readonly property bool isUnavailable: pageLoader.isCurrent && (pageLoader.status === Loader.Error || pageLoader._refused)
                    onIsUnavailableChanged: root._reportUnavailable(pageLoader.pageId, pageLoader.isUnavailable)

                    // Deep-link reveal latch: when this page is built + current,
                    // consume any pending anchor targeting it and invoke the page's
                    // duck-typed revealAnchor() contract. Covers first build
                    // (onLoaded), cached re-show (onIsCurrentChanged), and same-page
                    // deep links (the pendingAnchorChanged Connections below). A page
                    // without revealAnchor() leaves the anchor pending, so the page
                    // that replaces it in place (a module page replacing its
                    // placeholder) still reveals it.
                    function _maybeReveal(): void {
                        if (!pageLoader.item || !pageLoader.isCurrent || pageLoader.status !== Loader.Ready)
                            return;
                        // qmllint disable missing-property
                        // revealAnchor() is an optional duck-typed page contract that
                        // no page type declares, so the lint cannot see it.
                        if (typeof pageLoader.item.revealAnchor !== "function")
                            return;
                        const anchor = root.controller.takePendingAnchor(pageLoader.pageId);
                        if (anchor.length > 0)
                            pageLoader.item.revealAnchor(anchor);
                        // qmllint enable missing-property
                    }

                    // Only the current page tracks the page area's size. A hidden
                    // cached page keeps the size it had, so resizing the window lays
                    // out the shown page alone instead of every page in the cache,
                    // and a re-shown page lays out once, if the size changed while it
                    // was hidden.
                    anchors.fill: pageLoader.isCurrent ? parent : undefined
                    // Built once when this page first becomes current; the compiled
                    // unit is already warm, so construction is the only cost. Async
                    // so even a heavy first build never hitches the UI thread.
                    asynchronous: true
                    active: pageLoader.isCurrent || pageLoader._everCurrent
                    // Latch the initial current page so navigating away from it doesn't
                    // unload it (onIsCurrentChanged never fires for the initial value).
                    // The load _load starts here reports itself through
                    // onIsIncubatingChanged and onIsUnavailableChanged.
                    Component.onCompleted: {
                        if (pageLoader.isCurrent)
                            pageLoader._everCurrent = true;
                        pageLoader._load();
                    }

                    // This page's registry entry. Depends on _registryRevision
                    // (same idiom as _currentEntry): pageData() is non-notifying,
                    // so a bind that changes the page's controller or QML must
                    // force re-evaluation here.
                    readonly property var _entry: {
                        void (root._registryRevision);
                        return root.controller.registry.pageData(pageLoader.pageId);
                    }
                    // The QML last handed to the Loader, so a registry change
                    // that leaves this page's QML alone does not rebuild it.
                    property string _loadedUrl: ""
                    // _load refused to create a page that passes context because
                    // the registry has no controller for it. Cleared by the next
                    // load that goes ahead.
                    property bool _refused: false

                    // Creates the page from its entry. A page whose entry passes
                    // context is created with `controller` and `settingsApp` set
                    // before its bindings run, so it can declare them required,
                    // and is not created at all while the registry has no
                    // controller for it: _loadedUrl stays unset, so the bind that
                    // brings a controller (pageBound, through _entry) loads it.
                    // Every page also gets both assigned in onLoaded.
                    function _load(): void {
                        const entry = pageLoader._entry;
                        const url = String(entry.qmlSource);
                        if (url === pageLoader._loadedUrl)
                            return;
                        if (url !== "" && entry.passesContext) {
                            const pageController = root.controller.registry.controller(pageLoader.pageId);
                            if (!pageController) {
                                if (!pageLoader._refused)
                                    console.warn("PageHost: page", pageLoader.pageId, "passes context but has no controller, so it is created once it is bound to one");
                                pageLoader._refused = true;
                                pageLoader._loadedUrl = "";
                                pageLoader.source = "";
                                return;
                            }
                            pageLoader._refused = false;
                            pageLoader._loadedUrl = url;
                            pageLoader.setSource(url, {
                                "controller": pageController,
                                "settingsApp": root.controller
                            });
                            return;
                        }
                        pageLoader._refused = false;
                        pageLoader._loadedUrl = url;
                        pageLoader.source = url;
                    }
                    on_EntryChanged: pageLoader._load()
                    // Only the current page is shown + interactive. Hidden cached
                    // pages stay built but are not painted and take no input.
                    visible: pageLoader.isCurrent
                    enabled: pageLoader.isCurrent

                    // Fade the page up when it becomes the current one. Starts at 0
                    // and driven imperatively (not `opacity: isCurrent ? 1 : 0` with
                    // a `Behavior`): a Behavior never animates the INITIAL value, so
                    // the first build — whose Loader is created already-current —
                    // would snap to 1 with no fade; and on a cached re-show the
                    // unspecified eval order of the opacity binding vs the Behavior's
                    // `enabled` would drop the animation. Restarting the animation
                    // from the state transition covers first load (onLoaded), cached
                    // re-show (onIsCurrentChanged while already Ready), and resets to
                    // 0 on hide so the next show fades again. A page rebuilt in place
                    // while current keeps the opacity it had until onLoaded restarts
                    // the fade, which does not matter while there is no item to show.
                    opacity: 0
                    onIsCurrentChanged: {
                        if (pageLoader.isCurrent) {
                            // Latch so this page stays built/cached once visited.
                            pageLoader._everCurrent = true;
                            if (pageLoader.status === Loader.Ready) {
                                pageFadeIn.restart();
                                pageLoader._maybeReveal();
                            }
                        } else {
                            pageFadeIn.stop();
                            pageLoader.opacity = 0;
                        }
                    }

                    PhosphorMotionAnimation {
                        id: pageFadeIn

                        target: pageLoader
                        properties: "opacity"
                        from: 0
                        to: 1
                        profile: "widget.fadeIn"
                        durationOverride: Kirigami.Units.shortDuration * 1.8
                    }

                    onLoaded: {
                        if (!pageLoader.item)
                            return;

                        // Every page, including one created with both set: a bind
                        // that kept this page's QML while it was still incubating
                        // found no item to hand its controller to (_refreshPage), so
                        // this is where that controller arrives. The registry's
                        // controller is null once it has been destroyed, which leaves
                        // the page's own typed property null as well. A page that
                        // binds its own value (a read-only declaration) keeps it.
                        const pageController = root.controller.registry.controller(pageLoader.pageId);
                        PhosphorLoaderHelpers.injectIfAssignable(pageLoader.item, "controller", pageController);
                        PhosphorLoaderHelpers.injectIfAssignable(pageLoader.item, "settingsApp", root.controller);

                        // Fade in if this page is the one being shown (covers the
                        // common first-visit case: built async while current).
                        if (pageLoader.isCurrent) {
                            pageFadeIn.restart();
                            pageLoader._maybeReveal();
                        }
                    }

                    Connections {
                        // Same-page deep link: the page is already built + current, so
                        // neither onLoaded nor onIsCurrentChanged fires — react to the
                        // pending-anchor signal directly.
                        function onPendingAnchorChanged() {
                            pageLoader._maybeReveal();
                        }

                        target: root.controller
                    }
                }
            }

            // Shown while the current page's Loader is still incubating (first
            // visit to a heavy page, or a page rebuilt in place). Covers the page
            // area only, so the top and bottom slots stay usable, and hides
            // itself the moment loading ends, before the page fades up.
            PageLoadingIndicator {
                anchors.fill: parent
                z: 1
                loading: root._loadingPageId !== ""
            }

            // No page is current, or the current entry has no QML of its own
            // (a category node), so there is nothing to show but a prompt.
            Kirigami.PlaceholderMessage {
                objectName: "page-host-select-page"
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                z: 1
                visible: root._currentEntry === null || !root._currentEntry.hasQmlSource
                text: qsTr("Select a page from the sidebar")
                icon.name: "settings-configure"
            }

            // The current page has QML but cannot be shown (see the Loader's
            // isUnavailable). The log says why.
            Kirigami.PlaceholderMessage {
                objectName: "page-host-page-unavailable"
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                z: 1
                visible: root._unavailablePageId !== ""
                text: qsTr("This page could not be shown")
                icon.name: "dialog-error"
            }
        }

        ExtensionSlot {
            Layout.fillWidth: true
            settingsApp: root.controller
            pageId: root.controller.currentPageId
            slot: "bottom"
        }
    }

    // A page bound to a new controller and QML source (PageRegistry::bindPage,
    // a control module page whose module loaded) changes what pageData()
    // returns, and an already-built cached Loader keeps the controller it was
    // given. A page registered later needs nothing here: it cannot be current
    // or cached before it is registered.
    Connections {
        function onPageBound(id: string) {
            root._refreshPage(id);
        }

        target: root.controller.registry
    }

    function _refreshPage(id: string): void {
        // The revision bump re-evaluates every Loader's entry, so a page whose
        // QML changed is created again with the new controller (_load). A page
        // whose QML did not change keeps its item and gets the new controller
        // below. bindPage refuses a null controller and emits pageBound right
        // after storing the new one, so the registry's controller is live here.
        root._registryRevision++;
        const pageController = root.controller.registry.controller(id);
        for (let i = 0; i < cacheModel.count; ++i) {
            if (cacheModel.get(i).pageId !== id)
                continue;
            // The cache model's row is the Repeater's index (never reordered).
            const ld = pageRepeater.itemAt(i) as Loader;
            if (ld && ld.item)
                PhosphorLoaderHelpers.injectIfAssignable(ld.item, "controller", pageController);
            return;
        }
    }
}
