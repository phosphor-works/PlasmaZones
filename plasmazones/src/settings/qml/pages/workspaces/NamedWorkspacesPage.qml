// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/**
 * @brief Dynamic workspaces, Named Workspaces leaf.
 *
 * Card order: the cap warning (only while the daemon's desktop count sits at
 * the cap), the add form (a collapsible SettingsCard, initially collapsed once
 * entries exist, which is the profiles-page shape), then the reorderable
 * declaration list (drag-to-reorder rows expanding into their fields) with a
 * placeholder for the empty state.
 *
 * The declarations are ONE whole-replace config composite
 * (Workspaces.Named/Entries), so the page keeps a staged deep copy and every
 * edit commits the whole array. Like every other setting it reaches the
 * daemon on Save, which then creates, claims, unpins and rebinds the
 * per-workspace shortcuts with no restart. Order in the list is UI order;
 * each row's Position spin box edits the per-entry preferred slice index (-1,
 * shown as "Automatic", means before the trailing empty).
 *
 * Each entry carries a persistent id (namedEntryFields.id) that slot targets,
 * rule targets and the daemon follow through a rename. The page writes the
 * whole list through SettingsController::withWorkspaceEntryIds, which stamps
 * an id on any entry still lacking one and keeps every existing id.
 */
SettingsFlickable {
    id: root

    /// The config's field names inside an entry map.
    readonly property var _f: settingsController.namedEntryFields

    // Staged copy of appSettings.workspacesNamedEntries, in the config's own
    // entry shape (the six namedEntryFields).
    //
    // The list is keyed by the entry id (see _keyOf), never by index, and not
    // by name while an id exists, because `name` is mutable from inside the
    // row. ReorderableColumn keys its published-height cache, its per-row
    // expansion state and its focus restore by `idOf`, and a key that moved
    // on rename would strand all three.
    property var _entries: []

    // Bumped whenever any entry's name changes. A rename is applied IN PLACE
    // (see onFieldEdited), so `_entries` itself does not change identity and
    // nothing reading it through a binding would re-evaluate. The Add form's
    // refusal is the reader that matters.
    property int _namesTick: 0

    // The Add form's monitor pick, held by VALUE. An index would retarget to
    // whichever monitor slides into that row when one is plugged or unplugged
    // while the form is open.
    property string _addOutput: ""

    // Monitor options for the pickers: physical monitors only. The daemon
    // claims a named workspace on a whole monitor, collapsing a virtual
    // screen's id to its physical parent, so offering virtual screens would
    // offer pins that cannot mean what they say.
    readonly property var _screenOptions: {
        var options = [
            {
                "label": i18n("Any monitor"),
                "value": ""
            }
        ];
        var screens = settingsController.screens || [];
        var seen = {};
        for (var i = 0; i < screens.length; ++i) {
            var screen = screens[i];
            var physical = settingsController.physicalScreenId(screen.name || "");
            if (!physical || seen[physical])
                continue;
            seen[physical] = true;
            // A virtual screen's precomputed label names the virtual region,
            // so the physical entry rebuilt from it takes the monitor's own
            // vendor and model, the same rebuild DisplayMap applies.
            var label = screen.displayLabel || screen.name;
            if (screen.isVirtualScreen)
                label = [screen.manufacturer, screen.model].filter(Boolean).join(" ") || screen.connectorName || physical;
            options.push({
                "label": label,
                "value": physical
            });
        }
        return options;
    }

    /// The key a staged entry is listed under: its id, or its name while it
    /// has none (a hand-edited config the page has not saved yet).
    function _keyOf(entry) {
        if (!entry)
            return "";
        var id = entry[root._f.id] || "";
        return id !== "" ? id : "name:" + (entry[root._f.name] || "");
    }

    /// One entry in the config's shape, fields in a fixed order so two
    /// projections of the same list stringify identically.
    function _projected(entry) {
        var out = {};
        out[root._f.id] = entry[root._f.id] || "";
        out[root._f.name] = entry[root._f.name] || "";
        out[root._f.output] = entry[root._f.output] || "";
        out[root._f.position] = (entry[root._f.position] !== undefined && entry[root._f.position] !== null) ? entry[root._f.position] : -1;
        out[root._f.focusShortcut] = entry[root._f.focusShortcut] || "";
        out[root._f.moveShortcut] = entry[root._f.moveShortcut] || "";
        return out;
    }

    function _projectedList(entries) {
        var out = [];
        for (var i = 0; i < entries.length; ++i)
            out.push(root._projected(entries[i]));
        return out;
    }

    /// Rebuild the staged copy from the store when the two differ.
    ///
    /// Called on every store change and after every commit. An echo of our
    /// own commit matches the staged copy and changes nothing, so the row
    /// being edited keeps its delegate and its focus. Anything else is
    /// adopted: an entry the validator refused (a phantom Add or a dropped
    /// rename would otherwise linger as a row the store does not hold), and a
    /// list something else rewrote (a Discard, a profile switch, the daemon
    /// adopting a rename made in KWin).
    function _loadEntries() {
        var stored = root._projectedList(appSettings.workspacesNamedEntries || []);
        if (JSON.stringify(stored) === JSON.stringify(root._projectedList(root._entries)))
            return;
        root._entries = stored;
        root._namesTick++;
    }

    /// Write the staged list, with an id on every entry, then reconcile with
    /// what the store kept.
    function _commitEntries() {
        var wire = settingsController.withWorkspaceEntryIds(root._projectedList(root._entries));
        // Ids the write just stamped change those entries' list keys. Carry
        // each row's expansion over to its new key, then rebuild below so the
        // rows follow.
        var minted = false;
        for (var i = 0; i < wire.length && i < root._entries.length; ++i) {
            var newId = wire[i][root._f.id];
            if (root._entries[i][root._f.id] === newId)
                continue;
            var oldKey = root._keyOf(root._entries[i]);
            if (entryList.isRowExpanded(oldKey))
                entryList.setRowExpanded(newId, true);
            root._entries[i][root._f.id] = newId;
            minted = true;
        }
        appSettings.workspacesNamedEntries = wire;
        if (minted) {
            root._entries = root._projectedList(appSettings.workspacesNamedEntries || []);
            root._namesTick++;
            return;
        }
        // A write the store reduced to what it already held (an entry the
        // validator refused) emits no change signal, so the reconcile cannot
        // wait for one.
        root._loadEntries();
    }

    /// Every declared name, in list order. Names are canonical (trimmed) and
    /// compared exactly, which is the daemon's own identity rule: the config
    /// schema canonicalizes a name before storing it and the reconciler
    /// matches a declaration's name with plain string equality, so "Work" and
    /// "work" are two distinct workspaces.
    readonly property var _names: {
        void root._namesTick;
        var names = [];
        for (var i = 0; i < root._entries.length; ++i)
            names.push(root._entries[i][root._f.name] || "");
        return names;
    }

    /// Judge `raw` as the name of the entry at `index` (-1 for the Add form,
    /// which has no row of its own to exclude from the duplicate check).
    /// Returns `{refusal, name}`: why the text is refused (empty when it is
    /// not) and the canonical name it would be stored as.
    ///
    /// One rule in one place for both the Add form and a row's rename, and
    /// the schema's own: SettingsController::checkWorkspaceName runs the check
    /// the validator applies, and an entry it refuses is dropped on the next
    /// round-trip, which looks to the user like an edit that was simply lost.
    function _checkName(raw, index) {
        var check = settingsController.checkWorkspaceName(raw);
        if (!check.accepted) {
            if (check.verdict === "tooLong")
                return {
                    "refusal": i18np("A workspace name can be at most %n character long.", "A workspace name can be at most %n characters long.", settingsController.workspaceNameMaxLength),
                    "name": ""
                };
            if (check.verdict === "forbiddenCharacter")
                return {
                    "refusal": i18n("A workspace name cannot contain control characters."),
                    "name": ""
                };
            return {
                "refusal": i18n("A workspace needs a name."),
                "name": ""
            };
        }
        var names = root._names;
        for (var i = 0; i < names.length; ++i) {
            if (i !== index && names[i] === check.name)
                return {
                    "refusal": i18n("Another workspace already uses that name."),
                    "name": ""
                };
        }
        return {
            "refusal": "",
            "name": check.name
        };
    }

    /// Why the Add form cannot add right now, or empty.
    readonly property string _addRefusal: {
        void root._namesTick;
        if (root._entries.length >= settingsController.workspaceNamedEntryMaxCount)
            return i18np("You can add at most one named workspace.", "You can add at most %n named workspaces.", settingsController.workspaceNamedEntryMaxCount);
        return root._checkName(addNameField.text, -1).refusal;
    }

    Component.onCompleted: _loadEntries()

    Connections {
        function onWorkspacesNamedEntriesChanged() {
            root._loadEntries();
        }

        target: appSettings
    }

    contentHeight: content.implicitHeight
    clip: true

    ColumnLayout {
        id: content

        width: parent.width
        spacing: Kirigami.Units.largeSpacing

        // At the cap the daemon creates no new desktop, so a declaration
        // added now is stored but stays unrealized until room frees up.
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Warning
            visible: settingsController.workspacesActive && settingsController.workspacesAtCap
            text: i18n("Workspace limit reached. A named workspace you add now is kept, and it is created once another workspace is removed.")
        }

        SettingsCard {
            id: addCard

            Layout.fillWidth: true
            headerText: i18n("Add named workspace")
            searchAnchor: "workspacesNamedAdd"
            collapsible: true
            // Read off the STORE, not the staged copy: SettingsCard adopts
            // this one-shot in its own Component.onCompleted, which runs
            // before the page's, so the staged array is still empty then and
            // the form would always start expanded.
            initiallyCollapsed: appSettings.workspacesNamedEntries.length > 0

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing
                    Layout.rightMargin: Kirigami.Units.largeSpacing
                    Layout.topMargin: Kirigami.Units.smallSpacing
                    wrapMode: Text.WordWrap
                    opacity: 0.6
                    text: i18n("A named workspace is created when you save, keeps its place while empty, and can be pinned to a monitor. Shortcuts jump to it or send the active window there.")
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing
                    Layout.rightMargin: Kirigami.Units.largeSpacing
                    spacing: Kirigami.Units.largeSpacing

                    TextField {
                        id: addNameField

                        Layout.fillWidth: true
                        placeholderText: i18n("Workspace name")
                        maximumLength: settingsController.workspaceNameMaxLength
                        Accessible.name: i18n("New workspace name")
                        Accessible.description: root._addRefusal
                        // Enter adds, so a keyboard-only user does not have to
                        // tab past the monitor combo to reach the button.
                        onAccepted: {
                            if (addButton.enabled)
                                addButton.clicked();
                        }
                    }

                    WideComboBox {
                        id: addOutputCombo

                        textRole: "label"
                        valueRole: "value"
                        Accessible.name: i18n("Pinned monitor")
                        model: root._screenOptions
                        storedValue: root._addOutput
                        onActivated: root._addOutput = currentValue
                    }
                }

                // Why Add is unavailable, in words, so a disabled button is
                // never the only answer. Held back while the field is empty
                // and the count allows more, where "needs a name" would only
                // scold an untouched form.
                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing
                    Layout.rightMargin: Kirigami.Units.largeSpacing
                    visible: root._addRefusal !== "" && (addNameField.text.length > 0 || root._entries.length >= settingsController.workspaceNamedEntryMaxCount)
                    text: root._addRefusal
                    wrapMode: Text.WordWrap
                    font: Kirigami.Theme.smallFont
                    color: Kirigami.Theme.negativeTextColor
                }

                Button {
                    id: addButton

                    Layout.alignment: Qt.AlignRight
                    Layout.rightMargin: Kirigami.Units.largeSpacing
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                    text: i18n("Add")
                    icon.name: "list-add"
                    Accessible.name: i18n("Add named workspace")
                    // The schema's whole rule plus the duplicate and count
                    // checks: an entry the validator refuses is dropped on the
                    // round-trip, which would look like the Add had simply
                    // failed.
                    enabled: root._addRefusal === ""
                    onClicked: {
                        var check = root._checkName(addNameField.text, -1);
                        if (check.refusal !== "" || root._addRefusal !== "")
                            return;
                        var entry = {};
                        entry[root._f.id] = settingsController.newWorkspaceEntryId();
                        entry[root._f.name] = check.name;
                        entry[root._f.output] = addOutputCombo.indexOfValue(root._addOutput) >= 0 ? root._addOutput : "";
                        entry[root._f.position] = -1;
                        entry[root._f.focusShortcut] = "";
                        entry[root._f.moveShortcut] = "";
                        var arr = root._entries.slice();
                        arr.push(entry);
                        root._entries = arr;
                        root._namesTick++;
                        root._commitEntries();
                        addNameField.clear();
                        root._addOutput = "";
                    }
                }
            }
        }

        SettingsCard {
            Layout.fillWidth: true
            headerText: i18n("Named workspaces")
            headerTrailingText: root._entries.length > 0 ? Number(root._entries.length).toLocaleString(Qt.locale(), 'f', 0) : ""
            searchAnchor: "workspacesNamedList"

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                Kirigami.PlaceholderMessage {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.gridUnit
                    visible: root._entries.length === 0
                    icon.name: "virtual-desktops"
                    text: i18n("No named workspaces")
                    explanation: i18n("Named workspaces you add appear here.")
                }

                ReorderableColumn {
                    id: entryList

                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.smallSpacing
                    Layout.rightMargin: Kirigami.Units.smallSpacing
                    Layout.bottomMargin: Kirigami.Units.smallSpacing
                    Layout.preferredHeight: totalHeight
                    // The collapsed row is a two-line header (name over
                    // monitor); the default grip-column height (4 gridUnits)
                    // would pad it with dead space (the ChainEditor
                    // precedent, same value).
                    headerRowHeight: Kirigami.Units.gridUnit * 3
                    visible: root._entries.length > 0
                    items: root._entries
                    // Keyed by the persistent entry id, never by the name: see
                    // the `_entries` comment for why a mutable key corrupts
                    // the height cache, the expansion state and the focus
                    // restore.
                    //
                    // Null-guarded like ReorderableColumn's own defaults:
                    // during a model reset the delegate's modelData detaches
                    // before its destruction handler runs, and both resolvers
                    // are called from bindings that re-evaluate in that window.
                    idOf: function (item) {
                        return root._keyOf(item);
                    }
                    accessibleNameOf: function (item) {
                        return (item && item[root._f.name]) ? item[root._f.name] : i18n("(unnamed)");
                    }
                    reorderableOf: function (item) {
                        return true;
                    }
                    onMoveRequested: function (fromIndex, toIndex) {
                        var arr = root._entries.slice();
                        if (fromIndex < 0 || fromIndex >= arr.length || toIndex < 0 || toIndex >= arr.length)
                            return;

                        arr.splice(toIndex, 0, arr.splice(fromIndex, 1)[0]);
                        root._entries = arr;
                        root._commitEntries();
                    }

                    rowDelegate: NamedWorkspaceRow {
                        entry: parent.rowModelData
                        entryIndex: parent.rowIndex
                        screenOptions: root._screenOptions
                        // Read fresh at commit time rather than handed a plain
                        // array: sibling names change in place across renames,
                        // so a snapshot property would go stale.
                        checkName: function (raw, index) {
                            return root._checkName(raw, index);
                        }
                        // Restore the expansion the row had before the last
                        // rebuild (a reorder, a remove, a reload). Seeded
                        // imperatively, never bound: ExpandableRowDelegate
                        // toggles `expanded` by assignment, which would sever
                        // a binding on the first click (RuleSectionList's
                        // shape).
                        Component.onCompleted: expanded = parent.rowExpanded
                        onExpandedChanged: {
                            if (parent)
                                parent.setRowExpanded(expanded);
                        }
                        onFieldEdited: function (index, field, value) {
                            // In place, deliberately: reassigning _entries
                            // would rebuild every row delegate and drop the
                            // focus of the row mid-edit. The row mirrors its
                            // fields itself; structural ops (add / remove /
                            // reorder) still replace the array.
                            if (index < 0 || index >= root._entries.length)
                                return;

                            root._entries[index][field] = value;
                            if (field === root._f.name)
                                root._namesTick++;
                            root._commitEntries();
                        }
                        onRemoveRequested: function (index) {
                            if (index < 0 || index >= root._entries.length)
                                return;
                            var arr = root._entries.slice();
                            // Keep keyboard focus in the list: on the row that
                            // takes the removed one's place, else the one
                            // before it, else (the list is now empty) on the
                            // Add form's name field.
                            var neighbour = index + 1 < arr.length ? arr[index + 1] : (index > 0 ? arr[index - 1] : null);
                            arr.splice(index, 1);
                            entryList.pendingFocusId = neighbour ? root._keyOf(neighbour) : "";
                            root._entries = arr;
                            root._namesTick++;
                            root._commitEntries();
                            if (!neighbour) {
                                addCard.collapsed = false;
                                Qt.callLater(function () {
                                    addNameField.forceActiveFocus(Qt.OtherFocusReason);
                                });
                            }
                        }
                    }
                }
            }
        }
    }
}
