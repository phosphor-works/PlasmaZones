// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.phosphor.control
import "LoaderHelpers.js" as PhosphorLoaderHelpers

/**
 * One section another module adds to a page, as ExtensionSlot lists it. The
 * required properties are ExtensionSlotModel's roles.
 *
 * Shows a caption naming the module the section comes from, then the
 * section's QML. The section is created with `domain` (the extension's
 * staging domain) already set, so it must declare a `domain` property
 * (QtObject, and it may be required) and can read it in
 * Component.onCompleted. It gets `settingsApp` assigned once it is created,
 * if it declares that, and the frame's settingsApp does not change after
 * that while the section stays (ExtensionSlot builds its frames only once it has
 * one, and PageHost and every page hand it the same one). When the
 * module did not load, or the section cannot be shown, a card says so in the
 * section's place, so the rest of the page still works. The host's own reason
 * goes below the card, except for a module turned off after a crash, where
 * the card says it all.
 *
 * The frame is the search anchor ModuleHost's results name for the section,
 * `<module>:<extension>` (its extensionId). It registers under that anchor
 * with the nearest ancestor that has registerSearchAnchor(anchorId, item),
 * the call a page's reveal registry takes (PageHost hands that page the
 * pending anchor through its revealAnchor()), and unregisters with
 * unregisterSearchAnchor(anchorId, item) as it goes. A frame with no such
 * ancestor registers nowhere: PageHost's top and bottom slots sit outside the
 * page's scrolling area and are always on screen.
 */
ColumnLayout {
    id: root

    required property string extensionId
    required property string moduleName
    /** A page of the section's module, where it says why it did not load. */
    required property string modulePageId
    required property url qmlSource
    required property QtObject domain
    /** The section cannot be shown: see moduleFailed and error. */
    required property bool failed
    /** It cannot be shown because its module did not load. */
    required property bool moduleFailed
    required property bool disabledAfterCrash
    /** The host's untranslated reason when failed. */
    required property string error
    required property ApplicationController settingsApp

    readonly property bool _qmlFailed: !root.failed && sectionLoader.status === Loader.Error
    // Listed as showable but its domain is gone: the provider has not
    // re-listed it yet, or never will. Nothing is built without a domain.
    readonly property bool _domainGone: !root.failed && root.domain === null
    // The required properties arrive one at a time, at creation and when the
    // row changes, so a load waits for all of them: once the frame is
    // complete, then once per change, after the rest of it has landed.
    property bool _complete: false
    // The registry the anchor went to, kept so the frame unregisters from the
    // same one even when it has been taken out of the tree by then. A typed
    // property, so it reads null once that registry is gone.
    property Item _anchorRegistry: null
    property string _registeredAnchor: ""

    function _registerSearchAnchor() {
        if (root._anchorRegistry !== null || root.extensionId.length === 0)
            return;
        for (let p = root.parent; p; p = p.parent) {
            if (typeof p.registerSearchAnchor === "function") {
                p.registerSearchAnchor(root.extensionId, root);
                root._anchorRegistry = p;
                root._registeredAnchor = root.extensionId;
                return;
            }
        }
    }

    function _unregisterSearchAnchor() {
        if (root._anchorRegistry !== null && typeof root._anchorRegistry.unregisterSearchAnchor === "function")
            root._anchorRegistry.unregisterSearchAnchor(root._registeredAnchor, root);
        root._anchorRegistry = null;
        root._registeredAnchor = "";
    }

    function _load() {
        if (root.failed || root.domain === null) {
            sectionLoader.source = "";
            return;
        }
        sectionLoader.setSource(root.qmlSource, {
            "domain": root.domain
        });
    }

    function _scheduleLoad() {
        if (root._complete)
            reloadTimer.restart();
    }

    objectName: "module-extension:" + root.extensionId
    spacing: Kirigami.Units.smallSpacing
    Component.onCompleted: {
        root._complete = true;
        root._load();
        // Later, once the frame is in its slot and the slot on its page.
        Qt.callLater(root._registerSearchAnchor);
    }
    Component.onDestruction: root._unregisterSearchAnchor()
    onFailedChanged: root._scheduleLoad()
    onQmlSourceChanged: root._scheduleLoad()
    onDomainChanged: root._scheduleLoad()

    Timer {
        id: reloadTimer

        interval: 0
        onTriggered: root._load()
    }

    QQC2.Label {
        Layout.fillWidth: true
        text: qsTr("From %1").arg(root.moduleName)
        textFormat: Text.PlainText
        font: Kirigami.Theme.smallFont
        color: Kirigami.Theme.disabledTextColor
        elide: Text.ElideRight
    }

    Loader {
        id: sectionLoader

        objectName: "module-extension-section"
        Layout.fillWidth: true
        visible: !root.failed && !root._domainGone && sectionLoader.status === Loader.Ready
        onLoaded: {
            PhosphorLoaderHelpers.bindItemWidthToLoader(sectionLoader);
            PhosphorLoaderHelpers.injectIfAssignable(sectionLoader.item, "settingsApp", root.settingsApp);
        }
    }

    Kirigami.InlineMessage {
        id: errorCard

        objectName: "module-extension-error"
        Layout.fillWidth: true
        visible: root.failed || root._qmlFailed || root._domainGone
        type: root.disabledAfterCrash ? Kirigami.MessageType.Warning : Kirigami.MessageType.Error
        text: {
            if (root.disabledAfterCrash)
                return qsTr("This section is turned off, because %1 closed unexpectedly the last time it loaded the module it comes from.").arg(Qt.application.displayName);
            if (root.moduleFailed)
                return qsTr("This section could not be shown, because the module it comes from did not load.");
            if (root._qmlFailed)
                return qsTr("This section could not be shown, because its QML did not load.");
            return qsTr("This section could not be shown.");
        }
        actions: [
            Kirigami.Action {
                objectName: "module-extension-open-module"
                visible: root.moduleFailed && root.modulePageId.length > 0
                icon.name: "go-next"
                text: qsTr("Open Module Page")
                onTriggered: root.settingsApp.currentPageId = root.modulePageId
            }
        ]
    }

    // The host's own account of the failure, for a bug report, so it can be
    // selected and copied. Not translated: it names files, ids and contract
    // rules. Left out for a module turned off after a crash, as on
    // ModulePage: the card above already says that, translated.
    Kirigami.SelectableLabel {
        objectName: "module-extension-reason"
        Layout.fillWidth: true
        visible: root.failed && !root.disabledAfterCrash && root.error.length > 0
        text: root.error
        textFormat: TextEdit.PlainText
        wrapMode: TextEdit.Wrap
        color: Kirigami.Theme.disabledTextColor
    }
}
