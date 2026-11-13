// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <PhosphorShortcuts/IAdhocRegistrar.h>

#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

namespace PhosphorShortcuts {
class IBackend;
class Registry;
} // namespace PhosphorShortcuts

namespace PlasmaZones {

class Settings;

/**
 * @brief Navigation direction for keyboard navigation
 */
enum class NavigationDirection {
    Left,
    Right,
    Up,
    Down
};

/**
 * @brief Manages global keyboard shortcuts for PlasmaZones.
 *
 * Thin glue layer on top of PhosphorShortcuts::Registry. Owns one entry per
 * PlasmaZones action, wires each entry's current sequence to the matching
 * Settings getter, and fans activation into the domain-specific Q_SIGNALS
 * below. The actual key-grab mechanism (KGlobalAccel / XDG Portal /
 * D-Bus trigger) is selected inside the PhosphorShortcuts library.
 */
class ShortcutManager : public QObject, public PhosphorShortcutsIntegration::IAdhocRegistrar
{
    Q_OBJECT

public:
    explicit ShortcutManager(Settings* settings, QObject* parent = nullptr);
    ~ShortcutManager() override;

    /// The scroll adjust steps in percent of the work area, as the user tuned
    /// them. Public because the registration table's fire lambdas are
    /// capture-less function pointers at namespace scope and reach the manager
    /// only through their ShortcutManager* argument; narrow read-only getters
    /// rather than exposing the mutable Settings* the table would otherwise
    /// need. They also hold the unreachable-null defence in one place instead
    /// of a ternary per call site.
    int scrollColumnWidthStepPercent() const;
    int scrollWindowHeightStepPercent() const;
    /// Unlike its two siblings this one has no fire lambda behind it: the
    /// view STEP is the wheel's, and the ScrollingAdaptor reads it through
    /// the provider the daemon binds to this getter. It lives here with the
    /// other two so every step percent has one home and one null defence.
    int scrollViewScrollStepPercent() const;

public Q_SLOTS:
    void registerShortcuts();
    /// Re-applies current sequences and the gated families' active flags
    /// after a settings save. Flushes and emits cheatsheetModelChanged only
    /// when something actually moved.
    void updateShortcuts();
    void unregisterShortcuts();

public:
    /**
     * Whether the dynamic-workspaces family (the table rows marked
     * workspaceGated and both workspace slot families) may hold grabs. The
     * daemon sets it from the workspace controller's real state, true only
     * while a controller runs, so a feature switched on in settings but held
     * dormant (per-output desktops off, consent not given) parks the family
     * instead of leaving inert system-wide grabs. Defaults to false, which
     * is also what a build with the feature compiled out keeps: the family is
     * still registered and parked there, so its chords stay assignable.
     * A change re-applies the shortcuts like a settings save does.
     */
    void setWorkspaceFamilyActive(bool active);
    bool workspaceFamilyActive() const
    {
        return m_workspaceFamilyActive;
    }

public:
    /**
     * Register an ad-hoc shortcut that lives outside the main settings-driven
     * table. Used by subsystems that need a transient grab bound to a UI state
     * (e.g. the cancel-overlay Escape grab bound while the layout picker or
     * snap assist is showing). Batches with an immediate flush to the backend.
     * Idempotent — re-registering the same id updates the callback in place;
     * the description is updated in the registry record but is not re-sent to
     * the backend for an already-registered id (Registry::bind contract).
     * Ids colliding with the settings-driven table or any indexed slot
     * family's prefix are rejected with a warning: an adhoc unregister on
     * such an id would purge the persistent binding's saved
     * kglobalshortcutsrc record. The named-workspace chords are adhoc by
     * design and are not refused; they are the one adhoc family the
     * cheatsheet catalog lists (see catalogRows).
     */
    void registerAdhocShortcut(const QString& id, const QKeySequence& sequence, const QString& description,
                               std::function<void()> callback) override;

    /**
     * Release an ad-hoc shortcut previously bound via registerAdhocShortcut().
     * Idempotent; unknown ids are ignored.
     */
    void unregisterAdhocShortcut(const QString& id) override;

    /// Batch forms: every entry goes through the per-id path. On the REGISTER
    /// side the backend flush is deferred to one trailing call, so a
    /// multi-chord burst (the six layout-picker navigation grabs) costs one
    /// Portal round-trip instead of superseding its own in-flight Responses.
    /// The unregister side keeps the same bracketed shape for symmetry only:
    /// Registry::unbind applies immediately by contract and flush() does not
    /// include unbinds, so no round-trip is actually coalesced there.
    void registerAdhocShortcuts(
        const QVector<PhosphorShortcutsIntegration::IAdhocRegistrar::AdhocBinding>& bindings) override;
    void unregisterAdhocShortcuts(const QStringList& ids) override;

