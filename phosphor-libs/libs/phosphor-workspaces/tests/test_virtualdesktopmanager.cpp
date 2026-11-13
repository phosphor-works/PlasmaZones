// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorWorkspaces/VirtualDesktopManager.h>

#include <PhosphorEngine/DesktopRemap.h>

#include <QCoreApplication>
#include <QDBusMessage>
#include <QSignalSpy>
#include <QTest>

using PhosphorEngine::DesktopRemap;
using PhosphorWorkspaces::VirtualDesktopManager;

namespace {

/// Feeds the manager KWin's desktop lists and desktop signals directly, the
/// way its D-Bus handlers do once they have parsed a message. No KWin and no
/// D-Bus: the manager is never init()ed, so its getters that answer for an
/// absent compositor (desktopCount, currentDesktop, the names) keep their
/// fallbacks, and the tests read the signals and desktopIds() instead.
class ListDriver : public VirtualDesktopManager
{
public:
    using VirtualDesktopManager::DesktopEntry;
    using VirtualDesktopManager::ingestDesktopCreated;
    using VirtualDesktopManager::ingestDesktopDataChanged;

    bool commit(const QStringList& ids, const QString& currentId = QString(), const QStringList& names = {})
    {
        QList<DesktopEntry> entries;
        for (int i = 0; i < ids.size(); ++i) {
            entries.append(DesktopEntry{i, ids.at(i), names.value(i)});
        }
        return commitDesktopList(entries, currentId, 1);
    }
    bool commitEntries(const QList<DesktopEntry>& entries)
    {
        return commitDesktopList(entries, QString(), 1);
    }
    void removed(const QString& desktopId)
    {
        QVERIFY(QMetaObject::invokeMethod(this, "onKWinDesktopRemoved", Q_ARG(QString, desktopId)));
    }
    void count(uint count)
    {
        QVERIFY(QMetaObject::invokeMethod(this, "onNumberOfDesktopsChanged", Q_ARG(uint, count)));
    }
    void current(const QString& desktopId)
    {
        QVERIFY(QMetaObject::invokeMethod(this, "onKWinCurrentChanged", Q_ARG(QString, desktopId)));
    }
    /// KWin's desktopCreated as it arrives off the bus, before any parsing.
    void createdMessage(const QDBusMessage& message)
    {
        QVERIFY(QMetaObject::invokeMethod(this, "onKWinDesktopCreated", Q_ARG(QDBusMessage, message)));
    }
};

QStringList ids(const char* letters)
{
    QStringList out;
    for (const char* c = letters; *c; ++c) {
        out.append(QString(QLatin1Char(*c)));
    }
    return out;
}

} // namespace

class TestVirtualDesktopManager : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // The per-output desktop map, fed by the compositor's per-output reports.
    void unknownScreen_fallsBackToGlobal();
    void hasScreenDesktopReport_reflectsRealReportsOnly();
    void updateScreenDesktop_recordsAndResolves();
    void updateScreenDesktop_emitsOnlyOnChange();
    void updateScreenDesktop_rejectsInvalid();
    void seedScreenDesktop_recordsWithoutASwitch();
    void perScreenModeActive_tracksDivergence();
    void desktopCountShrink_clampsPerScreenEntries();

    // The id accessors and KWin call wrappers with no KWin behind them.
    void desktopIdAccessors_nullWithoutKWin();
    void kwinWrappers_noOpWithoutKWin();

    // The per-output desktop map across a screen-id rename.
    void renameScreen_movesTheRowAsASeed();
    void renameScreen_existingTargetWinsAndStaysQuiet();
    void renameScreen_ignoresUnknownAndDegenerateIds();
    void renameScreen_oldSpellingReportsCountForTheNewIdUntilItReports();

    // The list commit (F302).
    void commit_refusesListsKWinCannotSend();
    void commit_ordersByPosition();
    void commit_movesScreenDesktopsBeforeAnnouncingTheList();
    void commit_reportsRenamesOfSurvivingDesktops();

    // The list built from KWin's desktop signals.
    void removal_commitsAtCountChanged();
    void insert_commitsAtCountChanged();
    void rename_startsNoChange();
    void secondChange_isAnnouncedAgain();
    void currentChangedMidChange_waitsForTheCommit();
    void malformedCreate_stillEchoesAReadableId();
};

