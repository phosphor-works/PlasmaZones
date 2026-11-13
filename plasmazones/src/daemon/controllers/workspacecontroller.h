// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <PhosphorProtocol/WindowTypes.h>
#include <PhosphorWorkspaces/WorkspaceReconciler.h>

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariant>

#include <functional>

namespace PhosphorEngine {
class WindowRegistry;
struct WindowMetadata;
}
namespace PhosphorScreens {
class ScreenManager;
struct PhysicalScreen;
}
namespace PhosphorWorkspaces {
class VirtualDesktopManager;
}

namespace PlasmaZones {

/// Defined in shortcutmanager.h (an opaque declaration: scoped, int-based).
enum class NavigationDirection;

/// Daemon glue for dynamic per-monitor workspaces (constructed ONLY when the
/// feature is enabled and KWin runs per-output virtual desktops; the gate
/// wraps this object, not scattered ifs). Wires VirtualDesktopManager and
/// window-registry notifications into the WorkspaceReconciler, maintains the
/// per-desktop window census, computes the geometry screen order, persists
/// the map, and executes the reconciler's KWin requests. The engines follow a
/// desktop this controller creates or removes through the daemon's
/// desktopsRenumbered path, like any other change to KWin's desktop list.
class WorkspaceController : public QObject
{
    Q_OBJECT

public:
    WorkspaceController(PhosphorWorkspaces::VirtualDesktopManager* vdm, PhosphorEngine::WindowRegistry* registry,
                        PhosphorScreens::ScreenManager* screens, QObject* parent = nullptr);
    /// Final state write (the debounced save may be pending at shutdown).
    ~WorkspaceController() override;

    /// Begin: census seed, screen order, first-run adoption (deferred until
    /// every known screen has reported a current desktop, with a timeout
    /// fallback that adopts the screens that did report, and only once the
    /// compositor has reported at all; see start()).
    void start();

    PhosphorWorkspaces::WorkspaceReconciler& reconciler();

    /// Inject the window → screen resolver (the daemon backs it with the
    /// engines' live tracking and the placement store). Powers the
    /// owner-wins reunion arm: a window sitting on a desktop owned by another
    /// output is moved to that output so it stays visible (plan §4.7's second
    /// half: the desktop's owner screen is where its windows belong).
    void setWindowScreenResolver(std::function<QString(const QString& windowId)> resolver);

    /// Inject the sticky (on-all-desktops) predicate, backed by the same
    /// WindowTrackingService answer the adaptor's move slot refuses on. Every
    /// move verb consults it before arming a move watchdog for a move the
    /// adaptor is going to refuse. Unset means "cannot tell", and every caller
    /// then issues the move.
    void setWindowStickyPredicate(std::function<bool(const QString& windowId)> predicate);

    /// Inject the column-preserving half of the move-column verb: record the
    /// column under (strip screen, destination desktop) so its windows rejoin
    /// one column on arrival. Called with the exact strip screen id, the ids
    /// that will actually move, and the destination desktop number, AFTER the
    /// sticky filter and BEFORE the first move is emitted. The daemon backs it
    /// with WindowTrackingAdaptor::stashScrollColumnForWorkspaceMove, which
    /// also leaves out a window the move verb refuses, and only when the
    /// destination context runs scrolling. Unset, every window moves on its
    /// own.
    void setColumnStashHandler(
        std::function<bool(const QString& stripScreenId, const QStringList& windowIds, int targetDesktop)> handler);
    /// Inject the strip-preserving half of move-workspace-to-monitor: record
    /// the moved workspace's scrolling strips for the monitor they land on,
    /// so the riders rebuild their columns there. Called with the riders, the
    /// destination monitor (physical id from the map) and the desktop number,
    /// after the transfer and BEFORE the first rider move is emitted. The
    /// daemon backs it with WindowTrackingAdaptor::stashScrollStripsForWorkspaceMove.
    /// Unset, every rider arrives on its own.
    void setStripStashHandler(
        std::function<void(const QStringList& windowIds, const QString& targetScreenId, int desktop)> handler);

