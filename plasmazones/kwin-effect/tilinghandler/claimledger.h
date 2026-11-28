// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The compositor-state claims the tiling handler holds on a window (the
// windowed-fullscreen hold, the monocle maximize, the maximize to edges) and
// the algorithms that release them, with no KWin type in sight. The ledgers
// stay where they live (TilingHandler and the effect); ClaimLedger is a view
// of references to them, built per call. Every compositor read and write goes
// through ClaimHost, which the plugin implements over KWin (claimhost_kwin.cpp)
// and a unit test implements with a fake (phosphor-works/workspace#26).

#include "plasmazoneseffect/windowlayersnapshot.h"
#include "tilinghandler/scrolldecisions.h"

#include <QHash>
#include <QRect>
#include <QSet>
#include <QString>

#include <optional>

namespace PlasmaZones {

/// An opaque window handle. Never defined: the host turns it into its own
/// window type (KWin::EffectWindow in the plugin, a fake in a test).
struct ClaimWindow;

/// Everything the claim releases ask of the compositor. Each call is a live
/// query or a write at the point of use, so the order of reads and writes is
/// the one the releases had when they called KWin directly.
class ClaimHost
{
public:
    virtual ~ClaimHost() = default;
    /// The window with exactly this id, or null (findWindowByIdExact).
    virtual ClaimWindow* findExact(const QString& windowId) = 0;
    /// The window is a closing corpse (EffectWindow::isDeleted).
    virtual bool isDeleted(ClaimWindow* w) = 0;
    /// The window still has a client (EffectWindow::window() is not null).
    virtual bool hasClient(ClaimWindow* w) = 0;
    virtual bool isRequestedFullScreen(ClaimWindow* w) = 0;
    virtual bool isFullScreen(ClaimWindow* w) = 0;
    /// A user move or resize is running (isUserMove || isUserResize).
    virtual bool inUserGesture(ClaimWindow* w) = 0;
    /// KWin's requested maximize mode is MaximizeRestore.
    virtual bool requestedMaximizeRestored(ClaimWindow* w) = 0;
    /// Write MaximizeRestore under the geometry-apply gate and the maximize
    /// suppression counter, then re-seed the window's tracked screen.
    virtual void writeMaximizeRestore(ClaimWindow* w) = 0;
    /// Write setFullScreen(false) under the geometry-apply gate and the
    /// fullscreen suppression counter.
    virtual void writeFullScreenOff(ClaimWindow* w) = 0;
    virtual void setKeepFlags(ClaimWindow* w, bool keepAbove, bool keepBelow) = 0;
};

/// What a releaseAllClaims call actually handed back. Callers gate
/// follow-up work on it — the passive float shed re-resolves decorations
/// only when the windowed-fullscreen claim released, because that is the
/// one whose exit changes shouldDecorateWindow's answer.
struct ClaimReleaseResult
{
    bool windowedFullscreen = false;
    bool monocle = false;
    bool maximizedToEdges = false;
};

/// A view of the claim ledgers, by reference, and the release algorithms
/// over them. Built per call: the containers belong to TilingHandler and the
/// effect, and a re-entrant release (X11 emits from inside setFullScreen)
/// reaches the very same containers through a second view.
struct ClaimLedger
{
    QSet<QString>& maximizedToEdges; ///< TilingHandler::m_maximizedToEdgesWindows
    QSet<QString>& monocleMaximized; ///< TilingHandler::m_monocleMaximizedWindows
    QSet<QString>& monocleRestoreOwed; ///< TilingHandler::m_monocleRestoreOwed
    QSet<QString>& windowedFsClearInFlight; ///< TilingHandler::m_windowedFsClearInFlight
    QHash<QString, QRect>& windowedFullscreen; ///< PlasmaZonesEffect::m_windowedFullscreenWindows
    QHash<QString, WindowLayerSnapshot>& layerSnapshots; ///< PlasmaZonesEffect::m_windowedFsLayerSnapshots
    ClaimHost& host;

    /// TilingHandler::unmaximizeMonocleWindow.
    bool unmaximizeMonocle(const QString& windowId);
    /// TilingHandler::releaseMaximizedToEdges.
    bool releaseMaximizedToEdges(const QString& windowId, ClaimWindow* w);
    /// TilingHandler::forgetWindowedFullscreen.
    void forgetWindowedFullscreen(const QString& windowId);
    /// Take the windowed-fullscreen layer snapshot out of the ledger, or
    /// nothing when there is none. Erased BEFORE any setter runs: the setters
    /// emit KWin signals, and holding a QHash iterator across re-entrant code
    /// is undefined the day a connection routes back into this map.
    std::optional<WindowLayerSnapshot> takeLayerSnapshot(const QString& windowId);
    /// TilingHandler::restoreWindowedFullscreenLayerDemotion: the snapshot's
    /// keep flags go back on @p w. A null @p w means the window is gone, and
    /// dropping the snapshot is then the whole cleanup.
    void restoreLayerDemotion(const QString& windowId, ClaimWindow* w);
    /// TilingHandler::releaseWindowedFullscreenState.
    void releaseWindowedFullscreenState(const QString& windowId);
    /// TilingHandler::releaseAllClaims.
    ClaimReleaseResult releaseAllClaims(const QString& windowId, ClaimWindow* w, ScrollDecisions::ClaimScope scope);
    /// The untrack funnel's monocle half, before its ledger scrub: release the
    /// monocle claim when @p scope releases it, and answer whether membership
    /// is still held (a retain arm), which the scrub would otherwise drop.
    bool releaseMonocleBeforeScrub(const QString& windowId, ScrollDecisions::ClaimScope scope);
    /// The untrack funnel's monocle half, after its ledger scrub: a retained
    /// claim goes back into the ledger, and the restore owed goes with an
    /// entry that was not retained.
    void settleMonocleAfterScrub(const QString& windowId, bool retained);
    /// The evacuation scrub: every claim leaves the ledgers without a write to
    /// the compositor except the layer flags, since an evacuee keeps KWin's
    /// state for the output's return.
    void scrubForEvacuation(const QString& windowId, ClaimWindow* w);
};

} // namespace PlasmaZones
