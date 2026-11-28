// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The pure half of TilingHandler::retireStripScopedState: which strip-scoped
// relocations a strip change retires, and whether the drop then needs a full
// repaint. A template over the relocation's value type, because that type
// (ScrollVisualPlacement) lives in a header that pulls in KWin's GL headers,
// and this one must stay free of KWin so the unit tests can link it
// (phosphor-works/workspace#26).

#include <PhosphorCompositor/TilingState.h>

#include <QHash>
#include <QString>

namespace PlasmaZones::StripRetire {

/// Erase from @p visualDelta every relocation of a window on @p screenId's
/// strip, and answer whether any went.
///
/// The recorded screen answers first, and tiled membership answers ONLY for a
/// window it has nothing to say about. m_notifiedWindowScreens is written under
/// a narrower condition than the relocation itself: the apply loop inserts a
/// visual delta for any entry carrying a visual position, but records the
/// window's screen only when the window is in m_notifiedWindows, so a window
/// demoted or rolled back between the two ends up holding a relocation with no
/// recorded screen at all. That window is the whole reason for the second term,
/// and selecting on the recorded map alone left it wearing the OUTGOING strip's
/// position for good.
///
/// The fallback is deliberately NOT ORed in for windows that DO have a recorded
/// screen, which is the tempting spelling and is wrong in a way that does not
/// heal. screenForTiledWindow returns the first matching bucket in hash order,
/// and those buckets go stale — outputchange.cpp says so at its own
/// recorded-screen fallback, which exists for exactly that case. A window that
/// has moved to another screen but still sits in this one's bucket would lose
/// its relocation here, and nothing would put it back: the only writer of that
/// map is the tile-batch apply, the other screen's strip did not change so no
/// batch is coming, and the damage the caller issues is scoped to THIS output,
/// so the window would not even be repainted where it actually is.
///
/// Also not scrollTrackedScreenFor, which looks like the right helper and is
/// not: it is impure (it reports clip loss as a side effect) and both of its
/// answer arms are gated on the screen being in m_scrollingScreens, so for a
/// screen that has already left the scrolling set it answers empty for every
/// window and this loop would drop nothing at all.
template<typename Placement>
bool retireRelocations(QHash<QString, Placement>& visualDelta, const QHash<QString, QString>& notifiedScreens,
                       const PhosphorCompositor::BorderState& border, const QString& screenId)
{
    bool droppedAny = false;
    for (auto it = visualDelta.begin(); it != visualDelta.end();) {
        const QString& windowId = it.key();
        const auto recordedIt = notifiedScreens.constFind(windowId);
        const bool onThisScreen = recordedIt != notifiedScreens.constEnd()
            ? *recordedIt == screenId
            : PhosphorCompositor::TilingStateHelpers::screenForTiledWindow(border, windowId) == screenId;
        if (onThisScreen) {
            it = visualDelta.erase(it);
            droppedAny = true;
        } else {
            ++it;
        }
    }
    return droppedAny;
}

/// Whether the retire owes a full repaint. The strip's view spring is the
/// other half of a parked column's drawn position, and dropping it repaints
/// its output. When no output resolves for the id (an id that never named a
/// connected output, a drifted spelling of one, or a just-removed output's id
/// announced before its own prune), the relocations just dropped were still
/// paint inputs, so something has to repaint, and full is the honest fallback.
constexpr bool needsFullRepaint(bool stripMotionDropped, bool relocationDropped)
{
    return !stripMotionDropped && relocationDropped;
}

} // namespace PlasmaZones::StripRetire
