// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.phosphor.control

/**
 * A place on a page where other modules' sections appear.
 *
 * PageHost puts one at the top and one at the bottom of every page. A page
 * puts one before or after each anchor it lists in its metadata, or at a
 * named extension point:
 *
 *     FacetSlot {
 *         Layout.fillWidth: true
 *         settingsApp: page.settingsApp
 *         pageId: "placement:snapping"
 *         slot: "after:drag-modifier"
 *     }
 *
 * Each section the host resolved for that slot is shown in a
 * FacetExtensionFrame, in the order the extensions ask for. A slot with no
 * sections takes no space. settingsApp may arrive after the slot is built,
 * as PageHost's injection does, and the sections appear then.
 */
ColumnLayout {
    id: root

    required property ApplicationController settingsApp
    /** The page's scoped id, `<module>:<page>`. */
    required property string pageId
    /** `top`, `bottom`, `before:<anchor>`, `after:<anchor>`, or an extension point id. */
    required property string slot
    /** How many sections the slot shows. */
    readonly property int count: sections.count

    visible: root.count > 0
    spacing: Kirigami.Units.largeSpacing

    Repeater {
        model: FacetSlotModel {
            id: sections

            settingsApp: root.settingsApp
            pageId: root.pageId
            slot: root.slot
        }

        delegate: FacetExtensionFrame {
            Layout.fillWidth: true
            settingsApp: root.settingsApp
        }
    }
}
