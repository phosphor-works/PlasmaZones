// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_export.h"
#include <QObject>
#include <functional>
#include <QString>
#include <QStringList>

namespace PhosphorZones {
class Zone;
class LayoutRegistry;
}

namespace PhosphorEngineRuntime {

class WindowTracker;
class SnapService;

} // namespace PhosphorEngineRuntime
namespace PhosphorEngine {
class IPlacementEngine;
}
namespace PhosphorEngineRuntime {

/**
 * @brief The high-level convenience API: the service behind
 *        org.phosphor_works.Placement.Control
 *
 * Bound to D-Bus by ControlAdaptor (dbus/controladaptor.h), which also
 * carries the two members that are about the process rather than placement:
 * the support report and quit.
 *
 * Thin facade over existing adaptors for third-party integrations and scripts.
 * Provides one-call operations, API version negotiation, and a complete
 * state snapshot for monitoring tools.
 */
class PLASMAZONES_EXPORT ControlService : public QObject
{
    Q_OBJECT

public:
    explicit ControlService(WindowTracker* wta, SnapService* snapService, PhosphorZones::LayoutRegistry* layoutManager,
                            PhosphorEngine::IPlacementEngine* autotileEngine, QObject* parent = nullptr);
    ~ControlService() override = default;

    /// Null every borrowed pointer. Called from EngineRuntime::stop() before the
    /// unique_ptr members that own these objects run their destructors
    /// (the adaptor is Qt-parented to the daemon, so ~QObject — which
    /// destroys us — would otherwise run AFTER those unique_ptrs, leaving
    /// every m_* member dangling). Each public slot guards on null so the
    /// adaptor degrades to a no-op instead of UAF when a queued D-Bus
    /// call lands post-detach.
    void detach();

public Q_SLOTS:
    // ═══════════════════════════════════════════════════════════════════════════
    // Version and capabilities
    // ═══════════════════════════════════════════════════════════════════════════
    // High-level operations
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Snap a window to a specific zone in a layout
     * @param windowId Window to snap
     * @param zoneNumber Zone number (1-based; any number the layout has)
     * @param screenId Screen to snap on, a screen id or connector name; empty
     *        means the screen the window is on
     * @note Refused unless a keyboard snap could make the same move
     */
    void snapWindowToZone(const QString& windowId, int zoneNumber, const QString& screenId);

    /**
     * @brief Switch a screen between snapping and autotile, the way the
     *        mode-toggle shortcut does on that screen
     * @param screenId Screen id or connector name; a split monitor's id
     *        resolves to the virtual screen the cursor is in
     * @note Runs the handler setModeToggleHandler installs: the screen's
     *       current desktop and activity, the layout or algorithm it had
     *       before, no switch into a mode that is turned off, and the
     *       disabled-context notice. A scrolling screen goes to autotile.
     */
    void toggleAutotileForScreen(const QString& screenId);

    /**
     * @brief Get a complete state snapshot
     * @return JSON object: the layouts, the active layout, the tracked windows and the autotile state
     */
    QString getFullState();

    /**
     * @brief Every settings-driven shortcut as a JSON array, for the
     *        Phosphor shell's keybind cheatsheet.
     *
     * One object per action, uncompressed (a directional quad is four
     * rows): id, label, description, category, categoryOrder, rowOrder,
     * triggers (array of PortableText key sequences, the user's effective
     * bindings), assigned, mode ("all" | "snapping" | "autotile" |
     * "scrolling" | "layouts" | "managed"). "[]" before the daemon has
     * registered its shortcuts or after it has released them. Body in
     * controlservice_shortcuts.cpp.
     */
    QString getShortcutsJson();

Q_SIGNALS:
    /// The catalog behind getShortcutsJson changed: a sequence was rebound
    /// or the registration batch settled. Consumers re-read.
    void shortcutsChanged();

public:
    /// The catalog source. ShortcutManager lives in the daemon, not in the
    /// core library this adaptor ships in, so the daemon hands the read in
    /// as a callable rather than a type. Cleared by detach().
    void setShortcutCatalogProvider(std::function<QVariantList()> provider);
    /// The mode switch toggleAutotileForScreen runs on a resolved screen: the
    /// daemon's toggleScreenMode, which this library cannot name. Cleared by
    /// detach().
    void setModeToggleHandler(std::function<void(const QString& screenId)> handler);
    /// Relay for ShortcutManager::cheatsheetModelChanged: emits
    /// shortcutsChanged on the bus. Not a slot on purpose, so it is not a
    /// D-Bus method.
    void notifyShortcutsChanged();

private:
    WindowTracker* m_wta;
    SnapService* m_snapService;
    PhosphorZones::LayoutRegistry* m_layoutManager;
    PhosphorEngine::IPlacementEngine* m_autotileEngine;
    std::function<QVariantList()> m_shortcutCatalog;
    std::function<void(const QString& screenId)> m_modeToggle;
};

} // namespace PhosphorEngineRuntime
