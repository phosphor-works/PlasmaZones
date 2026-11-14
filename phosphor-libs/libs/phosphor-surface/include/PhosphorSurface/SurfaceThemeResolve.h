// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorSurface/phosphorsurface_export.h>

#include <QColor>
#include <QVariantMap>

namespace PhosphorSurfaceShaders {

struct SurfaceShaderEffect;

/**
 * @brief Theme colours a host supplies to resolveThemeParamColors().
 *
 * Every host that composes a decoration chain (the daemon's overlay
 * decorations, the KWin window decorations, the shell's chrome and the
 * settings preview) resolves these from its own colour source and passes them
 * in, so the flag→colour synthesis stays identical across them. The KWin host,
 * for one, takes accent and inactive from the daemon's border colours and
 * background and foreground from its own palette.
 */
struct SurfaceThemeColors
{
    QColor accent; ///< System accent / highlight (useSystemAccent active border).
    QColor inactive; ///< Unfocused accent (useSystemAccent inactive border).
    QColor background; ///< Theme window background (useThemeNeutral lerp base, useThemeTint halo).
    QColor foreground; ///< Theme window foreground (useThemeNeutral lerp target).
    /// Optional per-window identity colour (useWindowAccent); invalid when the
    /// host has no identity for the surface. Only its RGB is used. The default
    /// member initializer keeps the hosts' four-colour aggregate initialisers
    /// free of -Wmissing-field-initializers.
    QColor windowAccent = {};
};

/**
 * @brief Synthesise theme-derived colours into a pack's friendly param map,
 *        in place, honouring the host-consumed flags a surface pack may declare.
 *
 * - Border pack: `useWindowAccent`, when the host supplies a valid identity
 *   colour, gives active and inactive that RGB and keeps each declared
 *   colour's alpha, so focus contrast under identity comes ENTIRELY from the
 *   declared alpha pair (an opaque pair draws focused and unfocused alike).
 *   Otherwise `useThemeNeutral` (with `frameContrast`) fills both with a
 *   neutral frame-contrast colour (background lerped toward foreground), and
 *   failing that `useSystemAccent` fills them with the system accent /
 *   inactive. Both of those write opaque colours.
 * - Glow / shadow pack: `useThemeTint` replaces the halo colour with the theme
 *   background, preserving the pack's own colour alpha (its intensity knob).
 *   `useWindowAccent` with an identity colour gives the GLOW that colour
 *   instead, whether or not `useThemeTint` is on, again keeping its alpha. The
 *   shadow is never identity-coloured.
 *
 * Only a flag the pack declares (via @p effect.parameters) is read, so a pack
 * without a given flag is left alone and a flag at its default (or off) leaves
 * the caller's explicit colours in place. The identity and tint modes write
 * only colours the pack declares; the neutral and system modes write both
 * border keys regardless, and slot translation drops a key the pack does not
 * declare. Call this on the friendly param map BEFORE translating it to
 * slot-keyed uniforms (the flags are host-consumed and never reach the shader).
 */
PHOSPHORSURFACE_EXPORT void resolveThemeParamColors(const SurfaceShaderEffect& effect, QVariantMap& friendlyParams,
                                                    const SurfaceThemeColors& theme);

} // namespace PhosphorSurfaceShaders
