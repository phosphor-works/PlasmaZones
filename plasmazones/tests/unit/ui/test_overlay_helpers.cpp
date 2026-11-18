// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QTest>
#include <QSignalSpy>
#include <QGuiApplication>
#include <QQuickWindow>

// Shared overlay helpers — single source of truth for both production and tests.
// Previously these were duplicated here; now they live in overlay_helpers.h.
#include "daemon/overlayservice/overlay_helpers.h"
#include "helpers/TestHelpers.h"

using namespace PlasmaZones;

/**
 * @brief Unit tests for overlay service helper functions
 *
 * Tests cover:
 * - patchZonesWithHighlight: single/multi highlight, empty ids, null window
 * - writeQmlProperty: valid property, null object safety
 * - ensureShaderTimerStarted: idempotency
 *
 * These are the real inline definitions from overlay_helpers.h, included above.
 * Nothing is replicated here and nothing has to be kept in sync — an earlier
 * version of this docblock said otherwise, contradicting the include note at the
 * top of the file.
 */
class TestOverlayHelpers : public QObject
{
    Q_OBJECT

private:
    // Delegate to shared TestHelpers::makeZone (uses JsonKeys constants)
    static QVariantMap makeZone(const QString& id, float x, float y, float w, float h, int zoneNumber = 0,
                                bool highlighted = false)
    {
        return TestHelpers::makeZone(id, x, y, w, h, zoneNumber, highlighted);
    }

private Q_SLOTS:

    // ═══════════════════════════════════════════════════════════════════════════
    // P0: patchZonesWithHighlight
    // ═══════════════════════════════════════════════════════════════════════════

    void testPatchZonesWithHighlight_singleZoneHighlighted()
    {
        QQuickWindow window;
        window.setProperty("highlightedZoneId", QStringLiteral("z2"));
        window.setProperty("highlightedZoneIds", QVariantList());

        QVariantList zones;
        zones.append(makeZone(QStringLiteral("z1"), 0, 0, 960, 1080));
        zones.append(makeZone(QStringLiteral("z2"), 960, 0, 960, 1080));
        zones.append(makeZone(QStringLiteral("z3"), 0, 540, 960, 540));

        QVariantList result = patchZonesWithHighlight(zones, &window);

        QCOMPARE(result.size(), 3);
        QVERIFY(!result[0].toMap().value(QLatin1String("isHighlighted")).toBool());
        QVERIFY(result[1].toMap().value(QLatin1String("isHighlighted")).toBool());
        QVERIFY(!result[2].toMap().value(QLatin1String("isHighlighted")).toBool());
    }

    void testPatchZonesWithHighlight_multipleZoneIds()
    {
        QQuickWindow window;
        window.setProperty("highlightedZoneId", QString()); // empty single ID
        QVariantList highlightIds;
        highlightIds.append(QStringLiteral("z1"));
        highlightIds.append(QStringLiteral("z3"));
        window.setProperty("highlightedZoneIds", highlightIds);

        QVariantList zones;
        zones.append(makeZone(QStringLiteral("z1"), 0, 0, 960, 540));
        zones.append(makeZone(QStringLiteral("z2"), 960, 0, 960, 540));
        zones.append(makeZone(QStringLiteral("z3"), 0, 540, 960, 540));
        zones.append(makeZone(QStringLiteral("z4"), 960, 540, 960, 540));

        QVariantList result = patchZonesWithHighlight(zones, &window);

        QCOMPARE(result.size(), 4);
        QVERIFY(result[0].toMap().value(QLatin1String("isHighlighted")).toBool());
        QVERIFY(!result[1].toMap().value(QLatin1String("isHighlighted")).toBool());
        QVERIFY(result[2].toMap().value(QLatin1String("isHighlighted")).toBool());
        QVERIFY(!result[3].toMap().value(QLatin1String("isHighlighted")).toBool());
    }

