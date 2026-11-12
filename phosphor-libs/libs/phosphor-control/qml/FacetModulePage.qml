// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.phosphor.control

/**
 * The page a Facet module's page shows while the module is not loaded
 * (FacetHost::placeholderPageSource). A module normally loads before its
 * page is shown, so this is on screen when it did not: its library failed,
 * it broke its contract, it was turned off after crashing Facet, or nothing
 * tried to load it yet. It names the module, says what happened, shows the
 * host's diagnostic reason, and offers to load it when that can work. A
 * load that succeeds replaces this page with the module's own.
 */
Item {
    id: root

    /** The host's placeholder controller for the page: the module's name,
     *  its state, the host's reason, and retry(). PageHost creates the page
     *  with it and with `settingsApp` already set, as it creates every
     *  module page. */
    required property ModulePlaceholderPage controller
    /** Not read here. Declared because PageHost passes it to every module
     *  page when it creates one, and a page that does not declare it makes
     *  Qt log "Cannot assign to non-existent property" with no file or line
     *  to trace it to. */
    required property ApplicationController settingsApp

    readonly property string _name: root.controller.moduleName
    readonly property bool _failed: root.controller.failed
    readonly property bool _disabled: root.controller.disabledAfterCrash

    // A long reason can make the message taller than the page, so it scrolls
    // rather than spill over the chrome around the page.
    QQC2.ScrollView {
        id: scroller

        anchors.fill: parent
        contentWidth: scroller.availableWidth
        contentHeight: column.implicitHeight

        ColumnLayout {
            id: column

            x: Kirigami.Units.gridUnit * 2
            // Centred while it fits, from the top once it does not.
            y: Math.max(0, (scroller.availableHeight - column.implicitHeight) / 2)
            width: scroller.availableWidth - Kirigami.Units.gridUnit * 4
            spacing: Kirigami.Units.largeSpacing

            Kirigami.PlaceholderMessage {
                Layout.fillWidth: true
                icon.name: root._disabled ? "dialog-warning" : (root._failed ? "dialog-error" : "plugins")
                text: {
                    if (root._disabled)
                        return qsTr("%1 is turned off").arg(root._name);
                    if (root._failed)
                        return qsTr("%1 could not be loaded").arg(root._name);
                    return qsTr("%1 is not loaded yet").arg(root._name);
                }
                explanation: root._disabled ? qsTr("Facet closed unexpectedly the last time it loaded this module, so it stays off until you load it again.") : ""
                helpfulAction: Kirigami.Action {
                    enabled: root.controller.canRetry
                    icon.name: "view-refresh"
                    text: root._failed || root._disabled ? qsTr("Try Again") : qsTr("Load")
                    onTriggered: root.controller.retry()
                }
            }

            // The host's own account of the failure, for a bug report, so it
            // can be selected and copied. Not translated: it names files, ids
            // and contract rules.
            Kirigami.SelectableLabel {
                Layout.fillWidth: true
                visible: root._failed && text.length > 0
                text: root.controller.reason
                wrapMode: TextEdit.Wrap
                horizontalAlignment: TextEdit.AlignHCenter
                color: Kirigami.Theme.disabledTextColor
                textFormat: TextEdit.PlainText
            }
        }
    }
}
