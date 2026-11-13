// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Bound component behaviour: the Repeater delegates below read their own
// required properties and the page's members by id, never through an
// unqualified outer-scope lookup, which is what lets the nested controls read
// `slotDelegate.*` and `verbDelegate.*` unambiguously.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/**
 * @brief Dynamic workspaces, Quick Shortcuts leaf.
 *
 * Two cards, both edited in place and saved like every other setting:
 *
 * The workspace shortcuts card records the chords of the directional
 * workspace actions (focus, move window, move column, reorder, move to
 * monitor). Several ship unbound on purpose, because the natural chords
 * already belong to KWin or to another PlasmaZones family, and this is where
 * the user gives them one.
 *
 * The quick shortcuts card is the quick-layout model applied to workspaces: a
 * fixed set of slots, each with two chords recorded here. The move chord
 * sends the active window to the named workspace the slot is assigned to,
 * which the slot's picker sets. The switch chord reads no assignment: it goes
 * to the slot's POSITION in the list of the monitor under the pointer
 * (WorkspaceController::focusWorkspaceAt on the cursor screen), whatever is
 * there. A slot target is
 * stored as the entry's persistent id together with its name at assignment,
 * so it follows a rename, and a slot whose workspace is no longer declared
 * stays assigned (dormant) and shows the stored name, which the daemon also
 * falls back to.
 *
 * Every chord is checked against the other PlasmaZones global chords before
 * it is taken (SettingsController::workspaceShortcutConflict), so one chord
 * never names two actions.
 */
