// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "claimhost_kwin.h"

#include "tilinghandler.h"

#include <effect/effectwindow.h>
#include <window.h>

namespace PlasmaZones {

ClaimWindow* KWinClaimHost::findExact(const QString& windowId)
{
    return toClaim(m_handler->claimFindExact(windowId));
}

bool KWinClaimHost::isDeleted(ClaimWindow* w)
{
    return toEffect(w)->isDeleted();
}

bool KWinClaimHost::hasClient(ClaimWindow* w)
{
    return toEffect(w)->window() != nullptr;
}

bool KWinClaimHost::isRequestedFullScreen(ClaimWindow* w)
{
    const KWin::Window* kw = toEffect(w)->window();
    return kw && kw->isRequestedFullScreen();
}

bool KWinClaimHost::isFullScreen(ClaimWindow* w)
{
    const KWin::Window* kw = toEffect(w)->window();
    return kw && kw->isFullScreen();
}

bool KWinClaimHost::inUserGesture(ClaimWindow* w)
{
    const KWin::EffectWindow* ew = toEffect(w);
    return ew->isUserMove() || ew->isUserResize();
}

bool KWinClaimHost::requestedMaximizeRestored(ClaimWindow* w)
{
    const KWin::Window* kw = toEffect(w)->window();
    return !kw || kw->requestedMaximizeMode() == KWin::MaximizeRestore;
}

void KWinClaimHost::writeMaximizeRestore(ClaimWindow* w)
{
    m_handler->claimWriteMaximizeRestore(toEffect(w));
}

void KWinClaimHost::writeFullScreenOff(ClaimWindow* w)
{
    m_handler->claimWriteFullScreenOff(toEffect(w));
}

void KWinClaimHost::setKeepFlags(ClaimWindow* w, bool keepAbove, bool keepBelow)
{
    if (KWin::Window* kw = toEffect(w)->window()) {
        kw->setKeepAbove(keepAbove);
        kw->setKeepBelow(keepBelow);
    }
}

} // namespace PlasmaZones
