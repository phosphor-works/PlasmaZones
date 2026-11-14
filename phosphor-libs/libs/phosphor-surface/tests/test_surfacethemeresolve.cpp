// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorSurface/SurfaceShaderEffect.h>
#include <PhosphorSurface/SurfaceThemeResolve.h>
#include <PhosphorSurface/SurfaceWindowStateResolve.h>

#include <QtTest/QtTest>

using namespace PhosphorSurfaceShaders;

namespace {
SurfaceShaderEffect::ParameterInfo parameter(const QString& id, const QString& type, const QVariant& value)
{
    SurfaceShaderEffect::ParameterInfo result;
    result.id = id;
    result.type = type;
    result.defaultValue = value;
    return result;
}

SurfaceShaderEffect identityPack()
{
    SurfaceShaderEffect effect;
    effect.parameters = {
        parameter(QStringLiteral("useWindowAccent"), QStringLiteral("bool"), true),
        parameter(QStringLiteral("activeColor"), QStringLiteral("color"), QStringLiteral("#b3223344")),
        parameter(QStringLiteral("inactiveColor"), QStringLiteral("color"), QStringLiteral("#66223344")),
        parameter(QStringLiteral("glowColor"), QStringLiteral("color"), QStringLiteral("#80223344")),
    };
    return effect;
}

SurfaceThemeColors theme()
{
    return {QColor(QStringLiteral("#ffbb3300")), QColor(QStringLiteral("#ff557799")),
            QColor(QStringLiteral("#ff111111")), QColor(QStringLiteral("#ffffffff")),
            QColor(QStringLiteral("#ff12ab56"))};
}

QColor colorOf(const QVariantMap& values, const QString& key)
{
    return values.value(key).value<QColor>();
}
}