    void testPatchZonesWithHighlight_noHighlight()
    {
        QQuickWindow window;
        window.setProperty("highlightedZoneId", QString());
        window.setProperty("highlightedZoneIds", QVariantList());

        QVariantList zones;
        zones.append(makeZone(QStringLiteral("z1"), 0, 0, 960, 540, 1, true)); // was highlighted in input
        zones.append(makeZone(QStringLiteral("z2"), 960, 0, 960, 540, 2, false));

        QVariantList result = patchZonesWithHighlight(zones, &window);

        QCOMPARE(result.size(), 2);
        // patchZonesWithHighlight overrides isHighlighted based on window properties,
        // so even though input had isHighlighted=true, output should be false
        QVERIFY(!result[0].toMap().value(QLatin1String("isHighlighted")).toBool());
        QVERIFY(!result[1].toMap().value(QLatin1String("isHighlighted")).toBool());
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // P1: patchZonesWithHighlight edge cases
    // ═══════════════════════════════════════════════════════════════════════════

    void testPatchZonesWithHighlight_emptyZoneId_notHighlighted()
    {
        QQuickWindow window;
        window.setProperty("highlightedZoneId", QStringLiteral(""));
        window.setProperty("highlightedZoneIds", QVariantList());

        QVariantList zones;
        // PhosphorZones::Zone with empty id should never match empty highlightedZoneId
        QVariantMap z = makeZone(QString(), 0, 0, 960, 1080);
        zones.append(z);

        QVariantList result = patchZonesWithHighlight(zones, &window);

        QCOMPARE(result.size(), 1);
        // The code checks (!id.isEmpty() && id == hid), so empty ids never match
        QVERIFY(!result[0].toMap().value(QLatin1String("isHighlighted")).toBool());
    }

    void testPatchZonesWithHighlight_nullWindow_passthrough()
    {
        QVariantList zones;
        zones.append(makeZone(QStringLiteral("z1"), 0, 0, 960, 1080, 1, true));

        // Null window returns input unchanged
        QVariantList result = patchZonesWithHighlight(zones, nullptr);

        QCOMPARE(result.size(), 1);
        // isHighlighted preserved from input when window is null
        QVERIFY(result[0].toMap().value(QLatin1String("isHighlighted")).toBool());
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // P1: writeQmlProperty
    // ═══════════════════════════════════════════════════════════════════════════

    void testWriteQmlProperty_validProperty()
    {
        // Use a plain QObject with a dynamic property
        QObject obj;
        obj.setProperty("testProp", 42);

        writeQmlProperty(&obj, QStringLiteral("testProp"), 99);

        // writeQmlProperty falls back to setProperty for non-QML objects
        QCOMPARE(obj.property("testProp").toInt(), 99);
    }

    void testWriteQmlProperty_nullObject_nocrash()
    {
        // Must not crash when object is null
        writeQmlProperty(nullptr, QStringLiteral("anyProp"), QVariant(42));
        // If we get here, no crash occurred - test passes
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // P1: ensureShaderTimerStarted
    // ═══════════════════════════════════════════════════════════════════════════

    void testEnsureShaderTimerStarted_onlyStartsOnce()
    {
        QElapsedTimer timer;
        QMutex mutex;
        std::atomic<qint64> lastFrame{999};
        std::atomic<int> frameCount{42};

        // Timer is not valid initially
        QVERIFY(!timer.isValid());

        // First call should start the timer and reset counters
        ensureShaderTimerStarted(timer, mutex, lastFrame, frameCount);
        QVERIFY(timer.isValid());
        QCOMPARE(lastFrame.load(), qint64(0));
        QCOMPARE(frameCount.load(), 0);

        // Second call should be a no-op (timer already valid)
        // Set sentinel values to detect if they get reset
        lastFrame.store(100);
        frameCount.store(50);

        ensureShaderTimerStarted(timer, mutex, lastFrame, frameCount);

        // Values should NOT have been reset since timer was already valid
        QCOMPARE(lastFrame.load(), qint64(100));
        QCOMPARE(frameCount.load(), 50);
    }
};

// Custom main, and VESTIGIAL: QTEST_MAIN would do the same thing here. This target
// links Qt6::Quick, so QT_GUI_LIB is defined and QTEST_MAIN_SETUP already expands to
// QGuiApplication, which is the only application type the QQuickWindow below needs.
// QTEST_MAIN would additionally set AA_Use96Dpi, and nothing here depends on the DPI:
// these tests cover QVariantMap key logic, a QObject dynamic property and a
// QElapsedTimer, and the window is never shown, never sized and never measured.
// Two earlier versions of this comment each gave a different wrong reason for keeping
// the custom main. It is kept only because replacing it buys nothing.
int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    TestOverlayHelpers tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_overlay_helpers.moc"