    /// Whether the daemon's desktop-switch OSD should stay quiet for this
    /// per-output report. True for a report that put the screen on another
    /// screen's workspace (the snap-back hint speaks for it) and for the
    /// report that answers a snap-back this controller issued. An owed
    /// snap-back answer is consumed by the first report that lands on one of
    /// the screen's own workspaces, whether it matches or not, and dropped by
    /// noteScreenDesktopReport for an unchanged report naming another desktop
    /// (the snap-back refused), so a stale marker can never swallow a later
    /// switch. A landing on another screen's workspace keeps it: the
    /// snap-back's answer is still to come. Safe to call before or after the
    /// controller has seen the same report.
    bool takeDesktopSwitchOsdSuppression(const QString& screenId, int desktop);

    /// Every per-output report the compositor sent, changed or not, offered
    /// BEFORE VirtualDesktopManager records it. VirtualDesktopManager only
    /// announces a change, and the effect answers a switch it did not make
    /// (already there, refused, no such desktop) with a report of the live
    /// desktop, which is usually the value already recorded. Fed to the
    /// reconciler only in that case and only while a switch for the screen
    /// is in flight, so the answer retires the entry instead of leaving it
    /// to block the screen until it expires. A changed report reaches the
    /// reconciler through VirtualDesktopManager as usual. An unchanged report
    /// naming a desktop other than an owed snap-back's target is that
    /// snap-back's answer that it did not happen, and drops the owed marker.
    /// A changed one is left to takeDesktopSwitchOsdSuppression. A report
    /// under a renamed output's old spelling counts for its new id
    /// (VirtualDesktopManager::liveScreenIdFor).
    void noteScreenDesktopReport(const QString& reportedScreenId, int desktop);

    // ── Verbs (Phase 2). screenId is the PHYSICAL id of the acting screen, in
    // the id space the KWin effect reports (the daemon extracts it from its
    // shortcut-screen resolution); windowId (move verbs) is the daemon-resolved
    // active window. All walk the screen's OWN slice (no wrap at the edges,
    // niri semantics) and defer behind the reconciler's ledger during
    // structural churn. ───────────────────────────────────────────────────────
    /// delta -1 = up, +1 = down. A press while an earlier one is still in
    /// flight chains off that press's target instead of being refused.
    void focusWorkspace(const QString& screenId, int delta);
    /// Switch the screen to the 0-based slice index (focus-slot family).
    void focusWorkspaceAt(const QString& screenId, int sliceIndex);
    /// The destination is counted from the workspace the screen showed when
    /// the verb was issued, so a press deferred behind structural churn still
    /// moves the window one slot from where the user pressed it.
    void moveWindowToWorkspace(const QString& screenId, const QString& windowId, int delta);
    /// The scrolling column variant: every window of the focused column moves
    /// together. `columnWindows` is enumerated by the daemon from the scroll
    /// engine; `stripScreenId` is the strip it came from (a "<phys>/vs:N" id
    /// on a subdivided monitor), the key the column stash is recorded under.
    void moveColumnToWorkspace(const QString& screenId, const QString& stripScreenId, const QStringList& columnWindows,
                               int delta);
    /// Reorder the current workspace within its slice.
    void moveWorkspace(const QString& screenId, int delta);
    /// Re-own the current workspace to the neighbour output on the left or
    /// right, windows riding along; focuses it there. Any other direction is
    /// ignored.
    void moveWorkspaceToOutput(const QString& screenId, NavigationDirection direction);

