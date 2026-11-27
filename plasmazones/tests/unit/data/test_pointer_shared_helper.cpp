// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

// The shared pointer helper that PlasmaZones ships in data/pointer/shared,
// checked against the phosphor-pointer constants that depend on it. The
// library builds the entry prologue and mirrors the drawn curve in C++, but
// the GLSL that the compositor runs is in this tree. A change on one side
// that the other side does not follow fails here.

#include <PhosphorPointer/PointerShaderContract.h>
#include <PhosphorPointer/PointerShaderRegistry.h>

#include <QFile>
#include <QRegularExpression>
#include <QtTest/QtTest>

using namespace PhosphorPointerShaders;

class TestPointerSharedHelper : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testEntryPrologueIncludesTheShippedHelper();
    void testCurveConstantsMatchTheShippedShader();
};

void TestPointerSharedHelper::testEntryPrologueIncludesTheShippedHelper()
{
    // Both hosts and the validator assemble a pack the same way. The prologue
    // names an include that must ship, and the generated main calls the entry
    // point the pack defines and writes through the color hook the compositor
    // overrides.
    const QString prologue = PointerShaderRegistry::pointerEntryPrologue();
    static const QRegularExpression includeRe(QStringLiteral("#include <([^>]+)>"));
    const QRegularExpressionMatch include = includeRe.match(prologue);
    QVERIFY2(include.hasMatch(), "the prologue no longer includes the shared helper");

    const QString sharedDir = QStringLiteral(P_DATA_DIR "/pointer/shared");
    const QString included = sharedDir + QLatin1Char('/') + include.captured(1);
    QVERIFY2(QFile::exists(included),
             qPrintable(QStringLiteral("prologue includes %1 which does not ship").arg(included)));

    // That helper holds the contract of the entry point. The pack defines
    // pPointer, not the helper: a helper that defined it would clash with
    // every pack.
    QFile lib(included);
    QVERIFY(lib.open(QIODevice::ReadOnly));
    const QString libSource = QString::fromUtf8(lib.readAll());
    QVERIFY(!libSource.contains(QStringLiteral("vec4 pPointer(vec2")));
    // The helper also includes the uniform contract that declares the
    // finalize hook of the generated main.
    QVERIFY(libSource.contains(QStringLiteral("#include <pointer_uniforms.glsl>")));
    QFile uniforms(sharedDir + QStringLiteral("/pointer_uniforms.glsl"));
    QVERIFY(uniforms.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(uniforms.readAll()).contains(QStringLiteral("PZ_FINALIZE_COLOR")));
}

void TestPointerSharedHelper::testCurveConstantsMatchTheShippedShader()
{
    // phosphor-pointer's test_pointercurve mirrors the smoothing kernel and
    // the Catmull-Rom curve of pointer_lib.glsl in C++, with the tension from
    // PointerShaderContract. Nothing compiles that mirror
    // against the GLSL the compositor runs. This case pins the facts the
    // mirror depends on to the shipped file, so a retune of the curve in the
    // shader cannot leave the mirror measuring the old one.
    QFile file(QStringLiteral(P_DATA_DIR "/pointer/shared/pointer_lib.glsl"));
    QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(file.fileName()));
    const QString source = QString::fromUtf8(file.readAll());

    const QRegularExpression tension(QStringLiteral(R"(const\s+float\s+kPointerCurveTension\s*=\s*([0-9.]+)\s*;)"));
    const QRegularExpressionMatch tensionMatch = tension.match(source);
    QVERIFY2(tensionMatch.hasMatch(), "kPointerCurveTension not found in pointer_lib.glsl");
    QCOMPARE(tensionMatch.captured(1).toDouble(), PointerShaderContract::kPointerCurveTension);

    const QRegularExpression steps(QStringLiteral(R"(const\s+int\s+kPointerCurveSteps\s*=\s*([0-9]+)\s*;)"));
    const QRegularExpressionMatch stepsMatch = steps.match(source);
    QVERIFY2(stepsMatch.hasMatch(), "kPointerCurveSteps not found in pointer_lib.glsl");
    QCOMPARE(stepsMatch.captured(1).toInt(), PointerShaderContract::kPointerCurveSteps);

    // The one textual fact that separates inverse-time weighting from equal or
    // direct-time weighting. Nothing else catches a swap of these two.
    QVERIFY2(source.contains(QLatin1String("prev.xy * dtNext")),
             "the smoothing kernel no longer weights the previous neighbor by the FORWARD gap");
    QVERIFY2(source.contains(QLatin1String("next.xy * dtPrev")),
             "the smoothing kernel no longer weights the next neighbor by the BACKWARD gap");
    // Both gap floors that the arithmetic above depends on. They are matched
    // as the two specific expressions, not counted across the file, so an
    // unrelated epsilon elsewhere in the helper cannot fail this case with a
    // message that blames the smoothing kernel.
    QVERIFY2(source.contains(QLatin1String("max(here.z - prev.z, 0.0) + 1e-4")), "the backward gap lost its floor");
    QVERIFY2(source.contains(QLatin1String("max(next.z - here.z, 0.0) + 1e-4")), "the forward gap lost its floor");
}

QTEST_MAIN(TestPointerSharedHelper)
#include "test_pointer_shared_helper.moc"
