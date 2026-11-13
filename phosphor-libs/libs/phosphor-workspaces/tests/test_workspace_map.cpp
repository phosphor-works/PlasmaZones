// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "workspacereconcilerharness.h"

#include <PhosphorWorkspaces/WorkspaceMap.h>

#include <QTest>

using PhosphorWorkspaces::WorkspaceEntry;
using PhosphorWorkspaces::WorkspaceMap;
using WorkspaceTest::id;

namespace {
WorkspaceEntry entry(const QString& desktopId, const QString& name = QString())
{
    WorkspaceEntry e;
    e.desktopId = desktopId;
    e.name = name;
    return e;
}
}

class TestWorkspaceMap : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void insertRemove_ownerIndexInvariant();
    void insert_repairsDoubleOwnership();
    void reorderAndTransfer();
    void reorderSlice_permutationOnly();
    void renameScreen_reKeysInPlace();
    void takeSlice_returnsOrderedAndForgets();
    void globalPositionForInsert_arithmetic();
    void screenOrder_keepsSliceHoldersAndDeduplicates();
    void consistencyAndRepair();
    void serialization_roundTrip();
    void serialization_currentOnlyOnTheMatchingIndex();
    void fromJson_rejectsBadVersionAndGarbage();
    void fromJson_failureLeavesTheMapUntouched();
    void fromJson_firstOwnerInScreenOrderWins();
    void fromJson_mergesSliceKeysAndDeduplicatesTheOrder();
    void fromJson_acceptsOpaqueIdsAndTrimsNames();
    void remove_erasesTheEmptiedSlice();
};

void TestWorkspaceMap::insertRemove_ownerIndexInvariant()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    map.insert(QStringLiteral("A"), 0, entry(id(1)));
    map.insert(QStringLiteral("A"), 1, entry(id(2)));
    map.insert(QStringLiteral("B"), 0, entry(id(3)));

    QCOMPARE(map.ownerOf(id(1)), QStringLiteral("A"));
    QCOMPARE(map.ownerOf(id(3)), QStringLiteral("B"));
    QCOMPARE(map.sliceIndexOf(id(2)), 1);
    QCOMPARE(map.allDesktopIds(), QStringList({id(1), id(2), id(3)}));

    QVERIFY(map.remove(id(1)));
    QVERIFY(map.ownerOf(id(1)).isEmpty());
    QCOMPARE(map.sliceSize(QStringLiteral("A")), 1);
    QVERIFY(!map.remove(id(1)));
}

void TestWorkspaceMap::insert_repairsDoubleOwnership()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    map.insert(QStringLiteral("A"), 0, entry(id(1)));
    // Inserting the same id elsewhere removes it from the previous owner.
    map.insert(QStringLiteral("B"), 0, entry(id(1)));
    QCOMPARE(map.ownerOf(id(1)), QStringLiteral("B"));
    QCOMPARE(map.sliceSize(QStringLiteral("A")), 0);
}

void TestWorkspaceMap::reorderAndTransfer()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    map.insert(QStringLiteral("A"), 0, entry(id(1)));
    map.insert(QStringLiteral("A"), 1, entry(id(2), QStringLiteral("chat")));
    map.insert(QStringLiteral("A"), 2, entry(id(3)));

    QVERIFY(map.reorderWithinSlice(id(3), 0));
    QCOMPARE(map.slice(QStringLiteral("A")).first().desktopId, id(3));

    // Transfer keeps metadata (the named tag rides along).
    QVERIFY(map.transfer(id(2), QStringLiteral("B"), 0));
    QCOMPARE(map.ownerOf(id(2)), QStringLiteral("B"));
    QCOMPARE(map.entryFor(id(2)).name, QStringLiteral("chat"));
    QVERIFY(!map.transfer(id(99), QStringLiteral("B"), 0));
}