// A screen with no per-output desktop on record resolves to the global value.
void TestVirtualDesktopManager::unknownScreen_fallsBackToGlobal()
{
    VirtualDesktopManager vdm;
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-1")), vdm.currentDesktop());
    QVERIFY(!vdm.perScreenModeActive());
}

// hasScreenDesktopReport is true only for a REAL report (exact key, or the
// parent output of a virtual id), never for the global fallback
// currentDesktopForScreen would return. An adoption gate that waits for every
// screen's report reads it.
void TestVirtualDesktopManager::hasScreenDesktopReport_reflectsRealReportsOnly()
{
    VirtualDesktopManager vdm;
    QVERIFY(!vdm.hasScreenDesktopReport(QStringLiteral("DP-1")));
    vdm.updateScreenDesktop(QStringLiteral("DP-1"), 3);
    QVERIFY(vdm.hasScreenDesktopReport(QStringLiteral("DP-1")));
    // A virtual child resolves through its parent output.
    QVERIFY(vdm.hasScreenDesktopReport(QStringLiteral("DP-1/vs:1")));
    // Another screen still has no report even though
    // currentDesktopForScreen would answer with the global current.
    QVERIFY(!vdm.hasScreenDesktopReport(QStringLiteral("DP-2")));
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-2")), vdm.currentDesktop());
}

// Recorded per-screen desktops resolve independently, and unknown screens
// still fall back to the global desktop.
void TestVirtualDesktopManager::updateScreenDesktop_recordsAndResolves()
{
    VirtualDesktopManager vdm;
    vdm.updateScreenDesktop(QStringLiteral("DP-1"), 3);
    vdm.updateScreenDesktop(QStringLiteral("DP-2"), 5);

    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-1")), 3);
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-2")), 5);
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-9")), vdm.currentDesktop());
}

// screenDesktopChanged fires only when a screen's desktop actually changes.
void TestVirtualDesktopManager::updateScreenDesktop_emitsOnlyOnChange()
{
    VirtualDesktopManager vdm;
    QSignalSpy spy(&vdm, &VirtualDesktopManager::screenDesktopChanged);

    vdm.updateScreenDesktop(QStringLiteral("DP-1"), 3);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.last().at(0).toString(), QStringLiteral("DP-1"));
    QCOMPARE(spy.last().at(1).toInt(), 3);

    vdm.updateScreenDesktop(QStringLiteral("DP-1"), 3); // same value, no emit
    QCOMPARE(spy.count(), 1);

    vdm.updateScreenDesktop(QStringLiteral("DP-1"), 4); // changed, emit
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.last().at(1).toInt(), 4);
}

// Empty screen id or a non-positive desktop are rejected silently.
void TestVirtualDesktopManager::updateScreenDesktop_rejectsInvalid()
{
    VirtualDesktopManager vdm;
    QSignalSpy spy(&vdm, &VirtualDesktopManager::screenDesktopChanged);

    vdm.updateScreenDesktop(QString(), 3);
    vdm.updateScreenDesktop(QStringLiteral("DP-1"), 0);
    vdm.updateScreenDesktop(QStringLiteral("DP-1"), -2);

    QCOMPARE(spy.count(), 0);
    QVERIFY(!vdm.hasScreenDesktopReport(QStringLiteral("DP-1")));
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-1")), vdm.currentDesktop());
}

