// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-FileCopyrightText: 2026 MaslovKK
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

/**
 * @brief The tiling Window Handling card, shared between the advanced
 * Tiling → Window page and the simple-mode Tiling page. All rows bind the
 * appSettings context property, so the card carries no per-page state.
 */
SettingsCard {
    headerText: i18n("Window handling")
    searchAnchor: "windowHandling"
    collapsible: true

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        SettingsRow {
            title: i18n("New window placement")
            searchAnchor: "newWindowPlacement"
            description: i18n("Where newly opened windows appear in the tiling order")

            WideComboBox {
                Accessible.name: i18n("New window placement")
                textRole: "text"
                valueRole: "value"
                model: settingsController.valueOptions("Tiling.Behavior", "InsertPosition")
                storedValue: appSettings.autotileInsertPosition
                onActivated: appSettings.autotileInsertPosition = currentValue
            }
        }

        SettingsSeparator {}

        SettingsRow {
            title: i18n("Respect minimum size")
            searchAnchor: "respectMinimumSize"
            description: i18n("Prevent windows from being resized below their minimum, which may leave gaps")

            SettingsSwitch {
                checked: appSettings.autotileRespectMinimumSize
                accessibleName: i18n("Respect window minimum size")
                onToggled: function (newValue) {
                    appSettings.autotileRespectMinimumSize = newValue;
                }
            }
        }

        SettingsSeparator {}

        // Smart gaps is tiling-only and is a plain Setting (not part of
        // the rule-backed shared gap model on the Window Appearance page),
        // so it lives here with the tiling window-handling knobs.
        SettingsRow {
            title: i18n("Smart gaps")
            searchAnchor: "smartGaps"
            description: i18n("Remove all gaps when only one window is tiled")

            SettingsSwitch {
                checked: appSettings.autotileSmartGaps
                accessibleName: i18n("Smart gaps")
                onToggled: function (newValue) {
                    appSettings.autotileSmartGaps = newValue;
                }
            }
        }

        SettingsSeparator {}

        SettingsRow {
            title: i18n("Restore untiled windows to their previous position and size")
            searchAnchor: "restoreUntiledWindowsPosition"
            description: i18n("When an untiled (floated) window reopens, during the session or after a logout, it returns to the position, size and monitor it was last on instead of wherever the compositor would place it and whatever size the app asked for. A rule can override this either way, opting individual windows in or out.")

            SettingsSwitch {
                checked: appSettings.autotileRestoreFloatedWindowsOnLogin
                accessibleName: i18n("Restore untiled windows to their previous position and size")
                onToggled: function (newValue) {
                    appSettings.autotileRestoreFloatedWindowsOnLogin = newValue;
                }
            }
        }

        SettingsSeparator {}

        SettingsRow {
            title: i18n("Keep floating windows above")
            searchAnchor: "keepFloatingAbove"
            description: i18n("Keep the windows you float stacked above the tiled windows. A rule that sets a window layer takes precedence for the windows it matches.")

            SettingsSwitch {
                checked: appSettings.autotileKeepFloatingAbove
                accessibleName: i18n("Keep floating windows above")
                onToggled: function (newValue) {
                    appSettings.autotileKeepFloatingAbove = newValue;
                }
            }
        }

        SettingsSeparator {}

        SettingsRow {
            title: i18n("Sticky windows")
            searchAnchor: "stickyWindows"
            description: i18n("How to handle windows that appear on all desktops")

            WideComboBox {
                Accessible.name: i18n("Sticky windows")
                textRole: "text"
                valueRole: "value"
                model: settingsController.valueOptions("Tiling.Behavior", "StickyWindowHandling")
                storedValue: appSettings.autotileStickyWindowHandling
                onActivated: appSettings.autotileStickyWindowHandling = currentValue
            }
        }

        SettingsSeparator {}

        SettingsRow {
            title: i18n("Drag behavior")
            searchAnchor: "dragBehavior"
            description: i18n("Float converts a dragged tile to free-floating. Reorder keeps it tiled and swaps it into the drop slot.")

            WideComboBox {
                Accessible.name: i18n("Autotile drag behavior")
                Accessible.description: i18n("Selects how dragging a tiled window on an autotile screen behaves. Float converts it to free-floating and Reorder keeps it tiled, swapping it into the drop slot.")
                textRole: "text"
                valueRole: "value"
                model: settingsController.valueOptions("Tiling.Behavior", "DragBehavior")
                storedValue: appSettings.autotileDragBehavior
                onActivated: appSettings.autotileDragBehavior = currentValue
            }
        }

        SettingsSeparator {}

        SettingsRow {
            title: i18n("Overflow behavior")
            searchAnchor: "overflowBehavior"
            description: i18n("Float leaves windows beyond the max-windows cap floating. Unlimited tiles every window regardless of count.")

            WideComboBox {
                Accessible.name: i18n("Autotile overflow behavior")
                Accessible.description: i18n("Selects how windows beyond the max-windows cap are handled. Float leaves the excess windows floating and Unlimited tiles every window regardless of count.")
                textRole: "text"
                valueRole: "value"
                model: settingsController.valueOptions("Tiling.Behavior", "OverflowBehavior")
                storedValue: appSettings.autotileOverflowBehavior
                onActivated: appSettings.autotileOverflowBehavior = currentValue
            }
        }
    }
}