void TestWorkspaceMap::reorderSlice_permutationOnly()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A")});
    map.insert(QStringLiteral("A"), 0, entry(id(1)));
    map.insert(QStringLiteral("A"), 1, entry(id(2), QStringLiteral("chat")));
    map.insert(QStringLiteral("A"), 2, entry(id(3)));

    QVERIFY(map.reorderSlice(QStringLiteral("A"), {id(2), id(3), id(1)}));
    QCOMPARE(map.allDesktopIds(), QStringList({id(2), id(3), id(1)}));
    // Metadata travels with the entry.
    QCOMPARE(map.entryFor(id(2)).name, QStringLiteral("chat"));
    QCOMPARE(map.sliceIndexOf(id(1)), 2);
    // The same order again is no change.
    QVERIFY(!map.reorderSlice(QStringLiteral("A"), {id(2), id(3), id(1)}));
    // Anything that is not a permutation of the slice is refused untouched:
    // a missing id, a foreign id, a repeated id.
    QVERIFY(!map.reorderSlice(QStringLiteral("A"), {id(1), id(2)}));
    QVERIFY(!map.reorderSlice(QStringLiteral("A"), {id(1), id(2), id(9)}));
    QVERIFY(!map.reorderSlice(QStringLiteral("A"), {id(1), id(1), id(2)}));
    QCOMPARE(map.allDesktopIds(), QStringList({id(2), id(3), id(1)}));
}

void TestWorkspaceMap::renameScreen_reKeysInPlace()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
    map.insert(QStringLiteral("A"), 0, entry(id(1)));
    map.insert(QStringLiteral("B"), 0, entry(id(2)));
    map.insert(QStringLiteral("B"), 1, entry(id(3)));
    map.insert(QStringLiteral("C"), 0, entry(id(4)));
    // A displaced entry on C whose home is the screen being renamed.
    map.setHomeScreen(id(4), QStringLiteral("B"));

    QVERIFY(map.renameScreen(QStringLiteral("B"), QStringLiteral("B2")));
    // Same slot in the order, same slice, owner rows and stamps follow.
    QCOMPARE(map.screenOrder(), QStringList({QStringLiteral("A"), QStringLiteral("B2"), QStringLiteral("C")}));
    QCOMPARE(map.ownerOf(id(2)), QStringLiteral("B2"));
    QCOMPARE(map.sliceIndexOf(id(3)), 1);
    QVERIFY(!map.hasScreen(QStringLiteral("B")));
    QCOMPARE(map.entryFor(id(4)).homeScreenId, QStringLiteral("B2"));
    QCOMPARE(map.allDesktopIds(), QStringList({id(1), id(2), id(3), id(4)}));

    // Refused onto a screen that already holds a slice, and from one that is
    // not part of the world.
    QVERIFY(!map.renameScreen(QStringLiteral("A"), QStringLiteral("C")));
    QVERIFY(!map.renameScreen(QStringLiteral("Z"), QStringLiteral("Y")));
    QCOMPARE(map.ownerOf(id(1)), QStringLiteral("A"));
}

void TestWorkspaceMap::takeSlice_returnsOrderedAndForgets()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    map.insert(QStringLiteral("A"), 0, entry(id(1)));
    map.insert(QStringLiteral("A"), 1, entry(id(2)));

    const auto taken = map.takeSlice(QStringLiteral("A"));
    QCOMPARE(taken.size(), 2);
    QCOMPARE(taken.first().desktopId, id(1));
    QVERIFY(!map.hasScreen(QStringLiteral("A")));
    QVERIFY(map.ownerOf(id(1)).isEmpty());
    QVERIFY(!map.screenOrder().contains(QStringLiteral("A")));
}

void TestWorkspaceMap::globalPositionForInsert_arithmetic()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
    map.insert(QStringLiteral("A"), 0, entry(id(11)));
    map.insert(QStringLiteral("A"), 1, entry(id(12)));
    map.insert(QStringLiteral("B"), 0, entry(id(21)));

    QCOMPARE(map.globalPositionForInsert(QStringLiteral("A"), 0), 0u);
    QCOMPARE(map.globalPositionForInsert(QStringLiteral("A"), 2), 2u);
    QCOMPARE(map.globalPositionForInsert(QStringLiteral("A"), 99), 2u); // clamped to slice size
    QCOMPARE(map.globalPositionForInsert(QStringLiteral("B"), 1), 3u);
    QCOMPARE(map.globalPositionForInsert(QStringLiteral("C"), 0), 3u);
}