// A returning output's desktop is a seed, not a switch: it is recorded and
// announced on the seed signal only, every time (the engines dropped the
// output's desktop when it went away, so a value the map still holds is
// re-announced too), and invalid input is refused like a switch's.
void TestVirtualDesktopManager::seedScreenDesktop_recordsWithoutASwitch()
{
    VirtualDesktopManager vdm;
    QSignalSpy switched(&vdm, &VirtualDesktopManager::screenDesktopChanged);
    QSignalSpy seeded(&vdm, &VirtualDesktopManager::screenDesktopSeeded);

    vdm.seedScreenDesktop(QStringLiteral("DP-1"), 3);
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-1")), 3);
    QCOMPARE(seeded.count(), 1);
    QCOMPARE(seeded.last().at(0).toString(), QStringLiteral("DP-1"));
    QCOMPARE(seeded.last().at(1).toInt(), 3);

    vdm.seedScreenDesktop(QStringLiteral("DP-1"), 3);
    QCOMPARE(seeded.count(), 2);

    vdm.seedScreenDesktop(QString(), 2);
    vdm.seedScreenDesktop(QStringLiteral("DP-2"), 0);
    QCOMPARE(seeded.count(), 2);
    QCOMPARE(switched.count(), 0);

    // A later real switch on that screen still reports as one.
    vdm.updateScreenDesktop(QStringLiteral("DP-1"), 4);
    QCOMPARE(switched.count(), 1);
}

// Per-screen mode is active iff two screens are on different desktops, and
// deactivates when they reconverge.
void TestVirtualDesktopManager::perScreenModeActive_tracksDivergence()
{
    VirtualDesktopManager vdm;
    QVERIFY(!vdm.perScreenModeActive()); // empty

    vdm.updateScreenDesktop(QStringLiteral("DP-1"), 2);
    QVERIFY(!vdm.perScreenModeActive()); // single screen

    vdm.updateScreenDesktop(QStringLiteral("DP-2"), 2);
    QVERIFY(!vdm.perScreenModeActive()); // two screens, same desktop

    vdm.updateScreenDesktop(QStringLiteral("DP-2"), 5);
    QVERIFY(vdm.perScreenModeActive()); // diverged

    vdm.updateScreenDesktop(QStringLiteral("DP-2"), 2);
    QVERIFY(!vdm.perScreenModeActive()); // reconverged
}

// clampScreenDesktopsToCount: a per-screen entry above a count committed with
// no list behind it is pulled down to the count, and only that screen
// re-emits. A manager with KWin reaches the clamp from
// retryOrAbandonRefresh's count roll-back; this one has no KWin interface, so
// onNumberOfDesktopsChanged commits the count itself and is used here as the
// seam that reaches the same clamp.
void TestVirtualDesktopManager::desktopCountShrink_clampsPerScreenEntries()
{
    VirtualDesktopManager vdm;
    QVERIFY(QMetaObject::invokeMethod(&vdm, "onNumberOfDesktopsChanged", Q_ARG(uint, 7)));

    vdm.updateScreenDesktop(QStringLiteral("DP-1"), 3);
    vdm.updateScreenDesktop(QStringLiteral("DP-2"), 7);

    QSignalSpy spy(&vdm, &VirtualDesktopManager::screenDesktopChanged);
    // Count 7 -> 4. The entry on 7 must clamp to 4; the entry on 3 is
    // unaffected.
    QVERIFY(QMetaObject::invokeMethod(&vdm, "onNumberOfDesktopsChanged", Q_ARG(uint, 4)));

    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-1")), 3);
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-2")), 4);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.last().at(0).toString(), QStringLiteral("DP-2"));
    QCOMPARE(spy.last().at(1).toInt(), 4);

    // Boundary: an entry sitting EXACTLY at the new count must not be
    // touched (a `>=` in the clamp would pull it down and emit). Shrink to 3:
    // DP-1 sits at 3 and stays put while DP-2 on 4 clamps.
    QVERIFY(QMetaObject::invokeMethod(&vdm, "onNumberOfDesktopsChanged", Q_ARG(uint, 3)));
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-1")), 3);
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-2")), 3);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.last().at(0).toString(), QStringLiteral("DP-2"));

    // Growth: raising the count above every entry clamps nothing and emits
    // nothing.
    QVERIFY(QMetaObject::invokeMethod(&vdm, "onNumberOfDesktopsChanged", Q_ARG(uint, 9)));
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-1")), 3);
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-2")), 3);
    QCOMPARE(spy.count(), 2);
}

