// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QSize>
#include <QtGlobal>

#include <cmath>

namespace PhosphorEngineRuntime {

/// Bounds on a logical scale read off the bus. The daemon answers with the
/// compositor's fractional scale, and anything outside this range is not a
/// real output and would overflow the device-pixel arithmetic downstream.
inline constexpr qreal MinPlausibleScreenScale = 0.25;
inline constexpr qreal MaxPlausibleScreenScale = 8.0;

/// Whether @p pixelSize (device pixels) is the mode of an output whose
/// logical size is @p logical at @p scale. The one test for it: the daemon
/// accepts a compositor pixel-size report by it (ScreenReshapeTracker), and
/// the layout editor trusts the daemon's pixel size over its scale by it.
///
/// Each side may sit away from the logical side times the scale by the sum
/// of two allowances:
/// - the scale plus one pixel, because the compositor derives the logical
///   size by dividing the mode by its scale and rounding, which moves the
///   product by up to half the scale;
/// - one pixel per 240 logical pixels of the side, rounded up, because a
///   client never sees the compositor's exact scale. wp_fractional_scale_v1
///   sends it as a numerator over 120 (the protocol's preferred_scale event),
///   and Qt reads the per-surface ratio as that fraction, so the scale here
///   can sit up to 1/240 from the one the compositor divided by, and that
///   error grows with the side it multiplies.
///
/// A different mode, a rotation the logical geometry has not caught up with,
/// a coarse integer scale standing in for a fractional one, and another
/// monitor's size all land outside. False for an empty size and a
/// non-positive scale.
///
/// A monitor whose mode lands within the tolerance of another's still passes
/// for it. When one replaces another on the same connector and the QScreen
/// change arrives first, the outgoing monitor's report, still pending, is
/// checked against the incoming monitor's geometry and accepted if the two
/// are that close (a 1366x768 panel for a 1360x768 one at scale 1), and
/// stands until the incoming monitor's own report replaces it. The size, the
/// scale and the geometry are all a client sees of either monitor, so
/// nothing here can tell the two apart.
inline bool pixelSizeMatchesLogicalSize(const QSize& pixelSize, const QSize& logical, qreal scale)
{
    if (pixelSize.isEmpty() || logical.isEmpty() || !(scale > 0.0)) {
        return false;
    }
    const auto sideAgrees = [scale](int pixels, int logicalSide) {
        const qreal tolerance = scale + 1.0 + std::ceil(logicalSide / 240.0);
        return qAbs(pixels - logicalSide * scale) <= tolerance;
    };
    return sideAgrees(pixelSize.width(), logical.width()) && sideAgrees(pixelSize.height(), logical.height());
}

} // namespace PhosphorEngineRuntime