    /// Foreign-component rebind (Registry pass-through; KGlobalAccel backend
    /// only). The dynamic-workspaces stock-shortcut takeover uses these;
    /// false on the other backends, whose callers fall back to
    /// snap-back-with-hint.
    bool setForeignShortcuts(const QString& componentName, const QString& actionName,
                             const QList<QKeySequence>& sequences);
    /// Tri-state read, see PhosphorShortcuts::IBackend::foreignShortcuts. A
    /// nullopt means the query FAILED (no registry yet, wrong backend,
    /// kglobalacceld unreachable) and says nothing about the action's real
    /// binding; an engaged empty list means the action is genuinely unbound.
    /// Callers must not treat the two the same: clearing a chord on the
    /// strength of a failed query loses the user's binding with no backup.
    std::optional<QList<QKeySequence>> foreignShortcuts(const QString& componentName, const QString& actionName) const;

    /**
     * Catalog of every settings-driven shortcut for the cheatsheet overlay,
     * one QVariantMap per row, sorted by display category, then by authored
     * row order within it:
     *   id (QString), label (translated QString),
     *   category (translated QString), categoryOrder (int),
     *   rowOrder (int — the row's authoring position inside its category;
     *   the in-category sort key, with 9000 pinning the digit families to
     *   the end of their group),
     *   triggers (QStringList — the user's EFFECTIVE keys via backend
     *   read-back, falling back to the config value), assigned (bool),
     *   mode ("all" | "snapping" | "autotile" | "scrolling" | "layouts" |
     *   "managed" — which tiling mode the action is meaningful in; the
     *   overlay filters on it. "layouts" is a capability tag rather than a
     *   mode: it marks the layout-selection actions shown only when the
     *   screen's engine provides layouts. "managed" is the union of the two
     *   engine modes, autotile or scrolling, for a row that acts on either
     *   and is a no-op on snapping — see the catalog's contract block),
     *   description (translated QString — plain-prose explanation shown as
     *   the row's tooltip; always present, empty when the action needs none),
     *   templatesDescription (translated QString — Templates-capability
     *   variant of description, falling back to it when the row has none).
     * Ad-hoc/transient grabs never appear, except the named-workspace
     * chords (one row each, under Workspaces). Parked rows never appear
     * either: a family whose feature is off, or the workspace focus pair
     * while KWin keeps its stock chords, holds no grab and nothing the user
     * presses reaches it. Empty before registerShortcuts() and again after
     * unregisterShortcuts() (the daemon stop path).
     */
    QVariantList cheatsheetModel() const;

    /**
     * The same catalog UNCOMPRESSED: one row per registered action, with
     * the same keys as cheatsheetModel() and the same category / row
     * ordering, but no family folding, so a consumer that places chords
     * spatially (the Phosphor shell cheatsheet drawn on the placement map)
     * sees move_window_left and move_window_right as two rows with their
     * own triggers. Served over org.phosphor_works.Placement.Control.getShortcutsJson.
     */
    QVariantList shortcutCatalog() const;

    /// One collapsible cheatsheet family: parallel id / expected-final-token
    /// lists, the combined row label, the tail token for the merged chip,
    /// and an optional combined tooltip for the merged row.
    struct CheatsheetFamily
    {
        QStringList ids;
        QStringList expectedLastTokens;
        QString combinedLabel;
        QString tailToken;
        // Optional tooltip for the merged row. The merged row otherwise
        // keeps the FIRST member's description, which for an opposed pair
        // describes only one direction; a family that carries per-member
        // descriptions should supply a combined wording here. Empty = keep
        // the first member's.
        //
        // The default-member-initializer (= {}) is load-bearing, same as
        // KeyDef's trailing members: the directional/digit FamilySpecs are
        // 4-field aggregate initializers that omit this trailing field, and
        // GCC's -Wmissing-field-initializers fires per init site when an
        // omitted field lacks an NSDMI.
        QString combinedDescription = {};
    };

