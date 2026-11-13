// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// ═══════════════════════════════════════════════════════════════════════════════
// WindowTrackingAdaptor — cross-mode handoffs and the workspace move verb
//
// handleCrossModeMove / handleCrossModeSwap: relinquish a window from the source
// engine and place (or exchange) it on the target engine when a directional
// navigation crosses into a different tiling mode.
// handleCrossModeFocus: activate the target engine's entry-edge window — no
// window travels and no engine state changes.
// moveWindowToWorkspaceVerb: the dynamic-workspaces move, through the same
// handoff with same-engine moves allowed, and its compositor legs.
// ═══════════════════════════════════════════════════════════════════════════════

#include "windowtrackingadaptor.h"
#include "internal.h"

#include "core/platform/logging.h"
#include <PhosphorEngine/IPlacementEngine.h>
#include <PhosphorPlacement/WindowTrackingService.h>
#include <PhosphorScreens/ScreenIdentity.h>
#include <PhosphorScrollEngine/ScrollEngine.h>
#include <PhosphorSnapEngine/SnapEngine.h>
#include <PhosphorTileEngine/AutotileEngine.h>
#include <PhosphorZones/AssignmentEntry.h>
#include <PhosphorZones/Layout.h>
#include <PhosphorZones/LayoutRegistry.h>
#include <PhosphorZones/Zone.h>

#include <QHash>

#include <array>
#include <optional>
#include <utility>