// Without a KWin id list the id accessors answer their documented null values
// (empty list, empty id, index 0). desktopIds() synthesises nothing, unlike
// desktopNames(): a made-up id would match nothing in KWin.
void TestVirtualDesktopManager::desktopIdAccessors_nullWithoutKWin()
{
    VirtualDesktopManager vdm;
    QVERIFY(vdm.desktopIds().isEmpty());
    QVERIFY(vdm.desktopIdAt(1).isEmpty());
    QVERIFY(vdm.desktopIdAt(0).isEmpty());
    QVERIFY(vdm.desktopIdAt(-3).isEmpty());
    QCOMPARE(vdm.desktopIndexOf(QStringLiteral("{missing}")), 0);
}

// The create/remove/rename wrappers send nothing and change nothing without a
// KWin interface: no signal, no answer, and no cache updated ahead of a round
// trip (an optimistic append to the id list would claim a desktop KWin never
// made). desktopCount(), desktopNames() and currentDesktop() are not read
// here: without KWin they answer fixed fallbacks whatever the members hold.
void TestVirtualDesktopManager::kwinWrappers_noOpWithoutKWin()
{
    VirtualDesktopManager vdm;
    // A per-screen entry too: a rename or removal that walked the per-output
    // map without a live KWin would show up here.
    vdm.updateScreenDesktop(QStringLiteral("DP-1"), 2);

    QSignalSpy listSpy(&vdm, &VirtualDesktopManager::desktopListChanged);
    QSignalSpy createdSpy(&vdm, &VirtualDesktopManager::kwinDesktopCreated);
    QSignalSpy removedSpy(&vdm, &VirtualDesktopManager::kwinDesktopRemoved);
    QSignalSpy countSpy(&vdm, &VirtualDesktopManager::desktopCountChanged);
    QSignalSpy screenSpy(&vdm, &VirtualDesktopManager::screenDesktopChanged);
    QSignalSpy createAnswerSpy(&vdm, &VirtualDesktopManager::createDesktopAnswered);
    QSignalSpy removeAnswerSpy(&vdm, &VirtualDesktopManager::removeDesktopAnswered);

    vdm.createDesktop(0, QStringLiteral("x"));
    vdm.removeDesktop(QStringLiteral("{id}"));
    vdm.setDesktopName(QStringLiteral("{id}"), QStringLiteral("y"));
    QCoreApplication::processEvents();

    QCOMPARE(listSpy.count(), 0);
    QCOMPARE(createdSpy.count(), 0);
    QCOMPARE(removedSpy.count(), 0);
    QCOMPARE(countSpy.count(), 0);
    QCOMPARE(screenSpy.count(), 0);
    QCOMPARE(createAnswerSpy.count(), 0);
    QCOMPARE(removeAnswerSpy.count(), 0);

    QVERIFY(vdm.desktopIds().isEmpty());
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-1")), 2);
}

void TestVirtualDesktopManager::renameScreen_movesTheRowAsASeed()
{
    VirtualDesktopManager vdm;
    const QString oldId = QStringLiteral("DP-2");
    const QString newId = QStringLiteral("DP-2/DELL-U2415");

    vdm.updateScreenDesktop(oldId, 3);
    QVERIFY(vdm.hasScreenDesktopReport(oldId));

    QSignalSpy seeded(&vdm, &VirtualDesktopManager::screenDesktopSeeded);
    QSignalSpy changed(&vdm, &VirtualDesktopManager::screenDesktopChanged);
    vdm.renameScreen(oldId, newId);

    // Without the re-key the row stays under the dead id: the live id has no
    // report and currentDesktopForScreen falls back to the GLOBAL desktop (1),
    // which is the whole defect.
    QVERIFY(!vdm.hasScreenDesktopReport(oldId));
    QVERIFY(vdm.hasScreenDesktopReport(newId));
    QCOMPARE(vdm.currentDesktopForScreen(newId), 3);
    // A seed, never a switch: a switch would run the desktop-switch pass and
    // its OSD for an output that switched nothing (F221).
    QCOMPARE(changed.count(), 0);
    QCOMPARE(seeded.count(), 1);
    QCOMPARE(seeded.first().at(0).toString(), newId);
    QCOMPARE(seeded.first().at(1).toInt(), 3);
}

