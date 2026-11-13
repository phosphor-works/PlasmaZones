// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.phosphor.animation

/**
 * @brief The expandable list-row shell shared by RuleRow and the decoration
 * pack rows — extracted verbatim from RuleRow so every expandable row in the
 * app carries the SAME delegate look (hover/press highlight), the SAME
 * whole-row click-to-expand interaction, and the SAME animated expansion.
 *
 * An ItemDelegate whose contentItem stacks a consumer-filled header RowLayout
 * over a clipped, lazily-loaded expansion body:
 *
 *   ExpandableRowDelegate {
 *       expansionContent: Component { ... }   // lazy body, null = no expansion
 *       Label { ... }                          // header children (default alias)
 *       ExpandChevron { expanded: parent... }  // place the indicator anywhere
 *   }
 *
 * Interaction: a click on the header band toggles `expanded` (buttons and
 * other interactive controls in the header consume their own clicks first),
 * and so do Space, Enter and Return while the row holds keyboard focus. A
 * click inside the expanded body never collapses the row: the body is where
 * the user works. Inside ReorderableColumn the hosting row is the tab stop
 * and forwards those keys to toggle() (see `keyboardHosted`).
 * The expansion body animates via the project's accordion/fade motion
 * profiles, and its Loader stays alive through the collapse animation so the
 * body fades out instead of being torn down mid-transition (see the inline
 * comments, moved as-is from RuleRow).
 */
ItemDelegate {
    id: row

    /// Current expansion state — toggled by the whole-row click. Lives on the
    /// delegate so each row expands independently; resets on page reload.
    property bool expanded: false
    /// Whether the row expands at all. False = the whole-row click is inert
    /// (a row with no body to show, e.g. a parameter-less decoration pack).
    property bool expandable: true
    /// Lazily-instantiated expansion body. Null disables expansion entirely —
    /// the clip stays collapsed and nothing is ever loaded.
    property Component expansionContent: null
    /// Left inset of the expansion body relative to the row edge.
    property real expansionLeftMargin: Kirigami.Units.gridUnit * 2
    /// Header row children — the consumer's title/summary/badges/buttons.
    default property alias headerContent: headerRow.data
    /// Header row spacing (RuleRow's largeSpacing default).
    property alias headerSpacing: headerRow.spacing
    /// Set by a host that owns keyboard focus for the row, which
    /// ReorderableColumn does for every row it loads: its own row item is the
    /// tab stop (Alt+Up/Down acts on it) and it forwards Space, Enter and
    /// Return here through toggle(). A second tab stop on this delegate would
    /// sit between the host row and the row's controls and swallow the
    /// reorder chord, so a hosted row takes no focus of its own. A standalone
    /// row is its own tab stop.
    property bool keyboardHosted: false

    /// Flip the expansion state, as a header click does. Inert on a row that
    /// does not expand.
    function toggle() {
        if (row.expandable)
            row.expanded = !row.expanded;
    }

    hoverEnabled: true
    focusPolicy: keyboardHosted ? Qt.NoFocus : Qt.StrongFocus
    // Handled ahead of AbstractButton's own Space handling, which would emit
    // clicked() and run the press-position test below against a stale mouse
    // position.
    Keys.priority: Keys.BeforeItem
    Keys.onPressed: event => {
        // Only keys pressed on the row itself: one a control in the body
        // leaves unaccepted (Return in a text field) propagates up to here.
        if (row.keyboardHosted || !row.activeFocus || (event.modifiers & ~Qt.KeypadModifier) !== Qt.NoModifier)
            return;
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            event.accepted = true;
            row.toggle();
        }
    }
    Accessible.expandable: row.expandable
    Accessible.expanded: row.expanded
    // A click on the header band toggles expansion. Toolbar buttons, switches
    // and other interactive controls consume their own clicks, so only a
    // click on a passive part of the header (labels, badges, empty space)
    // reaches this handler from there. A click on a passive part of the
    // expanded body (a field label, the gap between controls) also lands
    // here, and must not fold the body away under the user's pointer, so it
    // is judged by where it was pressed. The ExpandChevron's rotation signals
    // the state.
    onClicked: {
        var headerBottom = headerRow.mapToItem(row, 0, headerRow.height).y + Kirigami.Units.smallSpacing / 2;
        if (row.pressY > headerBottom)
            return;
        row.toggle();
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            id: headerRow

            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing
        }

        // ── Expansion area ───────────────────────────────────────────────

        // Clipped container that animates the expansion in/out instead of
        // snapping height between 0 and the full body. Mirrors SettingsCard's
        // expand/collapse pattern: clip:true keeps the loaded body from
        // bleeding above the row during the height interpolation, and
        // PhosphorMotionAnimation on `Layout.preferredHeight` + `opacity`
        // hands the timing off to the project's motion profile (so
        // animation-speed and reduce-motion preferences propagate without
        // a hardcoded duration). The Loader stays `active` for one extra
        // animation cycle when collapsing so the body can fade out
        // gracefully instead of being torn down mid-transition.
        Item {
            id: expansionClip

            // `_active` lags `row.expanded` through the collapse animation by
            // also gating on `Layout.preferredHeight > 0` — Qt's animation
            // updates the height property directly during the transition,
            // so the value is positive while interpolating from full→0 and
            // only reaches 0 when the animation lands. Without this lag the
            // Loader would unload synchronously on collapse-start and the
            // user would see the viewport blank before the height finished
            // shrinking. Re-expanding mid-collapse keeps the Loader active
            // (no reload thrash) because the height never reaches 0.
            property bool _active: row.expansionContent !== null && (row.expanded || Layout.preferredHeight > 0)

            Layout.fillWidth: true
            Layout.leftMargin: row.expansionLeftMargin
            Layout.preferredHeight: row.expanded ? expansionLoader.implicitHeight + Kirigami.Units.smallSpacing : 0
            clip: true
            opacity: row.expanded ? 1 : 0
            visible: Layout.preferredHeight > 0 || opacity > 0

            Behavior on Layout.preferredHeight {
                PhosphorMotionAnimation {
                    profile: row.expanded ? "widget.accordionExpand" : "widget.accordionCollapse"
                    durationOverride: Kirigami.Units.shortDuration
                }
            }

            Behavior on opacity {
                PhosphorMotionAnimation {
                    profile: row.expanded ? "widget.fadeIn" : "widget.fadeOut"
                    durationOverride: Kirigami.Units.veryShortDuration * 2
                }
            }

            // Lazy-loaded expansion body. `active` gates instantiation on the
            // row actually being expanded — collapsed rows pay zero cost.
            Loader {
                id: expansionLoader

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                active: expansionClip._active
                visible: active
                sourceComponent: row.expansionContent
            }
        }
    }
}
