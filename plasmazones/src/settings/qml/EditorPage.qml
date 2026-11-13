// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.phosphor.control

SettingsFlickable {
    id: root

    // PageHost sets both before the page's bindings run, because this page's
    // registry entry passes context (PageRegistry::Entry::passesContext).
    // `controller` is this page's EditorPageController, the Q_PROPERTY surface
    // over the Editor.* settings. settingsApp is declared even though this
    // page does not read it: PageHost passes it to every such page, and
    // assigning it to a page that lacks the property logs an error on every
    // build.
    required property QtObject controller
    required property ApplicationController settingsApp

    contentHeight: content.implicitHeight
    clip: true

    ColumnLayout {
        id: content

        width: parent.width
        spacing: Kirigami.Units.largeSpacing

        // =================================================================
        // KEYBOARD SHORTCUTS CARD
        // =================================================================
        Item {
            Layout.fillWidth: true
            implicitHeight: shortcutsCard.implicitHeight

            SettingsCard {
                id: shortcutsCard

                anchors.fill: parent
                headerText: i18n("Keyboard Shortcuts")
                searchAnchor: "editorShortcuts"
                collapsible: true

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    SettingsRow {
                        title: i18n("Duplicate zone")
                        searchAnchor: "editorDuplicateShortcut"
                        description: i18n("Clone the currently selected zone")

                        ShortcutCaptureField {
                            accessibleName: i18n("Duplicate zone")
                            keySequence: root.controller.duplicateShortcut
                            placeholderText: root.controller.defaultDuplicateShortcut
                            onKeySequenceModified: seq => {
                                root.controller.duplicateShortcut = seq;
                            }
                        }
                    }

                    SettingsSeparator {}

                    SettingsRow {
                        title: i18n("Split horizontally")
                        searchAnchor: "editorSplitHorizontalShortcut"
                        description: i18n("Divide selected zone into left and right halves")

                        ShortcutCaptureField {
                            accessibleName: i18n("Split horizontally")
                            keySequence: root.controller.splitHorizontalShortcut
                            placeholderText: root.controller.defaultSplitHorizontalShortcut
                            onKeySequenceModified: seq => {
                                root.controller.splitHorizontalShortcut = seq;
                            }
                        }
                    }

                    SettingsSeparator {}

                    SettingsRow {
                        title: i18n("Split vertically")
                        searchAnchor: "editorSplitVerticalShortcut"
                        description: i18n("Divide selected zone into top and bottom halves")

                        ShortcutCaptureField {
                            accessibleName: i18n("Split vertically")
                            keySequence: root.controller.splitVerticalShortcut
                            placeholderText: root.controller.defaultSplitVerticalShortcut
                            onKeySequenceModified: seq => {
                                root.controller.splitVerticalShortcut = seq;
                            }
                        }
                    }

                    SettingsSeparator {}

                    SettingsRow {
                        title: i18n("Fill space")
                        searchAnchor: "editorFillShortcut"
                        description: i18n("Expand selected zone to fill surrounding empty area")

                        ShortcutCaptureField {
                            accessibleName: i18n("Fill space")
                            keySequence: root.controller.fillShortcut
                            placeholderText: root.controller.defaultFillShortcut
                            onKeySequenceModified: seq => {
                                root.controller.fillShortcut = seq;
                            }
                        }
                    }

                    SettingsSeparator {}

                    SettingsRow {
                        title: i18n("Reset shortcuts")
                        searchAnchor: "editorResetShortcuts"
                        description: i18n("Restore all editor shortcuts to their default values")

                        Button {
                            text: i18n("Reset to defaults")
                            icon.name: "edit-reset"
                            onClicked: root.controller.resetShortcutDefaults()
                        }
                    }
                }
            }
        }

        // =================================================================
        // SNAPPING CARD
        // =================================================================
        Item {
            Layout.fillWidth: true
            implicitHeight: snappingCard.implicitHeight

            SettingsCard {
                id: snappingCard

                anchors.fill: parent
                headerText: i18n("Snapping")
                searchAnchor: "editorSnapping"
                collapsible: true

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    // ── Snap Behavior ──────────────────────────────────────
                    SettingsRow {
                        title: i18n("Grid snapping")
                        searchAnchor: "editorGridSnapping"
                        description: i18n("Snap zones to a grid while dragging or resizing")

                        SettingsSwitch {
                            checked: root.controller.gridSnappingEnabled
                            accessibleName: i18n("Enable grid snapping")
                            onToggled: function (newValue) {
                                root.controller.gridSnappingEnabled = newValue;
                            }
                        }
                    }

                    SettingsSeparator {}

                    SettingsRow {
                        title: i18n("Edge snapping")
                        searchAnchor: "editorEdgeSnapping"
                        description: i18n("Snap zones to edges of neighboring zones")

                        SettingsSwitch {
                            checked: root.controller.edgeSnappingEnabled
                            accessibleName: i18n("Enable edge snapping")
                            onToggled: function (newValue) {
                                root.controller.edgeSnappingEnabled = newValue;
                            }
                        }
                    }

                    SettingsSeparator {}

                    // ── Grid Intervals ─────────────────────────────────────
                    SettingsRow {
                        title: i18n("Grid interval X")
                        searchAnchor: "editorSnapIntervalX"
                        description: i18n("Horizontal grid spacing as percentage of screen width")

                        SettingsSlider {
                            accessibleName: i18n("Grid interval X")
                            // Percent units, not the stored fraction: a step of
                            // 1/100 lands on values like 0.35000000000000003,
                            // which never compares equal to a saved 0.35.
                            from: Math.round(root.controller.snapIntervalMin * 100)
                            to: Math.round(root.controller.snapIntervalMax * 100)
                            stepSize: 1
                            tickMarkStepSize: 10
                            value: Math.round(root.controller.snapIntervalX * 100)
                            formatValue: function (v) {
                                return i18nc("a whole-number percentage", "%1%", Math.round(v));
                            }
                            onMoved: value => {
                                root.controller.snapIntervalX = Math.round(value) / 100;
                            }
                        }
                    }

                    SettingsSeparator {}

                    SettingsRow {
                        title: i18n("Grid interval Y")
                        searchAnchor: "editorSnapIntervalY"
                        description: i18n("Vertical grid spacing as percentage of screen height")

                        SettingsSlider {
                            accessibleName: i18n("Grid interval Y")
                            // Percent units, not the stored fraction: a step of
                            // 1/100 lands on values like 0.35000000000000003,
                            // which never compares equal to a saved 0.35.
                            from: Math.round(root.controller.snapIntervalMin * 100)
                            to: Math.round(root.controller.snapIntervalMax * 100)
                            stepSize: 1
                            tickMarkStepSize: 10
                            value: Math.round(root.controller.snapIntervalY * 100)
                            formatValue: function (v) {
                                return i18nc("a whole-number percentage", "%1%", Math.round(v));
                            }
                            onMoved: value => {
                                root.controller.snapIntervalY = Math.round(value) / 100;
                            }
                        }
                    }

                    SettingsSeparator {}

                    // ── Modifier ───────────────────────────────────────────
                    SettingsRow {
                        title: i18n("Override modifier")
                        searchAnchor: "editorSnapOverrideModifier"
                        description: i18n("Hold this key to temporarily bypass snap behavior")

                        ModifierComboBox {
                            accessibleName: i18n("Override modifier")
                            modifierValue: root.controller.snapOverrideModifier
                            onModifierSelected: value => {
                                root.controller.snapOverrideModifier = value;
                            }
                        }
                    }
                }
            }
        }

        // =================================================================
        // FILL ON DROP CARD
        // =================================================================
        Item {
            Layout.fillWidth: true
            implicitHeight: fillOnDropCard.implicitHeight

            SettingsCard {
                id: fillOnDropCard

                anchors.fill: parent
                headerText: i18n("Fill on Drop")
                searchAnchor: "editorFillOnDrop"
                showToggle: true
                toggleChecked: root.controller.fillOnDropEnabled
                onToggleClicked: checked => {
                    root.controller.fillOnDropEnabled = checked;
                }

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    SettingsRow {
                        title: i18n("Modifier key")
                        searchAnchor: "editorFillOnDropModifier"
                        description: i18n("Hold this key while dropping a zone to expand it into available space")

                        ModifierComboBox {
                            accessibleName: i18n("Fill on drop modifier key")
                            modifierValue: root.controller.fillOnDropModifier
                            onModifierSelected: value => {
                                root.controller.fillOnDropModifier = value;
                            }
                        }
                    }
                }
            }
        }

        // =================================================================
        // APPEARANCE CARD
        // =================================================================
        Item {
            Layout.fillWidth: true
            implicitHeight: appearanceCard.implicitHeight

            SettingsCard {
                id: appearanceCard

                anchors.fill: parent
                headerText: i18n("Appearance")
                searchAnchor: "editorAppearance"
                collapsible: true

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    SettingsRow {
                        title: i18n("Background opacity")
                        searchAnchor: "editorBackgroundOpacity"
                        description: i18n("Lower values let more of the desktop show through behind the editor")

                        SettingsSlider {
                            accessibleName: i18n("Background opacity")
                            // Percent units for the same reason as the grid
                            // intervals, and it matters more here: the default,
                            // 0.7, is one of the fractions a 0.01 step misses.
                            from: Math.round(root.controller.backgroundOpacityMin * 100)
                            to: Math.round(root.controller.backgroundOpacityMax * 100)
                            stepSize: 1
                            tickMarkStepSize: 10
                            value: Math.round(root.controller.backgroundOpacity * 100)
                            formatValue: function (v) {
                                return i18nc("a whole-number percentage", "%1%", Math.round(v));
                            }
                            onMoved: value => {
                                root.controller.backgroundOpacity = Math.round(value) / 100;
                            }
                        }
                    }
                }
            }
        }
    }
}
