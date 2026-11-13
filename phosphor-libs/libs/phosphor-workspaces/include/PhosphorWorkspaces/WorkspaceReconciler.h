// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorWorkspaces/WorkspaceMap.h>
#include "phosphorworkspaces_export.h"

#include <functional>

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace PhosphorWorkspaces {

/// One declared named workspace (config-driven; persistent while empty).
struct PHOSPHORWORKSPACES_EXPORT NamedWorkspace
{
    /// Persistent identity of the declaration, minted by whoever adds it to
    /// the config and kept across renames. The reconciler matches a realized
    /// desktop by this first, so a rename renames the desktop in place
    /// instead of creating a new one. Empty for a declaration written before
    /// ids existed, which is then matched by name.
    QString id;
    QString name; ///< unique, non-empty
    QString outputId; ///< pinned screen; empty = unpinned
    /// Preferred slice index, enforced on every apply; -1 = no preference
    /// (created before the trailing empty and then left where it is).
    int position = -1;
};

/// The dynamic-workspace lifecycle state machine. Owns the WorkspaceMap and the
/// pending-op ledger (echo safety). All inputs are plain method calls — no
/// D-Bus, no daemon types — so unit tests drive it with scripted sequences.
/// Outputs are signals the controller wires to VirtualDesktopManager (KWin
/// calls), the engines, and the state-file persistence.
///
/// Echo-safety contract: every create, remove and per-screen switch the
/// reconciler requests is recorded in the ledger FIRST. Name pushes
/// (requestSetDesktopName) are not ledgered: they carry no structural
/// consequence, and their retry is bounded by their own push record instead.
/// Incoming notifications try to match a ledger entry; matched notifications
/// update the map but trigger no reactive policy. Unmatched notifications are
/// external and get the full policy response. A create or remove KWin has
/// ECHOED stays open until the settled list confirms it, so the structural
/// window (hasPendingStructuralOps) closes only once the numbering the verbs
/// resolve against is the new one.
///
/// Entries expire after ledgerTimeoutMs() with a resyncRequested(), which
/// re-pulls KWin's global desktop list. That pull carries no per-output
/// state, so an expired per-screen switch is followed up here instead: the
/// screen is re-evaluated against the owner-wins rule, within a per-screen
/// budget (MaxSetCurrentRefusals).
///
/// Nothing runs policy before adoptAll(): until then the map is a candidate
/// restored from the state file, and creating, destroying or correcting
/// against it would act on ownership nobody has reconciled yet.
class PHOSPHORWORKSPACES_EXPORT WorkspaceReconciler : public QObject
{
    Q_OBJECT

public:
    explicit WorkspaceReconciler(QObject* parent = nullptr);

    static constexpr int LedgerTimeoutMs = 2000;
    static constexpr int DestroyDebounceMs = 300;
    /// How many times a removal KWin never answered is re-armed before the
    /// desktop is left alone. A genuinely refused removal (KWin declining to
    /// drop that desktop) would otherwise re-arm, re-issue, expire and resync
    /// forever; after this many rounds the surplus empty desktop simply stays.
    /// The budget is restored the moment the desktop's population changes or
    /// the desktop actually goes away.
    static constexpr int MaxRemovalRefusals = 3;
    /// How many expiry passes in which a screen's unnamed create went
    /// unanswered before that screen stops asking. The mirror of
    /// MaxRemovalRefusals, and for the same reason: expireLedger re-runs
    /// maintenance after a Create expiry so a transient stall retries, and
    /// without a budget a create KWin permanently refuses becomes one D-Bus
    /// call per ledger timeout for the life of the session. Named creates
    /// spend a budget of the same size kept per declaration instead. Every
    /// budget is restored when any create lands (KWin's ceiling is global, so
    /// one answered create is proof for every screen), when the desktop count
    /// drops, and when the screen or declaration goes away.
    static constexpr int MaxCreateRefusals = 3;
    /// How many per-screen corrections went unechoed or were refused before
    /// that screen's owner-wins corrections stop. Each expiry or refusing
    /// answer re-evaluates the screen, so a switch the compositor cannot
    /// perform would otherwise snap back forever. Restored by any report for
    /// the screen other than a refusing answer.
    static constexpr int MaxSetCurrentRefusals = 3;
    /// KWin's desktop ceiling (VirtualDesktopManager::maximum(), 25 in KWin
    /// 6.8). Shared by the daemon's gate and the settings app's cap badge. It
    /// is the STARTING value only: a lower ceiling is learned from the
    /// compositor at runtime, in expireLedger, and forgotten again as soon as
    /// a create succeeds or the live count exceeds it.
    static constexpr int DefaultDesktopCap = 25;
    /// How many Create expiries in one episode (the id list unchanged
    /// throughout) it takes before the ceiling is believed. KWin refuses a
    /// create past its maximum with no reply at all, and so does a stalled bus,
    /// so a single unanswered create is not evidence of a cap — see
    /// expireLedger.
    static constexpr int CapProbeExpiries = 2;
    /// The probe concludes on its CapProbeExpiries-th expiry, and every expiry
    /// before that one has to be followed by a re-drive for there to be a next
    /// one. A screen's create budget refuses the re-drive once it is spent, so
    /// a budget smaller than the probe would end every episode unconcluded.
    static_assert(CapProbeExpiries <= MaxCreateRefusals, "the cap probe needs a re-drive between its expiries");
    /// How many times a KWin rename that never took effect is re-pushed before
    /// the desktop is left with the name it has. The mirror of
    /// MaxRemovalRefusals, for the same reason: applyNamedWorkspaces runs on
    /// every settle, so a permanently declined rename would re-fire forever.
    /// The budget is restored when KWin's names agree, when the name reverts to
    /// dynamic, and when the desktop goes away.
    static constexpr int MaxNamePushRefusals = 3;

    const WorkspaceMap& map() const;
    quint64 generation() const;

    /// Load a state-file payload as the candidate map adoption reconciles.
    /// Parsed before anything is replaced, so a malformed or wrong-version
    /// file returns false and changes nothing. `canonicalScreenId`, when
    /// given, normalizes every restored home stamp into the caller's screen id
    /// space, and a stamp that then names the entry's own owner is dropped
    /// (the entry is not displaced). Announces through mapChanged.
    bool restoreState(const QString& json, const std::function<QString(const QString&)>& canonicalScreenId = {});
    /// True once adoptAll has accepted a world. Policy (maintenance, the
    /// destroy sweep, owner-wins) is suspended until then.
    bool isAdopted() const;

    /// KWin's desktop-count ceiling as the reconciler currently believes it.
    int desktopCap() const;
    /// The configured ceiling. Trailing-empty appends are suspended at the cap
    /// (capReached() hints once per episode). A learned ceiling below this
    /// value self-heals back to it, never to DefaultDesktopCap. Nothing in the
    /// daemon calls this; tests use it to build a small-cap world.
    void setDesktopCap(int cap);
    /// Ledger expiry and destroy debounce, in milliseconds. Defaults are
    /// LedgerTimeoutMs and DestroyDebounceMs; tests shorten them so the
    /// time-driven paths run in a fraction of the real timeouts.
    void setTimeouts(int ledgerTimeoutMs, int destroyDebounceMs);
    int ledgerTimeoutMs() const;
    int destroyDebounceMs() const;
    /// Fallback for the screen that adopts externally created desktops and
    /// realizes unpinned named workspaces. Consulted only when the provider
    /// below is unset or answers a screen the map does not know.
    void setFocusedScreen(const QString& screenId);
    /// Pull provider for the focused screen, asked at the moment a desktop
    /// needs a home. Its answer must be in the map's screen id space. The
    /// provider usually captures daemon objects, so the owner clears it
    /// (passes an empty function) before those go away.
    void setFocusedScreenProvider(std::function<QString()> provider);
    /// KWin's default label for the desktop at a 1-based position (KWin's own
    /// defaultName(), translated in KWin's catalog). Pushed wherever the
    /// reconciler means "unnamed", and used to tell a generated positional
    /// label from a name a user chose. Unset, the untranslated
    /// "Desktop <n>" form VirtualDesktopManager also uses is assumed.
    void setDefaultDesktopNameProvider(std::function<QString(int)> provider);

    // ── Inputs (notifications; plain calls) ─────────────────────────────────
    /// Authoritative ordered KWin id list settled (from desktopListChanged).
    /// Re-homes each screen's current desktop through the id delta (a removed
    /// current desktop moves to KWin's successor), repairs the map, adopts
    /// unowned ids, then runs invariant maintenance. `rawNames`, when given,
    /// must be KWin's raw names aligned with `ids`; they let the reconciler
    /// refresh positional labels a mid-list create or remove left stale, and a
    /// names-only change is acted on even when the id list is unchanged.
    void onDesktopListSettled(const QStringList& ids, const QStringList& rawNames = {});
    /// Early id-only echo of a KWin desktop creation (position and name
    /// unknown); matches the oldest open create.
    void onKwinDesktopCreated(const QString& desktopId);
    /// Echo of a KWin desktop creation with KWin's own position (0-based) and
    /// name, as its desktopCreated signal carries them. Every create of ours
    /// carries a name KWin keeps verbatim, so an echo with any other name is
    /// external, and the position picks among several open creates. A
    /// negative position or a null name means unknown.
    void onKwinDesktopCreatedAt(const QString& desktopId, int position, const QString& name);
    /// Early id-only echo of a KWin desktop removal.
    void onKwinDesktopRemoved(const QString& desktopId);
    /// KWin's answer to one of our createDesktop calls, with the call's own
    /// arguments (VirtualDesktopManager::createDesktopAnswered). KWin echoes a
    /// create before it returns, so `answered` with our create still
    /// un-echoed is a REFUSAL: KWin declines a create only at its maximum,
    /// and the ceiling is learned on the spot. `answered` false is a stall
    /// (an error or no reply in time): the create stays open to its deadline,
    /// since the echo can still land, and its expiry teaches the cap probe
    /// nothing.
    void onCreateDesktopAnswered(uint position, const QString& name, bool answered);
    /// KWin's answer to one of our removeDesktop calls. Answered while the
    /// removal is still un-echoed is a refusal (KWin keeps its last desktop
    /// and ignores an id it does not know); unanswered is a stall.
    void onRemoveDesktopAnswered(const QString& desktopId, bool answered);
    /// KWin's raw name for a desktop changed (VirtualDesktopManager::
    /// desktopRenamed), including the echo of a name we pushed. A named
    /// workspace's desktop given a name a user chose is reported through
    /// namedWorkspaceRenamedInKWin; a dynamic desktop's stale positional
    /// label is refreshed.
    void onKwinDesktopRenamed(const QString& desktopId, const QString& rawName);
    /// A screen's current desktop report (1-based int, effect path), resolved
    /// at once against the settled id list, which must already be the list the
    /// report's numbering refers to. A report past the end of that list is
    /// kept and resolved by the next settle. Returns true when the report was
    /// a matched echo of our own SetCurrent (the caller then skips its own
    /// reactive policy for it).
    ///
    /// The effect answers every switch request exactly once, so a report for
    /// a screen with a SetCurrent open that names a DIFFERENT desktop is the
    /// answer that the switch did not happen: the entry is retired at once
    /// (not left to block the screen until it expires), a refused correction
    /// spends one of the screen's MaxSetCurrentRefusals like an expiry does,
    /// and the report then gets the normal owner-wins policy for what the
    /// screen shows. A user switch that replaced earlier presses first lets
    /// the answers to those presses go by.
    bool onScreenDesktopReport(const QString& screenId, int desktop);
    /// A screen's current desktop as seeded rather than switched to (a replug
    /// or an id rename re-reporting where the output already is). Recorded
    /// like a report, and the owner-wins rule applies, but it is never taken
    /// as the echo of a switch we asked for, and it never claims an external
    /// desktop for the screen.
    void onScreenDesktopSeeded(const QString& screenId, int desktop);
    /// Window population for a desktop id changed (controller-maintained).
    void onPopulationChanged(const QString& desktopId, int windowCount);
    void onScreenAdded(const QString& screenId);
    void onScreenRemoved(const QString& screenId);
    /// A screen's id changed while the output stayed connected (a bare id
    /// flipping to its connector-qualified spelling). Re-keys the slice, the
    /// home stamps and every per-screen record in place. Must be called
    /// INSTEAD of a remove/add pair, which would stamp the slice displaced
    /// and then, for the last screen, drop it.
    void onScreenRenamed(const QString& fromScreenId, const QString& toScreenId);
    /// Screen order recomputed by the controller (geometry left-to-right).
    void onScreenOrderChanged(const QStringList& order);

    /// First-run / restart adoption (plan §4.3): distribute `ids` (KWin global
    /// order) over screens given each screen's current desktop id. Existing
    /// map content is kept where consistent (restore path passes a pre-loaded
    /// map), and a restored entry whose home screen is connected again goes
    /// home. Refused (false, nothing changed, still unadopted) for an empty id
    /// list or an empty screen order: KWin never has zero desktops, so either
    /// is a failed read, and adopting it would wipe the candidate map.
    ///
    /// Each still-unowned desktop goes, in this order: to the screen showing
    /// it; to `preferredScreenByDesktopId[id]` when that names a screen in the
    /// order (the caller passes the screen holding most of the desktop's
    /// windows, in the map's screen id space); otherwise to the owner of the
    /// nearest preceding owned desktop in KWin order, a leading run to the
    /// first screen.
    bool adoptAll(const QStringList& ids, const QHash<QString, QString>& currentDesktopIdByScreen,
                  const QHash<QString, QString>& preferredScreenByDesktopId = {});

    /// Reconcile the declared named workspaces against the map (called after
    /// adoption and on every declaration change; implemented in
    /// WorkspaceReconcilerNamed.cpp). Per declaration, in row order:
    /// the entry carrying the declaration's id is kept (and renamed in place
    /// when the declared name changed); else an entry carrying the name is
    /// kept and stamped with the id; else an unnamed desktop whose KWin name
    /// matches (`kwinNames` is aligned with the settled id list) is claimed;
    /// else a desktop is created with the name (cap-guarded and budgeted, so a
    /// refusal logs and hints, never crashes). A realized workspace is moved
    /// to its pinned output, and explicit positions are laid out per screen
    /// in (position, row) order. Map names no declaration claimed revert to
    /// dynamic, and KWin's name for them is reset to its positional default.
    ///
    /// `kwinNames` MUST be KWin's raw names (VirtualDesktopManager::
    /// rawDesktopNames()). KWin gives every unnamed desktop a positional
    /// default label, and a label the default-name provider recognizes as one
    /// never claims anything: a workspace declared as "Desktop 3" must not
    /// claim a dynamic desktop that merely carries KWin's default.
    void applyNamedWorkspaces(const QList<NamedWorkspace>& declarations, const QStringList& kwinNames);

    // ── Verb support ────────────────────────────────────────────────────────
    /// True while any structural op (create/remove) is open or echoed but not
    /// yet settled — verb execution defers until the ledger is quiet
    /// (renumbering window, plan §4.5). structuralOpsSettled() announces the
    /// moment this turns false.
    bool hasPendingStructuralOps() const;
    /// The desktop id a screen currently shows, as last reported and carried
    /// across renumbers by id; empty when unknown.
    QString currentDesktopIdOf(const QString& screenId) const;
    /// Every screen's current desktop as a 1-based position in the settled
    /// list, for the state file. Screens with no known
    /// current are absent.
    QHash<QString, int> currentIndexByScreen() const;
    /// The id `delta` slots up (-1) / down (+1) from the screen's current
    /// desktop WITHIN ITS OWN SLICE (foreign desktops are skipped by
    /// construction); empty at the slice edge (no wrap) or when unresolved.
    QString desktopIdAtOffset(const QString& screenId, int delta) const;
    /// Like desktopIdAtOffset, counted from `fromDesktopId` instead of the
    /// current desktop. The base a repeated next/prev chains off while an
    /// earlier press is still in flight (inFlightSetCurrentTarget).
    QString desktopIdAtOffsetFrom(const QString& screenId, const QString& fromDesktopId, int delta) const;
    /// The id at 0-based position `sliceIndex` of the screen's slice, or empty.
    QString desktopIdAtSliceIndex(const QString& screenId, int sliceIndex) const;
    /// The target of the screen's open per-screen switch, or empty.
    QString inFlightSetCurrentTarget(const QString& screenId) const;

    /// Issue a ledgered per-screen switch (focus verbs and snap-back).
    /// Refused (returns false) when the target is not in the settled list
    /// yet, when the target is empty, and while a switch for the screen is
    /// already open — the single-correction rule that breaks re-assertion
    /// loops.
    bool issueSetCurrent(const QString& screenId, const QString& desktopId);
    /// Issue a per-screen switch for a navigation verb, REPLACING an open one
    /// that was also issued through this call (a repeated next/prev before
    /// the first press echoed). An open owner-wins correction (snapBack,
    /// issueSetCurrent) is never replaced; the call is refused instead, so
    /// the loop breaker issueSetCurrent enforces stays intact.
    bool issueSetCurrentReplacing(const QString& screenId, const QString& desktopId);
    /// Owner-wins correction: return `screenId` to its last-shown owned
    /// desktop (fallback: its slice head). No-op when the screen already
    /// shows an owned desktop. Returns true when a correction was issued.
    bool snapBack(const QString& screenId);
    /// Reorder the screen's CURRENT workspace within its slice by delta. The
    /// map order only: KWin's global order is not rewritten. The trailing
    /// empty never moves, and nothing moves behind it.
    bool reorderCurrentWorkspace(const QString& screenId, int delta);
    /// Re-own the screen's current workspace to `targetScreenId`, inserted
    /// before that screen's trailing empty. Window relocation is the
    /// controller's job (§4.2); this is the map half. The source switches to
    /// the workspace that took the moved one's slot. Refused for the source's
    /// last desktop and for its trailing empty. Returns the moved desktop id
    /// (empty on failure).
    QString transferCurrentWorkspace(const QString& screenId, const QString& targetScreenId);

Q_SIGNALS:
    // Outputs → KWin (via VirtualDesktopManager). Creates, removes and
    // switches are ledgered before the emit; name pushes are not.
    /// Create a desktop at a 0-based position. `name` is never empty: a named
    /// workspace's name, or for a dynamic desktop the positional label KWin
    /// would give it (see setDefaultDesktopNameProvider).
    void requestCreateDesktop(uint position, const QString& name);
    void requestRemoveDesktop(const QString& desktopId);
    void requestSetCurrent(const QString& screenId, const QString& desktopId);
    /// Sync a desktop's KWin name (named-workspace claim, rename, revert to
    /// KWin's positional default). Creates carry their name in createDesktop
    /// and never ride this.
    void requestSetDesktopName(const QString& desktopId, const QString& name);

    /// Map mutated (any structural or metadata change), or a settle moved the
    /// published 1-based positions. The controller persists on it.
    void mapChanged();
    /// hasPendingStructuralOps() just turned false: the settled list now
    /// reflects every create and remove this reconciler had in flight. Deferred
    /// verbs resume on this. Emitted from inside the settle, so a consumer that
    /// needs other subscribers of the same desktop-list change to have run
    /// first connects with Qt::QueuedConnection.
    void structuralOpsSettled();
    /// An unmatched (external) switch put screenId on a desktop owned by
    /// another screen. The controller answers with snapBack.
    void foreignSwitchDetected(const QString& screenId, const QString& desktopId, const QString& ownerScreenId);
    /// A window populated `desktopId` while our removeDesktop for it was in
    /// flight (plan §4.3 destroy step 4): KWin will sweep its windows to an
    /// arbitrary neighbour when the removal lands. The controller snapshots
    /// the census and re-routes those windows to the owner's current
    /// workspace once the desktop is gone.
    void removalRaceDetected(const QString& desktopId, const QString& ownerScreenId);
    /// Our removeDesktop for `desktopId` expired unanswered: the desktop is
    /// staying, so any removalRaceDetected snapshot taken for it is stale.
    void removalAbandoned(const QString& desktopId);
    /// A verb moved a named workspace (reorder within its slice, or transfer
    /// to another screen). The declaration's pin and position are the
    /// controller's to rewrite, or the next apply moves it back. `namedId` is
    /// empty for a declaration without an id, which `name` then identifies.
    void namedWorkspaceMoved(const QString& namedId, const QString& name, const QString& screenId, int sliceIndex);
    /// KWin carries a user-chosen name for a named workspace's desktop that
    /// differs from the declaration and is not a name we pushed: it was
    /// renamed on KWin's side (Pager, System Settings). Emitted once per
    /// distinct KWin name; the reconciler does not push the declared name
    /// back, so adopting the rename into the declaration (same id) is the
    /// controller's call.
    void namedWorkspaceRenamedInKWin(const QString& namedId, const QString& desktopId, const QString& kwinName);
    /// The desktop cap stopped a create (once per episode).
    void capReached();
    /// A ledger entry expired; controller should re-pull authoritative state.
    void resyncRequested();

private:
    struct PendingOp
    {
        enum class Kind {
            Create,
            Remove,
            SetCurrent
        };
        Kind kind;
        /// Remove/SetCurrent target; for a Create, the id KWin echoed for it
        /// (empty until the echo).
        QString desktopId;
        QString screenId; ///< Create owner / SetCurrent screen
        /// Create only: the declared slice position of a NAMED create. The
        /// landing slot is resolved from it when the desktop arrives, not
        /// frozen at request time, because the slice can gain entries in
        /// between (orphan migration, external adoption, a pin transfer). An
        /// unnamed create always lands as the slice's last entry.
        int declaredPosition = -1;
        /// Create only: the GLOBAL position the request asked KWin to insert
        /// at. The settle path pairs open Creates with new ids by its rank,
        /// and the positional echo matches on it.
        int globalPosition = 0;
        QString name; ///< Create name (named workspaces); empty for a dynamic desktop
        /// Create: the name sent to KWin, which its echo carries back (the
        /// workspace name, or the positional label of a dynamic desktop).
        QString kwinName;
        QString namedId; ///< Create: the declaration id the desktop realizes
        qint64 deadline = 0;
        /// KWin echoed the create or removal; the op now only waits for the
        /// settled list that confirms it.
        bool echoed = false;
        /// SetCurrent issued by issueSetCurrentReplacing: a later press may
        /// replace it.
        bool replaceable = false;
        /// SetCurrent: answers still owed to the earlier switches this one
        /// replaced. The effect answers every request once, so the next this
        /// many reports naming another desktop belong to those, not to this.
        int supersededAnswers = 0;
        /// Create/Remove: KWin's method call has been answered (or failed).
        bool replied = false;
        /// Create/Remove: the call failed or timed out, so its expiry is a
        /// stall, not evidence about KWin's ceiling.
        bool stalled = false;
    };

    qint64 nowMs() const;
    /// Record where a screen is (shared by reports and seeds). Returns the
    /// desktop id it resolved to, or empty for a malformed report or one
    /// numbered against a list that has not settled yet.
    /// `restoresCorrectionBudget` false keeps the screen's spent correction
    /// budget: the report is the refusal of an open correction.
    QString recordScreenDesktop(const QString& screenId, int desktop, bool restoresCorrectionBudget = true);
    /// Ledger index of the screen's open SetCurrent, or -1. At most one is
    /// open per screen (issueSetCurrent refuses a second, the replacing form
    /// retires the one it replaces).
    int openSetCurrentIndex(const QString& screenId) const;
    /// The first un-replied Create/Remove op a call answer belongs to, or -1.
    int matchAnsweredOp(PendingOp::Kind kind, const QString& desktopId, int position, const QString& name) const;
    void ledgerAdd(PendingOp op);
    void stopLedgerTimerIfIdle();
    void expireLedger();
    /// Emit structuralOpsSettled when hasPendingStructuralOps() has turned
    /// false since `wasPending` was sampled.
    void announceIfQuiet(bool wasPending);
    /// KWin answered one of our creates: forget the cap-probe evidence and
    /// restore the configured ceiling if a previous episode had lowered it.
    void noteCreateSucceeded();
    /// A create landed: hand back every retry budget (the ceiling is global).
    void noteCreateLanded();
    /// True while an un-echoed Remove for this desktop is open.
    bool hasPendingRemove(const QString& desktopId) const;
    /// True while an un-echoed Create owned by this screen is open.
    bool hasPendingCreate(const QString& screenId) const;
    /// Un-echoed Create entries — the desktops KWin still owes us.
    int pendingCreateCount() const;
    /// Desktops KWin has as far as we know: the settled list plus owned
    /// desktops it has echoed but not settled yet. With pendingCreateCount it
    /// is what the cap gate counts.
    int knownDesktopCount() const;
    /// Drop every open ledger entry that targets this desktop id.
    void retireLedgerFor(const QString& desktopId);
    /// The first un-echoed Create this echo answers, or -1 for an external
    /// creation (see onKwinDesktopCreatedAt).
    int matchCreateEcho(int position, const QString& name) const;
    /// Pair the ids that are NEW in a settled list (in settled-list order)
    /// against the un-echoed Creates by rank of requested position. This is
    /// echo-loss recovery: KWin emits desktopCreated before the list reply
    /// that settles it, so in normal ordering the echo has already matched.
    /// Consumes every op it pairs; unpaired ids are left for the caller to
    /// adopt. Returns id → the Create it belongs to.
    QHash<QString, PendingOp> takeSettledCreates(const QStringList& newIds);
    /// Put a landed create into the map: on its requesting screen at the slot
    /// its intent resolves to now, or, with that screen gone, onto the
    /// fallback screen (a named one keeping its name, stamped home).
    /// Returns false when no screen could take it.
    bool realizeCreate(const PendingOp& op, const QString& desktopId);
    /// Trailing-empty + slice-never-empty repair for every screen. Return
    /// true when the map changed (a donor transfer).
    bool maintainInvariants();
    bool maintainScreen(const QString& screenId);
    void scheduleDestroyCheck(const QString& desktopId);
    /// Whether the destroy sweep may reap this desktop right now: owned,
    /// empty, dynamic, not displaced by hotplug, not shown on any screen, and
    /// neither its slice's last entry nor its only one.
    bool isReapable(const QString& desktopId) const;
    /// Request a desktop for a screen. Unnamed: the slice's next last entry.
    /// Named: realized at `declaredPosition` (namedSliceIndex).
    void requestCreateAt(const QString& screenId, int declaredPosition, const QString& name,
                         const QString& namedId = QString());
    /// KWin's 0-based insert position for slot `sliceIndex` of a screen's
    /// slice, derived from the KWin position of the slot's slice neighbour and
    /// corrected for creates and removes KWin will have applied first.
    uint kwinPositionForInsert(const QString& screenId, int sliceIndex) const;
    bool isDesktopEmpty(const QString& desktopId) const;
    bool isShown(const QString& desktopId) const;
    /// The trailing entry of a slice iff it is an empty dynamic desktop.
    QString trailingEmptyOf(const QString& screenId) const;
    /// The slot a new entry takes so it lands BEFORE the screen's trailing
    /// empty: the slice size, or one less when a trailing empty exists.
    int insertIndexBeforeTrailingEmpty(const QString& screenId) const;
    /// Where a named declaration's workspace belongs in a slice: its declared
    /// position clamped to the last slot BEFORE the trailing empty (-1 means
    /// exactly that slot).
    int namedSliceIndex(const QString& screenId, int declaredPosition) const;
    /// The slot that keeps a slice in KWin order for `desktopId` (before the
    /// first entry KWin lists after it).
    int kwinOrderedSlot(const QString& screenId, const QString& desktopId) const;
    /// The focused screen as the provider or the fallback answers it, or the
    /// first screen in order; empty when the world has no screen.
    QString adoptionTarget() const;
    /// Adopt an externally created desktop onto adoptionTarget(), as an
    /// ordinary dynamic desktop. Nothing exempts it from destroy-on-empty, so
    /// one that stays empty and is shown by no screen is removed again after
    /// the debounce (niri drops a stray empty workspace the same way): a
    /// desktop added from the Pager or System Settings lasts only once a
    /// window is on it or a screen shows it. Returns false when no screen
    /// exists to adopt onto, so the caller does not announce a map change that
    /// never happened.
    bool adoptExternal(const QString& desktopId);
    /// Hand an externally created, still empty desktop to the one screen that
    /// shows it, when that is not the screen it was adopted onto. Returns
    /// true when it moved.
    bool transferExternalToShowingScreen(const QString& desktopId);
    /// Bring every entry stamped home to `screenId` back onto it. Returns the
    /// screens that gave entries up.
    QSet<QString> migrateHome(const QString& screenId);
    void setCurrentFor(const QString& screenId, const QString& desktopId);
    void bumpGeneration();
    /// Owner-wins check for one screen's recorded current desktop; emits
    /// foreignSwitchDetected when it shows another screen's desktop, unless a
    /// structural op is in flight (the next settle re-sweeps), a correction
    /// for the screen is already open, or its correction budget is spent.
    void evaluateForeign(const QString& screenId);
    void evaluateForeignAll();
    QString defaultDesktopName(int oneBasedPosition) const;
    /// True for a label KWin or this reconciler generates for an unnamed
    /// desktop, at any position.
    bool isPositionalLabel(const QString& name) const;
    /// Push `name` for a desktop unless the same push is in flight or its
    /// budget is spent. Returns true when a push was emitted.
    bool pushDesktopName(const QString& desktopId, const QString& name);
    /// Re-push the positional default for every dynamic desktop whose KWin
    /// label is a positional one for some other position.
    void refreshPositionalLabels();

    WorkspaceMap m_map;
    quint64 m_generation = 0;
    bool m_adopted = false;
    QElapsedTimer m_clock;
    int m_ledgerTimeoutMs = LedgerTimeoutMs;
    int m_destroyDebounceMs = DestroyDebounceMs;
    QStringList m_lastIds; ///< id list as of the last settled reply
    /// KWin's raw names aligned with m_lastIds, or empty when not known.
    QStringList m_lastRawNames;
    QHash<QString, int> m_population; ///< desktopId → window count
    /// screenId → the desktop id it shows. Carried by id, so a renumber needs
    /// no arithmetic; a removed one is moved to KWin's successor at settle.
    QHash<QString, QString> m_currentIdByScreen;
    /// screenId → a reported 1-based desktop past the end of the settled list,
    /// resolved by the next settle.
    QHash<QString, int> m_pendingReportByScreen;
    /// screenId → last OWNED desktop id it showed (the snap-back target).
    QHash<QString, QString> m_lastOwnedByScreen;
    QString m_focusedScreen;
    std::function<QString()> m_focusedScreenProvider;
    std::function<QString(int)> m_defaultNameProvider;
    int m_configuredCap = DefaultDesktopCap;
    int m_desktopCap = DefaultDesktopCap;
    bool m_capHintShown = false;
    QList<PendingOp> m_ledger;
    QTimer m_ledgerTimer;
    QHash<QString, QTimer*> m_destroyTimers; ///< desktopId → debounce
    /// Desktops already reported through removalRaceDetected while their
    /// Remove is open — the signal is an edge, not a level, so every further
    /// population increment on the same doomed desktop must stay quiet.
    QSet<QString> m_racedDesktops;
    struct NamePush
    {
        QString name;
        qint64 deadline = 0;
        int refusals = 0; ///< pushes of this name KWin never acknowledged
    };
    /// desktopId → the KWin name we last pushed for it and when that push
    /// stops being assumed in flight. Without this an identical push re-fires
    /// on every settle while KWin declines the rename or its name list lags.
    /// Cleared the moment KWin's names agree, when the name reverts to
    /// dynamic, and when the desktop goes away.
    QHash<QString, NamePush> m_namePushes;
    /// desktopId → the KWin-side rename last reported through
    /// namedWorkspaceRenamedInKWin, so one rename is reported once.
    QHash<QString, QString> m_reportedKWinRenames;
    /// desktopId → how many of our Removes for it expired unanswered. Bounds
    /// the re-arm below MaxRemovalRefusals; cleared on the desktop's removal,
    /// on any population change, and when the id leaves KWin's list.
    QHash<QString, int> m_removalRefusals;
    /// screenId → expiry passes in which its unnamed create went unanswered.
    QHash<QString, int> m_createRefusals;
    /// Declaration key (id, or name without one) → expiries of its create.
    QHash<QString, int> m_namedCreateRefusals;
    /// screenId → per-screen switches that expired unechoed.
    QHash<QString, int> m_setCurrentRefusals;
    /// The id list the current cap-probe episode is being counted against, and
    /// how many Create expiries it has seen. An episode ends the moment the
    /// list changes or a create succeeds; see expireLedger and CapProbeExpiries.
    QStringList m_capProbeIds;
    int m_capProbeExpiries = 0;
    /// Externally created desktops adopted recently (id → when the claim
    /// lapses). One a single other screen turns out to show is handed to
    /// that screen instead of being snapped away from it.
    QHash<QString, qint64> m_recentExternal;
    /// The last screen's slice when it went away with no screen left to
    /// foster it, home-stamped. The next screenAdded takes it back.
    QList<WorkspaceEntry> m_parked;
    /// Desktops whose "not pinning yet" deferral was already logged.
    QSet<QString> m_pinDeferredLogged;
    /// The declarations and KWin names of the last applyNamedWorkspaces, so an
    /// expired named create can be re-driven without a new call.
    QList<NamedWorkspace> m_declarations;
    QStringList m_declarationKwinNames;
};

} // namespace PhosphorWorkspaces