    /**
     * Every action id in the STATIC registration table
     * (shortcutmanager_table.cpp), in declaration order. The indexed slot
     * families (kIndexedSlotCount ids each) are registered separately by
     * buildEntries() and are NOT returned here — this is the static portion
     * of the registration surface, not all of it.
     *
     * The table is an array in its own TU reached only through
     * ShortcutTable::staticEntries(), and
     * cheatsheetModel() is a COMPRESSED view of it (a directional quad or a
     * digit family collapses into a single row, so its later members have no
     * rows of their own). Neither
     * can enumerate the registration surface, which the Shortcuts.Scrolling
     * parity check needs in order to compare it against the config schema.
     */
    static QStringList staticShortcutIds();

    /**
     * Pure family-compression pass over cheatsheet rows (static so tests can
     * drive it without a shortcut backend). A family collapses into one row
     * when every member is assigned, carries exactly one trigger, ends in its
     * expected token, and shares the modifier prefix. A family whose members
     * are ALL unassigned collapses too, into one unassigned row, so an
     * unbound slot family reads as one line rather than a wall of empty
     * ones. Any other deviation keeps the individual rows. Returns the
     * surviving rows, unsorted.
     */
    static QVector<QVariantMap> compressCheatsheetFamilies(QVector<QVariantMap> rows,
                                                           const QVector<CheatsheetFamily>& families);

    /**
     * Test-only backend injection. Must be called before registerShortcuts()
     * (which otherwise creates the real platform backend lazily); a call
     * arriving after registration asserts in debug builds and releases the
     * live registration first in release builds. Lets tests observe the
     * exact IBackend calls the manager makes — in particular that
     * unregisterShortcuts() never purges persistent bindings via
     * IBackend::unregisterShortcut (discussion #851).
     *
     * unregisterShortcuts() DESTROYS the injected backend along with the
     * registry; a later registerShortcuts() falls back to the real platform
     * backend (KGlobalAccel / Portal / session bus) from inside the test.
     * Re-inject after every teardown before re-registering.
     */
    void setBackendForTesting(std::unique_ptr<PhosphorShortcuts::IBackend> backend);

Q_SIGNALS:
    /**
     * The cheatsheet catalog's contents changed: a sequence was rebound
     * (in-process or externally via System Settings / compositor), a gated
     * family was parked or revived, a named-workspace chord was bound or
     * released, or the registration batch settled. Consumers re-query
     * cheatsheetModel().
     *
     * Settings saves emit this only when a sequence or an active flag
     * actually moved; external rebinds surface through the
     * Registry::triggersChanged relay (filtered to catalogued ids), which
     * depends on the backend reporting them (KGlobalAccel does, the D-Bus
     * fallback backend does not) — on a non-reporting backend an open sheet
     * reflects an external rebind on its next show rather than live.
     */
    void cheatsheetModelChanged();

    // Dynamic per-monitor workspaces (delta -1 = up, +1 = down; the daemon
    // resolves the acting screen and forwards to the WorkspaceController).
    void workspaceFocusRequested(int delta);
    void workspaceMoveWindowRequested(int delta);
    void workspaceMoveColumnRequested(int delta);
    void workspaceReorderRequested(int delta);
    /// Only NavigationDirection::Left or Right is ever emitted.
    void workspaceMoveToMonitorRequested(NavigationDirection direction);
    /// Quick-shortcut slot (1-based): send the active window to the named
    /// workspace the settings app assigned to that slot. Not positional: the
    /// target is the same workspace whichever monitor acts.
    void workspaceMoveSlotRequested(int slot);
    /// Switch the acting monitor to workspace `slot` (1-based) of its own
    /// list (focus-slot family, positional per monitor).
    void workspaceFocusSlotRequested(int slot);

    void openEditorRequested();
    void openSettingsRequested();
    void toggleCheatsheetRequested();
    void previousLayoutRequested();
    void nextLayoutRequested();
    void quickLayoutRequested(int number);

    void moveWindowRequested(NavigationDirection direction);
    void spanWindowRequested(NavigationDirection direction);
    void focusZoneRequested(NavigationDirection direction);
    void pushToEmptyZoneRequested();
    void restoreWindowSizeRequested();
    void toggleWindowFloatRequested();
    void switchFocusFloatTilingRequested();
    void swapWindowRequested(NavigationDirection direction);
    void snapToZoneRequested(int zoneNumber);
    void rotateWindowsRequested(bool clockwise);
    void cycleWindowsInZoneRequested(bool forward);
    void resnapToNewLayoutRequested();
    void snapAllWindowsRequested();
    void layoutPickerRequested();
    void toggleLayoutLockRequested();