    // ── Named workspaces (Phase 3) ──────────────────────────────────────────
    /// Apply the config declarations (Workspaces.Named entries: id, name,
    /// output, position). Deferred until adoption; re-run on every
    /// declaration change and every settled desktop list.
    void applyNamedDeclarations(const QVariantList& entries);
    /// Focus / move-the-active-window-to a named workspace (the per-entry
    /// shortcut and quick-slot targets). The declaration is found by its
    /// persistent id, or by `name` for an entry without one, at the moment
    /// the verb runs, so a rename never strands a bound chord. A focus acts
    /// on the workspace's OWNER screen (a named workspace shows where it
    /// lives).
    void focusNamedWorkspace(const QString& entryId, const QString& name);
    void moveWindowToNamedWorkspace(const QString& entryId, const QString& name, const QString& windowId);
    /// Verdict of the RouteToWorkspace rule arm (see
    /// routeWindowToNamedWorkspace).
    enum class WorkspaceRouteVerdict {
        /// The target IS declared but cannot be placed right now: a structural
        /// op is in flight, adoption has not completed, the workspace is not
        /// realized, or its desktop has no live number yet. The cascade's
        /// positional RouteToDesktop is NOT a valid stand-in: that number was
        /// authored as the fallback for an UNDECLARED target, and applying it
        /// here silently lands the window on a different desktop. The window
        /// stays where it spawned (a transient case is parked and re-issued).
        Unresolvable = -1,
        /// No declaration carries the target. The positional route applies.
        Unrealized = 0,
    };
    /// The RouteToWorkspace rule arm: issue the move NOW and report the
    /// REALIZED desktop number so the caller's placement context can resolve
    /// on it. The declaration is the entry whose id equals @p entryId, else
    /// the entry named @p name. A value > 0 is the desktop the window was
    /// routed to (the sticky "already there" case reports its desktop too:
    /// the rule asked for the window to be on that workspace and it is).
    /// Values <= 0 are WorkspaceRouteVerdict.
    ///
    /// @p moveOutput decides the OUTPUT leg. Under per-output virtual desktops
    /// the target workspace belongs to its OWNER monitor's slice, so a window
    /// that only changes desktop never appears on the screen that shows that
    /// workspace. The caller passes true when nothing else in the rule cascade
    /// owns the window's monitor, and false when an explicit RouteToScreen
    /// does. @p ownerScreenOut reports the workspace's owner screen so the
    /// caller can resolve the snap / tiling placement against the DESTINATION
    /// monitor rather than the spawn one.
    ///
    /// @p ownerScreenOut is written only when a move is actually issued, and
    /// is left untouched otherwise. That includes one > 0 return: a sticky
    /// window is already on every workspace, so the call reports the realized
    /// desktop (to suppress the caller's positional fallback) while issuing
    /// no move and naming no owner. A caller must treat an untouched value as
    /// "no destination screen", not as an error.
    int routeWindowToNamedWorkspace(const QString& entryId, const QString& name, const QString& windowId,
                                    bool moveOutput, QString* ownerScreenOut = nullptr);

Q_SIGNALS:
    /// → adaptor setScreenDesktopRequested (effect per-output switch, by
    /// desktop id). @p purpose is UserSwitch for a focus verb and Correction
    /// for every switch the daemon makes on its own.
    void screenDesktopSwitchRequested(const QString& screenId, const QString& desktopId,
                                      PhosphorProtocol::ScreenDesktopSwitchPurpose purpose);
    /// → adaptor moveWindowToWorkspaceVerb (handoff-based window relocation).
    /// @p moveOutput false issues the desktop half only, leaving the window on
    /// its current monitor (the RouteToWorkspace open-path arm; see
    /// routeWindowToNamedWorkspace).
    /// @p targetDesktopId is the destination's STABLE id, carried beside the
    /// number so the compositor-side move names the desktop rather than a
    /// position that can renumber in between. Every move this controller
    /// issues carries it.
    /// @p subjectOfVerb is false for windows that move as a consequence of
    /// something else (a workspace's riders, the owner-wins reunion, the
    /// removal-race re-route), which the adaptor uses to leave a fullscreen
    /// window's state alone.
    void windowWorkspaceMoveRequested(const QString& windowId, const QString& targetScreenId, int targetDesktop,
                                      const QString& targetDesktopId, const QString& direction, bool moveOutput,
                                      bool subjectOfVerb);
    /// Owner-wins snap-back fired for this screen (OSD hint hook; gated by
    /// the snapBackOsdHint setting daemon-side).
    void snapBackOccurred(const QString& screenId);
    /// Windows that mapped onto a workspace during its removal window were
    /// swept by KWin and re-routed to the owner's current workspace (plan
    /// §4.3 destroy step 4); OSD hint hook, same gating as snap-back.
    void windowDisplacedByRemoval(const QString& screenId);

private:
    /// The map, the census and the reconciler all key screens by the id the
    /// KWin effect REPORTS. ScreenManager's connector names and the settings
    /// UI's stored output ids are other spellings, and every id from those two
    /// sources passes through here. Ids that already come from the effect (a
    /// report, the verbs' acting screen) are in this space already and are
    /// used as they are.
    static QString canonicalScreenId(const QString& connectorOrId);
    /// The effect-space id of a ScreenManager screen. Uses the identifier the
    /// screen carried rather than resolving its connector name, which no
    /// longer resolves once the output has gone (screenRemoved).
    static QString screenIdOf(const PhosphorScreens::PhysicalScreen& screen);
    void wireVirtualDesktops();
    void wireWindows();
    void wireScreens();
    void refreshScreenOrder();
    void onWindowAppeared(const QString& instanceId);
    void onWindowDisappeared(const QString& instanceId);
    void onMetadataChanged(const QString& instanceId, const PhosphorEngine::WindowMetadata& oldMeta,
                           const PhosphorEngine::WindowMetadata& newMeta);
    /// The desktop ids a window counts toward: its one desktop, or every
    /// desktop of a window placed on several. Empty for a sticky window (on
    /// all of them, so it holds none open) and for a number the current list
    /// cannot translate.
    QStringList censusDesktopIds(const PhosphorEngine::WindowMetadata& meta) const;
    /// The one census mutation for a window: replace the desktops it counts
    /// toward and move the population of each desktop that gained or lost
    /// it. Also feeds a removal-race record whose doomed desktop the window
    /// just arrived on.
    void setCensusRow(const QString& instanceId, const QStringList& desktopIds);
    /// Re-translate every window's desktops from the registry against the
    /// current list and rebuild every population from scratch, announcing
    /// each one that moved. Runs after a desktop list commits, once the
    /// registry has been renumbered for it, which is what makes the event-time
    /// translation (taken against whichever list was current then) converge.
    void reseedCensus();
    /// Post-commit pass, queued from desktopListChanged so it runs after
    /// every synchronous subscriber of the same commit (the registry's
    /// renumber in particular): reseed the census, then give parked
    /// open-path routes a chance against the new list.
    void scheduleSettledListPass();
    void runSettledListPass();
    bool m_settledListPassQueued = false;
    void adjustPopulationById(const QString& desktopId, int delta);
    void tryFirstAdoption();
    /// For adoption: per desktop id, the screen (canonical physical id, one
    /// in the reconciler's screen order) holding most of the census windows
    /// on that desktop, ties to the screen earlier in the order. A desktop
    /// none of whose windows has a known screen is absent. adoptAll hands a
    /// desktop no screen shows to this screen instead of to KWin order.
    QHash<QString, QString> preferredScreenByDesktopId() const;
    /// Arm (or re-arm) the adoption timeout fallback.
    void armAdoptionTimeout();
    void adoptOnTimeout();
    /// The shared post-adoption tail: realize the declarations parked while
    /// adoption was pending, then run the owner-wins reunion check over the
    /// census seeded before adoption (both are refused while !m_adopted).
    /// Both adoption paths (tryFirstAdoption and the timeout fallback) run it.
    void applyNamedDeclarationsAfterAdoption();
    /// Owner-wins correction for one screen, rate-limited by the snap-back
    /// cooldown. A correction the cooldown refuses is retried once the
    /// cooldown has passed, so the screen is not left on a foreign desktop.
    void snapBackScreen(const QString& screenId);
    /// Arm the cooldown retry of snapBackScreen for @p screenId after
    /// @p delayMs, recording its sequence in m_snapBackRetryPending. The retry
    /// runs only while the screen's entry still holds that sequence, so
    /// dropping the entry (a screen-id rename re-arming under the new id, an
    /// unplug) or re-arming it cancels this one.
    void scheduleSnapBackRetry(const QString& screenId, qint64 delayMs);

