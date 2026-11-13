// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import org.kde.kirigami as Kirigami

/**
 * @brief The routing-target pickers for ActionRow, split out of
 *        ActionParamEditors.
 *
 * The three actions that route a window somewhere (RouteToScreen,
 * RouteToDesktop, RouteToWorkspace) each need a combo showing friendly labels
 * over a wire value the daemon resolves, and all three keep a stored value
 * legible once it no longer matches anything live. They live in their own
 * file beside ActionParamEditors; the split is a home for the Components,
 * not a behavioural boundary.
 *
 * Each Component reads `row.*` (the ActionRow instance, injected below) and
 * `parent.modelData` (the hosting Loader's param descriptor, reached at
 * runtime), the same contract as the editors in ActionParamEditors. ActionRow
 * resolves these through `paramEditors.pickers._…Editor`.
 */
QtObject {
    id: pickers

    /// The ActionRow these pickers belong to. Named `row` for the same reason
    /// ActionParamEditors names it that: the moved Component bodies resolve
    /// `row.action` / `row.actionEdited` / `row._withParam` against it exactly
    /// as they did before, with no rewrites.
    required property var row

    // Monitor picker for RouteToScreen. Mirrors the ScreenId match-condition
    // editor (MatchLeafEditor's screenValueEditor): the user picks a friendly
    // label while the wire value stays the canonical screen id. A stored id whose
    // monitor is offline still surfaces (so the user sees what the rule pins to).
    property Component _screenIdEditor: Component {
        WideComboBox {
            id: screenCombo

            readonly property var _param: parent.modelData
            readonly property var _screens: (row.appSettings && row.appSettings.screens) || []
            model: _screens.map(function (s) {
                var label = s.displayLabel || s.name || "";
                if (s.isPrimary)
                    // Composed inside one i18nc so translators control the
                    // order and the separator survives RTL bidi runs.
                    label = i18nc("monitor name, then the primary-monitor marker", "%1 · %2", label, i18n("Primary"));
                return {
                    "label": label,
                    "name": s.name
                };
            })
            textRole: "label"
            valueRole: "name"
            currentIndex: {
                // Trimmed like the daemon reads it, so a hand-edited id with
                // stray whitespace still finds its monitor.
                var target = ("" + (row.action[_param.key] || "")).trim();
                for (var i = 0; i < screenCombo._screens.length; ++i) {
                    if (screenCombo._screens[i].name === target)
                        return i;
                }
                return -1;
            }
            displayText: currentIndex >= 0 ? currentText : (row.action[_param.key] || i18n("Choose a monitor…"))
            Accessible.name: _param.label
            onActivated: function (index) {
                if (currentValue !== row.action[_param.key])
                    row.actionEdited(row._withParam(_param.key, currentValue));
            }
        }
    }

    // Virtual-desktop picker for RouteToDesktop. Lists 1..virtualDesktopCount,
    // labelled with the desktop name when KWin reports one. The wire value is the
    // 1-based desktop number. A stored desktop beyond the current count still
    // surfaces its number so the rule's target stays legible. While dynamic
    // workspaces run, desktop numbers shift as workspaces come and go, so the
    // picker says so and points at the workspace action instead.
    property Component _virtualDesktopEditor: Component {
        WideComboBox {
            id: desktopCombo

            readonly property var _param: parent.modelData
            readonly property int _count: row.appSettings && row.appSettings.virtualDesktopCount > 0 ? row.appSettings.virtualDesktopCount : 1
            readonly property var _names: (row.appSettings && row.appSettings.virtualDesktopNames) || []
            model: {
                var items = [];
                for (var i = 1; i <= desktopCombo._count; ++i) {
                    var name = desktopCombo._names.length >= i ? desktopCombo._names[i - 1] : "";
                    items.push({
                        // Composed inside one i18nc so translators control the
                        // order and the separator, matching the monitor picker
                        // above and the match-side desktop picker.
                        "label": name && name.length > 0 ? i18nc("virtual desktop number, then its name", "%1: %2", i, name) : ("" + i),
                        "value": i
                    });
                }
                return items;
            }
            textRole: "label"
            valueRole: "value"
            currentIndex: {
                var target = Number(row.action[_param.key]);
                for (var i = 0; i < desktopCombo.model.length; ++i) {
                    if (desktopCombo.model[i].value === target)
                        return i;
                }
                return -1;
            }
            displayText: currentIndex >= 0 ? currentText : (row.action[_param.key] ? ("" + row.action[_param.key]) : i18n("Choose a desktop…"))
            Accessible.name: _param.label
            readonly property bool _numbersShift: !!row.appSettings && row.appSettings.workspacesActive === true
            readonly property string _shiftHint: i18n("Dynamic workspaces are on, so desktop numbers change as workspaces are added and removed. To send a window to a particular workspace, use the Open on workspace action.")
            Accessible.description: desktopCombo._numbersShift ? desktopCombo._shiftHint : ""
            ToolTip.visible: hovered && desktopCombo._numbersShift
            ToolTip.delay: Kirigami.Units.toolTipDelay
            ToolTip.text: desktopCombo._shiftHint
            onActivated: function (index) {
                if (currentValue !== row.action[_param.key])
                    row.actionEdited(row._withParam(_param.key, currentValue));
            }
        }
    }

    // Named-workspace picker for RouteToWorkspace. Lists the DECLARED
    // workspaces (Workspaces → Named Workspaces). A pick writes BOTH the
    // entry's persistent id (targetWorkspaceId) and its name
    // (targetWorkspaceName, the descriptor's key): the daemon resolves the id
    // first, so the rule follows a rename, and falls back to the name, which
    // is also what the rule shows while no declaration carries the id. An
    // entry without an id yet (a hand-edited config) is written by name
    // alone. A stored target no longer declared stays legible and is marked
    // as such: the rule is dormant, not broken, and not live either.
    property Component _workspaceNameEditor: Component {
        WideComboBox {
            id: workspaceCombo

            readonly property var _param: parent.modelData
            // PhosphorRules::ActionParam::TargetWorkspaceId. The id has no
            // param descriptor of its own (nobody types it), so its key is
            // named here, beside the one picker that writes it.
            readonly property string _idKey: "targetWorkspaceId"
            readonly property var _fields: (row.appSettings && row.appSettings.namedEntryFields) || ({
                    "id": "id",
                    "name": "name"
                })
            readonly property var _entries: (row.appSettings && row.appSettings.namedWorkspaces) || []
            readonly property string _storedId: ("" + (row.action[workspaceCombo._idKey] || "")).trim()
            readonly property string _storedName: ("" + (row.action[_param.key] || "")).trim()

            // Ids compare the way the daemon compares them (QUuid equality),
            // so a hand-edited id in another case or without braces matches.
            function _normId(id) {
                return ("" + (id || "")).trim().replace(/^\{|\}$/g, "").toLowerCase();
            }

            model: {
                var items = [];
                for (var i = 0; i < workspaceCombo._entries.length; ++i) {
                    var entry = workspaceCombo._entries[i];
                    items.push({
                        "label": entry[workspaceCombo._fields.name],
                        "id": entry[workspaceCombo._fields.id] || "",
                        "name": entry[workspaceCombo._fields.name]
                    });
                }
                return items;
            }
            textRole: "label"
            valueRole: "name"
            // The daemon's resolution order: the stored id, then the stored
            // name. -1 leaves the target dormant.
            currentIndex: {
                var wantId = workspaceCombo._normId(workspaceCombo._storedId);
                if (wantId !== "") {
                    for (var i = 0; i < workspaceCombo.model.length; ++i) {
                        if (workspaceCombo._normId(workspaceCombo.model[i].id) === wantId)
                            return i;
                    }
                }
                for (var j = 0; j < workspaceCombo.model.length; ++j) {
                    if (workspaceCombo.model[j].name === workspaceCombo._storedName)
                        return j;
                }
                return -1;
            }

            // Why the action cannot take effect right now, most fundamental
            // first, or empty when it can. With the feature compiled out, off,
            // or waiting for KWin's per-output setting the daemon installs no
            // route resolver, so the rule is inert whatever it names, and
            // pointing the user at the Named Workspaces page would send them
            // somewhere that cannot help.
            readonly property string _inertReason: {
                if (!row.appSettings)
                    return "";
                if (row.appSettings.workspacesAvailable === false)
                    return i18n("This build of PlasmaZones has no dynamic workspaces, so this action does nothing.");
                if (row.appSettings.workspacesEnabled === false)
                    return i18n("Dynamic workspaces are turned off, so this action does nothing. Turn them on under Settings → Workspaces.");
                if (row.appSettings.workspacesActive === false)
                    return i18n("Dynamic workspaces are waiting for KWin's per-output virtual desktops setting, so this action does nothing yet. See Settings → Workspaces → Behavior.");
                if (workspaceCombo._entries.length === 0)
                    return i18n("Add a named workspace under Settings → Workspaces → Named Workspaces first.");
                if (workspaceCombo.currentIndex < 0 && workspaceCombo._storedName !== "")
                    return i18n("No named workspace matches this target any more, so the rule does nothing until one is declared again.");
                return "";
            }

            // The combo stays ENABLED with an empty model all the same:
            // disabling it puts an already-stored target out of reach, and a
            // disabled QQC2 Control receives no hover events, so the tooltip
            // that says what is wrong would never appear. The display text
            // carries the same guidance for anyone who does not hover.
            ToolTip.visible: hovered && workspaceCombo._inertReason !== ""
            ToolTip.delay: Kirigami.Units.toolTipDelay
            ToolTip.text: workspaceCombo._inertReason
            Accessible.description: workspaceCombo._inertReason
            displayText: {
                if (currentIndex >= 0)
                    return currentText;
                if (workspaceCombo._storedName !== "")
                    return i18nc("%1 is the name of a workspace that is no longer declared", "%1 (not declared)", workspaceCombo._storedName);
                if (row.appSettings && (row.appSettings.workspacesAvailable === false || row.appSettings.workspacesEnabled === false))
                    return i18n("Workspaces are turned off");
                if (workspaceCombo._entries.length === 0)
                    return i18n("Add a named workspace first");
                return i18n("Choose a workspace…");
            }
            Accessible.name: _param.label
            onActivated: function (index) {
                var picked = workspaceCombo.model[index];
                if (!picked)
                    return;
                if (picked.name === workspaceCombo._storedName && workspaceCombo._normId(picked.id) === workspaceCombo._normId(workspaceCombo._storedId))
                    return;
                var next = row._withParam(_param.key, picked.name);
                if (picked.id !== "")
                    next[workspaceCombo._idKey] = picked.id;
                else
                    delete next[workspaceCombo._idKey];
                row.actionEdited(next);
            }
        }
    }
}