void TestVirtualDesktopManager::renameScreen_existingTargetWinsAndStaysQuiet()
{
    VirtualDesktopManager vdm;
    const QString oldId = QStringLiteral("DP-2");
    const QString newId = QStringLiteral("DP-2/DELL-U2415");

    vdm.updateScreenDesktop(oldId, 3);
    vdm.updateScreenDesktop(newId, 5); // a report already made under the live id

    QSignalSpy seeded(&vdm, &VirtualDesktopManager::screenDesktopSeeded);
    QSignalSpy changed(&vdm, &VirtualDesktopManager::screenDesktopChanged);
    vdm.renameScreen(oldId, newId);

    QVERIFY(!vdm.hasScreenDesktopReport(oldId)); // the stale row is dropped
    QCOMPARE(vdm.currentDesktopForScreen(newId), 5); // the fresher report stands
    QCOMPARE(seeded.count(), 0); // no value changed, so nothing is emitted
    QCOMPARE(changed.count(), 0);
}

void TestVirtualDesktopManager::renameScreen_ignoresUnknownAndDegenerateIds()
{
    VirtualDesktopManager vdm;
    vdm.updateScreenDesktop(QStringLiteral("DP-2"), 4);

    QSignalSpy seeded(&vdm, &VirtualDesktopManager::screenDesktopSeeded);
    QSignalSpy changed(&vdm, &VirtualDesktopManager::screenDesktopChanged);
    vdm.renameScreen(QStringLiteral("HDMI-A-1"), QStringLiteral("HDMI-A-1/X")); // nothing recorded
    vdm.renameScreen(QStringLiteral("DP-2"), QStringLiteral("DP-2")); // same id
    // An empty new id would file the row under a key no lookup can reach.
    vdm.renameScreen(QStringLiteral("DP-2"), QString());

    QCOMPARE(seeded.count(), 0);
    QCOMPARE(changed.count(), 0);
    QVERIFY(!vdm.hasScreenDesktopReport(QStringLiteral("HDMI-A-1/X")));
    QVERIFY(vdm.hasScreenDesktopReport(QStringLiteral("DP-2")));
    QCOMPARE(vdm.currentDesktopForScreen(QStringLiteral("DP-2")), 4);
}

// The daemon learns a rename from its own QScreen set and the effect
// re-spells on KWin's screenAdded/screenRemoved, with no order between the
// two, so the effect can still report under the old spelling after the
// rename. Such a report is the renamed output's: recorded under the new id,
// with no row coming back under the dead one. The effect's first report under
// the new id ends that, since its reports arrive in the order it sent them.
void TestVirtualDesktopManager::renameScreen_oldSpellingReportsCountForTheNewIdUntilItReports()
{
    VirtualDesktopManager vdm;
    const QString oldId = QStringLiteral("DP-2");
    const QString newId = QStringLiteral("DP-2/DELL-U2415");
    vdm.updateScreenDesktop(oldId, 3);
    vdm.renameScreen(oldId, newId);
    QCOMPARE(vdm.liveScreenIdFor(oldId), newId);

    QSignalSpy seeded(&vdm, &VirtualDesktopManager::screenDesktopSeeded);
    QSignalSpy changed(&vdm, &VirtualDesktopManager::screenDesktopChanged);
    vdm.updateScreenDesktop(oldId, 4);
    QVERIFY(!vdm.hasScreenDesktopReport(oldId));
    QCOMPARE(vdm.currentDesktopForScreen(newId), 4);
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.last().at(0).toString(), newId);
    vdm.seedScreenDesktop(oldId, 2);
    QVERIFY(!vdm.hasScreenDesktopReport(oldId));
    QCOMPARE(seeded.count(), 1);
    QCOMPARE(seeded.last().at(0).toString(), newId);

    // The effect has re-spelled the output: the old spelling is no alias any
    // more.
    vdm.updateScreenDesktop(newId, 5);
    QCOMPARE(vdm.liveScreenIdFor(oldId), oldId);

    // A rename back makes the old spelling live again, and the new one its
    // alias, with nothing left aliased to a live id.
    vdm.renameScreen(newId, oldId);
    QCOMPARE(vdm.liveScreenIdFor(newId), oldId);
    QCOMPARE(vdm.liveScreenIdFor(oldId), oldId);
    // Gone with the output it named.
    vdm.removeScreenDesktop(oldId);
    QCOMPARE(vdm.liveScreenIdFor(newId), newId);
}