SettingsFlickable {
    id: root

    /// The config's field names inside an entry map and a slot target map.
    readonly property var _f: settingsController.namedEntryFields

    /// The declared named workspaces that can be a slot target: those with a
    /// persistent id. An entry without one exists only in a hand-edited config
    /// the Named Workspaces page has not saved yet, and saving that page gives
    /// it one.
    readonly property var _targetable: {
        var out = [];
        var entries = settingsController.namedWorkspaces || [];
        for (var i = 0; i < entries.length; ++i) {
            if ((entries[i][root._f.id] || "") !== "")
                out.push(entries[i]);
        }
        return out;
    }

    /// The slot combos' shared option list: "None" plus every targetable
    /// workspace, valued by entry id. Built once on the page rather than once
    /// per slot, because every combo shows the same set.
    readonly property var slotOptions: {
        var items = [
            {
                "label": i18nc("@item no workspace assigned to the quick shortcut slot", "None"),
                "value": ""
            }
        ];
        for (var i = 0; i < root._targetable.length; ++i)
            items.push({
                "label": root._targetable[i][root._f.name],
                "value": root._targetable[i][root._f.id]
            });
        return items;
    }

    /// Ids compared the way the daemon compares them (QUuid equality), so a
    /// hand-edited id in another case or without braces still matches.
    function _sameId(a, b) {
        var norm = function (s) {
            return ("" + (s || "")).trim().replace(/^\{|\}$/g, "").toLowerCase();
        };
        return norm(a) !== "" && norm(a) === norm(b);
    }

    /// The declared entry a slot target resolves to, the daemon's order: the
    /// stored id first, then the stored name. Null when neither matches, which
    /// leaves the slot dormant.
    function _resolveTarget(target) {
        var id = (target && target[root._f.id]) || "";
        var name = ("" + ((target && target[root._f.name]) || "")).trim();
        var entries = settingsController.namedWorkspaces || [];
        for (var i = 0; i < entries.length; ++i) {
            if (root._sameId(entries[i][root._f.id], id))
                return entries[i];
        }
        if (name !== "") {
            for (var j = 0; j < entries.length; ++j) {
                if (entries[j][root._f.name] === name)
                    return entries[j];
            }
        }
        return null;
    }

    /// The directional workspace actions. `property` is the Settings
    /// Q_PROPERTY holding the chord, which is also its key for the conflict
    /// check.
    readonly property var _verbs: [
        {
            "property": "workspaceFocusUpShortcut",
            "title": i18n("Focus workspace above"),
            "description": i18n("Switch the monitor under the pointer to the workspace above. Used while Replace KWin desktop shortcuts is on, or once you pick a shortcut of your own.")
        },
        {
            "property": "workspaceFocusDownShortcut",
            "title": i18n("Focus workspace below"),
            "description": i18n("Switch the monitor under the pointer to the workspace below. Used while Replace KWin desktop shortcuts is on, or once you pick a shortcut of your own.")
        },
        {
            "property": "workspaceMoveWindowUpShortcut",
            "title": i18n("Move window to workspace above"),
            "description": ""
        },
        {
            "property": "workspaceMoveWindowDownShortcut",
            "title": i18n("Move window to workspace below"),
            "description": ""
        },
        {
            "property": "workspaceMoveColumnUpShortcut",
            "title": i18n("Move column to workspace above"),
            "description": i18n("On a scrolling monitor, moves the whole focused column.")
        },
        {
            "property": "workspaceMoveColumnDownShortcut",
            "title": i18n("Move column to workspace below"),
            "description": i18n("On a scrolling monitor, moves the whole focused column.")
        },
        {
            "property": "workspaceReorderUpShortcut",
            "title": i18n("Move workspace up"),
            "description": i18n("Swap the current workspace of the monitor under the pointer with the one above it.")
        },
        {
            "property": "workspaceReorderDownShortcut",
            "title": i18n("Move workspace down"),
            "description": i18n("Swap the current workspace of the monitor under the pointer with the one below it.")
        },
        {
            "property": "workspaceMoveToMonitorLeftShortcut",
            "title": i18n("Move workspace to monitor on the left"),
            "description": ""
        },
        {
            "property": "workspaceMoveToMonitorRightShortcut",
            "title": i18n("Move workspace to monitor on the right"),
            "description": ""
        }
    ]

    contentHeight: content.implicitHeight
    clip: true

    ColumnLayout {
        id: content

        width: parent.width
        spacing: Kirigami.Units.largeSpacing

        SettingsCard {
            Layout.fillWidth: true
            headerText: i18n("Workspace shortcuts")
            searchAnchor: "workspaceVerbShortcuts"
            collapsible: true

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                Repeater {
                    model: root._verbs

                    delegate: ColumnLayout {
                        id: verbDelegate

                        required property var modelData
                        required property int index

                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        SettingsSeparator {
                            visible: verbDelegate.index > 0
                        }

                        SettingsRow {
                            title: verbDelegate.modelData.title
                            description: verbDelegate.modelData.description

                            ShortcutCaptureField {
                                width: Kirigami.Units.gridUnit * 12
                                accessibleName: verbDelegate.modelData.title
                                placeholderText: i18nc("@info:placeholder no shortcut assigned", "None")
                                requireModifier: true
                                clearable: true
                                checkConflicts: true
                                conflictKey: verbDelegate.modelData.property
                                keySequence: appSettings[verbDelegate.modelData.property]
                                onKeySequenceModified: seq => {
                                    appSettings[verbDelegate.modelData.property] = seq;
                                }
                            }
                        }
                    }
                }
            }
        }

        SettingsCard {
            Layout.fillWidth: true
            headerText: i18n("Workspace quick shortcuts")
            searchAnchor: "workspaceQuickShortcuts"
            collapsible: true

            contentItem: ColumnLayout {
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.smallSpacing
                    Layout.leftMargin: Kirigami.Units.largeSpacing
                    Layout.rightMargin: Kirigami.Units.largeSpacing
                    wrapMode: Text.WordWrap
                    opacity: 0.6
                    text: i18n("Each slot has two shortcuts. The move shortcut sends the active window to the named workspace assigned to the slot. The switch shortcut ignores that assignment and goes to the workspace at the slot's position in the list of the monitor under the pointer.")
                }

                Repeater {
                    // The quick-slot count the indexed key builders and the
                    // daemon's slot entries share
                    // (ConfigDefaults::WorkspaceSlotCount).
                    model: settingsController.workspaceSlotCount

                    delegate: ColumnLayout {
                        id: slotDelegate

                        required property int index
                        readonly property int slotNumber: index + 1
                        // The Settings Q_PROPERTYs of this slot. Read through
                        // the bracket form so each binding tracks its own
                        // property's NOTIFY.
                        readonly property string moveProperty: "workspaceMoveSlot" + slotNumber + "Shortcut"
                        readonly property string focusProperty: "workspaceFocusSlot" + slotNumber + "Shortcut"
                        readonly property var target: appSettings["workspaceSlot" + slotNumber + "Target"] || ({})
                        readonly property string targetId: target[root._f.id] || ""
                        readonly property string storedName: ("" + (target[root._f.name] || "")).trim()
                        readonly property var resolved: root._resolveTarget(target)
                        readonly property bool assigned: targetId !== "" || storedName !== ""

                        Layout.fillWidth: true
                        spacing: 0

                        SettingsSeparator {
                            visible: slotDelegate.index > 0
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.margins: Kirigami.Units.smallSpacing
                            Layout.leftMargin: Kirigami.Units.largeSpacing
                            Layout.rightMargin: Kirigami.Units.largeSpacing
                            spacing: Kirigami.Units.largeSpacing

                            Label {
                                text: i18n("Quick Workspace %1", slotDelegate.slotNumber)
                                Layout.fillWidth: true
                                Layout.minimumWidth: Kirigami.Units.gridUnit * 8
                                elide: Text.ElideRight
                                font.bold: true
                            }

                            WideComboBox {
                                id: slotCombo

                                Layout.preferredWidth: Kirigami.Units.gridUnit * 14
                                model: root.slotOptions
                                textRole: "label"
                                valueRole: "value"
                                // storedValue, never a hand-rolled
                                // currentIndex binding: QQC2 severs
                                // currentIndex the moment the user picks an
                                // item, after which the combo stops following
                                // the slot. The Clear button beside it writes
                                // the target from elsewhere, so the display
                                // has to keep following it.
                                storedValue: slotDelegate.resolved ? slotDelegate.resolved[root._f.id] : ""
                                // An assigned workspace that is no longer
                                // declared stays legible and is marked as
                                // such: the slot is dormant, not broken, and
                                // not live either. WideComboBox clamps an
                                // unresolved storedValue to index 0, which
                                // here is "None", so without this fallback a
                                // dormant slot would read as unassigned. The
                                // stored name is shown, never the raw id.
                                // A slot that resolves by name to an entry
                                // with no id yet is live, but that entry is
                                // not among the options (they are valued by
                                // id), so the combo would also clamp it to
                                // "None". It shows the entry's name instead.
                                // Only the closed combo does: currentIndex
                                // and currentText stay on "None", so the open
                                // popup highlights that row, because no row
                                // stands for the entry until the Named page
                                // writes the list and gives it an id.
                                displayText: {
                                    var live = slotDelegate.resolved;
                                    if (live && (live[root._f.id] || "") === "")
                                        return "" + (live[root._f.name] || "");
                                    if (live || !slotDelegate.assigned)
                                        return currentText;
                                    if (slotDelegate.storedName !== "")
                                        return i18nc("%1 is the name of a workspace that is no longer declared", "%1 (not declared)", slotDelegate.storedName);
                                    return i18n("A workspace that is no longer declared");
                                }
                                Accessible.name: i18n("Workspace for quick shortcut %1", slotDelegate.slotNumber)
                                onActivated: function (index) {
                                    var option = root.slotOptions[index];
                                    var value = option ? (option.value || "") : "";
                                    if (value === "" ? slotDelegate.assigned : !root._sameId(value, slotDelegate.targetId))
                                        appSettings.setWorkspaceSlotTarget(slotDelegate.index, value);
                                }
                            }

                            ToolButton {
                                icon.name: "edit-clear"
                                enabled: slotDelegate.assigned
                                onClicked: {
                                    appSettings.setWorkspaceSlotTarget(slotDelegate.index, "");
                                    // The button disables itself on the write,
                                    // which would drop keyboard focus out of
                                    // the slot. Hand it to the picker beside it.
                                    slotCombo.forceActiveFocus(Qt.TabFocusReason);
                                }
                                ToolTip.visible: hovered
                                ToolTip.delay: Kirigami.Units.toolTipDelay
                                ToolTip.text: i18n("Clear assignment")
                                Accessible.name: i18n("Clear the workspace assigned to quick shortcut %1", slotDelegate.slotNumber)
                            }
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: Kirigami.Units.largeSpacing * 2
                            Layout.rightMargin: Kirigami.Units.largeSpacing
                            Layout.bottomMargin: Kirigami.Units.smallSpacing
                            columns: 2
                            columnSpacing: Kirigami.Units.largeSpacing
                            rowSpacing: Kirigami.Units.smallSpacing

                            Label {
                                Layout.fillWidth: true
                                text: i18nc("@label shortcut that moves the active window to the slot's workspace", "Move window there")
                                elide: Text.ElideRight
                            }

                            ShortcutCaptureField {
                                Layout.fillWidth: false
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                                accessibleName: i18n("Shortcut that moves the active window to quick workspace %1", slotDelegate.slotNumber)
                                placeholderText: i18nc("@info:placeholder no shortcut assigned", "None")
                                requireModifier: true
                                clearable: true
                                checkConflicts: true
                                conflictKey: slotDelegate.moveProperty
                                keySequence: appSettings[slotDelegate.moveProperty]
                                onKeySequenceModified: seq => {
                                    appSettings[slotDelegate.moveProperty] = seq;
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: i18nc("@label shortcut that switches to the workspace at the slot's position", "Switch to position %1", slotDelegate.slotNumber)
                                elide: Text.ElideRight
                            }

                            ShortcutCaptureField {
                                Layout.fillWidth: false
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                                accessibleName: i18n("Shortcut that switches to the workspace at position %1", slotDelegate.slotNumber)
                                placeholderText: i18nc("@info:placeholder no shortcut assigned", "None")
                                requireModifier: true
                                clearable: true
                                checkConflicts: true
                                conflictKey: slotDelegate.focusProperty
                                keySequence: appSettings[slotDelegate.focusProperty]
                                onKeySequenceModified: seq => {
                                    appSettings[slotDelegate.focusProperty] = seq;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