void TestWorkspaceMap::screenOrder_keepsSliceHoldersAndDeduplicates()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    map.insert(QStringLiteral("B"), 0, entry(id(1)));
    // A reorder omitting a slice-holding screen appends it, never orphans it.
    map.setScreenOrder({QStringLiteral("A")});
    QCOMPARE(map.screenOrder(), QStringList({QStringLiteral("A"), QStringLiteral("B")}));

    // A screen listed twice is listed once: the order is walked to sum
    // positions and serialize slices, so a repeat would count B twice.
    map.setScreenOrder({QStringLiteral("B"), QStringLiteral("A"), QStringLiteral("B"), QString()});
    QCOMPARE(map.screenOrder(), QStringList({QStringLiteral("B"), QStringLiteral("A")}));
    QCOMPARE(map.allDesktopIds(), QStringList({id(1)}));
}

void TestWorkspaceMap::consistencyAndRepair()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    map.insert(QStringLiteral("A"), 0, entry(id(1)));
    map.insert(QStringLiteral("A"), 1, entry(id(2)));
    map.insert(QStringLiteral("B"), 0, entry(id(4)));

    QVERIFY(map.consistentWith({id(1), id(2), id(4)}));
    QVERIFY(!map.consistentWith({id(1), id(2)}));
    QVERIFY(!map.consistentWith({id(1), id(2), id(3), id(4)}));

    // The second desktop and B's only one vanished and a third appeared:
    // repair drops the former and reports the latter.
    const QStringList unowned = map.repairAgainst({id(1), id(3)});
    QCOMPARE(unowned, QStringList({id(3)}));
    QVERIFY(map.ownerOf(id(2)).isEmpty());
    QCOMPARE(map.sliceSize(QStringLiteral("A")), 1);
    // A slice repair emptied leaves the hash, the same no-empty-slices rule
    // remove() keeps, while B stays part of the world through the order.
    QVERIFY(!map.hasScreen(QStringLiteral("B")));
    QVERIFY(map.knowsScreen(QStringLiteral("B")));
}

void TestWorkspaceMap::serialization_roundTrip()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    map.insert(QStringLiteral("A"), 0, entry(id(1), QStringLiteral("chat")));
    map.setNamedId(id(1), QStringLiteral("{decl-chat}"));
    map.insert(QStringLiteral("A"), 1, entry(id(2)));
    map.insert(QStringLiteral("B"), 0, entry(id(3)));
    map.setHomeScreen(id(3), QStringLiteral("C"));

    QHash<QString, int> current;
    current.insert(QStringLiteral("A"), 2);
    current.insert(QStringLiteral("B"), 3);
    const auto indexOf = [](const QString& desktopId) {
        if (desktopId == id(1)) {
            return 1;
        }
        if (desktopId == id(2)) {
            return 2;
        }
        return 3;
    };

    const QString stateJson = map.toJson(7, current, indexOf, /*includeState=*/true);
    QVERIFY(stateJson.contains(QStringLiteral("\"generation\":7")));
    QVERIFY(stateJson.contains(QStringLiteral("\"homeScreen\":\"C\"")));

    WorkspaceMap restored;
    QVERIFY(restored.fromJson(stateJson));
    QCOMPARE(restored.screenOrder(), map.screenOrder());
    QCOMPARE(restored.allDesktopIds(), map.allDesktopIds());
    QCOMPARE(restored.entryFor(id(1)).name, QStringLiteral("chat"));
    // The declaration id survives a restart: a rename after one is still a
    // rename in place.
    QCOMPARE(restored.entryFor(id(1)).namedId, QStringLiteral("{decl-chat}"));
    QCOMPARE(restored.entryFor(id(3)).homeScreenId, QStringLiteral("C"));
    QVERIFY(restored == map);

    // The wire flavour omits homeScreen.
    const QString wireJson = map.toJson(8, current, indexOf, /*includeState=*/false);
    QVERIFY(!wireJson.contains(QStringLiteral("homeScreen")));
}