void TestVirtualDesktopManager::commit_refusesListsKWinCannotSend()
{
    ListDriver vdm;
    QVERIFY(vdm.commit(ids("abc")));

    QSignalSpy listChanged(&vdm, &VirtualDesktopManager::desktopListChanged);
    QSignalSpy desktopsChanged(&vdm, &VirtualDesktopManager::desktopsChanged);
    QVERIFY(!vdm.commit({}));
    QVERIFY(!vdm.commit({QStringLiteral("a"), QString(), QStringLiteral("c")}));
    // A repeated id would index-align two positions to one desktop.
    QVERIFY(!vdm.commit({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("a")}));

    QCOMPARE(vdm.desktopIds(), ids("abc"));
    QCOMPARE(listChanged.count(), 0);
    QCOMPARE(desktopsChanged.count(), 0);
}

void TestVirtualDesktopManager::commit_ordersByPosition()
{
    ListDriver vdm;
    QSignalSpy listChanged(&vdm, &VirtualDesktopManager::desktopListChanged);
    QVERIFY(vdm.commitEntries({ListDriver::DesktopEntry{2, QStringLiteral("c"), QString()},
                               ListDriver::DesktopEntry{0, QStringLiteral("a"), QString()},
                               ListDriver::DesktopEntry{1, QStringLiteral("b"), QString()}}));
    QCOMPARE(vdm.desktopIds(), ids("abc"));
    QCOMPARE(vdm.desktopIdAt(3), QStringLiteral("c"));
    QCOMPARE(vdm.desktopIndexOf(QStringLiteral("b")), 2);
    QCOMPARE(listChanged.count(), 1);
}

void TestVirtualDesktopManager::commit_movesScreenDesktopsBeforeAnnouncingTheList()
{
    ListDriver vdm;
    QVERIFY(vdm.commit(ids("abcd")));
    vdm.updateScreenDesktop(QStringLiteral("DP-1"), 4); // on d
    vdm.updateScreenDesktop(QStringLiteral("DP-2"), 2); // on b, about to go

    // A consumer of the settled list reads each screen's desktop against it
    // (F111): d is 3 once b is gone, and DP-2 went to b's successor, c, now 2.
    int dp1AtListChanged = -1;
    int dp2AtListChanged = -1;
    connect(&vdm, &VirtualDesktopManager::desktopListChanged, this, [&]() {
        dp1AtListChanged = vdm.currentDesktopForScreen(QStringLiteral("DP-1"));
        dp2AtListChanged = vdm.currentDesktopForScreen(QStringLiteral("DP-2"));
    });
    QList<DesktopRemap> remaps;
    connect(&vdm, &VirtualDesktopManager::desktopsRenumbered, this, [&](const DesktopRemap& remap) {
        remaps.append(remap);
    });
    QSignalSpy screenChanged(&vdm, &VirtualDesktopManager::screenDesktopChanged);

    QVERIFY(vdm.commit(ids("acd")));

    QCOMPARE(dp1AtListChanged, 3);
    QCOMPARE(dp2AtListChanged, 2);
    // Moved silently: a renumber is not a switch.
    QCOMPARE(screenChanged.count(), 0);
    QCOMPARE(remaps.size(), 1);
    QCOMPARE(remaps.first().map(2), 0);
    QCOMPARE(remaps.first().map(4), 3);
}

