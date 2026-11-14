// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorSurface/SurfaceWindowStateResolve.h>

#include <PhosphorSurface/SurfaceShaderEffect.h>

#include <QLatin1String>
#include <QString>
#include <QVariant>

namespace PhosphorSurfaceShaders {

bool resolveWindowStateParams(const SurfaceShaderEffect& effect, QVariantMap& friendlyParams, bool maximized,
                              bool squareCorners)
{
    if (!maximized) {
        return true;
    }
    // A declared flag only: a value in the map for a flag the pack never
    // declared is not the pack's opinion and must not drop it from the chain.
    const QLatin1String hideKey("hideWhenMaximized");
    if (const SurfaceShaderEffect::ParameterInfo* hide = effect.findParameter(hideKey)) {
        if (friendlyParams.value(QString(hideKey), hide->defaultValue).toBool()) {
            return false;
        }
    }
    const QLatin1String radiusKey("cornerRadius");
    if (squareCorners && effect.findParameter(radiusKey)) {
        friendlyParams.insert(QString(radiusKey), 0);
    }
    return true;
}

} // namespace PhosphorSurfaceShaders
