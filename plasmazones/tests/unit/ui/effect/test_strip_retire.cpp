// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

// The selection half of TilingHandler::retireStripScopedState
// (kwin-effect/tilinghandler/stripretire.h, phosphor-works/workspace#26): the
// recorded screen answers first, tiled membership only for a window with no
// recorded screen, and the full-repaint fallback.

#include <QTest>

#include "tilinghandler/stripretire.h"

using namespace PlasmaZones;

namespace {

/// Stands in for ScrollVisualPlacement: the selection never reads the value.
struct Placement
{
    int x = 0;
};

const QString Screen = QStringLiteral("DP-1");
const QString Other = QStringLiteral("DP-2");

} // namespace

class TestStripRetire : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void recordedScreenMatchingIsRetired()
    {
        QHash<QString, Placement> delta{{QStringLiteral("a"), {1}}, {QStringLiteral("b"), {2}}};
        const QHash<QString, QString> recorded{{QStringLiteral("a"), Screen}, {QStringLiteral("b"), Other}};
        PhosphorCompositor::BorderState border;
        QVERIFY(StripRetire::retireRelocations(delta, recorded, border, Screen));
        QVERIFY(!delta.contains(QStringLiteral("a")));
        QVERIFY(delta.contains(QStringLiteral("b")));
    }

    // The fallback is NOT ORed in: a window recorded on another screen keeps
    // its relocation even when a stale tiled bucket still names this screen.
    void recordedScreenWinsOverAStaleBucket()
    {
        QHash<QString, Placement> delta{{QStringLiteral("a"), {1}}};
        const QHash<QString, QString> recorded{{QStringLiteral("a"), Other}};
        PhosphorCompositor::BorderState border;
        border.tiledWindowsByScreen[Screen].insert(QStringLiteral("a"));
        QVERIFY(!StripRetire::retireRelocations(delta, recorded, border, Screen));
        QVERIFY(delta.contains(QStringLiteral("a")));
    }

    // A window with no recorded screen is decided by tiled membership, the
    // demoted or rolled-back window the second term exists for.
    void noRecordedScreenFallsBackToTiledMembership()
    {
        QHash<QString, Placement> delta{{QStringLiteral("a"), {1}}, {QStringLiteral("b"), {2}}};
        PhosphorCompositor::BorderState border;
        border.tiledWindowsByScreen[Screen].insert(QStringLiteral("a"));
        border.tiledWindowsByScreen[Other].insert(QStringLiteral("b"));
        QVERIFY(StripRetire::retireRelocations(delta, {}, border, Screen));
        QVERIFY(!delta.contains(QStringLiteral("a")));
        QVERIFY(delta.contains(QStringLiteral("b")));
    }

    void nothingToRetireAnswersFalse()
    {
        QHash<QString, Placement> delta;
        PhosphorCompositor::BorderState border;
        QVERIFY(!StripRetire::retireRelocations(delta, {}, border, Screen));
    }

    // A full repaint only when relocations went and no strip motion did: the
    // motion's own drop repaints its output.
    void fullRepaintTable()
    {
        QVERIFY(StripRetire::needsFullRepaint(/*stripMotionDropped=*/false, /*relocationDropped=*/true));
        QVERIFY(!StripRetire::needsFullRepaint(true, true));
        QVERIFY(!StripRetire::needsFullRepaint(false, false));
        QVERIFY(!StripRetire::needsFullRepaint(true, false));
    }
};

QTEST_GUILESS_MAIN(TestStripRetire)
#include "test_strip_retire.moc"
