// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/engineruntimenamespace.h"
#include "runtime/host/ihostapps.h"

namespace PlasmaZones {

/**
 * @brief IHostApps for plasmazonesd: plasmazones-settings and plasmazones-editor
 *
 * Each starts with the environment the daemon inherited rather than the one it
 * runs with (childProcessEnvironment): the GPU-preference variables the daemon
 * exported come out, since an inherited DRI_PRIME or QT_VK_PHYSICAL_DEVICE_INDEX
 * would trip the child's pre-set-value guards and freeze it on this process's
 * stale pin, and so do the variables the daemon set for itself as a background
 * service, while what it cleared is put back. Only what the daemon changed is
 * touched, so the user's own session exports still reach the child.
 */
class PlasmaZonesApps : public IHostApps
{
public:
    /// In its own systemd scope (systemd-run --user --scope) so stopping the
    /// daemon's service does not take the settings app with it; started
    /// directly when systemd-run is unavailable.
    bool openSettings() override;
    /// plasmazones-editor from PATH, else beside the daemon's binary.
    bool openEditor(const EditorRequest& request) override;
};

} // namespace PlasmaZones