void TestWorkspaceMap::serialization_currentOnlyOnTheMatchingIndex()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    map.insert(QStringLiteral("A"), 0, entry(id(1)));
    map.insert(QStringLiteral("A"), 1, entry(id(2)));
    map.insert(QStringLiteral("B"), 0, entry(id(3)));
    const auto indexOf = [](const QString& desktopId) {
        return desktopId == id(1) ? 1 : (desktopId == id(2) ? 2 : 3);
    };

    // A shows its second desktop; B reports nothing.
    QHash<QString, int> current;
    current.insert(QStringLiteral("A"), 2);
    const QString json = map.toJson(1, current, indexOf, /*includeState=*/false);
    // Exactly one current flag, on A's second entry, and none for B: a screen
    // with no report must not be published as showing anything.
    QCOMPARE(json.count(QStringLiteral("\"current\":true")), 1);
    QVERIFY(json.contains(QStringLiteral("{\"current\":true,\"id\":\"%1\",\"index\":2}").arg(id(2))));
}

void TestWorkspaceMap::fromJson_rejectsBadVersionAndGarbage()
{
    WorkspaceMap map;
    QVERIFY(!map.fromJson(QStringLiteral("not json")));
    QVERIFY(!map.fromJson(QStringLiteral("{\"v\":99,\"screenOrder\":[],\"slices\":{}}")));
    QVERIFY(map.fromJson(QStringLiteral("{\"v\":1,\"screenOrder\":[],\"slices\":{}}")));
    QVERIFY(map.allDesktopIds().isEmpty());
}

void TestWorkspaceMap::fromJson_failureLeavesTheMapUntouched()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A")});
    map.insert(QStringLiteral("A"), 0, entry(id(1), QStringLiteral("chat")));
    const WorkspaceMap before = map;

    // A failed parse is no reason to lose what the caller already had.
    QVERIFY(!map.fromJson(QStringLiteral("not json")));
    QVERIFY(map == before);
    QVERIFY(!map.fromJson(QStringLiteral("{\"v\":99,\"screenOrder\":[\"B\"],\"slices\":{}}")));
    QVERIFY(map == before);
    QCOMPARE(map.ownerOf(id(1)), QStringLiteral("A"));
}

void TestWorkspaceMap::fromJson_firstOwnerInScreenOrderWins()
{
    // A duplicate id across slices is dropped on parse, and "first" means
    // first in the stored SCREEN ORDER. B precedes A here, against the
    // alphabetical order the JSON object's keys come back in, so a walk over
    // the keys would hand the desktop to A.
    WorkspaceMap map;
    const QString duplicate = QStringLiteral(
                                  "{\"v\":1,\"screenOrder\":[\"B\",\"A\"],\"slices\":{\"A\":[{\"id\":\"%1\"}],"
                                  "\"B\":[{\"id\":\"%1\"}]}}")
                                  .arg(id(1));
    QVERIFY(map.fromJson(duplicate));
    QCOMPARE(map.ownerOf(id(1)), QStringLiteral("B"));
    QCOMPARE(map.sliceSize(QStringLiteral("A")), 0);
}

void TestWorkspaceMap::fromJson_mergesSliceKeysAndDeduplicatesTheOrder()
{
    // The order repeats A and omits C, whose slice is nonetheless stored: C is
    // appended after the ordered screens and A is walked once.
    WorkspaceMap map;
    const QString json = QStringLiteral(
                             "{\"v\":1,\"screenOrder\":[\"A\",\"B\",\"A\"],\"slices\":{"
                             "\"A\":[{\"id\":\"%1\"}],\"B\":[{\"id\":\"%2\"}],\"C\":[{\"id\":\"%3\"}]}}")
                             .arg(id(1), id(2), id(3));
    QVERIFY(map.fromJson(json));
    QCOMPARE(map.screenOrder(), QStringList({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}));
    QCOMPARE(map.allDesktopIds(), QStringList({id(1), id(2), id(3)}));
    QCOMPARE(map.ownerOf(id(3)), QStringLiteral("C"));
}

