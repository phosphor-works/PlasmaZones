// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

import QtQuick

// The page every test module's pages and sections show. Never loaded by a
// test, only checked to exist. It still declares what PageHost sets on a
// module page (controller, settingsApp) and what FacetExtensionFrame sets on
// a section (domain), so it would load cleanly as either.
Item {
    property QtObject controller: null
    property QtObject settingsApp: null
    property QtObject domain: null
}
