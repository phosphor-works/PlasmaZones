// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The plugin's ClaimHost: the claim ledger's compositor calls, over KWin.

#include "claimledger.h"

namespace KWin {
class EffectWindow;
}

namespace PlasmaZones {

class TilingHandler;

class KWinClaimHost final : public ClaimHost
{
public:
    explicit KWinClaimHost(TilingHandler* handler)
        : m_handler(handler)
    {
    }

    /// The handle the ledger passes around is the effect window itself.
    static ClaimWindow* toClaim(KWin::EffectWindow* w)
    {
        return reinterpret_cast<ClaimWindow*>(w);
    }
    static KWin::EffectWindow* toEffect(ClaimWindow* w)
    {
        return reinterpret_cast<KWin::EffectWindow*>(w);
    }

    ClaimWindow* findExact(const QString& windowId) override;
    bool isDeleted(ClaimWindow* w) override;
    bool hasClient(ClaimWindow* w) override;
    bool isRequestedFullScreen(ClaimWindow* w) override;
    bool isFullScreen(ClaimWindow* w) override;
    bool inUserGesture(ClaimWindow* w) override;
    bool requestedMaximizeRestored(ClaimWindow* w) override;
    void writeMaximizeRestore(ClaimWindow* w) override;
    void writeFullScreenOff(ClaimWindow* w) override;
    void setKeepFlags(ClaimWindow* w, bool keepAbove, bool keepBelow) override;

private:
    TilingHandler* m_handler;
};

} // namespace PlasmaZones
