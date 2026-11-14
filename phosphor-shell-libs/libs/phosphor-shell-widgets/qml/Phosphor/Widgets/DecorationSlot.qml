// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
// Phosphor.Widgets.DecorationSlot, where a surface pack lands on chrome.
//
// Every chrome surface is a decoration host like a window frame (A1 §2.4),
// but the host that renders a pack chain (the shell's SurfaceDecoration)
// lives above these libraries, in the shell process. So a surface declares
// a slot: it names its frame item (the one a pack wraps, which carries
// `shaderAnchor: true`), its surface path in the decoration tree, and
// whether it is focused; the composition root hands it a Component that
// draws the chain. Without one the slot is empty and the surface draws
// its own stroke; with one, `active` says a chain is drawing (resolved and
// with no failed stage) so the surface can hand its outline to the pack.
//
//   DecorationSlot {
//       anchors.fill: parent
//       component: panel.decoration
//       contentItem: band
//       surfacePath: "shell.phosphor.bar"
//   }
//
// The instantiated item is given `contentItem`, `surfacePath`, `focused`
// and `surfaceOpacity` when it declares them, through bindings that follow
// this slot's own, so a surface that swaps its frame or loses focus keeps
// the pack in step.
//
// `surfaceOpacity` is the surface's TRANSITION fade (its open or close
// progress), not its material alpha. A ground drawn at 0.92 should not draw
// its pack's border at 0.92, so the slot does not derive this on its own. A
// surface whose frame item itself carries the fade (PaneHost's ground)
// binds surfaceOpacity to that opacity, since the pack's capture drops it;
// a static material alpha goes in the frame's colour (Theme.depth).

import QtQuick

Item {
    id: slot

    property Component component: null
    property Item contentItem: null
    property string surfacePath: ""
    property bool focused: true
    // SurfaceShaderItem renders through a custom scene-graph node. Hosts that
    // animate this slot's parent (for example PopoutHost's close scale/fade)
    // need the surface stages composited into textures first so the parent
    // transform and opacity are applied to the finished decoration.
    property bool layeredStages: false
    property real surfaceOpacity: 1.0

    // A chain is drawing this surface: the instantiated host reports
    // `decorationDrawing` (SurfaceDecoration's contract: a resolved chain
    // with no failed stage), and a host without that property counts as
    // active while it exists. A chain whose shader failed draws nothing, so
    // the surface keeps its own stroke then.
    readonly property bool active: loader.item !== null && (loader.item.decorationDrawing === undefined || loader.item.decorationDrawing === true)
    readonly property Item item: loader.item

    Loader {
        id: loader

        anchors.fill: parent
        active: slot.component !== null && slot.contentItem !== null
        sourceComponent: slot.component
    }

    Binding {
        target: loader.item
        property: "contentItem"
        value: slot.contentItem
        when: loader.item !== null && loader.item.contentItem !== undefined
        restoreMode: Binding.RestoreBindingOrValue
    }
    Binding {
        target: loader.item
        property: "surfacePath"
        value: slot.surfacePath
        when: loader.item !== null && loader.item.surfacePath !== undefined
        restoreMode: Binding.RestoreBindingOrValue
    }
    Binding {
        target: loader.item
        property: "focused"
        value: slot.focused
        when: loader.item !== null && loader.item.focused !== undefined
        restoreMode: Binding.RestoreBindingOrValue
    }
    Binding {
        target: loader.item
        property: "layeredStages"
        value: slot.layeredStages
        when: loader.item !== null && loader.item.layeredStages !== undefined
        restoreMode: Binding.RestoreBindingOrValue
    }
    Binding {
        target: loader.item
        property: "surfaceOpacity"
        value: slot.surfaceOpacity
        when: loader.item !== null && loader.item.surfaceOpacity !== undefined
        restoreMode: Binding.RestoreBindingOrValue
    }
}
