// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/**
 * @brief Dynamic workspaces, Behavior leaf.
 *
 * Card order: the KWin consent warning first, then the cap warning, then the
 * Behavior card. The feature's master enable is the sidebar toggle on the
 * Workspaces parent row (the placement-mode pattern), so this page
 * deliberately carries no duplicate enable switch; like the mode pages, its
 * rows stay editable while the feature is off.
 *
 * The consent warning shows while the feature is on but cannot run because
 * KWin's per-output virtual desktops setting is off and no consent to turn it
 * on is pending, which is when SettingsController::workspacesActive is false
 * with the feature enabled. That property follows the user's kwinrc while the
 * app runs, so the warning also returns when the KWin setting is turned off
 * later from System Settings.
 *
 * Consent is one-shot. "Turn On KWin Setting" sets the latch
 * (workspacesManageKWinPerOutput); like every other setting on this page it
 * reaches the daemon on Save, not on the click. The daemon then writes
 * kwinrc, reconfigures KWin and clears the latch again, so the latch is true
 * only while a write is still owed. A later switch-off in KWin is the user's
 * choice: the daemon goes dormant, never rewrites the setting on its own, and
 * this warning asks again. No restart is involved either way.
 *
 * The warning hides the moment the latch is set, which is BEFORE Save has
 * committed it. That is deliberate: a warning that stayed up would read as if
 * the consent had not registered.
 */
SettingsFlickable {
    id: root

    contentHeight: content.implicitHeight
    clip: true

    ColumnLayout {
        id: content

        width: parent.width
        spacing: Kirigami.Units.largeSpacing

        // No page-level info banner: the mode pages carry their explanation
        // in row descriptions, and this page does the same. The two
        // InlineMessages below are true WARNINGS, hidden in the normal state.
        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Warning
            text: i18n("Dynamic workspaces need KWin to switch virtual desktops independently on each monitor. PlasmaZones can turn on KWin's per-output virtual desktops setting when you save. It never turns the setting back off.")
            visible: appSettings.workspacesEnabled && !settingsController.workspacesActive

            actions: [
                Kirigami.Action {
                    text: i18n("Turn On KWin Setting")
                    icon.name: "dialog-ok-apply"
                    onTriggered: appSettings.workspacesManageKWinPerOutput = true
                }
            ]
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            type: Kirigami.MessageType.Warning
            text: i18n("Workspace limit reached. New workspaces cannot be added until one is removed.")
            visible: settingsController.workspacesActive && settingsController.workspacesAtCap
        }

        SettingsCard {
            Layout.fillWidth: true
            headerText: i18n("Behavior")
            searchAnchor: "workspacesBehavior"

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                SettingsRow {
                    title: i18n("Workspace hint")
                    searchAnchor: "workspacesSnapBackOsdHint"
                    description: i18n("Show a hint when a switch lands on a workspace that belongs to another monitor and is sent back.")

                    SettingsSwitch {
                        accessibleName: i18n("Show workspace hint")
                        checked: appSettings.workspacesSnapBackOsdHint
                        onToggled: function (newValue) {
                            appSettings.workspacesSnapBackOsdHint = newValue;
                        }
                    }
                }

                SettingsSeparator {}

                // The six actions are the daemon's kwinDesktopActions()
                // (daemon/workspaces.cpp); the text names them because the
                // user has to find them in System Settings to check or undo.
                SettingsRow {
                    title: i18n("Replace KWin desktop shortcuts")
                    searchAnchor: "workspacesRebindKWinShortcuts"
                    description: i18n("While dynamic workspaces are on, clear the shortcuts of six KWin actions that step through every monitor's desktops at once: Switch One Desktop Up, Switch One Desktop Down, Switch One Desktop to the Left, Switch One Desktop to the Right, Walk Through Desktops and Walk Through Desktops (Reverse). The workspace focus shortcuts use the Up and Down chords instead. KWin gets its shortcuts back when the feature is turned off.")

                    SettingsSwitch {
                        accessibleName: i18n("Replace KWin desktop shortcuts")
                        checked: appSettings.workspacesRebindKWinShortcuts
                        onToggled: function (newValue) {
                            appSettings.workspacesRebindKWinShortcuts = newValue;
                        }
                    }
                }
            }
        }
    }
}