    PhosphorWorkspaces::VirtualDesktopManager* m_vdm;
    PhosphorEngine::WindowRegistry* m_registry;
    PhosphorScreens::ScreenManager* m_screens;
    PhosphorWorkspaces::WorkspaceReconciler m_reconciler;
    /// Window count per desktop ID (ids are the fixed points across
    /// renumbering). desktopId → population.
    QHash<QString, int> m_populationById;
    /// The desktop IDs a window is counted on (see censusDesktopIds), so a
    /// metadata change adjusts the right buckets even after renumbering.
    /// instanceId → desktopIds. Windows that count toward nothing are absent.
    QHash<QString, QStringList> m_windowCensusDesktopIds;
    bool m_adopted = false;
    QTimer m_adoptionTimer;
    /// Monotonic clock behind every cooldown below (a wall-clock jump must
    /// neither unblock nor freeze a cooldown).
    QElapsedTimer m_clock;

    // ── Verb support (workspacecontroller_verbs.cpp) ────────────────────────
    /// Run now if no structural op is in flight, else queue until the ledger
    /// quiets (plan §4.5: verb translation waits out the renumbering window).
    void runWhenQuiet(std::function<void()> fn);
    void drainQuietQueue();
    /// Issue the per-screen switch for a desktop id (ledgered SetCurrent →
    /// effect command by desktop id). A UserSwitch replaces a user switch
    /// still in flight for the screen; a Correction is strict.
    void switchScreenToDesktop(const QString& screenId, const QString& desktopId,
                               PhosphorProtocol::ScreenDesktopSwitchPurpose purpose);
    /// The purpose the requestSetCurrent relay stamps on the switch the
    /// reconciler is emitting right now. Correction outside a user verb,
    /// because every switch the reconciler makes on its own is one.
    PhosphorProtocol::ScreenDesktopSwitchPurpose m_switchPurpose =
        PhosphorProtocol::ScreenDesktopSwitchPurpose::Correction;
    QList<std::function<void()>> m_quietQueue;
    /// Set while drainQuietQueue walks the batch, so a nested drain cannot
    /// re-enter it.
    bool m_draining = false;
    /// One parsed Workspaces.Named entry.
    struct NamedDeclaration
    {
        QString id; ///< persistent entry id; empty for an entry written without one
        QString name;
    };
    /// Last applied declarations, raw (re-applied after adoption and on
    /// every settle).
    QVariantList m_namedEntries;
    /// The same entries reduced to their identity, for the named verbs and
    /// the rule resolver.
    QList<NamedDeclaration> m_namedDeclarations;
    bool m_namedApplied = false;
    /// An apply body is waiting in the quiet queue. Later applies only
    /// refresh m_namedEntries, which the waiting body reads when it runs.
    bool m_namedApplyQueued = false;
    /// The declaration @p entryId identifies (QUuid compare), else the one
    /// named @p name; nullptr when neither is declared.
    const NamedDeclaration* findDeclaration(const QString& entryId, const QString& name) const;
    /// Open-path RouteToWorkspace requests that could not be placed yet
    /// (windowId → target): before adoption (the login / session-restore
    /// population such rules are written for), during structural churn, or
    /// while the realized desktop has no live number. Re-issued once the
    /// reconciler is quiet and the declarations are applied, for windows that
    /// still exist. The output-leg decision is parked with the target: it came
    /// from the rule cascade that is no longer in hand at the re-issue.
    struct ParkedNamedRoute
    {
        QString entryId;
        QString name;
        bool moveOutput = false;
    };
    QHash<QString, ParkedNamedRoute> m_parkedNamedRoutes;
    void drainParkedNamedRoutes();
    /// Declarations whose pinned output was reported as not connected, so
    /// the re-apply on every settle does not repeat it.
    QSet<QString> m_reportedUnknownPins;
    /// The map entry a declaration is realized on, or empty.
    QString desktopIdForDeclaration(const NamedDeclaration& declaration) const;