    void toggleAutotileRequested();
    void focusMasterRequested();
    void swapWithMasterRequested();
    void increaseMasterRatioRequested();
    void decreaseMasterRatioRequested();
    void increaseMasterCountRequested();
    void decreaseMasterCountRequested();
    void retileRequested();

    void swapVirtualScreenRequested(NavigationDirection direction);
    void rotateVirtualScreensRequested(bool clockwise);

    // Scrolling-mode column vocabulary. Directional focus/move/swap reuse
    // the generic navigation signals above; these are the scroll-specific
    // verbs. Direction deltas (consumeOrExpel, cyclePresets) carry -1 =
    // left/back, +1 = right/forward; the two ADJUST signals instead carry a
    // signed PERCENT of the work area, one step per keypress. The step is the
    // user-tunable scrollingColumnWidthStepPercent /
    // scrollingWindowHeightStepPercent setting (1-50, default 10), read
    // through the narrow getters above.
    void scrollFocusColumnEndRequested(bool last);
    void scrollMoveColumnToEndRequested(bool last);
    void scrollConsumeWindowRequested();
    void scrollExpelWindowRequested();
    void scrollConsumeOrExpelRequested(int delta);
    void scrollCenterColumnRequested();
    void scrollToggleColumnTabbedRequested();
    void scrollCycleTabRequested(int delta);
    /// One of the tab-ordinal slots fired; @p ordinal is 1-based.
    void scrollFocusTabRequested(int ordinal);
    void scrollToggleWindowedFullscreenRequested();
    void scrollCycleColumnWidthRequested(int delta);
    void scrollAdjustColumnWidthRequested(int deltaPercent);
    void scrollMaximizeColumnRequested();
    void scrollMaximizeToEdgesRequested();
    void scrollExpandColumnRequested();
    void scrollCycleWindowHeightRequested(int delta);
    void scrollAdjustWindowHeightRequested(int deltaPercent);
    void scrollMaximizeWindowHeightRequested();
    void scrollExpandWindowRequested();
    void scrollCenterVisibleColumnsRequested();
    /// false = top of the column, true = bottom.
    void scrollFocusWindowEndRequested(bool bottom);
    /// Edge-stop adjacent-column focus (the generic focus chords cross
    /// monitors instead). delta -1 = left, +1 = right; same for the wrap
    /// variant, which falls through to the far end at the strip edge.
    void scrollFocusColumnPlainRequested(int delta);
    void scrollFocusColumnWrapRequested(int delta);
    /// true = move the focused window to the float layer, false = re-tile it.
    void scrollMoveToFloatRequested(bool floating);
    /// Pan the strip view WITHOUT moving focus, by a signed PERCENT of the
    /// work area's extent along the strip. The page pair carries a whole
    /// viewport (100); negative is toward the strip's start. The step-sized
    /// pan has no keyboard row (it is the wheel's), so no emitter carries
    /// the step percent.
    void scrollViewRequested(int deltaPercent);
    void scrollEqualizeColumnWidthsRequested();
    void scrollMinimizeColumnWidthRequested();
    void scrollEqualizeWindowHeightsRequested();
    void scrollMinimizeWindowHeightRequested();

private:
    struct Entry
    {
        QString id;
        QKeySequence defaultSeq;
        QString description;
        // Dynamic-workspaces family: active only while
        // m_workspaceFamilyActive (see entryActive()).
        bool workspaceGated = false;
        // The workspace focus pair, whose defaults are KWin's own stock
        // "Switch One Desktop Up/Down" chords: parked while the stock
        // takeover is off and the chord is still the stock one, because KWin
        // registered first and wins the key (see entryActive()).
        bool sharesKWinStockChord = false;
        // Reads the current key sequence from m_settings (handles per-slot
        // getters for the quick-layout / snap-to-zone arrays).
        std::function<QKeySequence()> currentSeq;
        // Emits the domain Q_SIGNAL corresponding to this id.
        std::function<void()> fire;
    };

    // Adhoc registration deferred because the initial settings-driven
    // registration batch was in flight when the caller arrived. Drained from
    // the Registry ready() callback so subsystems that bind shortcuts in
    // response to user actions (e.g. the cancel-overlay Escape grab bound when
    // the layout picker or snap assist first shows) don't silently lose their
    // grab when that overlay appears in the first few hundred ms after daemon
    // startup on a Portal compositor.
    struct PendingAdhocOp
    {
        enum Kind {
            Register,
            Unregister
        };
        Kind kind;
        QString id;
        QKeySequence sequence;
        QString description;
        std::function<void()> callback;
    };

