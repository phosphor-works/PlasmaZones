// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorEngine/DesktopRemap.h>
#include <PhosphorEngine/WindowRegistry.h>

#include <QTest>

#include <algorithm>

using PhosphorEngine::DesktopRemap;
using PhosphorEngine::WindowMetadata;
using PhosphorEngine::WindowRegistry;

namespace {
WindowMetadata onDesktop(int desktop)
{
    WindowMetadata metadata;
    metadata.appId = QStringLiteral("org.example.app");
    metadata.virtualDesktop = desktop;
    return metadata;
}

WindowMetadata onDesktops(int desktop, const QList<int>& desktops)
{
    WindowMetadata metadata = onDesktop(desktop);
    metadata.virtualDesktops = desktops;
    return metadata;
}

QStringList sorted(QStringList list)
{
    std::sort(list.begin(), list.end());
    return list;
}
} // namespace

/// The window registry's record listing and the bounds of its desktop
/// renumber snapshot. The full behaviour suite lives in the app tier
/// (test_window_registry); these pin the library-level contracts.
class TestWindowRegistry : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void instanceIds_listsRecordsOnly()
    {
        WindowRegistry reg;
        // A canonical id frozen before any metadata arrived: no record, no
        // context to seed from, so instanceIds() leaves it out (F838).
        reg.canonicalizeWindowId(QStringLiteral("org.example.app|canonical-only"));
        reg.upsert(QStringLiteral("recorded"), onDesktop(1));
        QCOMPARE(reg.instanceIds(), QStringList{QStringLiteral("recorded")});
        QVERIFY(!reg.contains(QStringLiteral("canonical-only")));

        // Its first push makes it a record like any other.
        reg.upsert(QStringLiteral("canonical-only"), onDesktop(2));
        QCOMPARE(sorted(reg.instanceIds()),
                 (QStringList{QStringLiteral("canonical-only"), QStringLiteral("recorded")}));

        reg.remove(QStringLiteral("recorded"));
        QCOMPARE(reg.instanceIds(), QStringList{QStringLiteral("canonical-only")});
    }

    void endDesktopRenumber_dropsTheSnapshot()
    {
        WindowRegistry reg;
        reg.upsert(QStringLiteral("w"), onDesktop(3));
        reg.beginDesktopRenumber();
        // The change renumbered nothing (an append), so no remap comes for it.
        reg.endDesktopRenumber();

        // A later renumber with no change announced ahead of it (the start-up
        // one) must not move a record pushed in the live numbering.
        QCOMPARE(reg.remapDesktops(DesktopRemap::removal(2, 4)), 0);
        QCOMPARE(reg.metadata(QStringLiteral("w"))->virtualDesktop, 3);
    }

    void identityRemap_endsTheSnapshot()
    {
        WindowRegistry reg;
        reg.upsert(QStringLiteral("w"), onDesktop(3));
        reg.beginDesktopRenumber();
        QCOMPARE(reg.remapDesktops(DesktopRemap()), 0);

        QCOMPARE(reg.remapDesktops(DesktopRemap::removal(2, 4)), 0);
        QCOMPARE(reg.metadata(QStringLiteral("w"))->virtualDesktop, 3);
    }

    void snapshot_movesRecordsPushedBeforeTheChange()
    {
        WindowRegistry reg;
        reg.upsert(QStringLiteral("w"), onDesktop(3));
        reg.beginDesktopRenumber();
        QCOMPARE(reg.remapDesktops(DesktopRemap::removal(2, 4)), 1);
        QCOMPARE(reg.metadata(QStringLiteral("w"))->virtualDesktop, 2);
    }

    // KWin moves only a window that was on the removed desktop ALONE to the
    // successor. A window on several desktops just leaves the removed one, so
    // a span maps entry by entry and drops it (F890).
    void spanDropsTheRemovedDesktopInsteadOfTakingTheSuccessor()
    {
        WindowRegistry reg;
        reg.upsert(QStringLiteral("single"), onDesktop(2));
        reg.upsert(QStringLiteral("pair"), onDesktops(1, {1, 2}));
        reg.upsert(QStringLiteral("triple"), onDesktops(1, {1, 2, 4}));
        reg.upsert(QStringLiteral("lastPair"), onDesktops(3, {3, 4}));
        reg.beginDesktopRenumber();

        // Desktop 2 of 4 goes: 3 becomes 2 and 4 becomes 3.
        // "single" keeps its number (its successor is 2 again) and is not counted.
        QCOMPARE(reg.remapDesktops(DesktopRemap::removal(2, 4)), 3);

        // On 2 alone: sent to the desktop that takes its number.
        QCOMPARE(reg.metadata(QStringLiteral("single"))->virtualDesktop, 2);
        // {1, 2}: leaves 2 and stays on 1 only, not {1, 2(successor)}.
        QCOMPARE(reg.metadata(QStringLiteral("pair"))->virtualDesktop, 1);
        QVERIFY(reg.metadata(QStringLiteral("pair"))->virtualDesktops.isEmpty());
        // {1, 2, 4}: leaves 2, keeps 1 and the renumbered 4.
        QCOMPARE(reg.metadata(QStringLiteral("triple"))->virtualDesktop, 1);
        QCOMPARE(reg.metadata(QStringLiteral("triple"))->virtualDesktops, (QList<int>{1, 3}));
        // Untouched entries just renumber.
        QCOMPARE(reg.metadata(QStringLiteral("lastPair"))->virtualDesktop, 2);
        QCOMPARE(reg.metadata(QStringLiteral("lastPair"))->virtualDesktops, (QList<int>{2, 3}));
    }

    // A span every entry of which goes takes the successor of its first entry.
    void spanWithEveryEntryRemovedTakesTheSuccessor()
    {
        WindowRegistry reg;
        reg.upsert(QStringLiteral("w"), onDesktops(2, {2, 3}));
        reg.beginDesktopRenumber();

        // Desktops b and c of a, b, c, d go: d becomes 2.
        QCOMPARE(reg.remapDesktops(DesktopRemap::fromIds(
                     {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d")},
                     {QStringLiteral("a"), QStringLiteral("d")})),
                 1);

        QCOMPARE(reg.metadata(QStringLiteral("w"))->virtualDesktop, 2);
        QVERIFY(reg.metadata(QStringLiteral("w"))->virtualDesktops.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestWindowRegistry)
#include "test_windowregistry.moc"
