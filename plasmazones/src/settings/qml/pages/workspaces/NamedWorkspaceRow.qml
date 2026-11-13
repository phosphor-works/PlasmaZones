// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/**
 * @brief One named-workspace declaration row (ExpandableRowDelegate).
 *
 * Header, in order: icon, a title column (the name over the pinned monitor),
 * the "Pinned" badge, the expand chevron and the remove button. Expansion:
 * the editable fields, which are the name, pinned monitor, position and the
 * two per-workspace shortcuts.
 *
 * Edits emit whole-field commit signals; the page mutates its staged array
 * IN PLACE and writes the composite, so this row (and its expansion state)
 * survives every field edit that the store takes as written. Because the
 * entry object mutates in place, EVERY displayed field is mirrored on a local
 * property updated at commit time instead of bound straight to the map,
 * which would never re-evaluate. When the store does not take an edit as
 * written, or something else rewrites the list, the page rebuilds its rows
 * from the store and this row is created afresh from the stored entry.
 */
ExpandableRowDelegate {
    id: row

    /// The declaration map, keyed by settingsController.namedEntryFields.
    required property var entry
    required property int entryIndex
    /// Physical monitors for the monitor combo, as `{label, value}` rows with
    /// "Any monitor" (value "") first. The page builds it.
    required property var screenOptions
    /// Called with (rawText, entryIndex) and answering `{refusal, name}`:
    /// why that text cannot name the entry at that index (empty when it can),
    /// and the canonical name it would be stored as. Owned by the page so the
    /// Add form and every row apply one rule, read fresh at commit time (a
    /// snapshot would go stale across in-place renames of siblings).
    required property var checkName

    /// The config's field names inside an entry map.
    readonly property var _f: settingsController.namedEntryFields

    /// Every read of `entry` goes through here. The page null-guards its own
    /// resolvers for the same reason (see its `idOf` comment): during a model
    /// reset the delegate's modelData detaches before its destruction handler
    /// runs, and the bindings below re-evaluate in that window.
    function _field(name, fallback) {
        return (row.entry && row.entry[name] !== undefined && row.entry[name] !== null) ? row.entry[name] : fallback;
    }

    // Mirrors of every field the row displays.
    //
    // The page commits a field edit IN PLACE on the same map object, which
    // changes no property and re-evaluates no binding, so a control bound
    // straight to `entry.*` would freeze on whatever the row was built with.
    // Each mirror is seeded from the entry and then written by the handler
    // that commits its field.
    property string headerName: row._field(row._f.name, "")
    property string headerOutput: row._field(row._f.output, "")
    property int entryPosition: row._field(row._f.position, -1)
    property string entryFocusShortcut: row._field(row._f.focusShortcut, "")
    property string entryMoveShortcut: row._field(row._f.moveShortcut, "")
    /// The persistent entry id. Fixed for the life of the row: an entry that
    /// gains an id is a different list key, so the page rebuilds its row.
    readonly property string entryId: row._field(row._f.id, "")

    /// The name ReorderableColumn announces for this row. Published here
    /// because a rename lands in place, where the column's own resolver,
    /// reading the unchanged `items` entry, would keep the old name.
    readonly property string accessibleRowName: row.headerName !== "" ? row.headerName : i18n("(unnamed)")

    /// The key the conflict check knows this entry's chords by: the entry id,
    /// or the name while the entry has none.
    function _shortcutKey(field) {
        var keyed = {};
        keyed[row._f.id] = row.entryId;
        keyed[row._f.name] = row.headerName;
        return settingsController.namedEntryShortcutKey(keyed, field);
    }

    /// Why the last rename attempt was refused, or empty. Shown under the name
    /// field and cleared as soon as the user types again, or when the row
    /// folds away. A refusal that only snapped the text back would look like
    /// the edit was simply lost.
    property string nameError: ""

    signal fieldEdited(int index, string field, var value)
    signal removeRequested(int index)

    /// A pin as the daemon reads it: trimmed, and collapsed to the physical
    /// monitor (a virtual screen's id names its physical parent).
    function _pinKey(outputId) {
        var trimmed = ("" + (outputId || "")).trim();
        return trimmed === "" ? "" : settingsController.physicalScreenId(trimmed);
    }

    function _monitorLabel(outputId) {
        var key = row._pinKey(outputId);
        if (key === "")
            return i18n("Any monitor");
        for (var i = 0; i < screenOptions.length; ++i)
            if (screenOptions[i].value === key)
                return screenOptions[i].label;
        return i18nc("%1 is a monitor id the system does not currently report", "%1 (not connected)", key);
    }

    /// Whether the pinned monitor is one the system currently reports. A pin
    /// naming something absent is not a pin the daemon can act on right now:
    /// it either drops the id or defers the claim. The row cannot tell those
    /// two apart, so it does not guess. It drops the badge to its neutral
    /// flavour and says "not connected" in the subtitle, rather than showing
    /// the confident state pill for a pin nothing has realized.
    readonly property bool outputResolved: {
        var key = row._pinKey(row.headerOutput);
        if (key === "")
            return false;
        for (var i = 0; i < row.screenOptions.length; ++i)
            if (row.screenOptions[i].value === key)
                return true;
        return false;
    }

    expandable: true
    Accessible.name: row.accessibleRowName
    onExpandedChanged: {
        if (!row.expanded)
            row.nameError = "";
    }

    Kirigami.Icon {
        source: "virtual-desktops"
        Layout.alignment: Qt.AlignVCenter
        Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
        Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
    }

    // Title column, RuleRow's shape: bold name over a dimmed one-line
    // summary of where the workspace lives.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 0

        Label {
            Layout.fillWidth: true
            elide: Text.ElideRight
            font.bold: true
            text: row.accessibleRowName
        }

        Label {
            Layout.fillWidth: true
            elide: Text.ElideRight
            opacity: 0.7
            text: row._monitorLabel(row.headerOutput)
        }
    }

    // Pinned-monitor badge, only when actually pinned (the subtitle already
    // says "Any monitor" for the unpinned case). MetadataChip is the shared
    // badge the settings lists use, in its highlighted (state) flavour, which
    // is the same recipe RuleRow's category badge follows.
    MetadataChip {
        visible: row.headerOutput !== ""
        Layout.alignment: Qt.AlignVCenter
        highlighted: row.outputResolved
        text: i18nc("Badge on a named workspace pinned to a monitor", "Pinned")
        Accessible.description: row.outputResolved ? "" : i18n("The pinned monitor is not connected.")
    }

    ExpandChevron {
        expanded: row.expanded
    }

    ToolButton {
        icon.name: "edit-delete"
        Layout.alignment: Qt.AlignVCenter
        Accessible.name: i18nc("@action:button %1 is a named workspace", "Remove %1", row.accessibleRowName)
        ToolTip.text: i18n("Remove named workspace")
        ToolTip.visible: hovered
        ToolTip.delay: Kirigami.Units.toolTipDelay
        onClicked: row.removeRequested(row.entryIndex)
    }

    expansionContent: GridLayout {
        columns: 2
        columnSpacing: Kirigami.Units.largeSpacing
        rowSpacing: Kirigami.Units.smallSpacing

        Label {
            text: i18nc("@label:textbox name of a named workspace", "Name")
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing / 2

            TextField {
                id: nameField

                /// Point the field back at the stored name. Used for every
                /// exit from the rename handler, refusal or not, so the field
                /// always shows what the store actually holds, including the
                /// trimmed form after a rename that only changed surrounding
                /// whitespace. Reassigned as a Qt.binding rather than a plain
                /// string so the declarative `text: row.headerName` binding
                /// survives a later rename. Lives on the field because the
                /// field exists only inside this lazily loaded body, where the
                /// row's own scope cannot reach it.
                function restore() {
                    nameField.text = Qt.binding(function () {
                        return row.headerName;
                    });
                }

                Layout.fillWidth: true
                text: row.headerName
                maximumLength: settingsController.workspaceNameMaxLength
                Accessible.name: i18n("Workspace name")
                Accessible.description: row.nameError
                onTextEdited: row.nameError = ""
                onEditingFinished: {
                    if (text.trim() === row.headerName) {
                        // Unchanged: tabbing through must not dirty the page.
                        // Still put the stored form back, so a rename that
                        // only added whitespace does not leave the field
                        // showing text the store will never hold.
                        row.nameError = "";
                        nameField.restore();
                        return;
                    }
                    // The page owns the rule so the Add form and every row
                    // refuse the same input: the schema's own name check
                    // (SettingsController::checkWorkspaceName) plus the
                    // duplicate test. Committing a name the schema refuses
                    // would delete the declaration on the next round-trip
                    // and take its shortcuts with it.
                    var check = row.checkName(text, row.entryIndex);
                    if (check.refusal !== "") {
                        row.nameError = check.refusal;
                        nameField.restore();
                        return;
                    }
                    row.nameError = "";
                    row.headerName = check.name;
                    row.fieldEdited(row.entryIndex, row._f.name, check.name);
                    nameField.restore();
                }
            }

            Label {
                Layout.fillWidth: true
                visible: row.nameError !== ""
                text: row.nameError
                wrapMode: Text.WordWrap
                font: Kirigami.Theme.smallFont
                color: Kirigami.Theme.negativeTextColor
            }
        }

        Label {
            text: i18nc("@label:listbox monitor a named workspace is pinned to", "Monitor")
        }

        WideComboBox {
            textRole: "label"
            valueRole: "value"
            Accessible.name: i18n("Pinned monitor")
            model: row.screenOptions
            storedValue: row._pinKey(row.headerOutput)
            // A monitor that is currently unplugged is not in `screenOptions`,
            // and WideComboBox clamps an unresolved storedValue to index 0,
            // which is "Any monitor", so the pin would read as if it had been
            // dropped. Show the pin the way the header subtitle does instead.
            // Read through the header mirror, not the entry map: the page
            // commits a field edit IN PLACE on the same map object, which
            // changes no property and re-evaluates no binding.
            displayText: (row.headerOutput !== "" && !row.outputResolved) ? row._monitorLabel(row.headerOutput) : currentText
            onActivated: {
                // The mirror first: `storedValue` follows it, so this is also
                // what keeps the combo showing the pick.
                row.headerOutput = currentValue;
                row.fieldEdited(row.entryIndex, row._f.output, currentValue);
            }
        }

        Label {
            text: i18nc("@label:spinbox preferred place in the monitor's workspace list", "Position")
        }

        SettingsSpinBox {
            id: positionSpin

            // One-based, as displayed: 0 is "Automatic" (stored as -1) and N
            // is the Nth place (stored as the zero-based slice index N - 1),
            // so the value a screen reader reads is the number on screen.
            from: 0
            // The reconciler's DEFAULT desktop cap. It is an authoring
            // ceiling and nothing more: what the daemon actually honours is
            // the target monitor's own workspace count at the moment it
            // places the workspace, which is typically far lower and which
            // the settings app is never told (see
            // SettingsController::workspacesDesktopCap). The control is
            // therefore worded as a PREFERENCE (the tooltip says the number is
            // clamped to the monitor's list) rather than pretending the offered
            // range is all reachable.
            to: settingsController.workspacesDesktopCap
            unitText: ""
            accessibleName: i18n("Preferred position in the monitor's list")
            accessibleDescription: row.entryPosition < 0 ? i18nc("@item position chosen by PlasmaZones", "Automatic") : ""
            tooltipText: i18n("Where in the monitor's workspace list this workspace prefers to sit. A number past the end of that list lands at the end. Automatic places it before the trailing empty workspace.")
            textFromValue: function (value, locale) {
                return value <= 0 ? i18nc("@item position chosen by PlasmaZones", "Automatic") : Number(value).toLocaleString(locale, 'f', 0);
            }
            onValueModified: value => {
                row.entryPosition = value - 1;
                row.fieldEdited(row.entryIndex, row._f.position, value - 1);
            }
        }

        // SettingsSpinBox writes its own `value` on every step, which would
        // sever a plain `value:` binding after the first edit and leave the
        // spin deaf to the mirror. A Binding element keeps following it.
        Binding {
            target: positionSpin
            property: "value"
            value: row.entryPosition + 1
            when: !positionSpin.editing
            restoreMode: Binding.RestoreNone
        }

        Label {
            text: i18nc("@label shortcut that switches to the named workspace", "Focus shortcut")
        }

        ShortcutCaptureField {
            Layout.fillWidth: true
            accessibleName: i18n("Focus named workspace shortcut")
            placeholderText: i18nc("@info:placeholder no shortcut assigned", "None")
            requireModifier: true
            clearable: true
            checkConflicts: true
            conflictKey: row._shortcutKey(row._f.focusShortcut)
            keySequence: row.entryFocusShortcut
            onKeySequenceModified: seq => {
                row.entryFocusShortcut = seq;
                row.fieldEdited(row.entryIndex, row._f.focusShortcut, seq);
            }
        }

        Label {
            text: i18nc("@label shortcut that moves the active window to the named workspace", "Move window shortcut")
        }

        ShortcutCaptureField {
            Layout.fillWidth: true
            accessibleName: i18n("Move window to named workspace shortcut")
            placeholderText: i18nc("@info:placeholder no shortcut assigned", "None")
            requireModifier: true
            clearable: true
            checkConflicts: true
            conflictKey: row._shortcutKey(row._f.moveShortcut)
            keySequence: row.entryMoveShortcut
            onKeySequenceModified: seq => {
                row.entryMoveShortcut = seq;
                row.fieldEdited(row.entryIndex, row._f.moveShortcut, seq);
            }
        }
    }
}