class TestSurfaceThemeResolve : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void windowIdentityKeepsEachDefaultAlpha()
    {
        QVariantMap values;
        resolveThemeParamColors(identityPack(), values, theme());
        QCOMPARE(colorOf(values, QStringLiteral("activeColor")), QColor(QStringLiteral("#b312ab56")));
        QCOMPARE(colorOf(values, QStringLiteral("inactiveColor")), QColor(QStringLiteral("#6612ab56")));
        QCOMPARE(colorOf(values, QStringLiteral("glowColor")), QColor(QStringLiteral("#8012ab56")));
    }

    void windowIdentityKeepsExplicitAlphaIncludingTransparent()
    {
        QVariantMap values{{QStringLiteral("activeColor"), QColor(QStringLiteral("#00112233"))},
                           {QStringLiteral("inactiveColor"), QStringLiteral("#29112233")},
                           {QStringLiteral("glowColor"), QColor(QStringLiteral("#e8112233"))}};
        resolveThemeParamColors(identityPack(), values, theme());
        QCOMPARE(colorOf(values, QStringLiteral("activeColor")), QColor(QStringLiteral("#0012ab56")));
        QCOMPARE(colorOf(values, QStringLiteral("inactiveColor")), QColor(QStringLiteral("#2912ab56")));
        QCOMPARE(colorOf(values, QStringLiteral("glowColor")), QColor(QStringLiteral("#e812ab56")));
    }

    /// Only the identity colour's RGB is used. A translucent identity must not
    /// leak its own alpha into the pack's intensity, multiplied or otherwise.
    void windowIdentityAlphaNeverReplacesThePacksAlpha()
    {
        auto translucent = theme();
        translucent.windowAccent = QColor(QStringLiteral("#4012ab56"));
        QVariantMap values;
        resolveThemeParamColors(identityPack(), values, translucent);
        QCOMPARE(colorOf(values, QStringLiteral("activeColor")), QColor(QStringLiteral("#b312ab56")));
        QCOMPARE(colorOf(values, QStringLiteral("inactiveColor")), QColor(QStringLiteral("#6612ab56")));
        QCOMPARE(colorOf(values, QStringLiteral("glowColor")), QColor(QStringLiteral("#8012ab56")));
    }

    /// Under identity, focus contrast is the declared alpha pair and nothing
    /// else: both colours take the same RGB, so an opaque pair draws focused
    /// and unfocused alike. Pinned so the documented contract and the code
    /// cannot drift apart.
    void windowIdentityFocusContrastIsTheAlphaPairOnly()
    {
        QVariantMap values{{QStringLiteral("activeColor"), QStringLiteral("#ff112233")},
                           {QStringLiteral("inactiveColor"), QStringLiteral("#ff445566")}};
        resolveThemeParamColors(identityPack(), values, theme());
        QCOMPARE(colorOf(values, QStringLiteral("activeColor")), colorOf(values, QStringLiteral("inactiveColor")));
    }

    /// A border colour that does not parse falls back opaque, like every other
    /// border source; a halo colour falls back to half intensity.
    void unparseableColoursFallBackToTheirFamilysAlpha()
    {
        QVariantMap values{{QStringLiteral("activeColor"), QStringLiteral("not a colour")},
                           {QStringLiteral("glowColor"), QStringLiteral("not a colour")}};
        resolveThemeParamColors(identityPack(), values, theme());
        QCOMPARE(colorOf(values, QStringLiteral("activeColor")).alpha(), 255);
        QCOMPARE(colorOf(values, QStringLiteral("glowColor")).alpha(), 128);
    }

    void userCanDisableIdentityWithoutLosingExplicitColors()
    {
        QVariantMap values{{QStringLiteral("useWindowAccent"), false},
                           {QStringLiteral("activeColor"), QStringLiteral("#b3112233")},
                           {QStringLiteral("inactiveColor"), QStringLiteral("#66112233")},
                           {QStringLiteral("glowColor"), QStringLiteral("#80112233")}};
        const auto before = values;
        resolveThemeParamColors(identityPack(), values, theme());
        QCOMPARE(values, before);
    }

    void identityRequiresDeclaredFlagAndAvailableWindow()
    {
        auto effect = identityPack();
        effect.parameters.removeFirst();
        QVariantMap values{{QStringLiteral("useWindowAccent"), true},
                           {QStringLiteral("activeColor"), QStringLiteral("#b3112233")}};
        const auto before = values;
        resolveThemeParamColors(effect, values, theme());
        QCOMPARE(values, before);

        auto noWindow = theme();
        noWindow.windowAccent = QColor();
        QVERIFY(!noWindow.windowAccent.isValid());
        resolveThemeParamColors(identityPack(), values, noWindow);
        QCOMPARE(values, before);
    }

    void identityDoesNotInjectUndeclaredColors()
    {
        auto effect = identityPack();
        effect.parameters = {effect.parameters.first()};
        QVariantMap values;
        resolveThemeParamColors(effect, values, theme());
        QVERIFY(values.isEmpty());
    }

    void identityPrecedesOtherColorSources()
    {
        auto effect = identityPack();
        for (const auto& flag :
             {QStringLiteral("useThemeNeutral"), QStringLiteral("useSystemAccent"), QStringLiteral("useThemeTint")}) {
            effect.parameters.append(parameter(flag, QStringLiteral("bool"), true));
        }
        effect.parameters.append(
            parameter(QStringLiteral("shadowColor"), QStringLiteral("color"), QStringLiteral("#40223344")));
        QVariantMap values;
        resolveThemeParamColors(effect, values, theme());
        QCOMPARE(colorOf(values, QStringLiteral("activeColor")), QColor(QStringLiteral("#b312ab56")));
        QCOMPARE(colorOf(values, QStringLiteral("glowColor")), QColor(QStringLiteral("#8012ab56")));
        // The shadow is never identity-coloured: under useThemeTint it takes
        // the theme background, alpha kept.
        QCOMPARE(colorOf(values, QStringLiteral("shadowColor")), QColor(QStringLiteral("#40111111")));

        // Switching identity off restores the existing neutral and theme-tint
        // modes, including the halo alpha rather than an opaque replacement.
        values = {{QStringLiteral("useWindowAccent"), false}, {QStringLiteral("frameContrast"), 0.0}};
        effect.parameters.append(parameter(QStringLiteral("frameContrast"), QStringLiteral("float"), 0.2));
        resolveThemeParamColors(effect, values, theme());
        QCOMPARE(colorOf(values, QStringLiteral("activeColor")), theme().background);
        QCOMPARE(colorOf(values, QStringLiteral("inactiveColor")), theme().background);
        QCOMPARE(colorOf(values, QStringLiteral("glowColor")), QColor(QStringLiteral("#80111111")));
    }

    void missingWindowUsesSystemAccentWhenRequested()
    {
        auto effect = identityPack();
        effect.parameters.append(parameter(QStringLiteral("useSystemAccent"), QStringLiteral("bool"), true));
        auto noWindow = theme();
        noWindow.windowAccent = QColor();
        QVERIFY(!noWindow.windowAccent.isValid());
        QVariantMap values;
        resolveThemeParamColors(effect, values, noWindow);
        QCOMPARE(colorOf(values, QStringLiteral("activeColor")), noWindow.accent);
        QCOMPARE(colorOf(values, QStringLiteral("inactiveColor")), noWindow.inactive);
        QVERIFY(!values.contains(QStringLiteral("glowColor")));
    }

    void maximizedCornersFollowTheChainAnswer()
    {
        SurfaceShaderEffect effect;
        effect.parameters = {
            parameter(QStringLiteral("cornerRadius"), QStringLiteral("int"), 18),
        };
        const QVariantMap authored{{QStringLiteral("cornerRadius"), 27}};

        // The chain did not ask for square corners: the authored radius stays.
        QVariantMap resolved = authored;
        QVERIFY(resolveWindowStateParams(effect, resolved, true, false));
        QCOMPARE(resolved, authored);

        // The chain did: every pack declaring cornerRadius is squared, whether
        // or not it declares a square flag of its own.
        resolved = authored;
        QVERIFY(resolveWindowStateParams(effect, resolved, true, true));
        QCOMPARE(resolved.value(QStringLiteral("cornerRadius")).toInt(), 0);

        // Not maximized: nothing changes even when the chain would square.
        resolved = authored;
        QVERIFY(resolveWindowStateParams(effect, resolved, false, true));
        QCOMPARE(resolved, authored);
    }

    /// cornerRadius is written only for a pack that declares it. A parameter
    /// map alone cannot impose a radius on a pack that never asked for one.
    void maximizedCornersAreNotInjectedIntoAPackWithoutARadius()
    {
        SurfaceShaderEffect effect;
        effect.parameters = {parameter(QStringLiteral("glowSize"), QStringLiteral("int"), 12)};
        QVariantMap values;
        QVERIFY(resolveWindowStateParams(effect, values, true, true));
        QVERIFY(!values.contains(QStringLiteral("cornerRadius")));
    }

    void maximizedHaloHidingHonorsFlagAndUserOverride()
    {
        SurfaceShaderEffect effect;
        effect.parameters = {
            parameter(QStringLiteral("hideWhenMaximized"), QStringLiteral("bool"), true),
            parameter(QStringLiteral("shadowSize"), QStringLiteral("int"), 36),
            parameter(QStringLiteral("cornerRadius"), QStringLiteral("int"), 12),
        };
        QVariantMap values{{QStringLiteral("shadowSize"), 48}};
        const QVariantMap authored = values;
        // A dropped pack's map is left alone, even when the chain would
        // otherwise square its declared radius.
        QVERIFY(!resolveWindowStateParams(effect, values, true, true));
        QCOMPARE(values, authored);
        QVERIFY(resolveWindowStateParams(effect, values, false, true));
        QCOMPARE(values, authored);

        values.insert(QStringLiteral("hideWhenMaximized"), false);
        QVERIFY(resolveWindowStateParams(effect, values, true, false));
        QCOMPARE(values.value(QStringLiteral("shadowSize")).toInt(), 48);

        // A value for a flag the pack does not declare is not the pack's
        // opinion and cannot drop it.
        values.insert(QStringLiteral("hideWhenMaximized"), true);
        effect.parameters.removeFirst();
        QVERIFY(resolveWindowStateParams(effect, values, true, false));
    }
};

QTEST_GUILESS_MAIN(TestSurfaceThemeResolve)
#include "test_surfacethemeresolve.moc"
