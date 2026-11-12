// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.phosphor.control
import "LoaderHelpers.js" as PhosphorLoaderHelpers

/**
 * One section another module adds to a page, as FacetSlot lists it. The
 * required properties are FacetSlotModel's roles.
 *
 * Shows a caption naming the module the section comes from, then the
 * section's QML. The section is created with `domain` (the extension's
 * staging domain) already set, so it must declare a `domain` property
 * (QtObject, and it may be required) and can read it in
 * Component.onCompleted. It gets `settingsApp` if it declares that. When the
 * module did not load, or the section cannot be shown, a card says so in the
 * section's place, so the rest of the page still works. The host's own reason
 * goes below the card, except for a module turned off after a crash, where
 * the card says it all.
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

    objectName: "facet-extension:" + root.extensionId
    spacing: Kirigami.Units.smallSpacing
    Component.onCompleted: {
        root._complete = true;
        root._load();
    }
    onFailedChanged: root._scheduleLoad()
    onQmlSourceChanged: root._scheduleLoad()
    onDomainChanged: root._scheduleLoad()
    onSettingsAppChanged: PhosphorLoaderHelpers.injectIfAssignable(sectionLoader.item, "settingsApp", root.settingsApp)

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

        objectName: "facet-extension-section"
        Layout.fillWidth: true
        visible: !root.failed && !root._domainGone && sectionLoader.status === Loader.Ready
        onLoaded: {
            PhosphorLoaderHelpers.bindItemWidthToLoader(sectionLoader);
            PhosphorLoaderHelpers.injectIfAssignable(sectionLoader.item, "settingsApp", root.settingsApp);
        }
    }

    Kirigami.InlineMessage {
        id: errorCard

        objectName: "facet-extension-error"
        Layout.fillWidth: true
        visible: root.failed || root._qmlFailed || root._domainGone
        type: root.disabledAfterCrash ? Kirigami.MessageType.Warning : Kirigami.MessageType.Error
        text: {
            if (root.disabledAfterCrash)
                return qsTr("This section is turned off, because Facet closed unexpectedly the last time it loaded the module it comes from.");
            if (root.moduleFailed)
                return qsTr("This section could not be shown, because the module it comes from did not load.");
            if (root._qmlFailed)
                return qsTr("This section could not be shown, because its QML did not load.");
            return qsTr("This section could not be shown.");
        }
        actions: [
            Kirigami.Action {
                objectName: "facet-extension-open-module"
                visible: root.moduleFailed && root.modulePageId.length > 0
                icon.name: "go-next"
                text: qsTr("Open Module Page")
                onTriggered: root.settingsApp.currentPageId = root.modulePageId
            }
        ]
    }

    // The host's own account of the failure, for a bug report. Not
    // translated: it names files, ids and contract rules. Left out for a
    // module turned off after a crash, as on FacetModulePage: the card above
    // already says that, translated.
    QQC2.Label {
        objectName: "facet-extension-reason"
        Layout.fillWidth: true
        visible: root.failed && !root.disabledAfterCrash && root.error.length > 0
        text: root.error
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        color: Kirigami.Theme.disabledTextColor
    }
}