    // ── Move-verb watchdog (plan §4.2) ──────────────────────────────────────
    /// windowWorkspaceMoveRequested is fire-and-forget over D-Bus; with no
    /// effect loaded, nothing executes and nothing errors. Record the
    /// expectation and warn when no census arrival confirms it in time: the
    /// op itself is already dropped (nothing retries), the log is the
    /// diagnosis. Arrival confirmation lives in onMetadataChanged and the
    /// post-commit census reseed.
    void armMoveWatch(const QString& windowId, const QString& targetDesktopId);
    /// Every window move this controller issues goes through here. Returns
    /// false when nothing was issued: a sticky window is on every workspace
    /// already and the adaptor would drop the move, and a window already on
    /// the target desktop needs nothing unless its output should change too.
    /// A move that changes the desktop arms the watchdog. One that changes
    /// only the output cannot be confirmed by a census arrival, so it stamps
    /// the reunion cooldown instead of arming a watch.
    bool issueWindowMove(const QString& windowId, const QString& targetScreenId, int desktop,
                         const QString& targetDesktopId, const QString& direction, bool moveOutput, bool subjectOfVerb);
    bool isStickyWindow(const QString& windowId) const;
    QHash<QString, QString> m_pendingWindowMoves; ///< instanceId → expected desktopId
    /// instanceId → the sequence of the watch that owns the entry above, so a
    /// superseded watch's timer cannot retire its successor's expectation.
    QHash<QString, quint64> m_windowMoveSequences;
    quint64 m_windowMoveSequence = 0;
    /// Per-screen snap-back cooldown stamps (m_clock milliseconds).
    QHash<QString, qint64> m_lastSnapBackMs;
    /// screenId → the sequence of the retry scheduled for its cooldown-refused
    /// snap-back, so a superseded retry's timer cannot run in its successor's
    /// place.
    QHash<QString, quint64> m_snapBackRetryPending;
    quint64 m_snapBackRetrySequence = 0;
    /// screenId → the desktop id a snap-back sent it to, until the report
    /// answering it arrives (takeDesktopSwitchOsdSuppression).
    QHash<QString, QString> m_snapBackAnswerOwed;

