// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
// DecorationSlot hands its own surfaceOpacity, contentItem and surfacePath to
// the decoration it instantiates, and `active` follows the host's
// decorationDrawing report.

import QtQuick
import QtTest
import Phosphor.Widgets

TestCase {
    id: testCase

    name: "DecorationSlot"
    width: 200
    height: 100

    Component {
        id: stubDecoration

        Item {
            property Item contentItem: null
            property string surfacePath: ""
            property real surfaceOpacity: 1
            property bool decorationDrawing: true
        }
    }

    Component {
        id: slotComponent

        Item {
            property alias slot: slot
            property alias frame: frame

            anchors.fill: parent

            Rectangle {
                id: frame

                property bool shaderAnchor: true

                anchors.fill: parent
            }

            DecorationSlot {
                id: slot

                anchors.fill: parent
                component: stubDecoration
                contentItem: frame
                surfacePath: "test.surface"
            }
        }
    }

    function test_surfaceOpacityReachesTheDecoration() {
        const host = createTemporaryObject(slotComponent, testCase);
        verify(host.slot.item);
        compare(host.slot.item.surfaceOpacity, 1);
        host.slot.surfaceOpacity = 0.25;
        compare(host.slot.item.surfaceOpacity, 0.25);
        compare(host.slot.item.contentItem, host.frame);
        compare(host.slot.item.surfacePath, "test.surface");
    }

    function test_activeFollowsDecorationDrawing() {
        const host = createTemporaryObject(slotComponent, testCase);
        verify(host.slot.active);
        host.slot.item.decorationDrawing = false;
        verify(!host.slot.active);
    }
}
