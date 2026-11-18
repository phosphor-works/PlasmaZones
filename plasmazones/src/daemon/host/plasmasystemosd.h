// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/engineruntimenamespace.h"

#include "runtime/host/isystemosd.h"

namespace PlasmaZones {

/// ISystemOsd for Plasma: plasmashell's own text OSD (org.kde.osdService).
class PlasmaSystemOsd : public ISystemOsd
{
public:
    void showText(const QString& iconName, const QString& text) override;
};

} // namespace PlasmaZones
