// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "claimledger.h"

namespace PlasmaZones {

bool ClaimLedger::unmaximizeMonocle(const QString& windowId)
{
    if (!monocleMaximized.contains(windowId)) {
        return false;
    }
    // EXACT resolve: a stale monocle entry whose window is gone must restore
    // nothing — the fuzzy appId fallback would un-maximize an unrelated
    // same-app sibling under suppression, invisibly.
    ClaimWindow* w = host.findExact(windowId);
    if (!w || !host.hasClient(w)) {
        monocleMaximized.remove(windowId);
        monocleRestoreOwed.remove(windowId);
        return false;
    }
    // KWin's X11 maximize(Restore) has no fullscreen conditional: called on a
    // still-fullscreen window it moveResizes to geometryRestore, shrinking a
    // fullscreen game or video out of its presentation. Wayland ignores
    // maximize() while fullscreen is requested. Skip on the REQUESTED bit alone, not on
    // the union with the committed one.
    //
    // The union looked safer and was not. This project is Wayland-only, where
    // the committed bit trails a client round-trip, so after our own
    // setFullScreen(false) the requested bit reads false while the committed
    // one is still true. A union skips the whole exit gap, which is exactly
    // the window in which the maximize restore is owed and correct. The enter
    // gap is still covered: there the requested bit is true first, so this
    // guard fires before the surface ever commits, and a genuinely presenting
    // client has both bits set and skips too. Requested-false means fullscreen
    // is going away no matter who asked, and that is when we want the bit back.
    //
    // MEMBERSHIP IS RETAINED ON THIS ARM, deliberately, and it is why the
    // remove above became a contains. Shedding it here while the window is
    // still KWin-MaximizeFull would strand it with nothing left owning the
    // flag — byte for byte the defect that removing the fullscreen-enter drop
    // was meant to end. And nothing would take it back: the sole insert site
    // (the monocle tile batch) is gated on the window NOT already being
    // maximized, so "the next batch re-establishes membership" is false.
    // Holding the entry means the next call on a non-fullscreen window does
    // the real restore.
    if (host.isRequestedFullScreen(w)) {
        return false;
    }
    // MID-GESTURE IS A RETAIN TOO, and it is the retain every sibling maximize
    // write in this file takes: maximize() moveResizes, so restoring here
    // while the user is dragging snaps the window to its restore rect under
    // the pointer with nothing committed behind it until the gesture ends.
    // The batch's monocle Release branch is the reachable caller — a tile
    // batch arriving mid-drag is exactly what the Apply arm's own gesture
    // skip exists for, and this was the one arm of the pair without it.
    //
    // Retaining membership alone is NOT enough, and is why monocleRestoreOwed
    // exists. reconcileMaximizeAfterGesture pays skipped claims at the gesture
    // end, but its only monocle action is to re-apply MaximizeFull: a member
    // it finds there is assumed to still be owed its maximize. Retain without
    // recording the direction and it re-maximizes the window the batch was
    // demoting — or, when the gesture is a drag-to-float, permanently
    // maximizes the window the user just floated, since a floater gets no
    // batch to undo it. Shedding membership instead strands the bit: the sole
    // insert site is gated on the window NOT already being maximized, so no
    // later batch re-establishes an entry for a window still holding
    // MaximizeFull. So: keep the entry, and record that what it now owes is a
    // restore. The gesture end reads that first and pays it.
    if (host.inUserGesture(w)) {
        monocleRestoreOwed.insert(windowId);
        return false;
    }
    monocleRestoreOwed.remove(windowId);
    monocleMaximized.remove(windowId);
    // The write below is a no-op on a window KWin already reports restored
    // (maximize() emits nothing), so no windowMaximizedStateAboutToChange
    // refreshes the captured departure rect and the caller must not anchor a
    // maximize leg on it. Same test releaseMaximizedToEdges makes.
    const bool wroteRestore = !host.requestedMaximizeRestored(w);
    // The host writes under the geometry-apply gate and re-seeds the tracked
    // screen (KWinClaimHost::writeMaximizeRestore says why both are needed),
    // and the write itself is a no-op on a window already restored.
    host.writeMaximizeRestore(w);
    return wroteRestore;
}

bool ClaimLedger::releaseMaximizedToEdges(const QString& windowId, ClaimWindow* w)
{
    if (!maximizedToEdges.contains(windowId)) {
        return false;
    }
    // isDeleted before the client, because callers pass a pointer straight
    // out of signal scope rather than one they re-resolved: a closing window
    // answers a stale client that must not be moveResized.
    if (!w || host.isDeleted(w) || !host.hasClient(w)) {
        // Nothing left to hand the bit back to, so the entry is dead weight.
        maximizedToEdges.remove(windowId);
        return false;
    }
    // Fullscreen guard, for the reason unmaximizeMonocle spells out: on X11 a
    // MaximizeRestore on a fullscreen window still moveResizes it to
    // geometryRestore and shrinks a presenting surface (Wayland ignores it
    // while fullscreen is requested). Reachable here because windowed
    // fullscreen and column maximize are deliberately NOT exclusive.
    //
    // MEMBERSHIP IS RETAINED ON THIS ARM, the monocle ledger's choice rather
    // than its opposite. Shedding it here while the window is still
    // KWin-MaximizeFull strands the bit with nothing owning it, and the
    // "next batch re-asserts through the Apply arm" reasoning does not hold
    // for this function: Apply requires flagOnWire, and BOTH callers reach
    // this only when the engine has already dropped the maximize (the batch
    // Release arm by definition, cleanupAutotileTracking on untrack), so
    // Apply can never re-fire on a path that gets here. Retaining instead
    // leaves resolveMaximizeToEdgesAction answering Release on each following
    // batch until the client leaves fullscreen, which is the self-heal the
    // Apply arm's own fullscreen skip relies on in the other direction.
    //
    // That self-heal needs a following batch, so it does NOT cover the callers
    // that end strip membership outright (the untrack funnel, the float
    // funnels): for those the entry is held until the fullscreen-exit repair
    // in slotWindowFullScreenChanged, or until a teardown restore. Retaining
    // is still the better half of that trade — the old shed left the same bit
    // stranded with nothing even recording that we owed it.
    // Mid-gesture is a retain too, on the same terms as the fullscreen skip
    // and for the reason the batch Apply arm spells out: maximize() moveResizes
    // to the restore rect, and the geometry apply that would override it defers
    // during a drag, so releasing here snaps the window out from under the
    // user's pointer.
    //
    // WHEN the retained entry is paid depends on where the drag ends. A window
    // still on its strip is paid by the next batch that carries it, and the
    // engine emits on change, so a drag that leaves the strip alone schedules
    // none — the same bounded staleness the Apply arm accepts in the other
    // direction. A window the drag FLOATED off the strip gets no batch at all,
    // and reconcileMaximizeAfterGesture pays that case at the gesture end: it
    // re-tests strip membership and routes a claim with no strip left back
    // here, with the gesture flags now clear.
    if (host.isRequestedFullScreen(w) || host.inUserGesture(w)) {
        return false;
    }
    maximizedToEdges.remove(windowId);
    if (host.requestedMaximizeRestored(w)) {
        return false;
    }
    // maximize() emits windowFrameGeometryChanged SYNCHRONOUSLY and moves to
    // the restore rect, which can sit in a different virtual-screen region —
    // the same edge unmaximizeMonocle guards. Load-bearing here and not
    // merely symmetric: the cleanupAutotileTracking caller runs on the
    // cross-output transfer path holding no guard of its own, which is
    // precisely the case the VS-crossing detector reacts to. The host's write
    // saves and restores the gate, so it nests inside an already-guarded
    // caller such as the batch, and re-seeds the tracked screen after it.
    host.writeMaximizeRestore(w);
    return true;
}

void ClaimLedger::forgetWindowedFullscreen(const QString& windowId)
{
    // Membership half only, deliberately without the compositor call: the
    // desktop-switch demote must shed membership BEFORE its geometry restore
    // (or the apply-path exemption still fires) while the setFullScreen(false)
    // has to land AFTER m_managedScreens is rewritten (or the deferred
    // committed-exit signal evaluates against the stale set) — one atomic
    // helper cannot sit on both sides of that write.
    windowedFullscreen.remove(windowId);
}

std::optional<WindowLayerSnapshot> ClaimLedger::takeLayerSnapshot(const QString& windowId)
{
    const auto it = layerSnapshots.find(windowId);
    if (it == layerSnapshots.end()) {
        return std::nullopt;
    }
    // Erase BEFORE the setters, the reconcileRuleWindowLayer shape: they
    // emit KWin signals, and holding a QHash iterator across re-entrant
    // code is undefined the day a connection routes back into this map.
    const WindowLayerSnapshot snapshot = *it;
    layerSnapshots.erase(it);
    return snapshot;
}

void ClaimLedger::restoreLayerDemotion(const QString& windowId, ClaimWindow* w)
{
    const std::optional<WindowLayerSnapshot> snapshot = takeLayerSnapshot(windowId);
    if (!snapshot || !w) {
        return; // no snapshot, or the window already gone: dropping it IS the cleanup
    }
    host.setKeepFlags(w, snapshot->keepAbove, snapshot->keepBelow);
}

void ClaimLedger::releaseWindowedFullscreenState(const QString& windowId)
{
    // Compositor half: drop the client's fullscreen state if it still holds
    // it. Deliberately does NOT consult the membership hash — callers that
    // forget first (the demote) and callers that snapshot-and-clear (the bulk
    // restore) both arrive here with the entry already gone.
    ClaimWindow* w = host.findExact(windowId);
    if (!w || host.isDeleted(w) || !host.hasClient(w)) {
        restoreLayerDemotion(windowId, nullptr);
        return;
    }
    // Keep-flag restore runs even when fullscreen is already gone (the
    // client may have exited on its own before this release landed).
    restoreLayerDemotion(windowId, w);
    // Requested-state term: during our OWN enter round-trip the committed
    // state lags behind while requested is already true (the same lag
    // applyWindowGeometry's bail and the self-heal arm handle explicitly). A
    // release landing inside that gap must still un-set, or the client
    // commits fullscreen with membership already gone — the apply bail then
    // rejects every geometry and the ack arm can't fire, stranding the
    // window fullscreen at full-output size with no owner.
    if (!host.isFullScreen(w) && !host.isRequestedFullScreen(w)) {
        return;
    }
    // The host's write holds its own geometry-apply bracket, since
    // setFullScreen(false) synchronously emits windowFrameGeometryChanged on
    // X11 and none of the release call sites holds the guard, and goes through
    // the RAII suppression helper (KWinClaimHost::writeFullScreenOff).
    host.writeFullScreenOff(w);
}

ClaimReleaseResult ClaimLedger::releaseAllClaims(const QString& windowId, ClaimWindow* w,
                                                 ScrollDecisions::ClaimScope scope)
{
    using ScrollDecisions::Claim;
    using ScrollDecisions::claimReleasesOn;

    ClaimReleaseResult result;

    // Order is claimReleaseOrder's, spelled out rather than sorted: three
    // claims do not justify a sort, and writing them in order keeps the reason
    // readable. Windowed fullscreen FIRST — both maximize releases skip a
    // window that still holds fullscreen, and on X11 setFullScreen has already
    // landed by the time they run, so this order is what lets a window holding
    // both get a real restore instead of a skip.
    static_assert(ScrollDecisions::claimReleaseOrder(Claim::WindowedFullscreen)
                          < ScrollDecisions::claimReleaseOrder(Claim::MonocleMaximize)
                      && ScrollDecisions::claimReleaseOrder(Claim::WindowedFullscreen)
                          < ScrollDecisions::claimReleaseOrder(Claim::MaximizedToEdges),
                  "windowed fullscreen must be released before either maximize claim");

    if (claimReleasesOn(Claim::WindowedFullscreen, scope)) {
        // Membership OR a surviving layer snapshot — the guard
        // applyPassiveFloatShed argued for, and the one place this funnel
        // unifies rather than reproduces: a lone snapshot means membership was
        // dropped by a path that never called the release, so the release is
        // still owed. releaseWindowedFullscreenState is idempotent and
        // deliberately does not consult membership, so re-driving it off the
        // snapshot is safe everywhere, not only there.
        const bool hadMembership = windowedFullscreen.contains(windowId);
        // OUTSIDE the guard below, which is where both migrated sites had it:
        // a marker outliving its hold can only refuse a future adopt, and the
        // case that needs it most is exactly the one the guard rejects. On the
        // close half of the untrack funnel slotWindowClosed has already removed
        // membership and the release has already erased the snapshot, so the
        // guard answers no while an armed marker is still sitting there — and
        // ids are unique per window, so the armed marker would only leak, but
        // it is dropped here to keep the set bounded.
        windowedFsClearInFlight.remove(windowId);
        if (hadMembership || layerSnapshots.contains(windowId)) {
            if (hadMembership) {
                forgetWindowedFullscreen(windowId);
            }
            releaseWindowedFullscreenState(windowId);
            result.windowedFullscreen = true;
        }
    }
    if (claimReleasesOn(Claim::MonocleMaximize, scope)) {
        // What was HANDED BACK, not what was held: both maximize releases
        // RETAIN membership when they skip a still-fullscreen window, so a
        // pre-read contains() reports true for a claim the effect is still
        // holding.
        //
        // Precisely: these two fields report MEMBERSHIP DROPPED, which is not
        // the same as "the bit was written back". The unresolvable-window arms
        // in both releases shed membership without writing anything, so the
        // delta reads true there too. That is the right answer for a caller
        // asking "does the effect still owe this window a restore", and the
        // wrong one for a caller gating a repaint or a decoration re-resolve on
        // an actual geometry change — that caller wants the releases' own bool
        // returns. Today's only consumer (applyPassiveFloatShed) reads
        // windowedFullscreen alone, so nothing depends on the distinction yet.
        const bool hadMonocle = monocleMaximized.contains(windowId);
        unmaximizeMonocle(windowId);
        result.monocle = hadMonocle && !monocleMaximized.contains(windowId);
    }
    if (claimReleasesOn(Claim::MaximizedToEdges, scope)) {
        const bool hadMaximizedToEdges = maximizedToEdges.contains(windowId);
        releaseMaximizedToEdges(windowId, w);
        result.maximizedToEdges = hadMaximizedToEdges && !maximizedToEdges.contains(windowId);
    }
    return result;
}

bool ClaimLedger::releaseMonocleBeforeScrub(const QString& windowId, ScrollDecisions::ClaimScope scope)
{
    // A scope that releases the monocle maximize does so first: the scrub
    // drops the membership the release reads (UntrackFunnel, LiveDeparture,
    // F675). A release that has to wait (fullscreen, a gesture) keeps its
    // membership and owed restore past the scrub, for the fullscreen exit or
    // the gesture end to pay (F1078).
    if (!ScrollDecisions::claimReleasesOn(ScrollDecisions::Claim::MonocleMaximize, scope)) {
        return false;
    }
    unmaximizeMonocle(windowId);
    return monocleMaximized.contains(windowId);
}

void ClaimLedger::settleMonocleAfterScrub(const QString& windowId, bool retained)
{
    if (retained) {
        monocleMaximized.insert(windowId);
    } else {
        monocleRestoreOwed.remove(windowId);
    }
}

void ClaimLedger::scrubForEvacuation(const QString& windowId, ClaimWindow* w)
{
    // Windowed fullscreen: the membership goes, and with it PlasmaZones'
    // keep-below demotion. KWin re-fullscreens an evacuee on the output it
    // lands on, and a keep-below fullscreen window would sit under every
    // window there. The layer flags are not part of the state KWin compares
    // when it returns a window, so restoring them does not count as a touch
    // (F710).
    windowedFsClearInFlight.remove(windowId);
    forgetWindowedFullscreen(windowId);
    restoreLayerDemotion(windowId, (w && !host.isDeleted(w) && host.hasClient(w)) ? w : nullptr);
    // Maximize-to-edges: the entry goes and the maximize stays. Monocle needs
    // nothing here: the funnel's ledger scrub already dropped it bare.
    maximizedToEdges.remove(windowId);
}

} // namespace PlasmaZones