    // ── Owner-wins reunion + destroy-race displacement (plan §4.3/§4.7) ─────
    /// If the window's desktop is owned by a screen other than the one the
    /// window sits on, issue the cross-screen move that reunites them.
    void reuniteWindowWithOwner(const QString& instanceId);
    std::function<QString(const QString&)> m_windowScreenResolver;
    /// Sticky predicate (see setWindowStickyPredicate). Null until wired.
    std::function<bool(const QString&)> m_windowStickyPredicate;
    std::function<bool(const QString&, const QStringList&, int)> m_columnStashHandler;
    std::function<void(const QStringList&, const QString&, int)> m_stripStashHandler;
    /// Per-window reunion cooldown stamps (m_clock milliseconds): a reunion is
    /// an output move on an unchanged desktop, so no census arrival clears it,
    /// and the cooldown is what stops a slow effect from drawing repeat
    /// issues. Rider moves stamp it too, for the same reason.
    QHash<QString, qint64> m_lastReunionMs;
    /// Windows with a reunion body sitting in the quiet queue. Keeps a burst
    /// of reports during structural churn from queueing one body per report.
    QSet<QString> m_pendingReunions;
    /// The reunion last issued for a window and how many times in a row it
    /// was issued without the window reaching the owner. A move the
    /// compositor keeps refusing would otherwise be re-issued once a
    /// cooldown for the life of the window.
    struct ReunionAttempt
    {
        QString desktopId;
        QString ownerScreenId;
        int count = 0;
    };
    QHash<QString, ReunionAttempt> m_reunionAttempts;
    struct DisplacedByRemoval
    {
        QString ownerScreenId;
        QStringList windowIds;
    };
    /// Census snapshot per doomed desktop (removal-race arm); consumed when
    /// the removal lands, dropped when the reconciler abandons the removal.
    QHash<QString, DisplacedByRemoval> m_displacedByRemoval;

    // ── State persistence (Phase 4) ─────────────────────────────────────────
    static QString stateFilePath();
    void loadStateFile();
    void saveStateFile() const;
    /// Debounced save; a no-op until adoption, so the candidate map restored
    /// from last session's file is never written back over that file.
    void scheduleStateSave();
    QTimer m_stateSaveTimer;
};

} // namespace PlasmaZones
