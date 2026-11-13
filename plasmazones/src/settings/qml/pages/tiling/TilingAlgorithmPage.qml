// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.phosphor.control
import "../../js/AlgorithmCapabilities.js" as AlgoCaps

SettingsFlickable {
    id: root

    // PageHost sets both before the page's bindings run, because this page's
    // registry entry passes context (PageRegistry::Entry::passesContext).
    // settingsApp is declared even though this page does not read it: PageHost
    // passes it to every such page, and assigning it to a page that lacks the
    // property logs an error on every build.
    required property QtObject controller
    required property ApplicationController settingsApp

    readonly property int algorithmPreviewWidth: Kirigami.Units.gridUnit * 18
    readonly property int algorithmPreviewHeight: Kirigami.Units.gridUnit * 10
    // Cache the availableAlgorithms PROPERTY (read, not call — it is both a
    // Q_PROPERTY and a same-named Q_INVOKABLE, so the `()` form errors with
    // "is not a function"). Its NOTIFY-backed binding self-refreshes when the
    // list changes (e.g. a .luau reload), so no imperative refresh is needed.
    property var _cachedAlgos: settingsController.availableAlgorithms || []
    // `appSettings` is the ISettings context property (set app-wide via
    // rootContext); read it directly. Handlers here are attached to page-scope
    // objects and to AlgorithmPreviewCard (which declares no `appSettings`), so
    // the bare name resolves to the context property — the children that DO set
    // their own `appSettings` (PerScreenOverrideHelper, the LayoutComboBox nested
    // inside AlgorithmPreviewCard) are never in an ancestor binding's scope chain.
    readonly property string effectiveAlgorithm: settingValue("Algorithm", appSettings.defaultAutotileAlgorithm)
    // Working algorithm for the whole page (preview, custom-param controls,
    // capability lookups). Driven imperatively rather than bound to
    // algorithmCombo.currentValue: that value transiently resets to the first
    // list entry (BSP) whenever the combo rebuilds its model — e.g. the layout
    // reload that follows a Save — which used to drag the entire page onto BSP.
    // It is set the instant the user picks an algorithm (the combo's
    // onActivated, below) so switching is immediate, and re-synced to the
    // persisted setting on load / external edits / per-screen scope changes via
    // onEffectiveAlgorithmChanged.
    property string selectedAlgorithm: root.effectiveAlgorithm
    // The reserved "explicitly no algorithm" word
    // (PhosphorZones::NoTilingAlgorithm), which the library card's Clear
    // Default stores in defaultAutotileAlgorithm. Named rather than spelled
    // inline, matching MonitorStatePage's tokens; the constant's own doc block
    // lists every QML site that hardcodes the literal.
    readonly property string _noAlgorithmToken: "none"

    onEffectiveAlgorithmChanged: root.selectedAlgorithm = root.effectiveAlgorithm
    // Data-driven algorithm capabilities (lookup from cached availableAlgorithms by ID)
    readonly property var algoCapabilities: AlgoCaps.capabilitiesFor(root._cachedAlgos, root.selectedAlgorithm)
    readonly property bool algoSupportsSplitRatio: AlgoCaps.supportsSplitRatio(algoCapabilities)
    readonly property bool algoSupportsMasterCount: AlgoCaps.supportsMasterCount(algoCapabilities)
    readonly property bool algoSupportsCustomParams: AlgoCaps.supportsCustomParams(algoCapabilities)
    // Custom param definitions for the selected algorithm. The binding
    // depends on selectedAlgorithm and on _cachedAlgos, which is a fresh list
    // every time availableAlgorithms re-reads, so a Luau hot-reload that
    // changes the script's parameter declarations rebuilds the controls. It
    // reads those two directly rather than through algoCapabilities and
    // algoSupportsCustomParams: both of those also depend on
    // selectedAlgorithm, and whether this binding saw them before or after
    // their own update on a switch depended on notification order, so a
    // switch that flipped the capability flag could evaluate it (and rebuild
    // the Repeater and re-seed liveCustomParams) twice. Because
    // customParamsForAlgorithm() is a Q_INVOKABLE (not a Q_PROPERTY with
    // NOTIFY), QML's binding engine will NOT re-call it when saved values
    // change — so the Repeater model stays stable during slider drags.
    readonly property var customParamDefs: {
        if (!AlgoCaps.supportsCustomParams(AlgoCaps.capabilitiesFor(root._cachedAlgos, root.selectedAlgorithm)))
            return [];

        return root.controller.customParamsForAlgorithm(root.selectedAlgorithm);
    }
    // Live custom-param values (name → value) for the selected algorithm. This
    // is the single source of truth the param controls read and write, and the
    // value the live preview consumes — exactly the role appSettings holds for
    // split ratio / master count. customParamDefs (the Repeater model) is
    // intentionally not refreshed on edit, so the current values live here
    // instead. Re-seeded from the saved/default values whenever
    // customParamDefs re-evaluates (an algorithm switch or a catalog reload),
    // and from a fresh read when the per-algorithm settings move under the
    // page (see _committingSlot).
    property var liveCustomParams: ({})

    function _seedLiveCustomParams(defs) {
        let m = {};
        for (let i = 0; i < defs.length; ++i) {
            const d = defs[i];
            m[d.name] = (d.value !== undefined ? d.value : d.defaultValue);
        }
        root.liveCustomParams = m;
    }

    function setLiveCustomParam(name, value) {
        // Reassign a fresh object so the `var` property emits changed —
        // mutating in place would not re-trigger the preview binding or the
        // delegates' paramValue.
        let m = Object.assign({}, root.liveCustomParams);
        m[name] = value;
        root.liveCustomParams = m;
    }

    onCustomParamDefsChanged: _seedLiveCustomParams(root.customParamDefs)

    // Live per-algorithm built-in tuning (split ratio, master count, max
    // windows) for the selected algorithm. Like liveCustomParams, this is the
    // reactive source the sliders read in global scope so they don't snap back
    // to the non-reactive algorithmSettingsFor() value, and it re-seeds on
    // algorithm change so each algorithm shows its own saved tuning. Per-screen
    // overrides bypass this (they flow through settingValue/writeSetting's
    // per-screen path, which is reactive on its own).
    property var liveAlgoSettings: ({})

    function _seedLiveAlgoSettings() {
        root.liveAlgoSettings = root.controller.algorithmSettingsFor(root.selectedAlgorithm);
    }

    function setLiveAlgoSetting(key, value) {
        if (psHelper.isPerScreen)
            return;
        let m = Object.assign({}, root.liveAlgoSettings);
        m[key] = value;
        root.liveAlgoSettings = m;
    }

    onSelectedAlgorithmChanged: _seedLiveAlgoSettings()
    // A Luau hot-reload can change the algorithm's own defaults, which
    // algorithmSettingsFor() falls back to for an unsaved field. Keyed on the
    // catalog list rather than algoCapabilities, which also changes on every
    // algorithm switch and so re-seeded a second time on top of the handler
    // above. TilingSimplePage re-seeds on the same signal.
    on_CachedAlgosChanged: _seedLiveAlgoSettings()

    Component.onCompleted: {
        _seedLiveCustomParams(root.customParamDefs);
        _seedLiveAlgoSettings();
    }

    // Set while this page's own controls write the selected algorithm's
    // per-algorithm slot, so the store NOTIFY that write raises does not
    // re-seed mid-drag and fight the live handle. TilingSimplePage carries the
    // same guard, and the same caveat: it is raised and lowered around the
    // write in one statement block, which is only correct because
    // Settings::setAutotilePerAlgorithmSettings emits
    // autotilePerAlgorithmSettingsChanged synchronously, from inside the write.
    property bool _committingSlot: false

    /// Run @p write (one of this page's own per-algorithm writes) with the
    /// re-seed guard up. try/finally because QML has no RAII: a throw would
    /// otherwise latch the flag, and the page would silently stop following a
    /// Discard, a profile switch or an edit on the simple page.
    function _commitSlot(write) {
        root._committingSlot = true;
        try {
            write();
        } finally {
            root._committingSlot = false;
        }
    }

    // External per-algorithm slot changes (a Discard, a profile switch, a
    // page reset, an edit on the simple Tiling page) move the saved tuning and
    // custom parameters under the page, and both live maps only re-seed on an
    // algorithm switch otherwise. Reseeded from fresh controller reads:
    // customParamDefs is deliberately non-reactive, so it still holds the
    // values from before the change.
    Connections {
        function onAutotilePerAlgorithmSettingsChanged() {
            if (root._committingSlot)
                return;
            root._seedLiveAlgoSettings();
            root._seedLiveCustomParams(root.algoSupportsCustomParams ? root.controller.customParamsForAlgorithm(root.selectedAlgorithm) : []);
        }

        target: appSettings
    }

    // Whether the algorithm uses a center layout (ratio/count labels say
    // "Center" instead of "Master"). Read straight from the catalog — the
    // hardcoded "three-column || centered-master" fallback this used to carry
    // was both duplicated on the simple page and unreachable, since
    // algorithmservice.cpp sets centerLayout unconditionally.
    readonly property bool algoCenterLayout: AlgoCaps.centerLayout(algoCapabilities)

    function settingValue(key, globalValue) {
        return psHelper.settingValue(key, globalValue);
    }

    function writeSetting(key, value, globalSetter) {
        psHelper.writeSetting(key, value, globalSetter);
    }

    contentHeight: content.implicitHeight
    clip: true

    PerScreenOverrideHelper {
        id: psHelper

        appSettings: settingsController
        // Shared app-wide scope — a monitor picked on any per-monitor page
        // stays picked here.
        selectedScreenName: settingsController.scopeScreenName
        getterMethod: "getPerScreenAutotileSettings"
        setterMethod: "setPerScreenAutotileSetting"
    }

    ColumnLayout {
        id: content

        width: parent.width
        spacing: Kirigami.Units.largeSpacing

        // =================================================================
        // Algorithm Card (per-monitor) — opts into the header scope chip.
        // =================================================================
        SettingsCard {
            Layout.fillWidth: true
            headerText: i18n("Algorithm")
            searchAnchor: "algorithm"
            collapsible: true
            scopeEnabled: true
            scopeAppSettings: settingsController
            // Algorithm sub-domain only — must not report/reset the Gaps card's
            // per-monitor overrides (shared autotile map, disjoint key subsets).
            scopeHasOverridesMethod: "hasPerScreenAutotileAlgorithmSettings"
            scopeClearerMethod: "clearPerScreenAutotileAlgorithmSettings"

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                // Preview, description and picker — the shared block, also
                // hosted by TilingSimplePage. Every layout input the user can
                // edit feeds the preview from the live slider handle, so the
                // diagram updates as the user drags any of them (master ratio
                // and master count included, not just max windows). The
                // "Max N windows" caption also tracks the windows slider.
                AlgorithmPreviewCard {
                    Layout.fillWidth: true
                    previewWidth: root.algorithmPreviewWidth
                    previewHeight: root.algorithmPreviewHeight
                    algorithmId: root.selectedAlgorithm
                    description: AlgoCaps.description(root.algoCapabilities)
                    currentAlgorithmId: root.effectiveAlgorithm
                    captionText: i18np("Max %n window", "Max %n windows", previewWindowSlider.slider.value)
                    windowCount: previewWindowSlider.slider.value
                    // An algorithm with no ratio control of its own draws at the
                    // ratio the catalog declares for it. An algorithm the
                    // catalog does not describe at all yields undefined there,
                    // which must not reach this `real` property, so fall back to
                    // the user's configured global ratio rather than a literal.
                    splitRatio: {
                        // The slider works in percent; the preview takes the
                        // fraction.
                        if (root.algoSupportsSplitRatio)
                            return masterRatioSlider.slider.value / 100;

                        const catalogRatio = AlgoCaps.defaultSplitRatio(root.algoCapabilities);
                        return catalogRatio !== undefined ? catalogRatio : appSettings.autotileSplitRatio;
                    }
                    supportsMasterCount: root.algoSupportsMasterCount
                    masterCount: masterCountSlider.slider.value
                    customParams: root.liveCustomParams
                    zoneNumberDisplay: AlgoCaps.zoneNumberDisplay(root.algoCapabilities)
                    onAlgorithmActivated: selectedId => {
                        // An empty id means the combo's model rebuilt under the
                        // selection — fall back to the persisted global default.
                        const algoId = selectedId === "" ? appSettings.defaultAutotileAlgorithm : selectedId;
                        // The persisted default can be the reserved opt-out
                        // word (the library card's Clear Default). It is not
                        // a pickable algorithm here, and writing it through
                        // writeSetting in per-screen scope would plant the
                        // word in a per-screen Algorithm override — a slot
                        // whose engine-side resolver has no sentinel arm.
                        // A rebuild under a cleared default just keeps state.
                        if (algoId === root._noAlgorithmToken)
                            return;
                        // Drive the page off the user's pick immediately, then
                        // persist. Only the Algorithm key is written; resetting
                        // global max-windows / split-ratio / master-count here
                        // would clobber a sibling algorithm's per-algorithm slot.
                        root.selectedAlgorithm = algoId;
                        root.writeSetting("Algorithm", algoId, function (v) {
                            appSettings.defaultAutotileAlgorithm = v;
                        });
                    }
                }

                // Gated with the row it adjoins, per SettingsSeparator's own
                // contract — otherwise the collapsed row leaves a dangling
                // rule line behind it.
                SettingsSeparator {
                    enabled: root.selectedAlgorithm !== root._noAlgorithmToken
                }

                // Max windows
                SettingsRow {
                    title: i18n("Max windows")
                    searchAnchor: "maxWindows"
                    description: i18n("Maximum number of windows to tile on this screen")
                    // Hidden while the default is cleared: with no algorithm
                    // selected there is no per-algorithm slot to tune, and the
                    // controller refuses the write. Left in place, the slider
                    // would hold the dragged value for the session and snap
                    // back on reload — a silent discard. SettingsRow is
                    // `visible: enabled && …`, so this collapses the row
                    // exactly as the ratio and master rows collapse through
                    // their capability flags, which answer false for the
                    // reserved word. Those two are self-gating; this row is
                    // otherwise unconditional, so it needs the test spelled out.
                    enabled: root.selectedAlgorithm !== root._noAlgorithmToken

                    SettingsSlider {
                        id: previewWindowSlider

                        accessibleName: i18n("Maximum windows")
                        from: root.controller.autotileMaxWindowsMin
                        to: root.controller.autotileMaxWindowsMax
                        stepSize: 1
                        value: root.settingValue("MaxWindows", root.liveAlgoSettings.maxWindows !== undefined ? root.liveAlgoSettings.maxWindows : appSettings.autotileMaxWindows)
                        formatValue: function (v) {
                            return Math.round(v).toString();
                        }
                        onMoved: value => {
                            root.setLiveAlgoSetting("maxWindows", Math.round(value));
                            root._commitSlot(() => root.writeSetting("MaxWindows", Math.round(value), function (v) {
                                    root.controller.setAlgorithmMaxWindows(root.selectedAlgorithm, v);
                                }));
                        }
                    }
                }

                // Ratio and count rows, shown only when the catalog says the
                // algorithm exposes them. Each separator follows the rows it
                // leads, so an algorithm with a master count and no ratio gets
                // one rule line above the count rather than two stacked ones.
                SettingsSeparator {
                    visible: root.algoSupportsSplitRatio
                }

                SettingsRow {
                    visible: root.algoSupportsSplitRatio
                    title: root.algoCenterLayout ? i18n("Center ratio") : i18n("Master ratio")
                    searchAnchor: "masterRatio"
                    description: root.algoCenterLayout ? i18n("Width proportion allocated to the center column") : i18n("Width proportion allocated to the master area")

                    SettingsSlider {
                        id: masterRatioSlider

                        accessibleName: root.algoCenterLayout ? i18n("Center ratio") : i18n("Master ratio")
                        // Percent units, not the stored fraction: a step of
                        // 0.05 lands on values like 0.30000000000000004, which
                        // were written as-is and never compare equal to a
                        // saved 0.3. Whole percent steps, divided on write.
                        from: Math.round(root.controller.autotileSplitRatioMin * 100)
                        to: Math.round(root.controller.autotileSplitRatioMax * 100)
                        stepSize: 5
                        value: Math.round(root.settingValue("SplitRatio", root.liveAlgoSettings.splitRatio !== undefined ? root.liveAlgoSettings.splitRatio : appSettings.autotileSplitRatio) * 100)
                        formatValue: function (v) {
                            return i18nc("a whole-number percentage", "%1%", Math.round(v));
                        }
                        onMoved: value => {
                            const ratio = Math.round(value) / 100;
                            root.setLiveAlgoSetting("splitRatio", ratio);
                            root._commitSlot(() => root.writeSetting("SplitRatio", ratio, function (v) {
                                    root.controller.setAlgorithmSplitRatio(root.selectedAlgorithm, v);
                                }));
                        }
                    }
                }

                SettingsRow {
                    visible: root.algoSupportsSplitRatio
                    title: i18n("Ratio step size")
                    searchAnchor: "ratioStepSize"
                    description: i18n("Amount the ratio changes per keyboard shortcut press")

                    SettingsSlider {
                        accessibleName: i18n("Ratio step size")
                        // Percent units for the same reason as the ratio above.
                        from: Math.round(root.controller.autotileSplitRatioStepMin * 100)
                        to: Math.round(root.controller.autotileSplitRatioStepMax * 100)
                        stepSize: 1
                        value: Math.round(root.settingValue("SplitRatioStep", appSettings.autotileSplitRatioStep) * 100)
                        formatValue: function (v) {
                            return i18nc("a whole-number percentage", "%1%", Math.round(v));
                        }
                        onMoved: value => {
                            root.writeSetting("SplitRatioStep", Math.round(value) / 100, function (v) {
                                appSettings.autotileSplitRatioStep = v;
                            });
                        }
                    }
                }

                SettingsSeparator {
                    visible: root.algoSupportsMasterCount
                }

                SettingsRow {
                    visible: root.algoSupportsMasterCount
                    title: root.algoCenterLayout ? i18n("Center count") : i18n("Master count")
                    searchAnchor: "masterCount"
                    description: root.algoCenterLayout ? i18n("Number of windows in the center column") : i18n("Number of windows in the master area")

                    SettingsSlider {
                        id: masterCountSlider

                        accessibleName: root.algoCenterLayout ? i18n("Center count") : i18n("Master count")
                        from: root.controller.autotileMasterCountMin
                        to: root.controller.autotileMasterCountMax
                        stepSize: 1
                        value: root.settingValue("MasterCount", root.liveAlgoSettings.masterCount !== undefined ? root.liveAlgoSettings.masterCount : appSettings.autotileMasterCount)
                        formatValue: function (v) {
                            return Math.round(v).toString();
                        }
                        onMoved: value => {
                            root.setLiveAlgoSetting("masterCount", Math.round(value));
                            root._commitSlot(() => root.writeSetting("MasterCount", Math.round(value), function (v) {
                                    root.controller.setAlgorithmMasterCount(root.selectedAlgorithm, v);
                                }));
                        }
                    }
                }

                // =============================================================
                // Custom Algorithm Parameters
                // =============================================================
                // Unlike the built-in sliders above, custom params are stored
                // per-algorithm only — there is no per-screen override for them,
                // so the controls write the global per-algorithm value via
                // setCustomParam() regardless of the active monitor scope.
                Repeater {
                    model: root.customParamDefs

                    delegate: ColumnLayout {
                        required property var modelData
                        readonly property string paramLabel: modelData.description || modelData.name
                        // Show the raw param name as subtitle when description was used as title
                        readonly property string paramDescription: modelData.description ? modelData.name : ""
                        // Current value, read from the page's live map (the single
                        // source of truth). Falls back to the model's saved/default
                        // value before the map is seeded. Because this tracks
                        // liveCustomParams, the slider's on-release re-sync
                        // (Binding when: !pressed) and the switch / combo all settle
                        // on the value the user just set instead of snapping back to
                        // the never-refreshed model value.
                        readonly property var paramValue: {
                            const v = root.liveCustomParams[modelData.name];
                            if (v !== undefined)
                                return v;
                            return modelData.value !== undefined ? modelData.value : modelData.defaultValue;
                        }
                        readonly property real paramMin: modelData.minValue !== undefined ? modelData.minValue : 0
                        readonly property real paramMax: {
                            let mx = modelData.maxValue !== undefined ? modelData.maxValue : 1;
                            // Guard against degenerate range from malformed script metadata
                            return mx > paramMin ? mx : paramMin + 1;
                        }
                        readonly property real paramRange: paramMax - paramMin
                        // The number slider works in whole units of its step
                        // (hundredths up to a range of 1, tenths up to 10)
                        // and divides on write. A fractional step lands on
                        // values like 0.35000000000000003, which were stored
                        // as-is and never compare equal to a saved 0.35.
                        readonly property int paramScale: paramRange <= 1 ? 100 : (paramRange <= 10 ? 10 : 1)

                        Layout.fillWidth: true
                        spacing: 0

                        SettingsSeparator {}

                        SettingsRow {
                            title: paramLabel
                            description: paramDescription

                            // Number parameter: rendered as a SettingsSlider
                            SettingsSlider {
                                visible: modelData.type === "number"
                                accessibleName: paramLabel
                                from: paramMin * paramScale
                                to: paramMax * paramScale
                                stepSize: 1
                                // Hidden for a bool or enum param, where
                                // paramValue is not a number.
                                value: typeof paramValue === "number" ? paramValue * paramScale : from
                                formatValue: function (v) {
                                    if (paramScale === 100)
                                        return i18nc("a whole-number percentage", "%1%", Math.round(v));

                                    if (paramScale === 10)
                                        return (Math.round(v) / 10).toFixed(1);

                                    return Math.round(v).toString();
                                }
                                onMoved: value => {
                                    // Rounded to the step, then held inside
                                    // the declared range: bounds that are not
                                    // whole units can round past them, and the
                                    // controller would store the clamped value
                                    // while the live map kept the raw one.
                                    const v = Math.min(paramMax, Math.max(paramMin, Math.round(value) / paramScale));
                                    root.setLiveCustomParam(modelData.name, v);
                                    root._commitSlot(() => root.controller.setCustomParam(root.selectedAlgorithm, modelData.name, v));
                                }
                            }

                            // Bool parameter: rendered as a Switch
                            Switch {
                                visible: modelData.type === "bool"
                                Accessible.name: paramLabel
                                checked: paramValue === true
                                onToggled: {
                                    root.setLiveCustomParam(modelData.name, checked);
                                    root._commitSlot(() => root.controller.setCustomParam(root.selectedAlgorithm, modelData.name, checked));
                                }
                            }

                            // Enum parameter: rendered as a ComboBox
                            ComboBox {
                                visible: modelData.type === "enum"
                                Accessible.name: paramLabel
                                model: modelData.enumOptions || []
                                currentIndex: {
                                    let opts = modelData.enumOptions || [];
                                    // -1, never 0, for a value the script's
                                    // current options no longer offer. Falling
                                    // back to the FIRST option displayed a
                                    // value the algorithm will not actually
                                    // run with — a stale or hand-edited enum
                                    // silently misreported itself as the
                                    // default. The repo convention for an
                                    // unmatched selection is -1 plus a
                                    // verbatim displayText.
                                    return opts.indexOf(paramValue);
                                }
                                displayText: currentIndex >= 0 ? currentText : (paramValue || "")
                                onActivated: {
                                    root.setLiveCustomParam(modelData.name, currentText);
                                    root._commitSlot(() => root.controller.setCustomParam(root.selectedAlgorithm, modelData.name, currentText));
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
