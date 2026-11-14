// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorSurface/phosphorsurface_export.h>

#include <QVariantMap>

namespace PhosphorSurfaceShaders {

struct SurfaceShaderEffect;

/**
 * @brief Resolve the opt-in window-state parameters for one pack of a chain,
 *        on a copy of its authored parameters.
 *
 * @p maximized is the caller's answer for "the window fills its output": a
 * host passes true for a fullscreen window as well as a maximized one. When it
 * is false the map is left untouched and the pack stays in the chain.
 *
 * - `hideWhenMaximized`, declared and enabled on THIS pack, drops the pack from
 *   the rendered chain: the function returns false and leaves the map alone.
 *   Dropping a pack changes what the rest of the chain is composed from, so
 *   the caller recomputes the chain's outer padding (paddingRequest) and its
 *   silhouette votes (chainRoundBottomCorners, chainSquareWhenMaximized)
 *   without the packs it dropped.
 * - @p squareCorners is the CHAIN's answer from chainSquareWhenMaximized, not
 *   this pack's flag: a pane's silhouette belongs to the chain (see
 *   chainRoundBottomCorners), so when it is true every pack that declares
 *   `cornerRadius` gets 0, a halo that declares no square flag of its own
 *   included, and a border and the glow around it cannot disagree about the
 *   corners. Call this AFTER the host's own injection of `cornerRadius`, if it
 *   has one, or the injection undoes the squaring.
 *
 * Without those flags a user-authored profile keeps its configured shape and
 * layers. Returns true when the pack stays in the chain.
 *
 * Host-optional: only a host that knows whether its window fills its output
 * calls it. The PlasmaZones KWin effect's window decorations do, together with
 * chainSquareWhenMaximized; the overlay and shell hosts decorate surfaces with
 * no maximized state and do not. No bundled pack declares either flag.
 */
PHOSPHORSURFACE_EXPORT bool resolveWindowStateParams(const SurfaceShaderEffect& effect, QVariantMap& friendlyParams,
                                                     bool maximized, bool squareCorners);

} // namespace PhosphorSurfaceShaders
