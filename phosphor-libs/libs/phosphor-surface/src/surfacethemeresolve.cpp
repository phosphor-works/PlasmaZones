// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorSurface/SurfaceThemeResolve.h>

#include <PhosphorSurface/SurfaceShaderEffect.h>

#include <QLatin1String>
#include <QVariant>

#include <optional>

namespace PhosphorSurfaceShaders {

namespace {

/// Linear interpolation between two colours in [0,1] RGB, opaque result. Mirrors
/// Kirigami.ColorUtils.linearInterpolation (PopupFrame's built-in border used it).
QColor lerpColor(const QColor& a, const QColor& b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, 1.0);
}

} // namespace

void resolveThemeParamColors(const SurfaceShaderEffect& effect, QVariantMap& friendlyParams,
                             const SurfaceThemeColors& theme)
{
    // Effective value of a bool param: the caller's override, else the pack's
    // declared default; nullopt when the pack has no such param.
    const auto effectiveFlag = [&](QLatin1String id) -> std::optional<bool> {
        const SurfaceShaderEffect::ParameterInfo* param = effect.findParameter(id);
        if (!param) {
            return std::nullopt;
        }
        return friendlyParams.value(QString(id), param->defaultValue).toBool();
    };
    // Effective value of a real-valued param: the caller's override, else the
    // pack's declared default, else @p fallback.
    const auto effectiveReal = [&](QLatin1String id, double fallback) -> double {
        const SurfaceShaderEffect::ParameterInfo* param = effect.findParameter(id);
        if (!param) {
            return fallback;
        }
        return friendlyParams.value(QString(id), param->defaultValue).toDouble();
    };
    // Replace only a declared colour's RGB. Its alpha remains the pack/user's
    // intensity, including zero, and an omitted override uses the pack default.
    // @p fallbackAlpha is for a value that does not parse as a colour at all:
    // a border colour is drawn opaque by every other source, a halo colour's
    // alpha is its intensity knob, so each family falls back to its own norm.
    const auto tintDeclaredColor = [&](QLatin1String id, const QColor& tint, qreal fallbackAlpha) {
        const SurfaceShaderEffect::ParameterInfo* param = effect.findParameter(id);
        if (!param) {
            return;
        }
        const QString key(id);
        const QVariant current = friendlyParams.value(key, param->defaultValue);
        QColor color = current.value<QColor>();
        if (!color.isValid()) {
            color = QColor(current.toString());
        }
        QColor resolved = tint;
        resolved.setAlphaF(color.isValid() ? color.alphaF() : fallbackAlpha);
        friendlyParams.insert(key, resolved);
    };
    const bool useWindowAccent =
        theme.windowAccent.isValid() && effectiveFlag(QLatin1String("useWindowAccent")).value_or(false);

    // Border colours. Window identity takes precedence when requested and
    // available; otherwise a neutral frame-contrast line wins over the system
    // accent.
    //
    // Identity gives activeColor and inactiveColor the SAME identity RGB and
    // keeps each declared colour's alpha, so under identity the focus contrast
    // is carried entirely by the alpha pair the pack (or user) declares: a pack
    // that offers useWindowAccent has to declare a translucent inactive colour
    // for an unfocused window to read differently. The neutral and system
    // modes write fresh opaque colours instead and are distinguished by hue.
    //
    // The neutral and system modes insert both keys without asking whether the
    // pack declares them (the identity mode reads the declared alpha, so it has
    // to ask): translateSurfaceParams only emits a lane for a declared
    // parameter, so a key written for a pack that lacks it is dropped there.
    if (useWindowAccent) {
        tintDeclaredColor(QLatin1String("activeColor"), theme.windowAccent, 1.0);
        tintDeclaredColor(QLatin1String("inactiveColor"), theme.windowAccent, 1.0);
    } else if (effectiveFlag(QLatin1String("useThemeNeutral")).value_or(false)) {
        const QColor neutral = lerpColor(theme.background, theme.foreground,
                                         qBound(0.0, effectiveReal(QLatin1String("frameContrast"), 0.2), 1.0));
        friendlyParams.insert(QStringLiteral("activeColor"), neutral);
        friendlyParams.insert(QStringLiteral("inactiveColor"), neutral);
    } else if (effectiveFlag(QLatin1String("useSystemAccent")).value_or(false)) {
        QColor accent = theme.accent;
        accent.setAlphaF(1.0);
        QColor inactive = theme.inactive;
        inactive.setAlphaF(1.0);
        friendlyParams.insert(QStringLiteral("activeColor"), accent);
        friendlyParams.insert(QStringLiteral("inactiveColor"), inactive);
    }

    // Halo (glow / shadow) colours, keeping the pack's own colour alpha (its
    // intensity knob) in every mode.
    //
    // useThemeTint colours both halos with the theme background so they track
    // light / dark instead of a fixed colour. The background is low-chroma, so
    // this reads as a soft theme-matched shadow rather than an additive colour
    // smear. When the window identity is in effect the GLOW takes the identity
    // colour instead, with or without useThemeTint; the SHADOW never takes it.
    const bool useThemeTint = effectiveFlag(QLatin1String("useThemeTint")).value_or(false);
    if (useThemeTint) {
        tintDeclaredColor(QLatin1String("shadowColor"), theme.background, 0.5);
    }
    if (useWindowAccent) {
        tintDeclaredColor(QLatin1String("glowColor"), theme.windowAccent, 0.5);
    } else if (useThemeTint) {
        tintDeclaredColor(QLatin1String("glowColor"), theme.background, 0.5);
    }
}

} // namespace PhosphorSurfaceShaders
