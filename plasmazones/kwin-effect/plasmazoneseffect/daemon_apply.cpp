// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plasmazoneseffect.h"
#include "runningwindowsdecisions.h"
#include "windowcontexthold.h"

#include "shader_internal.h"
#include "compositor/effectlogging.h"

#include <PhosphorAnimation/ProfilePaths.h>
#include <PhosphorIdentity/WindowId.h>
#include <PhosphorProtocol/ClientHelpers.h>
#include <PhosphorProtocol/ServiceConstants.h>
#include <PhosphorProtocol/WindowMarshalling.h>
#include <PhosphorProtocol/WindowTypes.h>

#include <core/output.h>
#include <effect/effecthandler.h>
#include <virtualdesktops.h>
#include <window.h>
#include <workspace.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPointer>
#include <QScopedValueRollback>
#include <QStringList>

#include "tilinghandler/tilinghandler.h"
#include "handlers/dragtracker.h"
#include "handlers/navigationhandler.h"
#include "handlers/snapassisthandler.h"
#include "handlers/snaphandler.h"
#include "window_query.h"

#include <cstdlib>

namespace PlasmaZones {

namespace {
// A daemon move to another screen: the window's rule verdicts were resolved
// where it is, and are owed again where it lands (F160, F333). The output
// handler's frame arm drops them once a frame lands on @p to.
void noteRuleScreenOwed(QHash<KWin::EffectWindow*, QString>& owed, KWin::EffectWindow* w, const QString& from,
                        const QString& to)
{
    if (from != to) {
        owed.insert(w, to);
    } else {
        owed.remove(w);
    }
}
} // namespace

void PlasmaZonesEffect::emitNavigationFeedback(bool success, const QString& action, const QString& reason,
                                               const QString& sourceZoneId, const QString& targetZoneId,
                                               const QString& screenId)
{
    // Call D-Bus method on daemon to report navigation feedback (can't emit signals on another service's interface)
    if (!isDaemonReady("report navigation feedback")) {
        return;
    }
    PhosphorProtocol::ClientHelpers::fireAndForget(this, PhosphorProtocol::Service::Interface::WindowTracking,
                                                   QStringLiteral("reportNavigationFeedback"),
                                                   {success, action, reason, sourceZoneId, targetZoneId, screenId});
}

void PlasmaZonesEffect::slotFullscreenHandBackRequested(const QString& windowId)
{
    // A user verb the daemon placed no geometry for (a float with no
    // float-back, a move to a tiling engine whose tile batch would bail on a
    // fullscreen surface): the window leaves its own fullscreen where KWin's
    // restore rect has it (F546, F526, F551). A windowed-fullscreen member's
    // fullscreen is the strip's, and a minimized window has nothing to show.
    // By instance: the cross-mode producers name a window by the engine's
    // composite, which drifts from the effect's own after a class mutation.
    KWin::EffectWindow* w = findWindowByInstanceId(windowId);
    KWin::Window* kw = w && !w->isDeleted() ? w->window() : nullptr;
    if (!kw || !kw->isRequestedFullScreen() || w->isMinimized()
        || m_windowedFullscreenWindows.contains(getWindowId(w))) {
        return;
    }
    qCInfo(lcEffect) << "Ending the fullscreen of" << windowId << "for a user verb with no placement";
    const auto applyGuard = geometryApplyScope();
    m_tilingHandler->applyFullScreenSuppressed(kw, false);
}

void PlasmaZonesEffect::slotActivateWindowRequested(const QString& windowId)
{
    // Showing-desktop guard: this activation is daemon-relayed (snap engine
    // navigation/commit) and Workspace::activateWindow on a hidden window
    // synchronously cancels a peek — see isShowingDesktop's doc for the policy.
    if (PlasmaZonesEffect::isShowingDesktop()) {
        qCDebug(lcEffect) << "slotActivateWindowRequested: dropped during show desktop" << windowId;
        return;
    }
    // Instance-exact, like the desktop-move slot below: activation is about
    // one window, and the app-id fallback would activate a same-app SIBLING,
    // possibly on another desktop or activity, when the named window closed
    // inside the round trip.
    KWin::EffectWindow* w = findWindowByInstanceId(windowId);
    if (w) {
        KWin::effects->activateWindow(w);
    } else {
        qCDebug(lcEffect) << "slotActivateWindowRequested: window not found" << windowId;
    }
}

namespace {
/// The desktop KWin numbers @p desktop (1-based), or nullptr when no desktop
/// carries that number. KWin keeps x11DesktopNumber() equal to the desktop's
/// position plus one through every create, remove and move
/// (VirtualDesktopManager renumbers the tail each time), and the daemon reads
/// every number it sends off x11DesktopNumber(), so one bounds-checked index
/// serves every numbered request. The lower bound matters as much as the
/// upper: the number arrives over D-Bus, and 0 is WindowPlacement's own
/// "every desktop / unknown" sentinel.
KWin::VirtualDesktop* desktopByNumber(int desktop)
{
    const QList<KWin::VirtualDesktop*> all = KWin::effects->desktops();
    return desktop >= 1 && desktop <= all.size() ? all.at(desktop - 1) : nullptr;
}

/// The desktop whose stable id (VirtualDesktop::id()) is @p desktopId, or
/// nullptr for an empty id or a desktop removed since the daemon resolved it.
KWin::VirtualDesktop* desktopById(const QString& desktopId)
{
    if (desktopId.isEmpty()) {
        return nullptr;
    }
    const QList<KWin::VirtualDesktop*> all = KWin::effects->desktops();
    for (KWin::VirtualDesktop* vd : all) {
        if (vd && vd->id() == desktopId) {
            return vd;
        }
    }
    return nullptr;
}
} // namespace

void PlasmaZonesEffect::slotWindowDesktopMoveRequested(const QString& windowId, int desktop)
{
    // Instance-exact: every producer names a live window it just resolved
    // or navigated (the open-path RouteToDesktop, the directional and
    // cross-mode moves). The app-id fallback would move a same-app SIBLING
    // when the named window closed inside the round trip, and then park
    // that sibling for a desktop-arrival restore nobody asked for.
    KWin::EffectWindow* w = findWindowByInstanceId(windowId);
    if (!w) {
        qCDebug(lcEffect) << "slotWindowDesktopMoveRequested: window not found" << windowId;
        return;
    }
    // A number naming no desktop reaches applyWorkspaceMove as no target,
    // which routes it to the recovery restore.
    KWin::VirtualDesktop* const target = desktopByNumber(desktop);
    if (!target) {
        qCDebug(lcEffect) << "slotWindowDesktopMoveRequested: no desktop numbered" << desktop << "for" << windowId;
    }
    applyWorkspaceMove(w, target, nullptr, windowId, /*desktopLeg=*/true);
}

void PlasmaZonesEffect::slotWindowWorkspaceMoveRequested(const QString& windowId, const QString& desktopId, int desktop,
                                                         const QString& targetScreenId)
{
    if (windowId.isEmpty()) {
        return;
    }
    // Instance-exact, for the reason the numbered variant gives. The column
    // verb names a window by the engine's first-contact composite, which the
    // instance step still resolves after a class drift.
    KWin::EffectWindow* w = findWindowByInstanceId(windowId);
    if (!w) {
        qCDebug(lcEffect) << "slotWindowWorkspaceMoveRequested: window not found" << windowId;
        return;
    }
    // No id and no number is an output-only move, which the daemon sends for
    // a window that crosses to another monitor with no zone to land in (F686):
    // it joins the desktop the destination shows, as KWin's Move to Screen
    // does, and takes no desktop leg.
    const bool desktopLeg = !desktopId.isEmpty() || desktop >= 1;
    // The id when the daemon held one: a position resolved before the D-Bus
    // hop can name another desktop by the time it lands, because the
    // workspace feature renumbers alongside its own moves. An id that is gone
    // means the desktop was reaped meanwhile, and there is nowhere to put the
    // window: applyWorkspaceMove places it where it is instead.
    KWin::VirtualDesktop* const target = !desktopLeg ? nullptr
        : desktopId.isEmpty()                        ? desktopByNumber(desktop)
                                                     : desktopById(desktopId);
    if (desktopLeg && !target) {
        qCDebug(lcEffect) << "slotWindowWorkspaceMoveRequested: no desktop" << desktopId << desktop << "for"
                          << windowId;
    }
    // An unknown screen moves the desktop only, as the interface promises.
    KWin::LogicalOutput* const destination = outputForScreenId(targetScreenId);
    if (!targetScreenId.isEmpty() && !destination) {
        qCDebug(lcEffect) << "slotWindowWorkspaceMoveRequested: unknown screen" << targetScreenId
                          << "- moving the desktop only for" << windowId;
    }
    if (!desktopLeg && !destination) {
        return; // neither leg names anything this effect can carry out
    }
    applyWorkspaceMove(w, target, destination, windowId, desktopLeg);
}

void PlasmaZonesEffect::applyWorkspaceMove(KWin::EffectWindow* w, KWin::VirtualDesktop* target,
                                           KWin::LogicalOutput* destination, const QString& windowId, bool desktopLeg)
{
    if (!w || w->isDeleted() || !m_snapHandler) {
        return;
    }
    if (!desktopLeg) {
        applyOutputOnlyMove(w, destination, windowId);
        return;
    }
    // A sticky (on-all-desktops) window is already present on the target; pinning
    // it to a single desktop here would silently un-sticky it. Directional
    // cross-desktop move is meaningless for an everywhere window, so leave it.
    // Checked BEFORE the no-target arm below: a sticky window is present on every
    // desktop, so it was never displaced, and driving the recovery restore at
    // it (for a 0 "all desktops" sentinel, say) would re-place a window that
    // went nowhere.
    if (w->isOnAllDesktops()) {
        qCDebug(lcEffect) << "desktop move: window is on all desktops, ignoring" << windowId;
        return;
    }
    if (!target) {
        qCDebug(lcEffect) << "desktop move: no such desktop, placing" << windowId << "where it is instead";
        placeAfterRefusedDesktopMove(w);
        return;
    }
    KWin::Window* const kw = w->window();
    const bool wasActive = KWin::effects->activeWindow() == w;
    // KWin's PENDING output, the one its own sendToOutput compares against: a
    // move KWin has been asked for (the output leg below, or a placement the
    // daemon applied just ahead of this request) shows there before the
    // client commits it, and the committed output still names the old one.
    const auto pendingOutput = [w, kw]() -> KWin::LogicalOutput* {
        return kw ? kw->moveResizeOutput() : w->screen();
    };
    KWin::LogicalOutput* const source = pendingOutput();
    const QList<KWin::VirtualDesktop*> desktopsBefore = w->desktops();

    // Both legs as one step, output first. KWin's sendToOutput re-desks a
    // window onto the desktop its new output shows
    // (Window::finishInteractiveOutputChange), so a desktop written before it
    // would be swapped or doubled ({target, shown there}); written after it,
    // the desktop leg replaces whatever KWin chose. The edits the two legs emit
    // are held until both ran, so the daemon hears the final set once instead
    // of KWin's intermediate one.
    const bool outputLeg = destination && kw && source != destination;

    // KWin's own Window to Desktop hands focus on when it hides the active
    // window, and setDesktops does not: the hidden window kept the keyboard
    // and stayed the daemon's focused window, so the next shortcut moved it
    // again (F762). Whether the move hides it is judged on the output it
    // lands on.
    //
    // With an output leg the successor is picked BEFORE the legs, while the
    // window still sits on the output it is leaving (C55): activateNextWindow
    // picks on the window's committed output, which after sendToOutput names
    // the destination when the size is unchanged and still the source while a
    // size change waits for the client, so a hand-off after the legs focused
    // either monitor depending on the window's size. Handing off first also
    // keeps sendToOutput from making the destination the active output, which
    // it does only for the active window. Two costs, both kept: with Next Focus
    // Prefers Mouse the window is still under the pointer, so KWin skips it and
    // takes the focus chain's pick rather than whatever the pointer uncovers;
    // and a leg KWin then refuses (a Force screen or desktop rule) gives the
    // focus back, so it passes through the successor and returns. The refusal
    // cannot be decided first: KWin answers it in WindowRules::checkOutput and
    // checkDesktops, which libkwin does not export to effects.
    //
    // A desktop-only move keeps KWin's own order (Workspace::sendWindowToDesktops
    // hands off after setDesktops): the committed output does not change, so
    // the hand-off after the leg below picks on the right one already.
    // activateNextWindow returns when the window is not the active one.
    KWin::Workspace* const ws = KWin::workspace();
    bool handedFocusOn = false;
    if (outputLeg && wasActive && ws) {
        KWin::VirtualDesktop* const shownAtDestination = KWin::effects->currentDesktop(destination);
        if (shownAtDestination && shownAtDestination != target) {
            ws->activateNextWindow(kw);
            handedFocusOn = KWin::effects->activeWindow() != w;
        }
    }

    if (outputLeg) {
        m_contextHold.holds[w].reasons |= WindowContextHold::WorkspaceMove;
    }
    // Released as soon as the legs are settled, before anything below reads
    // the window: the held edit runs then on the final set, where an unheld
    // one would have run inside the desktop write.
    const auto endHold = [this, w, outputLeg]() {
        if (outputLeg) {
            releaseContextHold(w, WindowContextHold::WorkspaceMove);
        }
    };
    // A refused leg undoes whatever the legs already did, so the window stays
    // where it was rather than on a desktop its output never shows, which the
    // owner-wins snap-back would then bounce (F811), and is placed again there.
    // A window that handed its focus on for a move that did not happen takes
    // it back, since it never left the screen.
    const auto refuse = [this, w, source, desktopsBefore, pendingOutput, endHold, handedFocusOn](const char* leg) {
        qCInfo(lcEffect) << "workspace move: KWin kept" << getWindowId(w) << "off the target" << leg
                         << "- undoing the move";
        if (source && pendingOutput() != source) {
            KWin::effects->windowToScreen(w, source);
        }
        if (w->desktops() != desktopsBefore) {
            KWin::effects->windowToDesktops(w, desktopsBefore);
        }
        endHold();
        if (handedFocusOn && !w->isDeleted()) {
            KWin::effects->activateWindow(w);
        }
        placeAfterRefusedDesktopMove(w);
    };
    if (outputLeg) {
        KWin::effects->windowToScreen(w, destination);
        // A Force screen rule, or a window KWin does not send between outputs,
        // leaves it elsewhere without a word.
        if (pendingOutput() != destination) {
            refuse("output");
            return;
        }
    }
    // Single-desktop membership (not on-all-desktops) so the window genuinely
    // moves to the target.
    KWin::effects->windowToDesktops(w, {target});
    // A Force desktop rule refuses the write the same silent way.
    if (!w->isOnDesktop(target) || w->isOnAllDesktops()) {
        refuse("desktop");
        return;
    }
    endHold();
    const bool openContinuation = m_snapHandler->openResolveInFlight(getWindowId(w));
    // An open's RouteToDesktop names the desktop its placement keeps: the
    // re-desk of a crossing that placement makes leaves it there (F719).
    if (openContinuation) {
        m_contextHold.pinnedDesktops.insert(w, target->id());
    }
    // A crossing PlasmaZones' own placement armed before this request (the
    // daemon applies a snap target's zone ahead of the workspace move) would
    // otherwise re-desk the window at its landing onto the desktop the
    // destination shows, leaving it on two desktops (F364). The move named
    // this one, so the landing keeps it.
    if (const auto crossing = m_contextHold.pzCrossings.find(w); crossing != m_contextHold.pzCrossings.end()) {
        crossing->pinnedDesktopId = target->id();
    }

    // The window has just left the desktop on screen, so nothing will place it
    // until the user goes to where it went. On a tiling or scrolling screen that
    // handler's desktop-return catch-scan re-announces it; snapping has no such
    // sweep, so park it for SnapHandler to re-drive on arrival. The cause decides
    // what the arrival may do: an open's continuation runs the open's restore
    // chain, every other producer (the desktop shortcut, a cross-mode handoff,
    // the shell's move, a workspace verb) only re-applies the zone the window
    // holds there, so a window the shortcut left unsnapped is not auto-snapped on
    // arrival (F415).
    //
    // Judged on the output the window is GOING to (its pending output), not the
    // one it left and not the global current desktop. Under per-output virtual
    // desktops those differ: a window moved onto a desktop another monitor shows
    // is in view there, and judging it on the source output parked it and handed
    // its focus to a window it left behind (F623). The global reading both
    // over-parks and under-parks the same way.
    KWin::LogicalOutput* const landing = pendingOutput();
    KWin::VirtualDesktop* const shownHere =
        landing ? KWin::effects->currentDesktop(landing) : KWin::effects->currentDesktop();
    if (shownHere != target) {
        m_snapHandler->armDesktopArrivalRestore(getWindowId(w),
                                                openContinuation ? DesktopArrivalParks::Cause::OpenContinuation
                                                                 : DesktopArrivalParks::Cause::ReapplyOnly);
        // A desktop-only move hands focus on here, after its leg as KWin's own
        // Window to Desktop does. A window an output leg carried is still
        // active here only when the hand-off ahead of the legs did not see it
        // hidden (a script switched the destination's desktop inside the
        // legs). Either way the hidden window does not keep the keyboard
        // (F762).
        if (kw && ws && shownHere && !w->isOnDesktop(shownHere) && KWin::effects->activeWindow() == w) {
            ws->activateNextWindow(kw);
        }
    }
}

void PlasmaZonesEffect::applyOutputOnlyMove(KWin::EffectWindow* w, KWin::LogicalOutput* destination,
                                            const QString& windowId)
{
    KWin::Window* const kw = w->window();
    // Already there by KWin's own gate (the pending output), or not a window
    // KWin sends between outputs.
    if (!kw || !destination || kw->moveResizeOutput() == destination) {
        return;
    }
    KWin::LogicalOutput* const source = kw->moveResizeOutput();
    // KWin re-desks the window onto the desktop the destination shows, which
    // is the landing this move wants; the edit runs once, after the move.
    m_contextHold.holds[w].reasons |= WindowContextHold::WorkspaceMove;
    KWin::effects->windowToScreen(w, destination);
    const bool refused = kw->moveResizeOutput() != destination;
    if (refused && source && kw->moveResizeOutput() != source) {
        KWin::effects->windowToScreen(w, source);
    }
    releaseContextHold(w, WindowContextHold::WorkspaceMove);
    if (refused) {
        // A Force screen rule kept it: placed again where it is (F811).
        qCInfo(lcEffect) << "workspace move: KWin kept" << windowId << "off the target output";
        placeAfterRefusedDesktopMove(w);
    }
}

void PlasmaZonesEffect::placeAfterRefusedDesktopMove(KWin::EffectWindow* w)
{
    // Refusing the move is not the end of it. On the open path a RouteToDesktop
    // rule emits the move ahead of the same round trip's resolve, and the snap
    // engine then pins the window's float residence to the ROUTED desktop, and
    // a workspace verb has re-keyed or released the window's engine state for
    // the context it was sent to. If the move never happens, the window sits
    // where it was with its state filed elsewhere, so it is placed again where
    // it actually is. The concrete cases: a user who removed a virtual desktop
    // has rules naming one that no longer exists, a named workspace's desktop
    // reaped between the daemon's resolve and this slot, and a KWin rule that
    // forces the window's screen or desktop (F811, F581).
    const QString liveId = getWindowId(w);
    const QString screenId = getWindowScreenId(w);
    const bool openInFlight = m_snapHandler->openResolveInFlight(liveId);
    if (m_tilingHandler->isManagedScreen(screenId)) {
        // A tiling or scrolling screen: a workspace verb's reactive arrival
        // released the window from its engine before the compositor was asked,
        // and no catch-scan runs for a window that never left the desktop in
        // view. Re-announce it there as the re-placement it is, the effect-only
        // teardown first (a relay would take back a hold the daemon kept), the
        // free geometry stashed around it as the arrival arm does. Not while
        // the window's own open is still out: that announce places it, and a
        // teardown now would undo the tracking it just set up.
        if (openInFlight || m_tilingHandler->announceInFlight(liveId)) {
            return;
        }
        m_tilingHandler->savePreTileForDesktopMove(liveId);
        m_tilingHandler->cleanupAutotileTracking(liveId, ScrollDecisions::ClaimScope::LiveDeparture);
        m_tilingHandler->restorePreTileForDesktopMove(liveId, screenId);
        m_tilingHandler->notifyWindowAdded(w, /*knownFreeFloating=*/false, /*focusEligible=*/false);
        return;
    }
    // A snapping screen: the arrival restore re-drives the resolve for the
    // desktop the window is on (returning at the engine's already-placed
    // guards when the residence is already right). An open's own continuation
    // still owes the window its first placement; any other move only
    // re-applies the zone it holds where it lands (F415).
    m_snapHandler->armDesktopArrivalRestore(
        liveId, openInFlight ? DesktopArrivalParks::Cause::OpenContinuation : DesktopArrivalParks::Cause::ReapplyOnly);
    m_snapHandler->slotDesktopChangedRestoreArrivals();
}

void PlasmaZonesEffect::slotSetScreenDesktopRequested(const QString& screenId, const QString& desktopId,
                                                      int purposeWire)
{
    if (screenId.isEmpty()) {
        return;
    }
    // An output this effect does not know, or KWin's placeholder, gets no
    // answer: there is no live desktop to report for it. That is the only
    // unanswered case, and it is settled here, BEFORE anything switches, so a
    // request is never carried out and then left unanswered. The daemon's
    // SetCurrent ledger counts on one answer per request for every other
    // output (C58).
    KWin::LogicalOutput* const output = outputForScreenId(screenId);
    if (!output || output->isPlaceholder()) {
        qCDebug(lcEffect) << "slotSetScreenDesktopRequested: unknown screen" << screenId;
        return;
    }
    // Never null, here or after the switch below: KWin's
    // VirtualDesktopManager::currentDesktop answers an output it holds no
    // entry for with the desktop it would give a new output (one an activity
    // remembers, else the first), keeps at least one desktop, and only ever
    // moves an output to another real desktop.
    KWin::VirtualDesktop* const before = KWin::effects->currentDesktop(output);
    const PhosphorProtocol::ScreenDesktopSwitchPurpose purpose =
        PhosphorProtocol::clampScreenDesktopSwitchPurposeFromWire(purposeWire);
    const bool correction = purpose == PhosphorProtocol::ScreenDesktopSwitchPurpose::Correction;
    const QString reportedId = outputScreenId(output);
    const int reportedBefore = m_lastScreenDesktop.value(reportedId, -1);

    // By id: a number resolved at the daemon's emit could name another desktop
    // by now, since the workspace feature renumbers alongside its own switches.
    KWin::VirtualDesktop* const target = desktopById(desktopId);
    KWin::VirtualDesktopManager* const vdm = KWin::VirtualDesktopManager::self();
    if (!target) {
        qCDebug(lcEffect) << "slotSetScreenDesktopRequested: no desktop with id" << desktopId << "for" << screenId;
    } else if (vdm && !vdm->isPerOutputVirtualDesktops()) {
        // Refused under global virtual desktops: setCurrentDesktop would switch
        // EVERY output, and the daemon would then see each other output on a
        // desktop it does not own and snap it back, round after round (F14).
        // KWin flips this flag before it switches the outputs and reports the
        // mode change last, so the daemon can still ask in that window; the
        // flag read here is already the new one.
        qCInfo(lcEffect) << "slotSetScreenDesktopRequested: refusing a per-output switch of" << screenId
                         << "while KWin runs global virtual desktops";
    } else if (correction && isShowingDesktop()) {
        // A correction while Show Desktop is up would cancel the user's peek
        // (KWin activates a window on the switched desktop). The forced answer
        // below reports the screen still where it is, and a later correction
        // is the daemon's to issue (F366).
        qCDebug(lcEffect) << "slotSetScreenDesktopRequested: correction of" << screenId
                          << "deferred while showing the desktop";
    } else {
        // Per-output switch: only THIS output changes desktop. A correction is
        // carried out without the effect's own desktop-switch transition, and
        // ends any switch leg the user's own switch began on this output,
        // which the correction undoes (F15, F242). The flag is read by the
        // transition's desktopChanged handler, which KWin calls from inside
        // setCurrentDesktop, and dropped as the call returns.
        const QScopedValueRollback<bool> correctionScope(m_correctiveDesktopSwitch, correction);
        KWin::effects->setCurrentDesktop(target, output);
    }

    // The answer the daemon's SetCurrent ledger reads: this output's live
    // desktop, numbered like every screenDesktopChanged. A switch that
    // happened was already reported from inside the call, by the per-output
    // desktopChanged report or by the activation report KWin's switch
    // triggers ahead of it, and that report is the answer. Every other outcome
    // (no such desktop, a refusal, the deferral, a request already satisfied,
    // or a script that switched the output straight back) reported nothing,
    // so it is forced past the dedup here. Never both, so the daemon hears one
    // answer per request (F22).
    KWin::VirtualDesktop* const live = KWin::effects->currentDesktop(output);
    const int liveNumber = static_cast<int>(live->x11DesktopNumber());
    if (target && live != target) {
        qCWarning(lcEffect) << "slotSetScreenDesktopRequested:" << screenId << "is on" << liveNumber
                            << "after a request for" << desktopId;
    }
    const bool answeredInCall =
        live != before && reportedBefore != liveNumber && m_lastScreenDesktop.value(reportedId, -1) == liveNumber;
    if (!answeredInCall) {
        reportScreenDesktop(reportedId, liveNumber, /*force=*/true);
    }
}

void PlasmaZonesEffect::slotWindowOutputMoveExpected(const QString& windowId, const QString& targetScreenId,
                                                     const QString& sourceScreenId)
{
    if (windowId.isEmpty() || targetScreenId.isEmpty()) {
        return;
    }
    // Hand the one-shot to the autotile handler: it owns the cross-output
    // outputChanged transfer path that would otherwise re-issue close/open.
    // The handler is constructed with the effect, like every other slot in
    // this file assumes.
    m_tilingHandler->markExpectedOutputMove(windowId, targetScreenId, sourceScreenId);
}

void PlasmaZonesEffect::slotApplyGeometryRequested(const QString& windowId, int x, int y, int width, int height,
                                                   const QString& zoneId, const QString& screenId, bool sizeOnly,
                                                   int purposeWire)
{
    const PhosphorProtocol::PlacementPurpose purpose = PhosphorProtocol::clampPlacementPurposeFromWire(purposeWire);
    // Magnitude bound BEFORE either QRect construction below. QRect's
    // x/y/w/h constructor computes `x + w - 1`, which is signed overflow and
    // undefined for a garbled payload — and it happens inside the constructor,
    // so the `isValid()` and `width > 0` guards further down run too late to be
    // a defence. Same ceilings the tile wire applies, shared from WindowTypes.h.
    // Deliberately magnitude only, not a screen-bounds test: legitimate parked
    // columns sit far outside every output.
    // Widened before the absolute value: qAbs(INT_MIN) is itself undefined in
    // int and in practice yields INT_MIN back, which is negative, so every
    // comparison here would be false and the guard would fail OPEN on exactly
    // the payload it exists to stop. Mirrors WindowGeometryEntry's validator.
    if (std::abs(static_cast<qint64>(width)) > PhosphorProtocol::MaxWireExtent
        || std::abs(static_cast<qint64>(height)) > PhosphorProtocol::MaxWireExtent
        || std::abs(static_cast<qint64>(x)) > PhosphorProtocol::MaxWireOrigin
        || std::abs(static_cast<qint64>(y)) > PhosphorProtocol::MaxWireOrigin) {
        qCWarning(lcEffect) << "slotApplyGeometryRequested: implausible geometry for" << windowId << x << y << width
                            << height << "— dropping";
        return;
    }

    // Every apply names ONE window the daemon resolved moments ago, so it
    // resolves by instance (exact id, else the same instance under a drifted
    // prefix; the tiling engines address windows by the daemon's canonical
    // composite) and never by the app-id fallback: a miss means the window
    // closed inside the round trip, and the fallback would land its geometry,
    // its zone and its snapped mark (or, for a size-only apply, the resize and
    // the clearWindowSnapped below) on the sole surviving same-app sibling.
    KWin::EffectWindow* w = findWindowByInstanceId(windowId);
    if (!w) {
        qCDebug(lcEffect) << "slotApplyGeometryRequested: window not found" << windowId;
        return;
    }
    // Key ALL tracking by the window's LIVE id, not the daemon-supplied one:
    // an instance resolved under a drifted prefix carries a current id that
    // differs from the one the daemon sent. Tracking recorded under the
    // daemon's spelling would never be cleared (every later drag-out, float
    // and close path uses the live id), leaving a stale tiled entry and a
    // permanently hidden title bar. Every other commit path (batch, drag,
    // snap assist) already keys by the live id.
    const QString liveWindowId = getWindowId(w);
    // An open-path apply arrives under first-frame suppression, before the
    // window has painted, so it is a teleport: an animated move would play
    // over the open shader from a spot the user never saw. An apply that
    // lands after the suppression was released (deadline expired, or a late
    // float default) animates instead, which is right for
    // a window already visible.
    const bool freshOpen = m_restoreSuppress.contains(w);

    // Size-only restore: the daemon sets sizeOnly=true to restore a window's
    // free width/height while keeping it where it stands. Two producers: the
    // drag-out unsnap (the daemon kept us at the drop position but restored
    // pre-snap dimensions, logically a snap-out, not an in-zone resize) and
    // the open-path free-size restore (a window left floating that inherited
    // a snapped sibling's zone size from its app config, #1106).
    if (sizeOnly) {
        if (width > 0 && height > 0) {
            applySizeOnlyRestore(w, liveWindowId, screenId, QSize(width, height), freshOpen);
            // Either way the window is not zone-sized: the drag-out producer
            // left zone-managed sizing, the open-path one never had it (a
            // no-op there beyond one decoration re-resolve).
            m_snapHandler->clearWindowSnapped(liveWindowId);
        } else {
            // Symmetric with the non-sizeOnly invalid-geometry path below: a
            // garbled size-only payload is dropped, but log it rather than
            // failing silently.
            qCWarning(lcEffect) << "slotApplyGeometryRequested: invalid size-only dimensions for" << windowId << width
                                << "x" << height << "— dropping";
        }
        return;
    }

    QRect geometry(x, y, width, height);
    if (!geometry.isValid()) {
        qCWarning(lcEffect) << "slotApplyGeometryRequested: invalid geometry" << geometry;
        return;
    }
    // Consume the drag-to-float marker before the minimized branch below can bypass it.
    // The marker is a one-shot: this float-restore event is exactly the one it was
    // waiting for, so it must be cleared whether or not we go on to apply the geometry.
    // Consuming it only on the non-minimized path left it armed when a drag-floated window
    // was minimized, and the next legitimate float-restore for that window was then wrongly
    // skipped by the stale marker. A malformed payload rejected above returns before this
    // line and leaves the marker in place, but only until drag end, which clears it
    // whatever happened (lifecycle_wiring_drag.cpp).
    const bool wasDragFloated = zoneId.isEmpty() && m_dragActivation.floatedWindowIds.remove(liveWindowId);

    // Skip float-restore GEOMETRY on minimized windows: when a snapped window is minimized
    // we float it (to free the zone slot), but applying the pre-tile geometry while minimized
    // would poison what KWin restores to on unminimize, causing a visible flash of the
    // pre-snap geometry before the unfloat re-snaps to the zone.
    // A flag rather than a return, mirroring the batch twin's
    // skipMinimizedRestore: the snap-tracking discriminator below still runs
    // — the entry genuinely un-snaps the window regardless of visibility,
    // and a return here left the border set and the ZoneCache holding a
    // window that was no longer snapped (stale snapped-scoped chrome and
    // rule verdicts until an unrelated authoritative edge).
    bool skipGeometry = false;
    if (w->isMinimized() && zoneId.isEmpty()) {
        qCDebug(lcEffect) << "slotApplyGeometryRequested: skipping float-restore geometry on minimized window:"
                          << windowId;
        skipGeometry = true;
    }
    // Skip float-restore geometry for drag-to-float: when the user drags a window
    // off the autotile layout, the daemon restores pre-autotile geometry. But the
    // user expects the window to stay where they dropped it, not snap back.
    // Same flag, same reason: the un-snap bookkeeping below must still run.
    if (wasDragFloated) {
        qCInfo(lcEffect) << "slotApplyGeometryRequested: skipping float-restore for drag-floated window:"
                         << liveWindowId;
        skipGeometry = true;
    }
    // A skipped float-restore is still the window's latest command: a cascade
    // entry still pending for it would otherwise land its zone and mark it
    // snapped after the un-snap below. It moves nothing, so the rect the last
    // landed command asked for still stands, and a superseded configure's ack
    // still owes that answer: the frame hook would otherwise drop it as stale
    // and let the older size land (F1056, F1115).
    if (skipGeometry) {
        const quint64 previous = m_daemonGate.commandStamps.current(w);
        beginGeometryCommand(w);
        m_daemonGate.commandStamps.carryStaleAck(w, previous);
    }
    qCInfo(lcEffect) << "slotApplyGeometryRequested:" << windowId << "(live:" << liveWindowId << ") geo:" << geometry
                     << "zoneId:" << zoneId << "screen:" << screenId << "floating:" << isWindowFloating(liveWindowId)
                     << "currentFrame:" << w->frameGeometry()
                     << "restatement:" << (purpose == PhosphorProtocol::PlacementPurpose::Restatement);
    // No pre-snap capture here: every daemon zone apply follows its commit, so
    // the window is already in a zone and the daemon refuses its frame. The
    // daemon records the free frame itself before the commit
    // (SnapEngine::recordFreeFrameBeforeUserSnap).

    // Empty zoneId = float-restore (daemon placing the window back at its pre-snap geometry, e.g.
    // autotile drag-to-float, drag-out unsnap). Non-empty zoneId = snap into a target zone. The
    // shader-tree path differs accordingly so users can give snap-in and snap-out distinct effects.
    if (!skipGeometry) {
        // Same defensive pair as the batch path below and drag_end's
        // ApplySnap: pre-seed the tracked screen from the daemon's
        // authoritative answer (async follow-up frame changes), bracket the
        // apply (the synchronous one) — an ungated fire here let the
        // VS-crossing handler resolve a cross-screen apply against stale
        // state and report a phantom crossing.
        if (!screenId.isEmpty()) {
            noteRuleScreenOwed(m_ruleScreenOwed, w, getWindowScreenId(w), screenId);
            m_trackedScreenPerWindow[w] = screenId;
            m_tilingHandler->updateNotifiedScreen(liveWindowId, screenId);
            // A daemon move of its own focused window, reported now: a
            // virtual-screen move fires no outputChanged to report it later.
            reportActiveWindowScreen(w, screenId);
        }
        // The placement statement, on a snapping screen only
        // (PlacementStatement::statementFor), for a zone and a free spot
        // alike. After the pre-seed above, whose coverage its committed
        // configure rides; before the bracketed apply.
        const std::optional<PlacementStatement::Purpose> statement = PlacementStatement::statementFor(
            !screenId.isEmpty(), m_tilingHandler->isManagedScreen(screenId),
            purpose == PhosphorProtocol::PlacementPurpose::Restatement ? PlacementStatement::Purpose::Restatement
                                                                       : PlacementStatement::Purpose::UserVerb);
        bool keptState = false;
        QRect applyRect = geometry;
        if (statement) {
            // A re-statement keeps a maximize or fullscreen and moves nothing:
            // the zone or float-back is now the rect it returns to (F490,
            // F547, F560, F575).
            const PlacementStatement::Verdict verdict = m_tilingHandler->preparePlacement(w, geometry, *statement);
            keptState = !verdict.apply;
            applyRect = verdict.applyRect;
            // That keep lands no frame, so the output arm never sees the owed
            // screen arrive: the verdicts are owed now, on the screen the
            // pre-seed above already names. Deferred past a drag like there.
            if (keptState && m_ruleScreenOwed.remove(w)) {
                if (m_dragTracker->isDragging()) {
                    m_dragSuppressedRuleInvalidations.insert(liveWindowId);
                } else {
                    invalidateRuleCacheForStateChange(liveWindowId);
                }
            }
        }
        const auto applyGuard = geometryApplyScope();
        // A float-position restore on a fresh open teleports like the
        // size-only twin and the zone commits do: a glide from the spawn
        // rect is the "KDE opened it, then we moved it" read the suppression
        // exists to hide. A zone commit keeps its morph (not an open-path
        // producer on this slot).
        if (!keptState) {
            applyWindowGeometry(w, applyRect, /*allowDuringDrag=*/false,
                                /*skipAnimation=*/zoneId.isEmpty() && freshOpen,
                                zoneId.isEmpty() ? PhosphorAnimation::ProfilePaths::WindowPlaceOut
                                                 : PhosphorAnimation::ProfilePaths::WindowPlaceIn,
                                QRectF(), QRectF(), statement);
        }
    }
    // Track snapping's own border set (mirrors how autotile records at its
    // tile-apply) using a discriminator analogous to the batch path
    // (slotApplyGeometriesBatch). The batch path discriminates on screenId (empty =
    // float/restore) and clears any stale float marker before marking snapped; this
    // single-window path uses the empty zoneId as the float discriminator, since it
    // is only reached for explicit snap commits (which legitimately un-float) and
    // float-restores (which arrive with an empty zoneId). A window can never land in
    // both the snap and autotile border sets (preparePlacement untracked a window the
    // tiling side still held):
    //   - empty zoneId         → float-restore: leave snapping's set
    //   - empty/autotile screen → autotile-managed or unresolved: leave the set
    //                             (TilingHandler tracks autotile-screen windows)
    //   - snap-mode screen      → snap commit
    if (zoneId.isEmpty() || screenId.isEmpty() || m_tilingHandler->isManagedScreen(screenId)) {
        m_snapHandler->clearWindowSnapped(liveWindowId);
    } else {
        // Clear any stale float marker before marking snapped (mirrors the
        // batch path): a surviving float flag poisons the next pre-tile /
        // float-back capture and wrongly exempts the window from the
        // drain-time restore veto. Idempotent local FloatingCache write.
        m_navigationHandler->setWindowFloating(liveWindowId, false);
        m_snapHandler->markWindowSnapped(liveWindowId, screenId);
    }
    // Note: windowSnapped/recordSnapIntent are NOT called here. For daemon-driven
    // navigation, the daemon handles zone bookkeeping internally before emitting
    // applyGeometryRequested. For legacy callers (autotile float restore via
    // applyGeometryForFloat), zoneId is empty so no snap confirmation is needed.
}

void PlasmaZonesEffect::slotApplyGeometriesBatch(const PhosphorProtocol::WindowGeometryList& geometries,
                                                 const QString& action)
{
    if (geometries.isEmpty()) {
        return;
    }
    qCInfo(lcEffect) << "applyGeometriesBatch:" << action;

    struct PendingApply
    {
        QPointer<KWin::EffectWindow> window;
        QRect geometry;
        QString screenId; ///< daemon-authoritative target screen (empty = no override)
        quint64 commandStamp = 0; ///< this entry's WindowCommandStamps stamp
    };
    QVector<PendingApply> pending;

    // Resolution goes through resolveDaemonWindowIds: an entry naming a live
    // window gets that window (or nothing, when this effect does not handle
    // it), and only an entry naming no live window falls back to its app,
    // counting unclaimed windows only. Without the claim set, a batch
    // carrying BOTH a stale-uuid entry and the live-uuid entry for the same
    // app resolved both onto the one live window — a double apply where the
    // losing entry's (often empty) screenId then wiped the snap tracking the
    // winner just marked.
    QVector<const PhosphorProtocol::WindowGeometryEntry*> valid;
    QStringList validIds;
    for (const auto& entry : geometries) {
        // validationError() BEFORE the size test and before any toRect(): it
        // bounds the magnitudes, and the QRect construction inside toRect()
        // overflows before a `<= 0` check can see anything. The size test stays
        // as this consumer's own policy (a zero extent is meaningless here),
        // which the shared validator deliberately leaves to the caller.
        if (const QString invalid = entry.validationError(); !invalid.isEmpty()) {
            qCWarning(lcEffect) << "slotApplyGeometriesBatch: dropping entry —" << invalid;
            continue;
        }
        if (entry.width <= 0 || entry.height <= 0) {
            qCWarning(lcEffect) << "slotApplyGeometriesBatch: dropping" << entry.windowId << "with non-positive size"
                                << entry.width << "x" << entry.height;
            continue;
        }
        valid.append(&entry);
        validIds.append(entry.windowId);
    }
    const QVector<KWin::EffectWindow*> resolved = resolveDaemonWindowIds(validIds, /*admitFullscreen=*/true);
    for (int i = 0; i < valid.size(); ++i) {
        if (KWin::EffectWindow* const window = resolved.at(i)) {
            PendingApply p;
            p.window = QPointer<KWin::EffectWindow>(window);
            p.geometry = valid.at(i)->toRect();
            p.screenId = valid.at(i)->screenId;
            // Scheduling is the command: an older entry still pending for this
            // window, from any cascade, is superseded from here on.
            p.commandStamp = m_daemonGate.commandStamps.bump(window);
            pending.append(p);
        }
    }

    if (pending.isEmpty()) {
        return;
    }

    // Note: ensurePreSnapGeometryStored is NOT called here. Rotate/resnap/
    // vs_reconfigure batches move windows between zones — their pre-tile
    // geometry is already stored from the original snap. snap_all batches
    // carry previously-UNSNAPPED windows with no per-snap capture on this
    // path; those rely on the unified placement store's open-time /
    // free-geometry capture for their float-back instead. The daemon's batch
    // commit (SnapEngine, commit.cpp) unsnaps the __restore__ entries
    // (PhosphorEngine::RestoreSentinel, windows a layout overflows) and owns
    // their float-back rect; calling ensurePreSnapGeometryStored here would
    // race that and store the zone geometry as pre-tile, corrupting the
    // restore path on subsequent mode transitions.

    // Map the daemon's action string to a shader-tree ProfilePath. "resnap" is a layout
    // change (different layout or autotile recompute) — semantically a layout switch. "rotate"
    // moves windows between existing zones in the same layout — a snap-in. Everything else
    // ("restate" for a carry, re-apply or gap reflow, "vs_reconfigure" via the adaptor relay,
    // "snap_all" via the effect-local path, and any future string) defaults to WindowPlaceIn.
    const QString batchProfilePath = (action == QLatin1String("resnap"))
        ? PhosphorAnimation::ProfilePaths::WindowLayoutSwitch
        : PhosphorAnimation::ProfilePaths::WindowPlaceIn;

    // The per-window motion resolve happens inside this callable, at FIRE
    // time, so a motion-tree refetch landing part-way through a cascade
    // animates windows 0..k on the old timing and the rest on the new. That is
    // accepted rather than overlooked: resolving every window's profile up
    // front would pin timing that a queued-but-unstarted leg is better off
    // picking up, and the split needs a config save to land inside one
    // stagger, where the visible result is a batch that changes pace rather
    // than anything incorrect.
    applyStaggeredOrImmediate(
        pending.size(),
        [this, pending, batchProfilePath](int i) {
            const auto& p = pending[i];
            // Drop this apply if any newer command for THIS window has landed
            // since it was scheduled (a later batch carrying it, a keyboard
            // move, a float, a drag end): a later-firing tick of this older
            // cascade would otherwise put the stale rect back over it. Per
            // window, so a newer batch that does not carry the window leaves
            // its entry alone. Only observable with cascade stagger enabled.
            if (p.window && !m_daemonGate.commandStamps.isCurrent(p.window.data(), p.commandStamp)) {
                qCDebug(lcEffect) << "Batch apply superseded by a newer command for" << getWindowId(p.window.data());
                return;
            }
            // isDeleted too, not just destruction: close-shader grabs (which
            // this effect takes) keep deleted windows alive in the stacking
            // order for the close-animation duration, and the stagger delay
            // widens the race window — moving/animating a dying window would
            // also re-pollute the just-scrubbed id caches via getWindowId.
            if (!p.window || p.window->isDeleted()) {
                return;
            }
            // Seed the tracked-screen cache from the daemon's authoritative answer for
            // this batch BEFORE applyWindowGeometry, not after. Empty screenId means the
            // daemon didn't supply an authoritative answer (e.g. autotile float-restore
            // path) — fall through to the existing geometry-based behavior in that case.
            // The pre-seed handles async follow-up frame changes; m_daemonGate.inGeometryApply
            // (set below) handles the synchronous frame change emitted from inside
            // applyWindowGeometry, which would otherwise resolve the new position against
            // pre-rotation m_virtualScreenDefs and report a phantom cross-VS unsnap.
            if (!p.screenId.isEmpty()) {
                noteRuleScreenOwed(m_ruleScreenOwed, p.window, getWindowScreenId(p.window), p.screenId);
                m_trackedScreenPerWindow[p.window] = p.screenId;
                m_tilingHandler->updateNotifiedScreen(getWindowId(p.window), p.screenId);
                // Same report as the single-window apply.
                reportActiveWindowScreen(p.window, p.screenId);
            }
            const auto guard = geometryApplyScope();
            // Minimized guard for float/restore entries (empty screenId), the
            // batch twin of slotApplyGeometryRequested's check: applying the
            // pre-tile geometry while minimized would poison what KWin
            // restores to on unminimize, causing a visible flash of the
            // pre-snap geometry before the unfloat re-snaps to the zone. The
            // snap-tracking bookkeeping below still runs — the entry
            // genuinely un-snaps the window regardless of visibility.
            const bool skipMinimizedRestore = p.screenId.isEmpty() && p.window->isMinimized();
            if (!skipMinimizedRestore) {
                // The placement statement on a snapping screen. Every batch
                // entry is judged as a re-statement, those of the user-started
                // snap_all and rotate batches included, so a maximized or
                // fullscreen window keeps that state and the rect becomes the
                // one it returns to (F509, F547). A float/restore entry (empty
                // screenId) is judged on the screen its rect lies on, and keeps
                // a maximize the same way (F548).
                const QString target = p.screenId.isEmpty()
                    ? resolveEffectiveScreenId(p.geometry.center(), KWin::effects->screenAt(p.geometry.center()))
                    : p.screenId;
                const std::optional<PlacementStatement::Purpose> statement =
                    PlacementStatement::statementFor(!target.isEmpty(), m_tilingHandler->isManagedScreen(target),
                                                     PlacementStatement::Purpose::Restatement);
                bool keptState = false;
                QRect applyRect = p.geometry;
                if (statement) {
                    const PlacementStatement::Verdict verdict =
                        m_tilingHandler->preparePlacement(p.window, p.geometry, *statement);
                    keptState = !verdict.apply;
                    applyRect = verdict.applyRect;
                    // The single apply's owed-screen settle: a keep lands no frame.
                    if (keptState && m_ruleScreenOwed.remove(p.window)) {
                        const QString keptId = getWindowId(p.window);
                        if (m_dragTracker->isDragging()) {
                            m_dragSuppressedRuleInvalidations.insert(keptId);
                        } else {
                            invalidateRuleCacheForStateChange(keptId);
                        }
                    }
                }
                if (!keptState) {
                    applyWindowGeometry(p.window, applyRect, /*allowDuringDrag=*/false,
                                        /*skipAnimation=*/false, batchProfilePath, QRectF(), QRectF(), statement);
                }
            }
            // Snapping owns its border set (mirrors autotile). The daemon
            // supplies a non-empty authoritative screenId only for real
            // placements; an EMPTY screenId marks a float/restore entry
            // (overflow __restore__, autotile float-restore) — never a snap
            // commit. Use p.screenId directly: a current-screen fallback would
            // misclassify a float-restore as a snap and leave a stale border.
            //   - empty screenId      → float/restore: leave snapping's set
            //   - autotile-mode screen → now autotile-managed: leave snap set
            //                            (TilingHandler tracks it)
            //   - snap-mode screen     → snap commit (clears any stale float marker)
            const QString batchWid = getWindowId(p.window);
            if (p.screenId.isEmpty() || m_tilingHandler->isManagedScreen(p.screenId)) {
                m_snapHandler->clearWindowSnapped(batchWid);
            } else if (m_snapHandler->isMinimizeFloated(batchWid)) {
                // Its minimize floated it after this batch was scheduled
                // (F1098): the zone's rect stays its unminimize frame, but the
                // daemon holds it floating, so it is neither marked nor
                // unfloated here. The suspension's return re-states the zone
                // through the single apply, which marks it then.
            } else {
                // Real snap commit on a snap-mode screen. The daemon emits a non-empty
                // authoritative screenId ONLY for genuine placements; float/restore
                // entries carry an EMPTY screenId and are handled above. So this window
                // is being snapped and is no longer floating — even if the effect's
                // float cache is stale (e.g. a window snapped straight from a
                // floated-in-autotile state via the daemon's windowsReleased snap-zone
                // restore). Clear the stale float marker: a surviving float flag
                // poisons the next pre-tile/float-back capture (the zone rect would be
                // saved as the "free" geometry) and wrongly exempts the window from
                // the drain-time restore veto. setWindowFloating is an idempotent
                // local FloatingCache write (no signal/D-Bus), so it is called
                // unconditionally — no need to read-guard a no-op overwrite.
                m_navigationHandler->setWindowFloating(batchWid, false);
                m_snapHandler->markWindowSnapped(batchWid, p.screenId);
            }
        },
        [this, action]() {
            // No z-order restore: a snap batch has no overlap group and KWin's
            // moveResize does not raise, so replaying the stacking order from
            // the batch's arrival could only bury a window the user raised
            // during the cascade (F329).
            //
            // Show snap assist after resnap if applicable.
            //
            // A resnap follows a layout switch or a mode toggle; rotate,
            // vs_reconfigure and restate carry their own actions and show
            // none. It is a bulk operation, not a per-window snap, so the
            // continuation is anchored to the active window: snap assist shows ONLY if the
            // resnap actually placed the active window in a zone. Passing its
            // windowId as the anchor makes showContinuationIfNeeded gate on
            // "this window is snapped", which also guarantees at least one
            // zone is occupied. Without the anchor, a resnap that snapped
            // nothing (e.g. toggling to snap mode with no prior assignments)
            // left every zone empty and popped snap assist for all of them.
            if (action == QLatin1String("resnap") && m_snapAssistHandler->isEnabled()) {
                KWin::EffectWindow* activeWin = getActiveWindow();
                QString activeScreenId = activeWin ? getWindowScreenId(activeWin) : QString();
                if (activeWin && !activeScreenId.isEmpty() && !m_tilingHandler->isManagedScreen(activeScreenId)) {
                    m_snapAssistHandler->showContinuationIfNeeded(activeScreenId, getWindowId(activeWin));
                }
            }
        });
}

void PlasmaZonesEffect::slotRaiseWindowsRequested(const QStringList& windowIds)
{
    auto* ws = KWin::Workspace::self();
    if (!ws) {
        return;
    }

    for (const QString& windowId : windowIds) {
        // By instance, as the sibling slots resolve a daemon-named window: the
        // app-id fallback of findWindowById could raise a same-app sibling.
        KWin::EffectWindow* w = findWindowByInstanceId(windowId);
        if (w && !w->isDeleted()) {
            KWin::Window* kw = w->window();
            if (kw) {
                ws->raiseWindow(kw);
            }
        }
    }
}

void PlasmaZonesEffect::slotWindowFloatingChanged(const QString& windowId, bool isFloating, const QString& screenId)
{
    // Update local floating cache when daemon notifies us of state changes
    // This keeps the effect's cache in sync with the daemon, preventing
    // inverted toggle behavior when a floating window is drag-snapped.
    // Uses full windowId for per-instance tracking (appId fallback in isWindowFloating).
    qCInfo(lcEffect) << "Floating state changed for" << windowId << "- isFloating:" << isFloating;
    // Re-key to the window's LIVE id up front, mirroring slotWindowStateChanged
    // and slotApplyGeometryRequested: every effect-side cache below
    // (FloatingCache, drag-float skip set, snap tracking, rule-cache
    // invalidation) is keyed by the id the effect claimed with, so after a
    // cross-session uuid drift the daemon-supplied id would write entries the
    // rule matcher never reads. Preserve the daemon id when the exact instance
    // is unresolvable (ambiguous fuzzy hit) — same policy as the minimize
    // handlers below.
    bool stillMinimized = false;
    QString liveWindowId = windowId;
    KWin::EffectWindow* liveForOwner = nullptr;
    if (KWin::EffectWindow* live = findWindowByIdExact(windowId)) {
        stillMinimized = live->isMinimized();
        liveWindowId = getWindowId(live);
        liveForOwner = live;
    } else if (KWin::EffectWindow* fuzzy = findWindowById(windowId)) {
        // A same-app window exists but the exact instance is not
        // resolvable (drifted or ambiguous id). Default to PRESERVING
        // ownership: wrongly dropping a still-minimized window's marker
        // strands its unminimize, while wrongly keeping it is healed by
        // the next authoritative edge. Keep the fuzzy hit as the OWNER
        // resolve fallback too: with liveForOwner null and an empty
        // daemon-supplied screenId, the dual-hold repair below would
        // resolve the conflict to SNAP unconditionally instead of to the
        // window's actual screen mode.
        stillMinimized = true;
        liveForOwner = fuzzy;
    }
    m_navigationHandler->setWindowFloating(liveWindowId, isFloating);
    // This slot receives the WindowTracking interface's float signal, which
    // carries floats from every producer (the scroll passive channel's
    // windowFloatingStateSynced among them) and never reaches
    // TilingHandler::slotWindowFloatingChanged, so it never runs
    // applyFloatCleanup. The shed helper mirrors applyFloatCleanup's full
    // shed half — windowed-fullscreen release, clear-in-flight marker,
    // counter-assert rect, centering targets, parked paint hint (with
    // damage), decoration re-drive — not just the fullscreen drop; each of
    // those was otherwise silently bypassed on this channel. For the ACTIVE
    // channel (the Tiling interface's signal) the tiling handler's slot
    // performed the cleanup first and every remove in the shed is a no-op
    // belt. The release helper carries its own suppress counter and
    // inGeometryApply bracket, so the synchronous X11 exit signal cannot
    // re-enter the VS-crossing machinery from here.
    if (isFloating) {
        // Flip the snap facts BEFORE the shed: applyPassiveFloatShed can run
        // reconcileDecorationOnPlacementFlip, whose contract (decorations.cpp)
        // requires callers to flip engine facts first so the resolve sees the
        // new state. clearWindowSnapped drops the ZoneCache entry backing the
        // IsSnapped/Zone rule facts; it is also the backstop for float paths
        // that don't emit applyGeometryRequested with an empty zoneId (e.g. a
        // float toggle with no stored pre-tile geometry) or windowStateChanged
        // with an empty zone. Idempotent when the window wasn't snap-tracked.
        m_snapHandler->clearWindowSnapped(liveWindowId);
        m_tilingHandler->applyPassiveFloatShed(liveWindowId);
        // A float verdict at open is the placement: nothing else will move
        // the window. Every engine emits its geometry or size restore BEFORE
        // this signal, so an entry with no stamped target has no reposition
        // coming and is released now rather than at the 250 ms deadline (a
        // tiling-screen open has no resolve miss to release it). A stamped
        // target is left to the settle hook. By instance, never through the
        // app fallback liveForOwner may hold: a float notice for a window that
        // closed inside the round trip must not end a same-app sibling's
        // suppression, which would let that sibling paint at its spawn rect.
        if (KWin::EffectWindow* const subject = findWindowByInstanceId(windowId)) {
            if (const auto sup = m_restoreSuppress.constFind(subject);
                sup != m_restoreSuppress.constEnd() && !sup->targetGeometry.isValid()) {
                endRestoreSuppression(subject);
            }
        }
    }
    // When a window is unfloated (tiled/snapped), clear the drag-float skip flag.
    // Without this, a subsequent float toggle's geometry restore would be skipped
    // because m_dragActivation.floatedWindowIds still has the entry from the original drag.
    if (!isFloating) {
        m_dragActivation.floatedWindowIds.remove(liveWindowId);
        // A visible external unfloat normally moots any deferred recovery in
        // either engine. The daemon signal has no request generation, though,
        // so an older hidden-unfloat echo can arrive after the window becomes
        // visible. Preserve ownership while a recovery timer or confirmation
        // query is active; that query provides the terminal daemon state.
        // Mode teardown and a hidden snap-zone commit can legitimately clear
        // the daemon's live float while the window is still minimized. The
        // float is only the daemon-side occupancy mechanism; the effect-side
        // marker still owns the later unminimize transition. Dropping it here
        // loses that edge and leaves KWin restoring whichever geometry was last
        // applied by the old mode. Genuine visible unfloats still clear both
        // handlers below.
        // liveWindowId / stillMinimized were resolved at the top of the
        // function (the handler maps are keyed by the effect-side ids they
        // claimed with).
        const bool recoveryPending = m_tilingHandler->hasPendingUnminimizeUnfloat(liveWindowId)
            || m_snapHandler->hasPendingUnminimizeUnfloat(liveWindowId)
            || m_tilingHandler->hasUnfloatInFlight(liveWindowId) || m_snapHandler->hasUnfloatInFlight(liveWindowId);
        if (stillMinimized || recoveryPending) {
            qCDebug(lcEffect) << "Preserving minimize-float ownership across non-terminal unfloat:" << liveWindowId;
            // Single-owner repair: preservation must never leave the marker in
            // BOTH engines (a missed cross-mode adoption hop can). Resolve a
            // dual hold to the window's current screen mode — the same rule
            // every adoption site applies.
            if (m_tilingHandler->isMinimizeFloated(liveWindowId) && m_snapHandler->isMinimizeFloated(liveWindowId)) {
                QString ownerScreen = screenId;
                if (ownerScreen.isEmpty() && liveForOwner) {
                    ownerScreen = getWindowScreenId(liveForOwner);
                }
                qCWarning(lcEffect) << "Minimize-float marker held by BOTH engines for" << liveWindowId
                                    << "— resolving to owner of screen" << ownerScreen;
                if (m_tilingHandler->isManagedScreen(ownerScreen)) {
                    m_snapHandler->removeMinimizeFloated(liveWindowId);
                } else {
                    m_tilingHandler->removeMinimizeFloated(liveWindowId);
                }
            }
        } else {
            m_tilingHandler->removeMinimizeFloated(liveWindowId);
            m_snapHandler->removeMinimizeFloated(liveWindowId);
        }
    } else {
        // clearWindowSnapped for this branch ran above, before the shed.

        // Invalidate any stale instant-restore entry for this app. The snap
        // restore cache (SnapHandler) is a single-shot latency cache populated at
        // daemon-ready from the daemon's pending restores. Once a window
        // floats, its saved zone no longer applies: the close capture records
        // a floating window as floating (it should reopen floating). But a
        // stale cache entry would still "Instant snap restore"
        // the reopened window into its old zone WITHOUT a daemon commit
        // (resolveWindowRestore finds nothing), leaving a ghost — visually
        // snapped but untracked. Dropping the entry makes the reopen take the
        // authoritative daemon path so the window stays floating. Keyed by
        // appId to survive the window's identity change across close/reopen.
        m_snapHandler->invalidateRestore(::PhosphorIdentity::WindowId::extractAppId(liveWindowId));
    }
    // The change-gated write above (setWindowFloating, plus the zone-cache
    // clear that now runs inside clearWindowSnapped on the floating branch)
    // re-resolves this window's rules only when a match field actually flips.
    // That is not enough on the cross-monitor drag-out path: the cross-screen
    // handoff pre-sets the floating / zone caches while the window is still
    // moving, so by the time this
    // authoritative windowFloatingChanged arrives both writes are no-ops and the
    // hide-title-bar / border reconcile never runs — the floated window keeps its
    // snap chrome (hidden title bar), ignoring the show-title-bar-when-floating rule.
    // This signal IS the authoritative placement-state change, so reconcile
    // unconditionally; invalidateRuleCacheForStateChange coalesces to a single flush
    // per event-loop turn, so the redundant call when a write did flip the cache is
    // cheap. Mirrors the drag-end paths, which invalidate directly and unconditionally.
    invalidateRuleCacheForStateChange(liveWindowId);
}

void PlasmaZonesEffect::slotWindowStateChanged(const QString& windowId, const PhosphorProtocol::WindowStateEntry& state)
{
    // Validate the daemon payload at the boundary, mirroring the other
    // daemon-data slots (DragPolicy / BridgeRegistrationResult). A garbled entry
    // naming no window must not write a zone keyed by an empty/garbage id.
    if (const QString err = state.validationError(); !err.isEmpty()) {
        qCWarning(lcEffect) << "slotWindowStateChanged: rejecting invalid entry —" << err;
        return;
    }
    // Re-key to the window's LIVE id before writing, not the daemon-supplied one.
    //
    // ZoneCache keys on the instance UUID (extractInstanceId), and so does the IsSnapped /
    // Zone rule-match READ — but through a cross-session restore the daemon can still hold
    // the pre-restore UUID, so a write keyed on it files the entry under an instance id the
    // rule matcher will never read, and a restored snapped window's IsSnapped / Zone(...)
    // rules silently stop matching. Every other commit path already keys by the live id
    // (slotApplyGeometryRequested spells out why). A window that cannot be resolved falls
    // back to the daemon id — best-effort, and correct for the ordinary same-session case
    // where the two ids are identical.
    //
    // A notice that states no zone (unsnapped, screen_changed, a float) resolves by instance
    // alone, and both writes below take that one id (F1111): the app fallback would hand a
    // closed window's notice, or a stale pre-restore id the daemon still holds beside the
    // window's own, to the sole surviving same-app window, which the daemon has not unsnapped.
    // Only a notice that names a zone re-keys through the fallback, where the drift is what
    // the rule fields need covered. The window such a notice reached through the fallback is
    // remembered under the daemon's id, and the notice that clears it under that id goes to
    // the same window, so the write and its clear resolve alike (F1130).
    QString liveWindowId = windowId;
    KWin::EffectWindow* live = nullptr;
    if (state.zoneId.isEmpty()) {
        live = findWindowByInstanceId(windowId);
        const QString target = m_navigationHandler->takeZoneNoticeTarget(windowId);
        if (!live) {
            live = findWindowByInstanceId(target);
        }
    } else {
        live = findWindowById(windowId);
        // A stale id's zone that the fallback hands to a window whose own id
        // filed its zone is not that window's: written, the survivor's Zone
        // field would follow whichever notice came last (F1152).
        if (!m_navigationHandler->noteZoneNoticeTarget(windowId, live ? getWindowId(live) : QString())) {
            qCDebug(lcEffect) << "slotWindowStateChanged: zone notice under" << windowId << "not filed on"
                              << getWindowId(live) << "- its own id holds its zone";
            return;
        }
    }
    if (live) {
        liveWindowId = getWindowId(live);
    }
    // Keep the effect-side zone cache current so the IsSnapped / Zone rule-match
    // fields resolve against the live placement. An empty zoneId (unsnapped /
    // floated / screen-changed) removes the entry. setWindowZone re-resolves this
    // window's rules when the zone actually changes (coalescing with the floating
    // path's invalidation when a float toggle emits both signals — see
    // flushPendingRuleInvalidations), so no separate invalidate call is needed.
    m_navigationHandler->setWindowZone(liveWindowId, state.zoneId);
    // A window KWin moved to another screen, or one whose zone a layout change
    // removed, is no longer snapped: a snapped mark left behind keeps its snap
    // decoration and has a later minimize floated as a snap suspension (F328).
    // Both notices state no zone, so the id is the one the clear resolved above.
    if (state.changeType == QLatin1String("screen_changed") || state.changeType == QLatin1String("unsnapped")) {
        m_snapHandler->clearWindowSnapped(liveWindowId);
    }
}

void PlasmaZonesEffect::slotWindowMinimizedChanged(KWin::EffectWindow* w)
{
    if (!w) {
        return;
    }
    const bool minimized = w->isMinimized();

    // window.minimize shader transition fires for EVERY window the
    // animation filter admits — like the open / close / focus events, it
    // gates only on `shouldAnimateWindow` (enforced inside
    // tryBeginShaderForEvent), NOT on the tiling filters below. A window
    // excluded from tiling (min-size, exclusion rule) still animates its
    // other lifecycle events, so it must animate minimize too. It also
    // fires for BOTH snap and autotile screens — the shader event is
    // screen-mode-independent and the autotile handler's own
    // minimised-change slot does not fire it (which would otherwise be
    // asymmetric per-screen UX for the same user-configured
    // "WindowMinimize" event).
    //
    // Un-minimize plays forward (0→1, "appear"); going-to-minimized plays
    // the same shader in reverse (1→0, "going away"), matching the
    // close / unmaximize reverse-leg convention.
    //
    // The going-to-minimized direction was historically skipped on the
    // claim that KWin pulls the surface (collapses the frame to 0×0)
    // before this signal fires. That was a misdiagnosis: minimizing keeps
    // the frame geometry and the last committed buffer intact and only
    // disables painting via PAINT_DISABLED_BY_MINIMIZE, which
    // beginShaderTransition lifts with an EffectWindowVisibleRef for the
    // transition's lifetime (the mechanism KWin's own Magic Lamp / Squash
    // minimize effects use).
    // animateMinimized opts the going-to-minimized leg past the
    // begin-side minimized-window reject; the un-minimize leg runs on a
    // visible window where the flag is moot.
    //
    // Spurious-pair suppression: plasmashell notification stacking makes
    // KWin emit minimizedChanged(true) on tiled windows with the matching
    // unminimize ~1-2 ms later (the same quirk kMinimizeFloatDebounceMs
    // in tilinghandler/minimizefloat.cpp debounces on the float side). The
    // reverse leg installs immediately — a genuine minimize must not
    // start late, and a spurious pair paints at most one barely-started
    // frame — but an unminimize landing inside the window means the pair
    // was noise: silently drop the reverse leg instead of superseding it
    // with a full un-minimize replay, which with an icon pack made every
    // tiled window pour out of its taskbar icon on every notification.
    if (minimized) {
        // Stamp only a leg that is provably OURS, identified by generation.
        // tryBeginShaderForEvent can install nothing (no pack assigned,
        // null-shader sentinel, animation filter), and stamping blindly
        // would let a spurious pair kill an unrelated leg mid-animation —
        // a future reverse leg, or a superseding one. A fresh install is
        // detected by the generation changing; a repeated minimize event
        // whose prior minimize leg is still live (same-effect
        // short-circuit) refreshes the stamp's timestamp while keeping
        // the same generation.
        const auto* pre = m_shaderManager.findTransition(w);
        const quint64 preGeneration = pre ? pre->generation : 0;
        tryBeginShaderForEvent(w, PhosphorAnimation::ProfilePaths::WindowMinimize, animationDurationMs(),
                               /*reverse=*/true, /*holdCloseGrab=*/false, /*holdAddedGrab=*/false,
                               /*animateMinimized=*/true);
        if (const auto* post = m_shaderManager.findTransition(w); post && post->reverse) {
            const auto stampIt = m_minimizeShaderStamp.constFind(w);
            const bool freshInstall = post->generation != preGeneration;
            const bool ourLiveLeg =
                stampIt != m_minimizeShaderStamp.constEnd() && stampIt->generation == post->generation;
            if (freshInstall || ourLiveLeg) {
                // Stamp with a FRESH clock sample, not one taken at slot
                // entry: a cold-cache install compiles the pack on this
                // thread (tens of ms for a heavy shader), and the paired
                // spurious unminimize measures its gap from ITS slot
                // entry — an entry-time stamp would inflate the measured
                // gap by the compile cost and let the session's first
                // notification-induced pair escape suppression.
                m_minimizeShaderStamp.insert(w, {ShaderInternal::shaderClockNowMs(), post->generation});
            }
        }
    } else {
        const qint64 nowMs = ShaderInternal::shaderClockNowMs();
        const MinimizeShaderStamp stamp = m_minimizeShaderStamp.take(w);
        const bool spuriousPair = stamp.timeMs > 0 && nowMs - stamp.timeMs < kSpuriousMinimizePairMs;
        // Only the exact leg we stamped is ours to drop — the generation
        // check keeps a superseding leg (or anything else live on the
        // window) on its own teardown. Liveness is part of the SUPPRESSION
        // decision, not just the teardown: when our stamped leg is no
        // longer live, the earlier shape skipped BOTH the teardown and the
        // forward leg, so a fast genuine minimize/unminimize cycle got no
        // unminimize animation at all.
        const auto* st = m_shaderManager.findTransition(w);
        const bool stampedLegLive = st && st->reverse && st->generation == stamp.generation;
        if (spuriousPair && stampedLegLive) {
            endShaderTransition(w);
        } else {
            tryBeginShaderForEvent(w, PhosphorAnimation::ProfilePaths::WindowMinimize, animationDurationMs(),
                                   /*reverse=*/false);
        }
    }

    // Snap-mode-only minimize→float bookkeeping is owned by SnapHandler
    // (mirrors TilingHandler running its own minimize→float machine for
    // autotile screens). Unlike the shader event above, this state
    // machine only concerns windows the tiling system manages.
    if (!shouldHandleWindow(w) || !isTileableWindow(w)) {
        return;
    }
    m_snapHandler->handleMinimizeChanged(w, getWindowId(w), getWindowScreenId(w), minimized);
}

void PlasmaZonesEffect::slotRunningWindowsRequested()
{
    qCInfo(lcEffect) << "Running windows requested by KCM";

    QJsonArray windowArray;
    // Which rows are kept, and why a classed and a classless window are keyed
    // differently, is RunningWindowsDecisions::RowFilter's to say.
    RunningWindowsDecisions::RowFilter filter;
    RunningWindowsDecisions::PayloadBudget budget(PhosphorProtocol::Service::MaxRunningWindowsPayloadBytes);
    int overBudget = 0;

    // Iterate in reverse (top-to-bottom) so the filter keeps the topmost
    // window of each group, which is the one the user most likely means.
    const auto windows = KWin::effects->stackingOrder();
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        KWin::EffectWindow* w = *it;
        // !isDeleted: a window mid-close-animation must not be offered in
        // the rule picker (same stacking-walk hygiene as the other walks).
        if (!w || w->isDeleted()) {
            continue;
        }

        // Include all normal, non-special windows (relaxed filter for the picker).
        // isCriticalNotification is a distinct KWin window type from isNotification,
        // so both must be rejected — a window flagged only critical-notification
        // would otherwise show up in the app picker. Bare override-redirect
        // windows (tray proxies, see window_query.h) are client plumbing that
        // no gate ever manages, so a rule authored against one could never act.
        if (w->isSpecialWindow() || w->isDesktop() || w->isDock() || w->isSkipSwitcher() || w->isNotification()
            || w->isCriticalNotification() || w->isOnScreenDisplay() || w->isPopupWindow()
            || windowIsBareOverrideRedirect(w)) {
            continue;
        }

        const QString rawClass = w->windowClass();

        // Hide the daemon's own overlay / editor windows from the rule picker —
        // surfacing `plasmazonesd` or `plasmazones-editor` as an authoring
        // target invites users to write rules against the very surfaces that
        // implement the rule engine. The settings app windowClass falls
        // outside `isOwnOverlayClass` so it stays pickable.
        if (isOwnOverlayClass(rawClass)) {
            continue;
        }

        // Pull the desktop-file basename from the underlying KWin::Window
        // (EffectWindow doesn't expose desktopFileName directly). Empty for a
        // window without a registered desktop file, which the picker's
        // DesktopFile mode then greys out.
        QString desktopFile;
        if (KWin::Window* kw = w->window()) {
            desktopFile = kw->desktopFileName();
        }

        // A client that sets no app_id, and whose executable KWin cannot read
        // for the fallback (a gamescope binary carrying file capabilities, for
        // one), reaches us with a blank class. Placement still manages it, so
        // it stays listed: the picker greys out the modes it has no value for
        // and offers its caption to Title mode.
        const RunningWindowsDecisions::Row row = RunningWindowsDecisions::makeRow(rawClass, w->caption(), desktopFile);
        if (!filter.admits(row)) {
            continue;
        }
        const QJsonObject obj = RunningWindowsDecisions::toJson(row);
        if (!budget.take(QJsonDocument(obj).toJson(QJsonDocument::Compact).size())) {
            ++overBudget;
            continue;
        }
        filter.record(row);
        windowArray.append(obj);
    }

    if (overBudget > 0) {
        qCWarning(lcEffect) << "Running windows: left out" << overBudget
                            << "windows to keep the list inside the daemon's payload cap";
    }
    QString jsonString = QString::fromUtf8(QJsonDocument(windowArray).toJson(QJsonDocument::Compact));
    qCDebug(lcEffect) << "Providing" << windowArray.size() << "running windows to daemon";

    // Send result back to daemon via D-Bus
    if (m_daemonGate.serviceRegistered) {
        PhosphorProtocol::ClientHelpers::fireAndForget(this, PhosphorProtocol::Service::Interface::Settings,
                                                       QStringLiteral("provideRunningWindows"), {jsonString},
                                                       QStringLiteral("provideRunningWindows"));
    } else {
        qCWarning(lcEffect) << "provideRunningWindows: daemon not ready";
    }
}

} // namespace PlasmaZones
