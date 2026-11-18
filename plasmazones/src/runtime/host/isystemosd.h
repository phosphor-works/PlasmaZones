// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

class QString;

namespace PhosphorEngineRuntime {

/**
 * @brief The desktop's own text OSD: a short notice with an icon, drawn by
 *        whatever shows the desktop's other OSDs
 *
 * plasmazonesd shows it through plasmashell (PlasmaSystemOsd). A host with no
 * such service draws it itself.
 */
class ISystemOsd
{
public:
    virtual ~ISystemOsd() = default;

    /// Show @p text with the icon named @p iconName.
    virtual void showText(const QString& iconName, const QString& text) = 0;
};

} // namespace PhosphorEngineRuntime
