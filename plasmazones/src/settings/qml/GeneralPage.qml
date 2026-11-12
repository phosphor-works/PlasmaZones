// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.phosphor.control

SettingsFlickable {
    id: root

    // PageHost sets both before the page's bindings run, because this page's
    // registry entry passes context (PageRegistry::Entry::passesContext).
    // `controller` is this page's GeneralPageController. settingsApp is
    // declared even though this page does not read it: PageHost passes it to
    // every such page, and assigning it to a page that lacks the property
    // logs an error on every build.
    required property QtObject controller
    required property ApplicationController settingsApp

    // The `appSettings` context property under a name of its own, for a child
    // that declares an `appSettings` property: `appSettings: appSettings`
    // inside that child would resolve to the child's own property.
    readonly property var settingsBackend: appSettings

    contentHeight: content.implicitHeight
    clip: true

    ColumnLayout {
        // Animations editor moved to its own top-level sidebar entry
        // (Settings → Animations → General). The General page used to
        // host an inline Animations card; that's been removed in the
        // animation-UI rework so there's a single place to edit motion
        // settings rather than two paths editing the same backing fields.

        id: content

        width: parent.width
        spacing: Kirigami.Units.largeSpacing

        // =====================================================================
        // ON-SCREEN DISPLAY CARD
        // =====================================================================
        OsdCard {
            Layout.fillWidth: true
            appSettings: root.settingsBackend
        }

        // =====================================================================
        // RENDERING CARD
        // =====================================================================
        SettingsCard {
            headerText: i18n("Rendering")
            collapsible: true
            searchAnchor: "rendering"

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                SettingsRow {
                    title: i18n("Rendering device")
                    description: i18n("GPU that draws the zone overlays and on-screen displays. Automatic lets the graphics driver decide. KWin composites window contents, so those are unaffected.")
                    searchAnchor: "gpuDevice"

                    WideComboBox {
                        enabled: !settingsController.daemonRunning
                        // Device names are unbounded machine data and the
                        // SettingsRow control slot is a plain Row positioner
                        // (no Layout.* sizing, no clipping), so cap the
                        // collapsed field width; the popup still sizes to the
                        // widest item.
                        width: Kirigami.Units.gridUnit * 14
                        Accessible.name: i18n("Rendering device")
                        textRole: "text"
                        valueRole: "value"
                        // Machine-dependent, so enumerated by the page
                        // controller instead of declared in the schema.
                        model: root.controller.availableGpus
                        storedValue: appSettings.gpuDevice
                        // Guard the write: `enabled` does not reach an
                        // already-open popup (it re-parents to the Overlay),
                        // so a daemon that starts while the popup is up could
                        // otherwise still commit a rendering change. The
                        // refusal branch must revert the display — the combo
                        // committed currentIndex before activated() fired.
                        onActivated: {
                            if (settingsController.daemonRunning) {
                                revertToStoredValue();
                                return;
                            }
                            appSettings.gpuDevice = currentValue;
                        }
                    }
                }

                SettingsRow {
                    title: i18n("Rendering backend")
                    description: i18n("Graphics API used for overlay rendering")
                    searchAnchor: "renderingBackend"

                    WideComboBox {
                        enabled: !settingsController.daemonRunning
                        Accessible.name: i18n("Rendering backend")
                        // One list of {text, value} pairs rather than two
                        // parallel arrays that have to stay index-aligned.
                        textRole: "text"
                        valueRole: "value"
                        model: settingsController.valueOptions("Rendering", "Backend")
                        storedValue: appSettings.renderingBackend
                        // Same open-popup guard and display revert as the
                        // device combo above.
                        onActivated: {
                            if (settingsController.daemonRunning) {
                                revertToStoredValue();
                                return;
                            }
                            appSettings.renderingBackend = currentValue;
                        }
                    }
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    // Match SettingsRow's horizontal inset so the banner lines
                    // up with the rows above it instead of running edge-to-edge
                    // to the card border.
                    Layout.leftMargin: Kirigami.Units.largeSpacing
                    Layout.rightMargin: Kirigami.Units.largeSpacing
                    type: Kirigami.MessageType.Information
                    text: settingsController.daemonRunning ? i18n("Stop the daemon to change rendering settings.") : i18n("Rendering changes take effect the next time the daemon starts.")
                    visible: settingsController.daemonRunning || appSettings.renderingBackend !== root.controller.startupRenderingBackend || appSettings.gpuDevice !== root.controller.startupGpuDevice
                }
            }
        }

        // =====================================================================
        // SHADER EFFECTS CARD
        // =====================================================================
        // The frame rate applies to all shader categories (overlay, animation,
        // surface decoration), so this card is global (moved here from
        // Snapping → Overlay Appearance). The card deliberately has no master
        // toggle: shader use is decided per layout (a layout whose shader is
        // "none" draws the rectangle overlay), so a global switch would gate
        // nothing the layouts don't already control. The audio spectrum knobs
        // live in their own card below (Shaders.Audio config group).
        SettingsCard {
            headerText: i18n("Shader Effects")
            searchAnchor: "shaderEffects"
            collapsible: true
            // Advanced-only: shader frame-rate tuning is a fine-grained
            // performance knob most users never touch.
            advancedOnly: true

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                SettingsRow {
                    title: i18n("Frame rate")
                    searchAnchor: "frameRate"
                    description: i18n("Target refresh rate for shader animations")

                    SettingsSlider {
                        accessibleName: i18n("Frame rate")
                        from: root.controller.shaderFrameRateMin
                        to: root.controller.shaderFrameRateMax
                        tickMarkStepSize: 30
                        value: appSettings.shaderFrameRate
                        valueSuffix: " " + i18nc("frames per second, unit appended to a slider value", "fps")
                        labelWidth: Kirigami.Units.gridUnit * 4
                        onMoved: value => appSettings.shaderFrameRate = Math.round(value)
                    }
                }
            }
        }

        // =====================================================================
        // AUDIO SPECTRUM CARD
        // =====================================================================
        // The enable switch and the bar count (Shaders.Audio), the part of CAVA
        // simple mode keeps. The switch is disabled while cava is missing, and
        // the bar count shows only while the switch is on and cava is present.
        // The values feed both runtimes (daemon overlays and the KWin effect)
        // plus this app's own shader preview. The remaining analysis knobs
        // (smoothing, gain, cutoffs, channels, filters, backend/source) are
        // advanced depth and live in the Audio Analysis card below.
        SettingsCard {
            headerText: i18n("Audio Spectrum")
            searchAnchor: "audioSpectrum"
            collapsible: true

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                SettingsRow {
                    title: i18n("Audio spectrum")
                    searchAnchor: "audioSpectrumEnabled"
                    // A missing cava is reported once, by the warning below.
                    description: i18n("Feed audio spectrum data to shaders that support it")

                    SettingsSwitch {
                        id: audioVizSwitch

                        enabled: root.controller.cavaAvailable
                        checked: appSettings.enableAudioVisualizer
                        accessibleName: i18n("Enable CAVA audio spectrum")
                        onToggled: function (newValue) {
                            appSettings.enableAudioVisualizer = newValue;
                        }
                    }
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    Layout.leftMargin: Kirigami.Units.largeSpacing
                    Layout.rightMargin: Kirigami.Units.largeSpacing
                    type: Kirigami.MessageType.Warning
                    text: i18n("CAVA is not installed. Install the <b>cava</b> package to enable audio-reactive shader effects.")
                    visible: !root.controller.cavaAvailable
                }

                // One gate per knob stack: disabling a container propagates to
                // every child, and SettingsRow/SettingsSeparator lead their
                // `visible:` binding with `enabled`, so rows AND separators
                // collapse together when the toggle is off (per-row gates left
                // the separators behind as orphaned divider lines).
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    enabled: audioVizSwitch.checked && root.controller.cavaAvailable
                    // Hide as well as disable: the rows collapse themselves
                    // via `visible: enabled`, but a visible zero-height
                    // container still costs the parent a spacing slot.
                    visible: audioVizSwitch.checked && root.controller.cavaAvailable

                    SettingsSeparator {}

                    SettingsRow {
                        title: i18n("Spectrum bars")
                        searchAnchor: "spectrumBars"
                        description: i18n("Number of frequency bands in the audio visualization")

                        SettingsSlider {
                            accessibleName: i18n("Spectrum bars")
                            from: root.controller.audioSpectrumBarCountMin
                            to: root.controller.audioSpectrumBarCountMax
                            stepSize: 2
                            tickMarkStepSize: 16
                            value: appSettings.audioSpectrumBarCount
                            valueSuffix: ""
                            labelWidth: Kirigami.Units.gridUnit * 4
                            onMoved: value => appSettings.audioSpectrumBarCount = Math.round(value)
                        }
                    }
                }
            }
        }

        // =====================================================================
        // AUDIO ANALYSIS CARD (advanced)
        // =====================================================================
        // The rest of the CAVA analysis parameter set, in its own card so one
        // advancedOnly flag governs the whole block instead of every row
        // carrying its own. The card is gated on the same enable switch and
        // cava presence as the bar-count stack above, and is hidden as well as
        // disabled for the same reason that stack is: a header-only card the
        // user cannot expand still costs the page a largeSpacing slot.
        //
        // The advancedOnly term inside `visible` is required, not redundant. A
        // consumer `visible:` binding REPLACES SettingsCard's own
        // `visible: !advancedOnly || settingsController.advancedMode`, so
        // omitting it would put the card back in simple mode.
        SettingsCard {
            headerText: i18n("Audio Analysis")
            searchAnchor: "audioAnalysis"
            collapsible: true
            advancedOnly: true
            visible: (!advancedOnly || settingsController.advancedMode) && audioVizSwitch.checked && root.controller.cavaAvailable

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                SettingsRow {
                    title: i18n("Noise reduction")
                    searchAnchor: "audioNoiseReduction"
                    description: i18n("How smoothly the bars respond. Higher values are slower and calmer while lower values are fast and twitchy.")

                    SettingsSlider {
                        accessibleName: i18n("Noise reduction")
                        from: root.controller.audioNoiseReductionMin
                        to: root.controller.audioNoiseReductionMax
                        tickMarkStepSize: 10
                        value: appSettings.audioNoiseReduction
                        valueSuffix: ""
                        labelWidth: Kirigami.Units.gridUnit * 4
                        onMoved: value => appSettings.audioNoiseReduction = Math.round(value)
                    }
                }

                SettingsRow {
                    title: i18n("Extra smoothing")
                    searchAnchor: "audioExtraSmoothing"
                    description: i18n("Additional smoothing applied on top of noise reduction")

                    SettingsSlider {
                        accessibleName: i18n("Extra smoothing")
                        from: root.controller.audioExtraSmoothingMin
                        to: root.controller.audioExtraSmoothingMax
                        tickMarkStepSize: 10
                        value: appSettings.audioExtraSmoothing
                        labelWidth: Kirigami.Units.gridUnit * 4
                        onMoved: value => appSettings.audioExtraSmoothing = Math.round(value)
                    }
                }

                SettingsSeparator {}

                SettingsRow {
                    title: i18n("Automatic gain")
                    searchAnchor: "audioAutosens"
                    description: i18n("Continuously adjusts sensitivity so the bars fill the available range")

                    SettingsSwitch {
                        id: audioAutosensSwitch

                        checked: appSettings.audioAutosens
                        accessibleName: i18n("Automatic gain")
                        onToggled: function (newValue) {
                            appSettings.audioAutosens = newValue;
                        }
                    }
                }

                SettingsRow {
                    title: i18n("Sensitivity")
                    searchAnchor: "audioSensitivity"
                    description: audioAutosensSwitch.checked ? i18n("Starting gain that automatic gain adapts from") : i18n("Fixed gain applied to the audio signal")

                    SettingsSlider {
                        accessibleName: i18n("Sensitivity")
                        from: root.controller.audioSensitivityMin
                        to: root.controller.audioSensitivityMax
                        tickMarkStepSize: 50
                        value: appSettings.audioSensitivity
                        labelWidth: Kirigami.Units.gridUnit * 4
                        onMoved: value => appSettings.audioSensitivity = Math.round(value)
                    }
                }

                SettingsSeparator {}

                SettingsRow {
                    title: i18n("Lowest frequency")
                    searchAnchor: "audioLowerCutoff"
                    description: i18n("Sounds below this frequency are ignored")

                    SettingsSlider {
                        accessibleName: i18n("Lowest frequency")
                        from: root.controller.audioLowerCutoffHzMin
                        to: root.controller.audioLowerCutoffHzMax
                        tickMarkStepSize: 50
                        value: appSettings.audioLowerCutoffHz
                        valueSuffix: " " + i18nc("hertz, unit appended to a slider value", "Hz")
                        labelWidth: Kirigami.Units.gridUnit * 4
                        onMoved: value => appSettings.audioLowerCutoffHz = Math.round(value)
                    }
                }

                SettingsRow {
                    title: i18n("Highest frequency")
                    searchAnchor: "audioHigherCutoff"
                    description: i18n("Sounds above this frequency are ignored")

                    SettingsSlider {
                        accessibleName: i18n("Highest frequency")
                        from: root.controller.audioHigherCutoffHzMin
                        to: root.controller.audioHigherCutoffHzMax
                        tickMarkStepSize: 1000
                        value: appSettings.audioHigherCutoffHz
                        valueSuffix: " " + i18nc("hertz, unit appended to a slider value", "Hz")
                        labelWidth: Kirigami.Units.gridUnit * 4
                        onMoved: value => appSettings.audioHigherCutoffHz = Math.round(value)
                    }
                }

                SettingsSeparator {}

                SettingsRow {
                    title: i18n("Channels")
                    searchAnchor: "audioChannelMode"
                    description: i18n("Stereo shows left and right bars side by side. Mono collapses to one set of bars.")

                    WideComboBox {
                        Accessible.name: i18n("Channels")
                        textRole: "text"
                        valueRole: "value"
                        model: settingsController.valueOptions("Shaders.Audio", "ChannelMode")
                        storedValue: appSettings.audioChannelMode
                        onActivated: appSettings.audioChannelMode = currentValue
                    }
                }

                SettingsRow {
                    title: i18n("Reverse bar order")
                    searchAnchor: "audioReverse"
                    description: i18n("Flip the frequency order of the bars")

                    SettingsSwitch {
                        checked: appSettings.audioReverse
                        accessibleName: i18n("Reverse bar order")
                        onToggled: function (newValue) {
                            appSettings.audioReverse = newValue;
                        }
                    }
                }

                SettingsSeparator {}

                SettingsRow {
                    title: i18n("Monstercat filter")
                    searchAnchor: "audioMonstercat"
                    description: i18n("Spreads each bar into its neighbors for a smoother outline")

                    SettingsSwitch {
                        checked: appSettings.audioMonstercat
                        accessibleName: i18n("Monstercat filter")
                        onToggled: function (newValue) {
                            appSettings.audioMonstercat = newValue;
                        }
                    }
                }

                SettingsRow {
                    title: i18n("Wave filter")
                    searchAnchor: "audioWaves"
                    description: i18n("Rounds the spectrum into soft waves")

                    SettingsSwitch {
                        checked: appSettings.audioWaves
                        accessibleName: i18n("Wave filter")
                        onToggled: function (newValue) {
                            appSettings.audioWaves = newValue;
                        }
                    }
                }

                SettingsSeparator {}

                SettingsRow {
                    title: i18n("Audio backend")
                    searchAnchor: "audioInputMethod"
                    description: i18n("Leave on Automatic unless capture fails with the detected backend")

                    WideComboBox {
                        Accessible.name: i18n("Audio backend")
                        textRole: "text"
                        valueRole: "value"
                        model: settingsController.valueOptions("Shaders.Audio", "InputMethod")
                        storedValue: appSettings.audioInputMethod
                        onActivated: appSettings.audioInputMethod = currentValue
                    }
                }

                SettingsRow {
                    title: i18n("Audio source")
                    searchAnchor: "audioInputSource"
                    description: i18n("Capture device or monitor source. Keep it set to auto to follow the default output.")

                    TextField {
                        id: audioSourceField

                        // SettingsRow lays its default children out in a
                        // plain Row positioner, so Layout.* attached
                        // properties are ignored here — set the width
                        // directly or the field renders at implicit width.
                        width: Kirigami.Units.gridUnit * 12
                        Accessible.name: i18n("Audio source")
                        // editingFinished also fires on focus loss, and a page
                        // switch that does not move focus first (Ctrl+PgDown)
                        // hides this page AFTER the controller has made the
                        // next page active. Without the envelope, the commit
                        // would mark that next page dirty instead of this one.
                        onEditingFinished: {
                            settingsController.beginExternalEdit("general");
                            appSettings.audioInputSource = text;
                            settingsController.endExternalEdit();
                        }

                        // Defer host-driven updates while the user is
                        // typing. A plain `text:` binding re-fires on every
                        // audioInputSourceChanged, focused or not, so it
                        // overwrote the edit in progress before the focus
                        // guard was ever consulted — the guard only started
                        // working once an unfocused change had severed the
                        // binding it was meant to hold back.
                        Binding on text {
                            value: appSettings.audioInputSource
                            when: !audioSourceField.activeFocus
                            restoreMode: Binding.RestoreNone
                        }
                    }
                }
            }
        }

        // =====================================================================
        // LAYOUT ASSIGNMENT CARD
        // =====================================================================
        // Mode-neutral: the toggle governs the synthesized default for BOTH the
        // snapping and tiling engines, so it lives on the General page rather
        // than either mode's behavior page.
        SettingsCard {
            headerText: i18n("Layout assignment")
            collapsible: true
            searchAnchor: "layoutAssignment"

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                SettingsRow {
                    title: i18n("Don't assign a layout by default")
                    searchAnchor: "suppressDefaultLayoutAssignment"
                    description: i18n("Snapping and tiling stay off until you assign a layout. A rule can re-enable the default per monitor.")

                    SettingsSwitch {
                        checked: appSettings.suppressDefaultLayoutAssignment
                        accessibleName: i18n("Don't assign a layout by default")
                        onToggled: function (newValue) {
                            appSettings.suppressDefaultLayoutAssignment = newValue;
                        }
                    }
                }
            }
        }

        // =====================================================================
        // WINDOW FILTERING CARD
        // =====================================================================
        // The global size filters previously hosted on the standalone
        // "Exclusions" page. The per-app and per-class lists that used to
        // live there have folded into Rules (Application-subject
        // Exclude rules), so the page itself was deleted; these global
        // knobs survive here because they apply to ALL windows uniformly
        // rather than matching specific applications. The card's transient
        // row is hidden: nothing in snapping or tiling reads the
        // excludeTransientWindows setting it would edit.
        WindowFilterCard {
            Layout.fillWidth: true

            showTransientRow: false

            minWidth: appSettings.minimumWindowWidth
            minWidthFrom: root.controller.minimumWindowWidthMin
            minWidthTo: root.controller.minimumWindowWidthMax
            minWidthDescription: i18n("Windows narrower than this are excluded")
            minWidthDisabledDescription: i18n("Disabled (no width threshold)")
            minWidthAccessibleName: i18n("Minimum window width")
            onMinWidthModified: value => {
                appSettings.minimumWindowWidth = value;
            }

            minHeight: appSettings.minimumWindowHeight
            minHeightFrom: root.controller.minimumWindowHeightMin
            minHeightTo: root.controller.minimumWindowHeightMax
            minHeightDescription: i18n("Windows shorter than this are excluded")
            minHeightDisabledDescription: i18n("Disabled (no height threshold)")
            minHeightAccessibleName: i18n("Minimum window height")
            onMinHeightModified: value => {
                appSettings.minimumWindowHeight = value;
            }
        }

        // =====================================================================
        // CONFIGURATION CARD
        // =====================================================================
        SettingsCard {
            headerText: i18n("Configuration")
            searchAnchor: "configuration"
            collapsible: true

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                SettingsRow {
                    title: i18n("Backup")
                    searchAnchor: "backup"
                    description: i18n("Export all settings to a file")

                    Button {
                        text: i18n("Export Settings")
                        icon.name: "document-export"
                        Accessible.name: text
                        onClicked: exportConfigDialog.open()
                    }
                }

                SettingsSeparator {}

                SettingsRow {
                    title: i18n("Restore")
                    searchAnchor: "restore"
                    description: i18n("Import settings from a previously exported file")

                    Button {
                        text: i18n("Import Settings")
                        icon.name: "document-import"
                        Accessible.name: text
                        onClicked: importConfigDialog.open()
                    }
                }

                SettingsSeparator {}

                SettingsRow {
                    title: i18n("Reset")
                    description: i18n("Restore every setting on every page to its default value")
                    searchAnchor: "resetDefaults"

                    Button {
                        text: i18n("Reset to Defaults")
                        icon.name: "document-revert"
                        Accessible.name: text
                        // Reach the chrome-owned confirmation dialog through
                        // the `window.defaultsConfirmDialog` alias declared
                        // in Main.qml. A bare `defaultsConfirmDialog.open()`
                        // would resolve against this page's scope — Loader
                        // breaks file-id lookup so the id from Main.qml is
                        // unreachable here — and throw `ReferenceError`
                        // at runtime. The `typeof window` guard mirrors the
                        // same defensive shape used elsewhere in the file
                        // for cross-host (KCM / preview) compatibility,
                        // where `window` may be undeclared.
                        onClicked: {
                            if (typeof window !== "undefined" && window && window.defaultsConfirmDialog) {
                                window.defaultsConfirmDialog.open();
                            }
                        }
                    }
                }
            }
        }
    }

    FileDialog {
        id: exportConfigDialog

        title: i18n("Export Settings")
        nameFilters: [i18n("PlasmaZones Config (*.json)"), i18n("All files (*)")]
        defaultSuffix: "json"
        fileMode: FileDialog.SaveFile
        // Success only. A failure toasts through settingsTransferFailed with
        // the reason, which the bool cannot carry.
        onAccepted: {
            if (settingsController.exportAllSettings(settingsController.urlToLocalFile(selectedFile)) && typeof window !== "undefined" && window && window.showToast)
                window.showToast(i18n("Settings exported"));
        }
    }

    FileDialog {
        id: importConfigDialog

        title: i18n("Import Settings")
        nameFilters: [i18n("PlasmaZones Config (*.json *.conf *.ini *.rc)"), i18n("All files (*)")]
        fileMode: FileDialog.OpenFile
        onAccepted: {
            if (settingsController.importAllSettings(settingsController.urlToLocalFile(selectedFile)) && typeof window !== "undefined" && window && window.showToast)
                window.showToast(i18n("Settings imported"));
        }
    }

    // Both transfers report their failures here rather than through the bool:
    // a refused path, a file that vanished, and a file that is not settings at
    // all are the same `false` and want different words.
    Connections {
        function onSettingsTransferFailed(reason) {
            if (typeof window !== "undefined" && window && window.showToast)
                window.showToast(reason);
        }

        target: settingsController
    }
}
