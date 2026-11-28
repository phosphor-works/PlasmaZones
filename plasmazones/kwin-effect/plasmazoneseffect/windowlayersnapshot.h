// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace PlasmaZones {

/// Pre-write keepAbove/keepBelow pair captured the first time a SetWindowLayer
/// rule or the keep-floating-above default is applied to a window. See
/// PlasmaZonesEffect::m_ruleWindowLayerSnapshots. In a header of its own, with
/// no KWin include, so the claim ledger (tilinghandler/claimledger.h) can hold
/// the windowed-fullscreen snapshots without the effect's state header.
struct WindowLayerSnapshot
{
    bool keepAbove = false;
    bool keepBelow = false;
};

} // namespace PlasmaZones