void TestVirtualDesktopManager::commit_reportsRenamesOfSurvivingDesktops()
{
    ListDriver vdm;
    QVERIFY(vdm.commit(ids("ab"), QString(), {QStringLiteral("Mail"), QStringLiteral("Web")}));

    QSignalSpy renamed(&vdm, &VirtualDesktopManager::desktopRenamed);
    QSignalSpy listChanged(&vdm, &VirtualDesktopManager::desktopListChanged);
    QSignalSpy desktopsChanged(&vdm, &VirtualDesktopManager::desktopsChanged);
    // b renamed, c new: only the rename of a desktop on both sides counts.
    QVERIFY(vdm.commit(ids("abc"), QString(), {QStringLiteral("Mail"), QStringLiteral("Browse"), QStringLiteral("X")}));

    QCOMPARE(renamed.count(), 1);
    QCOMPARE(renamed.first().at(0).toString(), QStringLiteral("b"));
    QCOMPARE(renamed.first().at(1).toString(), QStringLiteral("Browse"));
    QCOMPARE(listChanged.count(), 1);
    QCOMPARE(desktopsChanged.count(), 1);
}

void TestVirtualDesktopManager::removal_commitsAtCountChanged()
{
    ListDriver vdm;
    QVERIFY(vdm.commit(ids("abcd")));

    QSignalSpy changing(&vdm, &VirtualDesktopManager::desktopListChanging);
    QSignalSpy settled(&vdm, &VirtualDesktopManager::desktopListChangeSettled);
    QSignalSpy listChanged(&vdm, &VirtualDesktopManager::desktopListChanged);
    QList<DesktopRemap> remaps;
    connect(&vdm, &VirtualDesktopManager::desktopsRenumbered, this, [&](const DesktopRemap& remap) {
        remaps.append(remap);
    });

    // KWin removing b: each shifted desktop at its new position, the removal,
    // then the count that closes the change.
    vdm.ingestDesktopDataChanged(QStringLiteral("c"), 1, QString());
    vdm.ingestDesktopDataChanged(QStringLiteral("d"), 2, QString());
    vdm.removed(QStringLiteral("b"));
    QCOMPARE(changing.count(), 1); // once for the whole change
    QCOMPARE(listChanged.count(), 0); // nothing committed mid-change
    vdm.count(3);

    // Committed with no list read at all: the renumber reaches the stores
    // before any report KWin sends after the change.
    QCOMPARE(listChanged.count(), 1);
    QCOMPARE(vdm.desktopIds(), ids("acd"));
    QCOMPARE(remaps.size(), 1);
    QCOMPARE(remaps.first().map(2), 0);
    QCOMPARE(remaps.first().map(3), 2);
    QCOMPARE(remaps.first().map(4), 3);
    QCOMPARE(settled.count(), 1);
}

void TestVirtualDesktopManager::insert_commitsAtCountChanged()
{
    ListDriver vdm;
    QVERIFY(vdm.commit(ids("abc")));

    QList<DesktopRemap> remaps;
    connect(&vdm, &VirtualDesktopManager::desktopsRenumbered, this, [&](const DesktopRemap& remap) {
        remaps.append(remap);
    });
    QSignalSpy created(&vdm, &VirtualDesktopManager::kwinDesktopCreated);

    // KWin inserting x at position 1: the displaced desktops first, then the
    // new one (whose own position signal KWin does not send).
    vdm.ingestDesktopDataChanged(QStringLiteral("b"), 2, QString());
    vdm.ingestDesktopDataChanged(QStringLiteral("c"), 3, QString());
    vdm.ingestDesktopCreated(QStringLiteral("x"), 1, QStringLiteral("X"));
    QCOMPARE(created.count(), 1);
    QCOMPARE(vdm.desktopIds(), ids("abc")); // the echo goes out ahead of the commit
    vdm.count(4);

    QCOMPARE(vdm.desktopIds(), ids("axbc"));
    QCOMPARE(remaps.size(), 1);
    QCOMPARE(remaps.first().map(1), 1);
    QCOMPARE(remaps.first().map(2), 3);
    QCOMPARE(remaps.first().map(3), 4);
}

