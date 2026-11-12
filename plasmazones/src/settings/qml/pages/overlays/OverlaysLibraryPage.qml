// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import org.phosphor.control

/**
 * @brief Appearance → Overlays → Library → Shaders — installed overlay
 * shader-pack browser.
 *
 * Thin wrapper around the pack-agnostic `ShaderBrowserPage`. The actual
 * UI (drop zone, filter bar, grouped card grid, detail dialog) lives in
 * the shared component; this file only provides the bridge (the
 * OverlaysPageController surface) and overlay-domain copy.
 *
 * Shader assignment lives on Overlays → Layouts
 * (OverlaysAssignmentsPage — global default + per-layout
 * overrides); this page exists so users can survey what's installed,
 * see parameter metadata, and jump to the user shader directory to drop
 * in their own packs.
 */
ShaderBrowserPage {
    id: page

    // PageHost sets both before the page's bindings run, because this page's
    // registry entry passes context (PageRegistry::Entry::passesContext).
    // settingsApp is declared even though this page does not read it: PageHost
    // passes it to every such page, and assigning it to a page that lacks the
    // property logs an error on every build.
    required property QtObject controller
    required property ApplicationController settingsApp

    bridge: page.controller
    settingsCategory: "OverlaysLibraryFilterBar"
    // Named by its place in the sidebar: Snapping has a Layouts page too, and
    // that one is not where overlays are assigned.
    infoBannerText: i18nc("@info banner on the overlay shader browser", "Browse the zone overlay shaders you have installed. To assign one, either as the global default or for a single layout, go to Overlays → Layouts.")
    // Closures (not pre-evaluated strings) so `i18ncp` runs with the
    // LIVE usage count — required for locales with more than two plural
    // forms and so `%n` displays the actual count rather than a baked-in
    // 1 / 2 sentinel.
    usageHeaderTextFn: function (count) {
        return i18ncp("@info shader usage section header (snapping overlay)", "Used by %n layout", "Used by %n layouts", count);
    }
    usageChipTextFn: function (count) {
        return i18ncp("@info shader usage chip (snapping overlay)", "%n layout", "%n layouts", count);
    }
    // Both strings above count LAYOUTS, and the usage list also carries the
    // "Global default" row, which is not one. The dialog still lists that
    // row; only the count leaves it out. A shader that is only the global
    // default counts 0, so its chip shows that row's label and the dialog
    // lists the row with no count line.
    usageCountFn: function (effectId, usages) {
        return page.controller.shaderEffectLayoutUsageCount(effectId);
    }
}
