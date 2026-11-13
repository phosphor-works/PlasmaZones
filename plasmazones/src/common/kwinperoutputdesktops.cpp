// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/kwinperoutputdesktops.h"

#ifdef USE_KDE_FRAMEWORKS
#include <KConfig>
#include <KConfigGroup>
#endif

namespace PlasmaZones {

bool kwinPerOutputVirtualDesktopsEnabled()
{
#ifdef USE_KDE_FRAMEWORKS
    // NoGlobals: kdeglobals carries nothing for this group, and KWin's own
    // read cascades the same XDG_CONFIG_DIRS files this one does.
    const KConfig kwinrc(kwinConfigFileName(), KConfig::NoGlobals);
    return KConfigGroup(&kwinrc, kwinWindowsGroup()).readEntry(kwinPerOutputVirtualDesktopsKey(), false);
#else
    return false;
#endif
}

bool enableKWinPerOutputVirtualDesktops()
{
#ifdef USE_KDE_FRAMEWORKS
    // KConfig, never QSettings: QSettings::sync() rewrites the WHOLE file in
    // its own ini dialect, mangling KConfig-only constructs elsewhere in
    // kwinrc (nested [group][subgroup] headers, [$e]/[$i] markers, localized
    // keys). KConfig writes just this key.
    KConfig kwinrc(kwinConfigFileName(), KConfig::NoGlobals);
    KConfigGroup windows(&kwinrc, kwinWindowsGroup());
    if (windows.isEntryImmutable(kwinPerOutputVirtualDesktopsKey())) {
        return false;
    }
    windows.writeEntry(kwinPerOutputVirtualDesktopsKey(), true);
    return kwinrc.sync();
#else
    return false;
#endif
}

} // namespace PlasmaZones