void TestVirtualDesktopManager::rename_startsNoChange()
{
    ListDriver vdm;
    QVERIFY(vdm.commit(ids("ab")));

    QSignalSpy changing(&vdm, &VirtualDesktopManager::desktopListChanging);
    // Same position, new name: nothing is renumbered, so nothing announces a
    // change the window registry would have to mark its records for.
    vdm.ingestDesktopDataChanged(QStringLiteral("b"), 1, QStringLiteral("Web"));
    QCOMPARE(changing.count(), 0);
}

void TestVirtualDesktopManager::secondChange_isAnnouncedAgain()
{
    ListDriver vdm;
    QVERIFY(vdm.commit(ids("abc")));

    QSignalSpy changing(&vdm, &VirtualDesktopManager::desktopListChanging);
    QSignalSpy settled(&vdm, &VirtualDesktopManager::desktopListChangeSettled);

    // Two removals back to back, before any list read could land. A record
    // the compositor pushed between them is in the first one's numbering, so
    // the second must be announced on its own for the registry to move that
    // record again (F466).
    vdm.ingestDesktopDataChanged(QStringLiteral("c"), 1, QString());
    vdm.removed(QStringLiteral("b"));
    vdm.count(2);
    QCOMPARE(changing.count(), 1);
    QCOMPARE(settled.count(), 1);

    vdm.ingestDesktopDataChanged(QStringLiteral("c"), 0, QString());
    vdm.removed(QStringLiteral("a"));
    QCOMPARE(changing.count(), 2);
    vdm.count(1);
    QCOMPARE(settled.count(), 2);
    QCOMPARE(vdm.desktopIds(), ids("c"));
}

void TestVirtualDesktopManager::currentChangedMidChange_waitsForTheCommit()
{
    ListDriver vdm;
    QVERIFY(vdm.commit(ids("abcd"), QStringLiteral("b")));

    QList<int> currents;
    connect(&vdm, &VirtualDesktopManager::currentDesktopChanged, this, [&](int desktop) {
        currents.append(desktop);
    });

    // KWin removing the current desktop b: the shifts, then the active output
    // moved to b's successor c, then the removal. c is position 2 in the new
    // list; resolving it against the old one would publish 3 (F950).
    vdm.ingestDesktopDataChanged(QStringLiteral("c"), 1, QString());
    vdm.ingestDesktopDataChanged(QStringLiteral("d"), 2, QString());
    vdm.current(QStringLiteral("c"));
    vdm.removed(QStringLiteral("b"));
    vdm.count(3);
    QVERIFY(!currents.contains(3));

    // The commit numbered c as 2, the position the current desktop already
    // had: a later switch to d (3) is a real change.
    vdm.current(QStringLiteral("d"));
    QCOMPARE(currents, QList<int>{3});
}

void TestVirtualDesktopManager::malformedCreate_stillEchoesAReadableId()
{
    ListDriver vdm;
    QVERIFY(vdm.commit(ids("ab")));
    QSignalSpy created(&vdm, &VirtualDesktopManager::kwinDesktopCreated);
    const auto signal = []() {
        return QDBusMessage::createSignal(QStringLiteral("/VirtualDesktopManager"),
                                          QStringLiteral("org.kde.KWin.VirtualDesktopManager"),
                                          QStringLiteral("desktopCreated"));
    };

    // The id is there but the desktop data is not: the create still happened.
    // KWin answers the createDesktop call after this signal, and an answer
    // with no echo ahead of it is read as KWin refusing the create at its
    // ceiling.
    QDBusMessage idOnly = signal();
    idOnly << QStringLiteral("x");
    vdm.createdMessage(idOnly);
    QCOMPARE(created.count(), 1);
    QCOMPARE(created.first().at(0).toString(), QStringLiteral("x"));

    // Nothing readable: nothing is echoed.
    vdm.createdMessage(signal());
    QDBusMessage notAnId = signal();
    notAnId << 7;
    vdm.createdMessage(notAnId);
    QDBusMessage emptyId = signal();
    emptyId << QString();
    vdm.createdMessage(emptyId);
    QCOMPARE(created.count(), 1);
}

QTEST_GUILESS_MAIN(TestVirtualDesktopManager)
#include "test_virtualdesktopmanager.moc"