namespace PlasmaZones {

namespace {

/// The zone in @p target's layout on the destination context that carries
/// the same ordinal as the zone @p windowId holds in its snap source, or
/// empty when either layout lacks one (ws_to_monitor: a snap window keeps its
/// place in the layout). Ordinals, not ids: two contexts' layouts share no
/// zone ids, but zone N of each is the slot the user knows as N.
QString zoneWithSameOrdinal(PhosphorZones::LayoutRegistry* layouts, PhosphorSnapEngine::SnapEngine* source,
                            const QString& windowId, const QString& targetScreenId, int targetDesktop,
                            const QString& activity)
{
    if (!layouts || !source) {
        return {};
    }
    const QString zoneId = source->zoneForWindow(windowId);
    const std::optional<PhosphorEngine::PlacementStateKey> held = source->heldKeyForWindow(windowId);
    if (zoneId.isEmpty() || !held) {
        return {};
    }
    const PhosphorZones::Layout* fromLayout = layouts->layoutForScreen(held->screenId, held->desktop, activity);
    const PhosphorZones::Zone* fromZone = fromLayout ? fromLayout->zoneById(QUuid::fromString(zoneId)) : nullptr;
    if (!fromZone) {
        return {};
    }
    const PhosphorZones::Layout* toLayout = layouts->layoutForScreen(targetScreenId, targetDesktop, activity);
    const PhosphorZones::Zone* toZone = toLayout ? toLayout->zoneByNumber(fromZone->zoneNumber()) : nullptr;
    return toZone ? toZone->id().toString() : QString();
}

} // namespace

PhosphorEngine::PlacementEngineBase*
WindowTrackingAdaptor::engineForMode(PhosphorZones::AssignmentEntry::Mode mode) const
{
    switch (mode) {
    case PhosphorZones::AssignmentEntry::Autotile:
        return m_autotileEngine.data();
    case PhosphorZones::AssignmentEntry::Scrolling:
        return m_scrollEngine.data();
    case PhosphorZones::AssignmentEntry::Snapping:
        return m_snapEngine.data();
    }
    return nullptr;
}

void WindowTrackingAdaptor::handleCrossModeMove(const QString& windowId, const QString& targetScreenId,
                                                int targetDesktop, const QString& direction)
{
    // sender() is only valid inside this signal dispatch; the impl below
    // never touches it, so every other caller passes its engine explicitly.
    crossModeMoveImpl(qobject_cast<PhosphorEngine::PlacementEngineBase*>(sender()), windowId, targetScreenId,
                      targetDesktop, direction, /*allowSameEngine=*/false, WorkspaceMoveWire{});
}

void WindowTrackingAdaptor::moveWindowToWorkspaceVerb(const QString& rawWindowId, const QString& targetScreenId,
                                                      int targetDesktop, const QString& targetDesktopId,
                                                      const QString& direction, bool moveOutput, bool subjectOfVerb)
{
    if (rawWindowId.isEmpty() || targetDesktop < 1) {
        return;
    }
    // Canonicalize at the entry. The controller's census-driven emitters (the
    // owner-wins reunion, the removal-race re-route, the parked-route drain)
    // walk the registry by instanceId and hand this slot a BARE compositor
    // instance id. Every consumer downstream (the engines' tracking maps and,
    // over the wire, the effect's window lookup) is keyed on the composite id,
    // so a bare id would find no engine here and no window there.
    // shadowWindowId() maps instanceId to the registry's canonical composite
    // and is the identity for an id that is already composite.
    const QString windowId = shadowWindowId(rawWindowId);
    if (refusesWorkspaceMove(windowId)) {
        return;
    }

    // The engine that genuinely holds the window, by MEMBERSHIP. isWindowTracked
    // is per-engine (the scroll engine answers from a raw reverse-map key a
    // refused adoption can leave behind), so it could name an engine that does
    // not hold the window at all. An engine holding it in the context its
    // screen shows wins; a background holder (a window on a desktop no screen
    // shows, as the reunion and re-route arms move) is the fallback.
    PhosphorEngine::PlacementEngineBase* sourceEngine = nullptr;
    QString sourceScreen;
    std::optional<PhosphorEngine::PlacementStateKey> heldKey;
    // The desktop of a background holder: no screen shows it, so a refused
    // receive must re-home the window there rather than on the desktop in
    // view. 0 for a window held in the context its screen shows.
    int backgroundDesktop = 0;
    const std::array<PhosphorEngine::PlacementEngineBase*, 3> engines = {m_scrollEngine.data(), m_autotileEngine.data(),
                                                                         m_snapEngine.data()};
    for (PhosphorEngine::PlacementEngineBase* engine : engines) {
        if (!engine) {
            continue;
        }
        if (const QString held = engine->heldScreenForWindow(windowId); !held.isEmpty()) {
            sourceEngine = engine;
            sourceScreen = held;
            heldKey = engine->heldKeyForWindow(windowId);
            break;
        }
    }
    if (!sourceEngine) {
        for (PhosphorEngine::PlacementEngineBase* engine : engines) {
            if (!engine) {
                continue;
            }
            if (const auto key = engine->heldKeyForWindow(windowId)) {
                sourceEngine = engine;
                sourceScreen = key->screenId;
                heldKey = key;
                if (key->desktop != WindowTrackingInternal::shownDesktop(*this, key->screenId)) {
                    backgroundDesktop = key->desktop;
                }
                break;
            }
        }
    }

    // Where the window lands. Without the output leg it keeps the monitor it
    // is on, so the destination context is its own screen; a workspace route
    // that names a desktop only must not re-key engine state onto an output
    // the window never reaches. With it, the workspace's owner monitor, at
    // the sub-screen workspaceLandingScreen picks on a split monitor (the map
    // names physical monitors, the engines key virtual screens). An owner
    // that has not settled yet (empty) leaves the window on its own screen:
    // an unowned workspace has no other monitor to name.
    QString landingScreen = sourceScreen;
    if (moveOutput) {
        if (!targetScreenId.isEmpty()) {
            landingScreen = workspaceLandingScreen(windowId, sourceScreen, targetScreenId);
        } else {
            qCWarning(lcDbusWindow) << "workspace move: no owner screen for" << windowId << "- it stays on"
                                    << sourceScreen;
        }
    }

    // A window no engine PLACES moves the way KWin's own desktop move would
    // move it: untracked, a user float, or free on a snapping screen. A
    // handoff would place it, snapping a free window into the entry zone or
    // tiling a float on arrival, and the verb asked to move the window, not to
    // re-place it. The engines follow the compositor's report of the move (the
    // desktop reconcile carries a held window's membership to the desktop it
    // lands on). A minimized window's float is the engine's suspension of its
    // placement, not the user's float, so it still hands off and keeps its
    // place.
    const bool placedBySource = sourceEngine
        && (sourceEngine == m_snapEngine.data() ? !snapEngine()->zoneForWindow(windowId).isEmpty()
                                                : sourceEngine->isWindowTiled(windowId));
    if (!sourceEngine || (!placedBySource && !m_service->isSuspensionFloat(windowId))) {
        // The output leg matters here too: a move-workspace-to-monitor rider
        // keeps its desktop, so the desktop leg alone would leave the floating
        // rider on the source output. The effect skips a leg that is already
        // satisfied.
        emitMoveLegs(windowId, targetDesktop, targetDesktopId, moveOutput ? landingScreen : QString(),
                     /*workspaceVerb=*/true);
        return;
    }
    if (landingScreen.isEmpty()) {
        return;
    }
    // Already where the verb sends it: the engine holds the window on the
    // landing screen's state for the target desktop. Handing it over would
    // release it from the engine, and on a tiling destination nothing
    // re-adopts it, since the window never changes desktop and so never meets
    // the arrival catch-scan.
    if (heldKey && heldKey->desktop == targetDesktop
        && PhosphorScreens::ScreenIdentity::screensMatch(heldKey->screenId, landingScreen)) {
        qCDebug(lcDbusWindow) << "workspace move:" << windowId << "is already on desktop" << targetDesktop << "of"
                              << landingScreen;
        return;
    }
    WorkspaceMoveWire wire;
    wire.workspaceVerb = true;
    wire.desktopId = targetDesktopId;
    wire.moveOutput = moveOutput;
    wire.subjectOfVerb = subjectOfVerb;
    // The holder resolved above, handed through: the impl's own lookup is the
    // source engine's reverse map, which a background holder can leave empty,
    // and an empty source screen arms no output-move marker and gives a
    // refused receive nowhere to re-home the window (C33).
    wire.sourceScreenId = sourceScreen;
    wire.sourceDesktop = backgroundDesktop;
    crossModeMoveImpl(sourceEngine, windowId, landingScreen, targetDesktop, direction, /*allowSameEngine=*/true, wire);
}

bool WindowTrackingAdaptor::refusesWorkspaceMove(const QString& windowId) const
{
    // Sticky (on-all-desktops) windows: the effect REFUSES a desktop move for
    // them outright (PlasmaZonesEffect::applyWorkspaceMove returns for
    // isOnAllDesktops), so running the handoff would re-key the daemon's
    // engine state onto a desktop the real window never moves to. The window
    // is already present on every workspace, which is what the verb was
    // asking for. m_service is non-null post-construction (class invariant).
    if (m_service->isWindowSticky(windowId)) {
        qCDebug(lcDbusWindow) << "workspace move: refusing sticky window" << windowId;
        return true;
    }
    // The window an interactive drag is moving: the drop decides where it
    // goes, and moving it under the pointer mid-drag would fight the drag the
    // way every other placement verb is kept from doing (F773).
    if (isInteractiveDragWindow(windowId)) {
        qCDebug(lcDbusWindow) << "workspace move: refusing the window being dragged" << windowId;
        return true;
    }
    return false;
}

bool WindowTrackingAdaptor::stashScrollColumnForWorkspaceMove(const QString& stripScreenId,
                                                              const QStringList& windowIds, int targetDesktop)
{
    auto* const scroll = qobject_cast<PhosphorScrollEngine::ScrollEngine*>(m_scrollEngine.data());
    if (!scroll || stripScreenId.isEmpty() || targetDesktop < 1) {
        return false;
    }
    // Only what the verb will move: a window it refuses stays in the strip,
    // and a tile recorded for it would wait for an arrival that never comes,
    // with the column rebuilt around a member it does not have (C75). The ids
    // go to the engine as the strip enumerated them.
    QStringList moving;
    moving.reserve(windowIds.size());
    for (const QString& windowId : windowIds) {
        if (!windowId.isEmpty() && !refusesWorkspaceMove(shadowWindowId(windowId))) {
            moving.append(windowId);
        }
    }
    if (moving.isEmpty()) {
        return false;
    }
    return scroll->stashColumnForDesktopMove(stripScreenId, moving, targetDesktop);
}

void WindowTrackingAdaptor::stashScrollStripsForWorkspaceMove(const QStringList& rawWindowIds,
                                                              const QString& targetScreenId, int targetDesktop)
{
    auto* const scroll = qobject_cast<PhosphorScrollEngine::ScrollEngine*>(m_scrollEngine.data());
    if (!scroll || targetScreenId.isEmpty() || targetDesktop < 1 || rawWindowIds.isEmpty() || !m_layoutManager) {
        return;
    }
    const QString activity = m_layoutManager->currentActivity();
    // (source strip, landing strip) pairs in first-seen order, each with its
    // riders. The source strip is the one the scroll engine holds the window
    // on, which is the source the verb resolves first for a scrolled window.
    QList<std::pair<QString, QString>> pairs;
    QHash<QString, QStringList> ridersByPair;
    const auto pairKey = [](const QString& from, const QString& to) {
        return from + QLatin1Char('\n') + to;
    };
    for (const QString& raw : rawWindowIds) {
        const QString windowId = shadowWindowId(raw);
        if (windowId.isEmpty() || refusesWorkspaceMove(windowId)) {
            continue;
        }
        QString sourceStrip = scroll->heldScreenForWindow(windowId);
        if (sourceStrip.isEmpty()) {
            if (const auto key = scroll->heldKeyForWindow(windowId)) {
                sourceStrip = key->screenId;
            }
        }
        if (sourceStrip.isEmpty()) {
            continue;
        }
        const QString landing = workspaceLandingScreen(windowId, sourceStrip, targetScreenId);
        if (landing.isEmpty() || PhosphorScreens::ScreenIdentity::screensMatch(landing, sourceStrip)
            || runningModeFor(landing, targetDesktop, activity) != PhosphorZones::AssignmentEntry::Scrolling) {
            continue;
        }
        const QString key = pairKey(sourceStrip, landing);
        auto it = ridersByPair.find(key);
        if (it == ridersByPair.end()) {
            pairs.append({sourceStrip, landing});
            it = ridersByPair.insert(key, {});
        }
        it->append(windowId);
    }
    for (const auto& [sourceStrip, landing] : std::as_const(pairs)) {
        scroll->stashStripForOutputMove(sourceStrip, landing, targetDesktop,
                                        ridersByPair.value(pairKey(sourceStrip, landing)));
    }
}

void WindowTrackingAdaptor::emitMoveLegs(const QString& windowId, int desktop, const QString& desktopId,
                                         const QString& outputScreenId, bool workspaceVerb)
{
    const bool desktopLeg = desktop > 0 || !desktopId.isEmpty();
    if (!desktopLeg && outputScreenId.isEmpty()) {
        return;
    }
    if (workspaceVerb || !outputScreenId.isEmpty()) {
        // One step carrying both legs, so the effect judges the arrival on
        // the destination output rather than the one the window is leaving.
        // The id is preferred over the number: the workspace feature
        // renumbers concurrently with its own moves (the move fills the
        // destination, a filled workspace makes the reconciler cut a new
        // trailing empty), and that shift can land before the effect applies
        // a number. Observed live: a route to the workspace at position 2
        // arrived on the desktop that had just taken position 2.
        Q_EMIT windowWorkspaceMoveRequested(windowId, desktopId, desktopLeg ? desktop : 0, outputScreenId);
        return;
    }
    Q_EMIT windowDesktopMoveRequested(windowId, desktop);
}

void WindowTrackingAdaptor::crossModeMoveImpl(PhosphorEngine::PlacementEngineBase* sourceEngine,
                                              const QString& windowId, const QString& targetScreenId, int targetDesktop,
                                              const QString& direction, bool allowSameEngine,
                                              const WorkspaceMoveWire& wire)
{
    if (!sourceEngine || windowId.isEmpty() || targetScreenId.isEmpty() || !m_layoutManager) {
        return;
    }

    // Target mode at the DESTINATION context: a cross-desktop crossing targets a
    // (possibly different) mode on the same screen's target desktop; a monitor
    // crossing targets the neighbour screen on the current desktop (targetDesktop
    // == 0). The mode that context RUNS, not the configured one: a tiling mode
    // switched off, or a context disabled, runs snapping there, and handing the
    // window to the configured engine would leave it with an engine that does
    // not run on that context.
    const int effectiveDesktop = targetDesktop > 0 ? targetDesktop : currentDesktopForScreen(targetScreenId);
    const QString activity = m_layoutManager->currentActivity();
    const PhosphorZones::AssignmentEntry::Mode targetMode = runningModeFor(targetScreenId, effectiveDesktop, activity);
    PhosphorEngine::PlacementEngineBase* targetEngine = engineForMode(targetMode);
    if (!targetEngine || (targetEngine == sourceEngine && !allowSameEngine)) {
        // Target engine unavailable, or not actually cross-mode. The workspace
        // verb passes allowSameEngine: onto a tiling (autotile or scrolling)
        // target the move takes the reactive branch below (release, then the
        // desktop-return catch-scan adopts it), and onto a snap target the
        // handoff re-keys it in place, since guardedHandoff skips the release
        // when source and destination are one engine.
        return;
    }
    // No isActiveOnScreen pre-gate on the move as a whole (or in the swap
    // twin), unlike handleCrossModeFocus. The immediate placements below hand
    // the window over through refusal-recovering receives (guardedHandoff
    // here, receiveVerified in the swap) that re-home it into the source on a
    // refusal, a logged bounce rather than a stranding, and the gate would
    // turn that recoverable bounce into a silent no-op. The reactive desktop
    // arrival has no receive to refuse: the destination's catch-scan adopts
    // the window once its desktop is shown.

    // Where the window currently lives: the re-home screen on a refusal, and
    // the screen the output leg leaves. The workspace verb hands over the
    // holder it resolved by membership (WorkspaceMoveWire::sourceScreenId).
    const QString sourceScreen =
        wire.sourceScreenId.isEmpty() ? sourceEngine->screenForTrackedWindow(windowId) : wire.sourceScreenId;

    // For a SNAP target, resolve the landing zone BEFORE relinquishing the
    // source. A snap window moved by a workspace verb keeps its zone's ordinal
    // in the destination's layout (ws_to_monitor), so the riders of a moved
    // workspace keep their places instead of piling into one entry zone.
    // Otherwise, or when the destination layout has no such zone, the edge
    // zone facing the way the window came, on the layout of the desktop it
    // lands on. A tiling target needs no zone (a strip's landing is
    // insertIndex, below).
    QStringList landingZoneIds;
    auto* const snapTarget = qobject_cast<PhosphorSnapEngine::SnapEngine*>(targetEngine);
    if (snapTarget) {
        QString zoneId;
        if (wire.workspaceVerb && sourceEngine == m_snapEngine.data()) {
            zoneId = zoneWithSameOrdinal(m_layoutManager, snapEngine(), windowId, targetScreenId, effectiveDesktop,
                                         activity);
        }
        if (zoneId.isEmpty()) {
            zoneId = snapTarget->entryZoneForCrossing(direction, targetScreenId, targetDesktop);
        }
        if (!zoneId.isEmpty()) {
            landingZoneIds = QStringList{zoneId};
        }
    }

    // The min size is queried BEFORE any release (the release drops the
    // source's tracking, and the receiver seeds from ctx.minSize). The source
    // reflows on its own: every engine's handoffRelease closes up what the
    // window leaves (autotile schedules a coalesced retile of the context in
    // view and marks a background one dirty, the scroll engine schedules its
    // own, a snap source just vacates a zone).
    const QSize windowMinSize = sourceEngine->windowMinimumSize(windowId);

    // Place on the target. A cross-DESKTOP move onto a TILING desktop is a
    // reactive arrival: the window changes desktops below and that engine's
    // desktop-return catch-scan places it when the desktop is shown, as for
    // KWin's own desktop move. A receive now would land it in the state of the
    // desktop in view, and the scroll engine refuses a screen whose current
    // desktop is not scrolling, so the move did nothing (F305). The edge-aware
    // insertIndex does not apply to it. Every other case places immediately:
    //   - monitor crossing (current desktop): handoffReceive tiles / snaps it now;
    //   - cross-desktop onto a SNAP desktop: snap handoffReceive honours toDesktop
    //     (assigns the zone on the target desktop + off-desktop geometry).
    const bool reactiveDesktopArrival = targetMode != PhosphorZones::AssignmentEntry::Snapping && targetDesktop > 0;
    // A window moved to a tiling engine leaves its own fullscreen first: the
    // tile batch that places it would bail on a fullscreen surface, and the
    // reactive desktop arrival places nothing at all (F526, F551). A snap
    // target ends it in its own placement. Only for the window the verb is
    // about: a collateral rider of a workspace moved to another monitor, or a
    // daemon re-route, keeps its fullscreen (F894). On the immediate branch
    // only when the target engine runs the destination screen: a receive it
    // refuses re-homes the window into the source, which never asked it to
    // leave fullscreen. Sent BEFORE the receive, because an autotile receive
    // retiles synchronously and its tile batch must find the window windowed.
    // The reactive branch has no receive to refuse, so it always sends it.
    if (!snapTarget && wire.subjectOfVerb
        && (reactiveDesktopArrival || targetEngine->isActiveOnScreen(targetScreenId))) {
        Q_EMIT fullscreenHandBackRequested(windowId);
    }
    bool placedOnTarget = false;
    // Whether the compositor must move the window to the target screen's
    // output itself, because no placement geometry carries it there.
    bool needsOutputMove = false;
    const bool crossesOutput =
        !sourceScreen.isEmpty() && !PhosphorScreens::ScreenIdentity::screensMatch(targetScreenId, sourceScreen);
    if (!reactiveDesktopArrival) {
        PhosphorEngine::IPlacementEngine::HandoffContext ctx;
        ctx.windowId = windowId;
        ctx.toScreenId = targetScreenId;
        ctx.toDesktop = targetDesktop;
        ctx.fromEngineId = sourceEngine->engineId();
        ctx.sourceZoneIds = landingZoneIds;
        ctx.minSize = windowMinSize;
        // Only a window its engine places reaches here (the workspace verb
        // moves a float or free window without a handoff), so the arrival is
        // placed too.
        ctx.wasFloating = false;
        // Edge-aware entry for a SCROLLING target: moving right crosses into
        // the strip's LEFT edge, so the arrival becomes the FIRST column
        // (index 0); moving left enters from the right and appends
        // (insertIndex -1 → columnCount() in handoffReceive). This mirrors
        // the snap target's entryZoneForCrossing and the scroll engine's own
        // in-strip boundary semantics — without it every crossing appended
        // at the far end, the opposite edge from the one the window entered.
        // A vertical crossing ("up"/"down" between stacked monitors) has no
        // strip edge to enter from; insertIndex stays -1 and the window
        // appends at the right end, same as "left".
        if (targetMode == PhosphorZones::AssignmentEntry::Scrolling && direction == QLatin1String("right")) {
            ctx.insertIndex = 0;
        }
        // An autotile target's handoffReceive also announces the arrival's
        // tiled (non-floating) state on the passive float-sync channel —
        // intended: the arrival IS tiled, and the relay's last-broadcast
        // gate dedups when the bit already agrees.
        //
        // Guarded: handoffReceive can silently refuse, and the source has
        // already released inside the helper — on refusal the helper
        // re-homes into the source so the window is not stranded
        // tracked-by-no-engine. The re-home desktop is the one the window
        // never left: the compositor legs below are adoption-gated, so on a
        // refusal the real window stays where it was, which is the desktop in
        // view (0) unless the verb found it held on a background desktop.
        placedOnTarget =
            WindowTrackingInternal::guardedHandoff(sourceEngine, targetEngine, ctx, sourceScreen, wire.sourceDesktop);
        // A MONITOR crossing physically relocates the window to a different
        // output. Mark the imminent output change as daemon-owned so the
        // effect's reactive outputChanged handler does not re-issue
        // windowClosed/windowOpened and tear down the placement just made.
        // Post-receive and adoption-gated (same contract as the swap path):
        // a refused receive must not arm a one-shot for a move that never
        // happens — it would swallow the next genuine outputChanged. The
        // effect applies geometry through deferred commits, so the marker
        // signal lands before the output change is processed.
        //
        // The source screen travels on the wire because this arm site runs
        // AFTER the handoff: handoffReceive already pushed the destination's
        // tiles, so the effect's own notified-screen record names the
        // destination by now and cannot answer "where did it come from".
        // screensMatch, not a raw compare: a connector-name / EDID-id spelling
        // difference makes raw inequality unreliable, and a spurious
        // one arms a one-shot for a move that never happens — which then
        // swallows the window's next genuine outputChanged.
        if (placedOnTarget && crossesOutput) {
            announceOutputMove(windowId, targetScreenId, sourceScreen, targetEngine);
            // A snap arrival with no landing zone (an empty destination
            // layout) lands free and places no geometry, so nothing would
            // carry the window to the output the marker above expects it on.
            if (snapTarget && landingZoneIds.isEmpty()) {
                needsOutputMove = true;
            }
        }
    } else {
        // Reactive desktop arrival: no receive here (the target engine's
        // catch-scan places it on the target desktop), so just release. The
        // desktop move below must still fire — the catch-scan only runs
        // once the window actually lands on the target desktop. A snap source
        // releases its record slot too: the window leaves the desktop it was
        // snapped on, and a slot left snapped would read back as a zone on
        // the desktop it moved to (F516).
        sourceEngine->handoffRelease(windowId);
        if (sourceEngine == m_snapEngine.data()) {
            m_service->releaseEngineSlot(windowId, PhosphorEngine::WindowPlacement::snapEngineId());
        }
        placedOnTarget = true;
        // The OUTPUT leg has no other carrier on this branch. The immediate
        // arm above relocates the window through the target engine's placement
        // geometry, but here nothing is placed — the effect's desktop-return
        // catch-scan adopts the window, and that scan keys each window to the
        // output it is PHYSICALLY on. Left on the source output the window is
        // adopted into the wrong screen's tiling state, or into none at all
        // when that output is not tiled.
        //
        // Deliberately NO windowOutputMoveExpected marker: that one-shot exists
        // to stop the effect re-issuing windowClosed/windowOpened for a move the
        // daemon already carried in its own state. Here the source engine has
        // released and no engine holds the window, so the close/open re-issue is
        // exactly the adoption path this branch relies on.
        needsOutputMove = crossesOutput;
    }

    // The compositor legs. A cross-desktop move needs the desktop leg; the
    // output leg rides with it wherever no placement geometry relocates the
    // window, and a workspace verb that moves the output always names the
    // destination output, so the effect judges the arrival's visibility and
    // focus there. Adoption-gated: after a refused handoff the state was
    // re-homed onto the SOURCE context, and physically relocating the real
    // window anyway would part it from its state.
    if (placedOnTarget) {
        const QString outputScreen = (needsOutputMove || wire.moveOutput) ? targetScreenId : QString();
        emitMoveLegs(windowId, targetDesktop > 0 ? targetDesktop : 0, targetDesktop > 0 ? wire.desktopId : QString(),
                     outputScreen, wire.workspaceVerb);
    }
}

void WindowTrackingAdaptor::handleCrossModeFocus(const QString& targetScreenId, const QString& direction, bool* handled)
{
    auto* sourceEngine = qobject_cast<PhosphorEngine::PlacementEngineBase*>(sender());
    if (!sourceEngine || targetScreenId.isEmpty() || !m_layoutManager) {
        return;
    }
    // Monitor crossings only, so the target context is the neighbour screen's
    // CURRENT desktop — the same resolution handleCrossModeMove uses with
    // targetDesktop == 0.
    const QString activity = m_layoutManager->currentActivity();
    const PhosphorZones::AssignmentEntry::Mode targetMode =
        m_layoutManager->modeForScreen(targetScreenId, currentDesktopForScreen(targetScreenId), activity);
    PhosphorEngine::PlacementEngineBase* targetEngine = engineForMode(targetMode);
    if (!targetEngine || targetEngine == sourceEngine) {
        // Either the engine is absent, or the raw mode cascade answered the
        // SOURCE's own mode — which happens for a context-DISABLED neighbour,
        // since modeForScreen consults no resolver while the emitting engine
        // gates on its live (resolver-filtered) screen set. `handled` stays
        // false, so the emitter reports no_target either way.
        return;
    }
    // The resolved engine must actually be ACTIVE on the target screen: the
    // mode cascade alone also names disabled-context screens the engine never
    // adopted, and asking such an engine for an entry window would walk state
    // it does not have.
    if (!targetEngine->isActiveOnScreen(targetScreenId)) {
        return;
    }
    // The entry-edge window facing the source, per the target's own
    // vocabulary — the same per-engine resolution handleCrossModeSwap uses
    // for its partner.
    QString target;
    if (auto* autotileTarget = qobject_cast<PhosphorTileEngine::AutotileEngine*>(targetEngine)) {
        target = autotileTarget->entryWindowForCrossing(targetScreenId, direction);
    } else if (auto* scrollTarget = qobject_cast<PhosphorScrollEngine::ScrollEngine*>(targetEngine)) {
        target = scrollTarget->entryWindowForCrossing(targetScreenId, direction);
    } else if (auto* snapTarget = qobject_cast<PhosphorSnapEngine::SnapEngine*>(targetEngine)) {
        const QString entryZone = snapTarget->entryZoneForCrossing(direction, targetScreenId);
        if (!entryZone.isEmpty()) {
            target = snapTarget->windowInZoneOnScreen(entryZone, targetScreenId);
        }
    }
    if (target.isEmpty()) {
        return; // empty entry edge — handled stays false, emitter reports no_target
    }
    Q_EMIT activateWindowRequested(target);
    if (handled) {
        *handled = true;
    }
}

void WindowTrackingAdaptor::handleCrossModeSwap(const QString& windowId, const QString& targetScreenId,
                                                int targetDesktop, const QString& direction)
{
    auto* sourceEngine = qobject_cast<PhosphorEngine::PlacementEngineBase*>(sender());
    if (!sourceEngine || windowId.isEmpty() || targetScreenId.isEmpty() || !m_layoutManager) {
        return;
    }

    // Swap is never extended across virtual desktops (exchanging with a window on
    // a desktop you can't see is meaningless — move owns cross-desktop relocation).
    // No engine emits a cross-desktop crossModeSwapRequested; this guard is
    // defensive so a stray desktop-targeted swap is a clean no-op, never a move.
    if (targetDesktop > 0) {
        return;
    }

    const QString activity = m_layoutManager->currentActivity();
    const PhosphorZones::AssignmentEntry::Mode targetMode =
        m_layoutManager->modeForScreen(targetScreenId, currentDesktopForScreen(targetScreenId), activity);
    const bool targetIsAutotile = targetMode == PhosphorZones::AssignmentEntry::Autotile;
    PhosphorEngine::PlacementEngineBase* targetEngine = engineForMode(targetMode);
    if (!targetEngine || targetEngine == sourceEngine) {
        return; // target engine unavailable, or not actually cross-mode
    }
    const QString sourceScreen = sourceEngine->screenForTrackedWindow(windowId);
    if (sourceScreen.isEmpty()) {
        return;
    }

    // ── Resolve the swap partner: the target surface's entry-edge window facing
    //    the source, and the slot the focused window will land in (the partner's
    //    slot). ──
    QString partner;
    QStringList focusedLandingZones; // F's landing on a SNAP target (the entry zone)
    int focusedLandingIndex = -1; // F's landing on an AUTOTILE target (partner's index)
    if (targetIsAutotile) {
        if (auto* autotileTarget = qobject_cast<PhosphorTileEngine::AutotileEngine*>(targetEngine)) {
            partner = autotileTarget->entryWindowForCrossing(targetScreenId, direction);
            if (!partner.isEmpty()) {
                focusedLandingIndex = autotileTarget->windowOrderIndexForWindow(targetScreenId, partner);
            }
        }
    } else if (auto* scrollTarget = qobject_cast<PhosphorScrollEngine::ScrollEngine*>(targetEngine)) {
        partner = scrollTarget->entryWindowForCrossing(targetScreenId, direction);
        if (!partner.isEmpty()) {
            // Scroll landing slots are COLUMN indices; the scroll engine's
            // handoffReceive consumes insertIndex in that unit.
            focusedLandingIndex = scrollTarget->columnIndexForWindow(targetScreenId, partner);
        }
    } else if (auto* snapTarget = qobject_cast<PhosphorSnapEngine::SnapEngine*>(targetEngine)) {
        const QString entryZone = snapTarget->entryZoneForCrossing(direction, targetScreenId);
        if (!entryZone.isEmpty()) {
            focusedLandingZones = QStringList{entryZone};
            partner = snapTarget->windowInZoneOnScreen(entryZone, targetScreenId);
        }
    }

    // No partner (empty entry slot) → a plain one-way cross-mode move. The
    // impl, not the slot: the slot recovers its source from sender(), which
    // only survives here because this is a nested slot invocation — passing
    // the engine explicitly keeps the degrade valid from any context.
    if (partner.isEmpty()) {
        crossModeMoveImpl(sourceEngine, windowId, targetScreenId, targetDesktop, direction,
                          /*allowSameEngine=*/false, WorkspaceMoveWire{});
        return;
    }

    // ── Capture the partner's landing on the SOURCE: the focused window's vacated
    //    slot. Captured BEFORE any relinquish so the indices/zones are still live. ──
    QStringList partnerLandingZones; // partner's landing on a SNAP source (F's zone)
    int partnerLandingIndex = -1; // partner's landing on an AUTOTILE source (F's index)
    if (sourceEngine == m_autotileEngine.data()) {
        if (auto* autotileSource = qobject_cast<PhosphorTileEngine::AutotileEngine*>(sourceEngine)) {
            partnerLandingIndex = autotileSource->windowOrderIndexForWindow(sourceScreen, windowId);
        }
    } else if (auto* scrollSource = qobject_cast<PhosphorScrollEngine::ScrollEngine*>(sourceEngine)) {
        partnerLandingIndex = scrollSource->columnIndexForWindow(sourceScreen, windowId);
    } else if (auto* snapSource = qobject_cast<PhosphorSnapEngine::SnapEngine*>(sourceEngine)) {
        const QString fZone = snapSource->zoneForWindow(windowId);
        if (!fZone.isEmpty()) {
            partnerLandingZones = QStringList{fZone};
        }
    }

    // (The one-shot output-move markers are armed at the TAIL of this
    // function, after the receives, and only for windows the destination
    // genuinely adopted — arming for a refused receive would leave a marker
    // that swallows that window's next genuine outputChanged, the exact
    // hazard handleCrossModeMove documents.)

    // ── Relinquish both windows from their current engines (tracking-only).
    //    Min sizes are queried first: release drops each source's tracking,
    //    and the receivers seed from ctx.minSize. ──
    const QSize focusedMinSize = sourceEngine->windowMinimumSize(windowId);
    const QSize partnerMinSize = targetEngine->windowMinimumSize(partner);
    sourceEngine->handoffRelease(windowId);
    targetEngine->handoffRelease(partner);

    // ── Place both windows, VERIFYING each receive. The paired-release
    //    order above is load-bearing (each arrival lands in the other's
    //    vacated slot), so this cannot route through guardedHandoff's
    //    release-then-receive shape — instead each receive is followed by
    //    the same adoption check and, on refusal, a re-home into the
    //    engine that just released the window (its own slot is free again
    //    because the OTHER window is not there yet / was also refused).
    //    An unverified refusal would strand the window tracked by no
    //    engine — the exact hazard guardedHandoff exists for. ──
    //    The re-home carries the window's OWN original slot (@p fallbackZones /
    //    @p fallbackIndex), not a cleared one. A snap fallback engine needs a
    //    zone to put the window BACK: with sourceZoneIds empty its
    //    handoffReceive takes the !wasFloating tail, which records the window's
    //    residence as a free, unsnapped window and places nothing, so a
    //    snapped window refused by the destination would come back unsnapped.
    //    Each window's own slot is already in scope: F's original zone on the
    //    source screen is what the PARTNER was going to land in, and vice
    //    versa.
    const auto receiveVerified = [](PhosphorEngine::IPlacementEngine* dest,
                                    PhosphorEngine::IPlacementEngine* fallbackEngine, const QString& fallbackScreen,
                                    const QStringList& fallbackZones, int fallbackIndex,
                                    PhosphorEngine::IPlacementEngine::HandoffContext ctx) {
        dest->handoffReceive(ctx);
        if (dest->isWindowTracked(ctx.windowId)) {
            return true;
        }
        qCWarning(lcDbusWindow) << "cross-mode swap:" << dest->engineId() << "refused" << ctx.windowId;
        PhosphorEngine::IPlacementEngine::HandoffContext back = ctx;
        back.toScreenId = fallbackScreen;
        back.insertIndex = fallbackIndex;
        back.sourceZoneIds = fallbackZones;
        back.fromEngineId = dest->engineId();
        fallbackEngine->handoffReceive(back);
        if (fallbackEngine->isWindowTracked(ctx.windowId)) {
            qCInfo(lcDbusWindow) << "cross-mode swap: re-homed" << ctx.windowId << "into" << fallbackEngine->engineId()
                                 << "on" << fallbackScreen;
        } else {
            qCWarning(lcDbusWindow) << "cross-mode swap: re-home REFUSED too -" << ctx.windowId
                                    << "is tracked by no engine";
        }
        return false;
    };
    // Whether each receive was actually adopted. The downstream reflow and
    // move-marker gates read these instead of asking isWindowTracked again:
    // the verdict is decided here, and re-deriving it left two sources of
    // truth for one fact.
    // Only the PARTNER's verdict is usable AS THE ADOPTION ANSWER. The focused
    // window's is re-read after both receives (see focusedStillOnTarget
    // below), because the partner's re-home can evict it. focusedAdopted is
    // still needed though: paired with focusedStillOnTarget it is what
    // DISTINGUISHES an eviction (adopted, then displaced, so nothing re-homed
    // it) from a plain refusal (receiveVerified already re-homed it).
    bool focusedAdopted = false;
    bool partnerAdopted = false;
    // Each window bound for a tiling engine leaves its own fullscreen first,
    // as on a move (F526); a snap side ends it in its own placement. Gated, as
    // on the move's immediate branch, on the receiving engine running the
    // screen the window is bound for: a receive it refuses re-homes the window
    // where it was, which never asked it to leave fullscreen. Sent before the
    // receives, because an autotile receive retiles synchronously.
    if (!qobject_cast<PhosphorSnapEngine::SnapEngine*>(targetEngine)
        && targetEngine->isActiveOnScreen(targetScreenId)) {
        Q_EMIT fullscreenHandBackRequested(windowId);
    }
    if (!qobject_cast<PhosphorSnapEngine::SnapEngine*>(sourceEngine) && sourceEngine->isActiveOnScreen(sourceScreen)) {
        Q_EMIT fullscreenHandBackRequested(partner);
    }
    // (Both placements: an autotile receiver's handoffReceive also announces
    // the arrival's tiled state on the passive float-sync channel —
    // intended; the relay's last-broadcast gate dedups an agreeing bit.)
    {
        PhosphorEngine::IPlacementEngine::HandoffContext ctx;
        ctx.windowId = windowId;
        ctx.toScreenId = targetScreenId;
        ctx.fromEngineId = sourceEngine->engineId();
        ctx.sourceZoneIds = focusedLandingZones;
        ctx.minSize = focusedMinSize;
        ctx.insertIndex = focusedLandingIndex;
        ctx.wasFloating = false;
        // Re-home slot for F is its ORIGINAL slot on the source screen, which
        // is the one the partner was going to take.
        focusedAdopted =
            receiveVerified(targetEngine, sourceEngine, sourceScreen, partnerLandingZones, partnerLandingIndex, ctx);
    }
    {
        PhosphorEngine::IPlacementEngine::HandoffContext ctx;
        ctx.windowId = partner;
        ctx.toScreenId = sourceScreen;
        ctx.fromEngineId = targetEngine->engineId();
        ctx.sourceZoneIds = partnerLandingZones;
        ctx.minSize = partnerMinSize;
        ctx.insertIndex = partnerLandingIndex;
        ctx.wasFloating = false;
        // Mirror: the partner's re-home slot is its own original slot on the
        // target screen, the one F was going to take.
        //
        // COLLISION NOTE, so this is not re-raised as data loss: when F's
        // receive was refused, receiveVerified re-homed F into the source at
        // (partnerLandingZones, partnerLandingIndex) — the same slot this
        // receive now targets. No engine EVICTS on receive (snap assigns the
        // window into the zone beside any co-occupant, autotile and scroll
        // inserts shift indices), so the worst outcome is a shared zone or a
        // shifted tile order on the source screen: degraded, visible, and
        // user-recoverable, with both windows still tracked. A pre-emptive
        // slot reroute here would need engine-specific vacancy queries for a
        // state the user can fix with one drag.
        partnerAdopted =
            receiveVerified(sourceEngine, targetEngine, targetScreenId, focusedLandingZones, focusedLandingIndex, ctx);
    }

    // No explicit reflow on either side: each handoffRelease above already
    // scheduled one for the screen the window left (autotile retiles the
    // context in view on a coalesced timer, the scroll engine schedules its
    // own, a snap engine just vacates a zone), so a refused receive that left
    // a slot empty still closes up.
    //
    // focusedAdopted is RE-CHECKED here rather than trusted from the first
    // receive. DEFENSIVE: no in-tree engine evicts a co-occupant on receive
    // (snap assigns the arrival into the zone beside any occupant, autotile
    // and scroll inserts shift indices — all three receivers verified), so
    // with today's engines this always re-reads true when focusedAdopted is
    // true. The re-read stands because it is the only check that would catch
    // a future receiver that DOES displace, and the recovery below depends
    // on knowing, not assuming.
    const bool focusedStillOnTarget = targetEngine->isWindowTracked(windowId);
    // Adopted, then EVICTED by the partner's re-home landing in the same slot
    // (unreachable with today's non-evicting receivers — see the re-check
    // note above — kept as the recovery half of the same defence).
    // receiveVerified already returned true for F, so its own refusal path
    // never ran and nothing has re-homed it — F is tracked by no engine at
    // all, silently. Recover it the way receiveVerified would have: back into
    // the source engine on the source screen, at its original slot (which is
    // the one the partner was going to take).
    // Gated on !partnerAdopted too. The landing slot used below is F's ORIGINAL
    // source slot, which is exactly where the partner goes when ITS receive
    // SUCCEEDS — so re-homing there while the partner holds it would displace
    // one of the two windows, and would also falsify the partner's own
    // move-marker gate below. The eviction this recovers from is by definition
    // caused by the partner's RE-HOME, i.e. its receive was refused. F leaving
    // the target while the partner WAS adopted is a different fault; log it
    // rather than guessing.
    if (focusedAdopted && !focusedStillOnTarget && partnerAdopted) {
        qCWarning(lcDbusWindow) << "cross-mode swap:" << windowId << "left" << targetEngine->engineId()
                                << "but the partner was adopted, so its source slot is taken - not re-homing,"
                                << "window is now tracked by no engine";
    }
    if (focusedAdopted && !focusedStillOnTarget && !partnerAdopted) {
        qCWarning(lcDbusWindow) << "cross-mode swap:" << windowId << "was adopted by" << targetEngine->engineId()
                                << "then evicted by the partner's re-home - re-homing into" << sourceEngine->engineId();
        PhosphorEngine::IPlacementEngine::HandoffContext back;
        back.windowId = windowId;
        back.toScreenId = sourceScreen;
        back.fromEngineId = targetEngine->engineId();
        back.sourceZoneIds = partnerLandingZones;
        back.insertIndex = partnerLandingIndex;
        back.minSize = focusedMinSize;
        back.wasFloating = false;
        sourceEngine->handoffReceive(back);
        if (!sourceEngine->isWindowTracked(windowId)) {
            qCWarning(lcDbusWindow) << "cross-mode swap: eviction re-home REFUSED too -" << windowId
                                    << "is tracked by no engine";
        } else {
            // The recovery re-homed F back across outputs (target → source).
            // Arm the daemon-owned-move marker for it like every other
            // tracked-success placement in this file — an unmarked genuine
            // outputChanged would be re-issued as windowClosed/windowOpened
            // and tear down the placement just made. Tracked-success only;
            // the refused branch above arms nothing.
            if (!PhosphorScreens::ScreenIdentity::screensMatch(sourceScreen, targetScreenId)) {
                announceOutputMove(windowId, sourceScreen, targetScreenId, sourceEngine);
            }
        }
    }

    // ── Arm the daemon-owned-move markers for the windows that were
    //    genuinely adopted. Post-receive arming is still ahead of the
    //    compositor's outputChanged even where the receive placed the window
    //    synchronously (an autotile receive retiles at once): the effect
    //    applies placement geometry through deferred commits, so this signal,
    //    sent on the same connection, lands before the output change is
    //    processed. A refused receive arms nothing. ──
    //    Both markers carry their source explicitly, same reason as the move
    //    path: the receives above already re-pointed the effect's
    //    notified-screen records at each window's destination.
    //    focusedStillOnTarget, not focusedAdopted: arming the one-shot for a
    //    window the target no longer holds would swallow that window's next
    //    genuine outputChanged. screensMatch rather than a raw compare, for
    //    the connector-name / EDID-id spelling reasons guardedHandoff
    //    documents — a spurious inequality here arms a move that never happens.
    if (!PhosphorScreens::ScreenIdentity::screensMatch(targetScreenId, sourceScreen)) {
        if (focusedStillOnTarget) {
            announceOutputMove(windowId, targetScreenId, sourceScreen, targetEngine);
        }
        // The isWindowTracked term is the partner-side twin of the one
        // deliberate focusedStillOnTarget re-read above, with the same
        // justification: arming the one-shot for a window the source no
        // longer holds would swallow its next genuine outputChanged. With
        // today's non-evicting receivers it always agrees with
        // partnerAdopted; it stands for the same future-receiver reason.
        if (partnerAdopted && sourceEngine->isWindowTracked(partner)) {
            announceOutputMove(partner, sourceScreen, targetScreenId, sourceEngine);
        }
    }
}

} // namespace PlasmaZones
