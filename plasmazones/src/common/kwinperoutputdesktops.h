// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plasmazones_export.h"

#include <QString>

namespace PlasmaZones {

/// KWin's config file, group and key for "switch virtual desktops
/// independently for each screen". These name KWin's settings, not ours, so
/// they live here beside the one reader and the one writer rather than with
/// the ConfigDefaults accessors for config.json.
inline QString kwinConfigFileName()
{
    return QStringLiteral("kwinrc");
}
inline QString kwinWindowsGroup()
{
    return QStringLiteral("Windows");
}
inline QString kwinPerOutputVirtualDesktopsKey()
{
    return QStringLiteral("PerOutputVirtualDesktops");
}

/**
 * Reads KWin's per-output virtual desktops flag from kwinrc.
 *
 * This lives here because BOTH processes ask the question and they must
 * agree: the daemon gates the whole dynamic-workspaces feature on it, and
 * the settings app decides whether to show the consent prompt.
 *
 * Read through KConfig, the way KWin reads it: the system kwinrc files in
 * XDG_CONFIG_DIRS cascade under the user's, an immutable ([$i]) entry is
 * honoured, and every boolean spelling KConfig writes is understood.
 *
 * A build without KDE Frameworks has no dynamic workspaces at all (the
 * feature is compiled out there), so it answers false without reading.
 * A missing file or key also answers false, which is the safe direction:
 * the feature stays dormant and the consent prompt appears, rather than the
 * daemon assuming a mode KWin is not in.
 */
PLASMAZONES_EXPORT bool kwinPerOutputVirtualDesktopsEnabled();

/**
 * Turn PerOutputVirtualDesktops on in the user's kwinrc. The daemon calls it
 * only with the user's recorded consent, and never in the other direction.
 *
 * Returns false when nothing was written: an immutable entry (a system
 * administrator pinned the value), a config KConfig cannot write, or a build
 * without KDE Frameworks. Asking KWin to reconfigure afterwards is the
 * caller's job.
 */
PLASMAZONES_EXPORT bool enableKWinPerOutputVirtualDesktops();

} // namespace PlasmaZones