    /// Clears the in-flight flag, replays deferred settings/adhoc work, and
    /// publishes the catalog. Runs from Registry::ready or, if the backend
    /// never answers, from the fallback timer. Settles the batch identified by
    /// @p generation only, and only once, so a late arrival on either path is
    /// a no-op rather than a second publish.
    void settleRegistration(quint64 generation);

    /// How long to wait for the backend's ready() before settling anyway.
    ///
    /// Sized against the slowest HEALTHY backend rather than a typical one:
    /// PortalBackend answers ready() synchronously on its error paths, but on
    /// the happy path it defers to the BindShortcuts Response, which can sit
    /// behind an interactive permission dialog with no latency guarantee (see
    /// portalbackend.cpp's handleBindShortcutsResponse). Settling early would
    /// drain queued adhoc grabs into a still-in-flight batch.
    ///
    /// The cost of waiting is real, so this is not set arbitrarily high:
    /// until it fires, settings rebinds defer and adhoc grabs queue, which
    /// leaves the picker / snap-assist Escape binding dead. Matched to the
    /// compositor-bridge watchdog, the project's other "a healthy system
    /// answered long ago" bound.
    static constexpr int kRegistrationSettleTimeoutMs = 20000;

    /// Identifies the current registration batch. Incremented by every
    /// registerShortcuts()/unregisterShortcuts(), so a fallback timer armed
    /// for an earlier batch cannot settle a later one.
    quint64 m_registrationGeneration = 0;

    void buildEntries();
    /// The catalog rows before family compression, in registration order:
    /// the shared producer behind cheatsheetModel() and shortcutCatalog().
    QVector<QVariantMap> catalogRows() const;
    /// Category blocks in display order, then authored row order within.
    static void sortCatalogRows(QVector<QVariantMap>& rows);
    /// Re-applies every entry's current sequence and active flag; returns
    /// true when any sequence or any active flag actually differed from what
    /// the registry held.
    bool rebindAll();
    /// Whether @p e may hold a grab with @p seq as its current sequence.
    bool entryActive(const Entry& e, const QKeySequence& seq) const;
    /// Delete every record the platform keeps under this component that is
    /// not one of the settings-driven ids: adhoc grabs a crashed earlier run
    /// left behind (kglobalacceld keeps a dead client's grab for the whole
    /// session) and ids since renamed or removed. Runs once per
    /// registerShortcuts(), before anything adhoc can be bound.
    void purgeStaleRecords();
    /// Whether @p id is an adhoc id the catalog lists (a named-workspace
    /// chord), so binding or releasing it changes the cheatsheet.
    static bool isCataloguedAdhocId(const QString& id);
    /// Emits cheatsheetModelChanged when an adhoc op since the last call
    /// touched a catalogued id.
    void publishAdhocCatalogChange();
    /// updateShortcuts' body. @p deferFlush leaves the rebinds pending AND
    /// the cheatsheetModelChanged emit to the caller — settleRegistration
    /// coalesces the flush with the drained adhoc ops into ONE backend round
    /// trip (a second flush supersedes the first's in-flight portal Request,
    /// and a superseded request whose RPC errors loses its grabs outright)
    /// and emits only after that flush, since the model prefers the
    /// backend's read-back.
    bool applyShortcutUpdates(bool deferFlush);
    /// Returns true when it drained ops and issued the trailing flush.
    bool drainPendingAdhocOps();
    /// Drop every queued adhoc op for @p id. Both queueing paths supersede an
    /// earlier op for the same id (last write wins), so they share this.
    void erasePendingAdhocOps(const QString& id);

    Settings* m_settings = nullptr;

    std::unique_ptr<PhosphorShortcuts::IBackend> m_backend;
    std::unique_ptr<PhosphorShortcuts::Registry> m_registry;

    QVector<Entry> m_entries;
    QVector<PendingAdhocOp> m_pendingAdhocOps;

    bool m_registrationInProgress = false;
    bool m_settingsDirty = false;
    bool m_workspaceFamilyActive = false;
    // Set when an adhoc bind/unbind touched a catalogued id, consumed by
    // publishAdhocCatalogChange() after the flush that carried it.
    bool m_adhocCatalogDirty = false;
    // Set by the adhoc batch forms while their loop runs the per-id paths,
    // so those paths skip their trailing flush and the batch flushes once.
    bool m_suppressAdhocFlush = false;
};

} // namespace PlasmaZones
