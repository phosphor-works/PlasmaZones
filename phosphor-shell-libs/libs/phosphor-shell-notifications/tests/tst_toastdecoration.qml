// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
// A toast's card fades on its own opacity as it is swiped. A surface pack
// captures the card from below that opacity, so the toast forwards it to the
// decoration as surfaceOpacity.

import QtQuick
import QtTest
import Phosphor.Notifications

TestCase {
    id: testCase

    name: "ToastDecoration"
    width: 480
    height: 200

    Component {
        id: stubDecoration

        Item {
            property Item contentItem: null
            property real surfaceOpacity: 1
        }
    }

    Component {
        id: toastComponent

        Toast {
            width: 400
            summary: "Summary"
            decoration: stubDecoration
        }
    }

    function findDecoration(item) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i];
            if (child.contentItem !== undefined && child.surfaceOpacity !== undefined && child.component === undefined)
                return child;
            const found = findDecoration(child);
            if (found)
                return found;
        }
        return null;
    }

    function test_cardFadeReachesTheDecoration() {
        const toast = createTemporaryObject(toastComponent, testCase);
        verify(toast);
        const decoration = findDecoration(toast);
        verify(decoration);
        const card = decoration.contentItem;
        verify(card);
        card.opacity = 0.4;
        compare(decoration.surfaceOpacity, 0.4);
    }
}