void TestWorkspaceMap::fromJson_acceptsOpaqueIdsAndTrimsNames()
{
    WorkspaceMap map;
    // A desktop id is opaque. KWin mints unbraced UUIDs and keeps whatever a
    // loaded kwinrc carried, so a braced UUID, an unbraced one and a non-UUID
    // string are all ownership worth keeping across a restart. Only the empty
    // and the absurdly long are refused.
    const QString overlong(WorkspaceMap::MaxDesktopIdLength + 1, QLatin1Char('x'));
    const QString ids = QStringLiteral(
                            "{\"v\":1,\"screenOrder\":[\"A\"],\"slices\":{\"A\":[{\"id\":\"%1\"},"
                            "{\"id\":\"{%2}\"},{\"id\":\"desktop-from-kwinrc\"},{\"id\":\"\"},{\"id\":\"%3\"}]}}")
                            .arg(id(1), id(2), overlong);
    QVERIFY(map.fromJson(ids));
    QCOMPARE(map.allDesktopIds(),
             QStringList({id(1), QStringLiteral("{%1}").arg(id(2)), QStringLiteral("desktop-from-kwinrc")}));

    // A whitespace-only name is no name: kept as-is it would render as nothing
    // and still make the desktop destroy-exempt. A padded real name keeps its
    // text without the padding.
    // The trim decides namedness, so a whitespace-only name also drops the
    // declaration id it carried: a dynamic desktop realizes no declaration.
    // The name is the only trimmed field. Desktop ids are opaque and compared
    // verbatim against KWin's, so a padded one is kept padded.
    const QString names = QStringLiteral(
                              "{\"v\":1,\"screenOrder\":[\"A\"],\"slices\":{\"A\":[{\"id\":\"%1\",\"name\":\"   \","
                              "\"namedId\":\"{abc}\"},{\"id\":\"%2\",\"name\":\"  chat \",\"namedId\":\"{def}\"},"
                              "{\"id\":\" padded \"}]}}")
                              .arg(id(3), id(4));
    QVERIFY(map.fromJson(names));
    QVERIFY(map.entryFor(id(3)).name.isEmpty());
    QVERIFY(map.entryFor(id(3)).namedId.isEmpty());
    QCOMPARE(map.entryFor(id(4)).name, QStringLiteral("chat"));
    QCOMPARE(map.entryFor(id(4)).namedId, QStringLiteral("{def}"));
    QCOMPARE(map.ownerOf(QStringLiteral(" padded ")), QStringLiteral("A"));
    QVERIFY(map.ownerOf(QStringLiteral("padded")).isEmpty());
}

void TestWorkspaceMap::remove_erasesTheEmptiedSlice()
{
    WorkspaceMap map;
    map.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    map.insert(QStringLiteral("A"), 0, entry(id(1)));

    // hasScreen() answers "holds a slice", so a screen whose last desktop just
    // left must answer false — an empty list left in the hash made it true.
    QVERIFY(map.hasScreen(QStringLiteral("A")));
    QVERIFY(map.remove(id(1)));
    QVERIFY(!map.hasScreen(QStringLiteral("A")));
    // Still part of the world, though: the screen order is untouched.
    QVERIFY(map.knowsScreen(QStringLiteral("A")));

    // The same invariant across a transfer, which is a remove plus an insert.
    map.insert(QStringLiteral("A"), 0, entry(id(2)));
    QVERIFY(map.transfer(id(2), QStringLiteral("B"), 0));
    QVERIFY(!map.hasScreen(QStringLiteral("A")));
    QVERIFY(map.hasScreen(QStringLiteral("B")));
}

QTEST_GUILESS_MAIN(TestWorkspaceMap)
#include "test_workspace_map.moc"
