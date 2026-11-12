// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

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

    /** The placeholder controller PageHost injects. Exposes moduleName,
     *  failed, disabledAfterCrash, reason, canRetry and retry(). */
    property QtObject controller: null

    readonly property string _name: root.controller ? root.controller.moduleName : ""
    readonly property bool _failed: root.controller !== null && root.controller.failed
    readonly property bool _disabled: root.controller !== null && root.controller.disabledAfterCrash

    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
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
                enabled: root.controller !== null && root.controller.canRetry
                icon.name: "view-refresh"
                text: root._failed || root._disabled ? qsTr("Try Again") : qsTr("Load")
                onTriggered: root.controller.retry()
            }
        }

        // The host's own account of the failure, for a bug report. Not
        // translated: it names files, ids and contract rules.
        QQC2.Label {
            Layout.fillWidth: true
            visible: root._failed && text.length > 0
            text: root.controller ? root.controller.reason : ""
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            color: Kirigami.Theme.disabledTextColor
            textFormat: Text.PlainText
        }
    }
}
