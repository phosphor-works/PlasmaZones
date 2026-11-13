// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "workspacereconcilerharness.h"

#include <PhosphorWorkspaces/WorkspaceReconciler.h>

#include <QSignalSpy>
#include <QTest>

using PhosphorWorkspaces::NamedWorkspace;
using PhosphorWorkspaces::WorkspaceEntry;
using PhosphorWorkspaces::WorkspaceMap;
using PhosphorWorkspaces::WorkspaceReconciler;
using WorkspaceTest::adoptTwoScreens;
using WorkspaceTest::FastDebounceMs;
using WorkspaceTest::FastLedgerMs;
using WorkspaceTest::id;
using WorkspaceTest::sliceIds;

namespace {
/// Long enough for any number of debounces to have fired.
constexpr int DebounceSettleMs = FastDebounceMs * 6;
/// A generous bound for QTRY on something a few expiries away.
constexpr int ExpiriesMs = FastLedgerMs * 12;

/// A desktop someone adds on B (named, at the end of KWin's list, where System
/// Settings puts it) with a window already on it: it moves the settled id
/// list, which ends a cap-probe episode, without answering any create of ours
/// and without being surplus the destroy sweep would reap.
void churnOnB(WorkspaceReconciler& rec, QStringList& ids, int n)
{
    rec.setFocusedScreen(QStringLiteral("B"));
    rec.onPopulationChanged(id(n), 1);
    rec.onKwinDesktopCreatedAt(id(n), static_cast<int>(ids.size()), QStringLiteral("added %1").arg(n));
    ids.append(id(n));
    rec.onDesktopListSettled(ids);
}

/// Count the creates the spy recorded for a named workspace (or for a dynamic
/// desktop, which goes to KWin under its positional "Desktop <n>" label).
int creates(const QSignalSpy& spy, bool namedOnly)
{
    int count = 0;
    for (const QList<QVariant>& args : spy) {
        const bool positional = args.at(1).toString().startsWith(QLatin1String("Desktop "));
        if (positional != namedOnly) {
            ++count;
        }
    }
    return count;
}

/// Answer nothing and let every unanswered create expire, with the id list
/// changing between expiries so the cap probe never concludes and only the
/// refusal budgets can stop the re-drives.
void starveCreates(WorkspaceReconciler& rec, QStringList& ids, int& nextId, int passes)
{
    QSignalSpy resyncSpy(&rec, &WorkspaceReconciler::resyncRequested);
    for (int pass = 1; pass <= passes; ++pass) {
        QTRY_COMPARE_WITH_TIMEOUT(resyncSpy.count(), pass, ExpiriesMs);
        churnOnB(rec, ids, nextId++);
    }
}

NamedWorkspace named(const QString& declId, const QString& name)
{
    NamedWorkspace decl;
    decl.id = declId;
    decl.name = name;
    return decl;
}
}

/// The time-driven half of the reconciler: the destroy debounce, ledger expiry,
/// the live cap probe and the refusal budgets. Separated from
/// test_workspace_reconciler because these cases wait on timers (shortened
/// through setTimeouts), and because these are the paths where being right on
/// the SECOND round is the whole point.
class TestWorkspaceLedger : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void destroyOnEmpty_debouncedAndRechecked();
    void destroyOnEmpty_shownDesktopKeptUntilTheScreenLeavesIt();
    void destroyOnEmpty_neverTrailingOrLast();
    void destroyOnEmpty_displacedEntryRidesASlowReplugHome();
    void cap_suspendsTrailingEmptyAndHintsOncePerEpisode();
    void capProbe_oneExpiryRetriesAndKeepsCreating();
    void capProbe_learnsFromRepeatedExpiryAndSelfHeals();
    void capProbe_learnedCapStillGivesAnEmptyScreenADonor();
    void capProbe_successClearsTheEvidence();
    void removalRefusals_stopAtTheBudgetAndResumeOnPopulation();
    void createRefusals_trailingRepairStopsAtTheBudget();
    void createRefusals_emptySliceTakesADonorOnceSpent();
    void createRefusals_budgetComesBackWhenKWinAnswers();
    void createRefusals_namedCreatesSpendTheirOwnBudget();
    void namedCreate_expiredIsReDrivenWithinItsBudget();
    void namePush_budgetStopsTheRepeats();
    void setCurrent_expiryReevaluatesWithinABudget();
    void expiry_resyncsAnnouncesAndFreesTheScreen();
    void removalRace_signalledOnceForADoomedDesktop();
    void foreign_pausedDuringRemovalThenReevaluatedAtSettle();
    void adoption_keepsKnownPopulations();
    void setCurrent_untranslatableTargetNeverReachesTheLedger();
    void capProbe_stalledCreatesNeverTeachTheCap();
    void named_revertedEmptyWorkspaceIsReaped();
    void removal_lostEchoIsClosedByTheSettle();
    void expiry_ticksThatRetireNothingRaiseNothing();
    void namedCreate_reDriveReadsKWinsNamesAsTheyAreNow();
    void setCurrent_expiredUserSwitchesSpendNoCorrectionBudget();
};

void TestWorkspaceLedger::destroyOnEmpty_debouncedAndRechecked()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec)); // A shows d1
    // Occupy A's trailing empty, realize the new one.
    rec.onPopulationChanged(id(3), 1);
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)});

    QSignalSpy removeSpy(&rec, &WorkspaceReconciler::requestRemoveDesktop);

    // {d3} (now mid-slice, occupied, not shown) empties → debounce, then remove.
    rec.onPopulationChanged(id(3), 0);
    QCOMPARE(removeSpy.count(), 0); // not before the debounce
    QTRY_COMPARE_WITH_TIMEOUT(removeSpy.count(), 1, DebounceSettleMs * 4);
    QCOMPARE(removeSpy.first().first().toString(), id(3));
    rec.onKwinDesktopRemoved(id(3));
    rec.onDesktopListSettled({id(1), id(5), id(2), id(4)});

    // The re-check, on a desktop nobody shows: {d5} is occupied (its own
    // trailing empty {d6} lands), empties, and re-fills inside the debounce.
    // When the timer fires it is NOT empty and is not removed.
    rec.onPopulationChanged(id(5), 1);
    rec.onKwinDesktopCreated(id(6));
    rec.onDesktopListSettled({id(1), id(5), id(6), id(2), id(4)});
    rec.onPopulationChanged(id(5), 0);
    rec.onPopulationChanged(id(5), 2);
    QTest::qWait(DebounceSettleMs);
    QCOMPARE(removeSpy.count(), 1);
}

void TestWorkspaceLedger::destroyOnEmpty_shownDesktopKeptUntilTheScreenLeavesIt()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    rec.onPopulationChanged(id(3), 1);
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)}); // A: {d1, d3, d5}
    rec.onScreenDesktopReport(QStringLiteral("A"), 2); // A shows d3

    QSignalSpy removeSpy(&rec, &WorkspaceReconciler::requestRemoveDesktop);
    // The last window leaves the desktop the user is looking at. It stays,
    // the way niri keeps the active workspace: removing it would yank the
    // screen to a neighbour under the user.
    rec.onPopulationChanged(id(3), 0);
    QTest::qWait(DebounceSettleMs);
    QCOMPARE(removeSpy.count(), 0);

    // The moment the screen moves on, it is surplus like any other.
    rec.onScreenDesktopReport(QStringLiteral("A"), 1);
    QTRY_COMPARE_WITH_TIMEOUT(removeSpy.count(), 1, DebounceSettleMs * 4);
    QCOMPARE(removeSpy.first().first().toString(), id(3));
}

void TestWorkspaceLedger::destroyOnEmpty_neverTrailingOrLast()
{
    QSignalSpy* removeSpy = nullptr;
    {
        // The trailing empty: occupied (its replacement requested and not
        // answered yet, so it is still the slice's last entry), then emptied.
        // Emptying it is not surplus, it IS the invariant.
        WorkspaceReconciler rec;
        rec.setTimeouts(WorkspaceReconciler::LedgerTimeoutMs, FastDebounceMs);
        QVERIFY(adoptTwoScreens(rec)); // A shows d1, not d3
        QSignalSpy spy(&rec, &WorkspaceReconciler::requestRemoveDesktop);
        removeSpy = &spy;
        rec.onPopulationChanged(id(3), 1);
        QVERIFY(rec.hasPendingStructuralOps());
        rec.onPopulationChanged(id(3), 0);
        QTest::qWait(DebounceSettleMs);
        QCOMPARE(removeSpy->count(), 0);
    }
    {
        // A slice's only desktop, on a screen with no reported current (so
        // the shown-desktop rule cannot be what keeps it).
        WorkspaceMap candidate;
        candidate.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
        WorkspaceEntry d1;
        d1.desktopId = id(1);
        candidate.insert(QStringLiteral("A"), 0, d1);
        WorkspaceEntry d2;
        d2.desktopId = id(2);
        candidate.insert(QStringLiteral("B"), 0, d2);
        WorkspaceReconciler rec;
        rec.setTimeouts(WorkspaceReconciler::LedgerTimeoutMs, FastDebounceMs);
        QVERIFY(rec.restoreState(candidate.toJson(1, {}, nullptr, true)));
        QHash<QString, QString> current;
        current.insert(QStringLiteral("A"), id(1));
        QVERIFY(rec.adoptAll({id(1), id(2)}, current));
        QVERIFY(rec.currentDesktopIdOf(QStringLiteral("B")).isEmpty());
        QSignalSpy spy(&rec, &WorkspaceReconciler::requestRemoveDesktop);
        rec.onPopulationChanged(id(2), 1);
        rec.onPopulationChanged(id(2), 0);
        QTest::qWait(DebounceSettleMs);
        QCOMPARE(spy.count(), 0);
        QCOMPARE(rec.map().ownerOf(id(2)), QStringLiteral("B"));
    }
}

void TestWorkspaceLedger::destroyOnEmpty_displacedEntryRidesASlowReplugHome()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec)); // A: {d1(occ), d3}, B: {d2(occ), d4}
    rec.onScreenDesktopReport(QStringLiteral("B"), 4); // B shows d4, not d2
    QSignalSpy removeSpy(&rec, &WorkspaceReconciler::requestRemoveDesktop);

    // d2 empties, arming its destroy check, and B is unplugged before it
    // fires. Fostered on A, both of B's desktops are empty and mid-slice,
    // which is exactly what the sweep reaps, and the check armed BEFORE the
    // unplug fires after it. They are displaced, not surplus.
    rec.onPopulationChanged(id(2), 0);
    rec.onScreenRemoved(QStringLiteral("B"));
    QTest::qWait(DebounceSettleMs * 2);
    QCOMPARE(removeSpy.count(), 0);

    // A replug long after every debounce has had its chance: B gets its
    // workspaces back exactly as it left them.
    rec.onScreenAdded(QStringLiteral("B"));
    QCOMPARE(sliceIds(rec, QStringLiteral("B")), QStringList({id(2), id(4)}));
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(3)}));
}

void TestWorkspaceLedger::cap_suspendsTrailingEmptyAndHintsOncePerEpisode()
{
    WorkspaceReconciler rec;
    rec.setDesktopCap(2);
    rec.onScreenOrderChanged({QStringLiteral("A")});
    QSignalSpy capSpy(&rec, &WorkspaceReconciler::capReached);
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);

    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(rec.adoptAll({id(1), id(2)}, current)); // already at cap of 2

    // Both desktops occupied → trailing-empty append wanted, but capped.
    rec.onPopulationChanged(id(1), 1);
    rec.onPopulationChanged(id(2), 1);
    QCOMPARE(createSpy.count(), 0);
    QCOMPARE(capSpy.count(), 1);
    // Maintenance asks again on the next settle (KWin swapped the two) and is
    // refused again, but the hint is per episode, not per refusal.
    rec.onDesktopListSettled({id(2), id(1)});
    QCOMPARE(createSpy.count(), 0);
    QCOMPARE(capSpy.count(), 1);
}

void TestWorkspaceLedger::capProbe_oneExpiryRetriesAndKeepsCreating()
{
    // One unanswered create is a lost create, not a ceiling. A D-Bus stall or a
    // KWin restart expires a Create exactly the way a cap refusal does, and
    // lowering the ceiling on that evidence suspends every later create for the
    // rest of the session, with no way left for the daemon to disprove it. So
    // the first expiry must re-ask instead.
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    rec.onScreenOrderChanged({QStringLiteral("A")});
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(rec.adoptAll({id(1), id(2)}, current));

    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy capSpy(&rec, &WorkspaceReconciler::capReached);

    rec.onPopulationChanged(id(1), 1);
    rec.onPopulationChanged(id(2), 1);
    QCOMPARE(createSpy.count(), 1);

    // The op expires unanswered and the request is made again.
    QTRY_COMPARE_WITH_TIMEOUT(createSpy.count(), 2, ExpiriesMs);
    QCOMPARE(capSpy.count(), 0);
    QCOMPARE(rec.desktopCap(), WorkspaceReconciler::DefaultDesktopCap);
}

void TestWorkspaceLedger::capProbe_learnsFromRepeatedExpiryAndSelfHeals()
{
    // A genuine refusal, though, answers every attempt the same way: KWin
    // declines createDesktop past its maximum silently, so a run of expiries
    // with the id list unchanged throughout IS the compositor's ceiling.
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    rec.onScreenOrderChanged({QStringLiteral("A")});
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(rec.adoptAll({id(1), id(2)}, current));

    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy capSpy(&rec, &WorkspaceReconciler::capReached);

    rec.onPopulationChanged(id(1), 1);
    rec.onPopulationChanged(id(2), 1);
    QCOMPARE(createSpy.count(), 1);

    QTRY_COMPARE_WITH_TIMEOUT(capSpy.count(), 1, ExpiriesMs);
    QCOMPARE(rec.desktopCap(), 2);
    // Exactly one attempt per expiry, and the retries stop once the ceiling is
    // believed — no re-ask/expire spin.
    QCOMPARE(createSpy.count(), WorkspaceReconciler::CapProbeExpiries);
    QTest::qWait(FastLedgerMs * 3);
    QCOMPARE(createSpy.count(), WorkspaceReconciler::CapProbeExpiries);

    // At the learned cap, further trailing-empty wants are suspended: the next
    // settle runs maintenance, which asks and is refused at the gate.
    const int atCap = createSpy.count();
    rec.onDesktopListSettled({id(2), id(1)});
    QCOMPARE(createSpy.count(), atCap);

    // Self-heal: an external create pushes the count past the learned cap, so
    // the ceiling was not where we thought and appends resume.
    rec.onKwinDesktopCreated(id(3));
    rec.onDesktopListSettled({id(2), id(1), id(3)});
    QCOMPARE(rec.desktopCap(), WorkspaceReconciler::DefaultDesktopCap);
    rec.onPopulationChanged(id(3), 1); // trailing occupied again
    QCOMPARE(createSpy.count(), atCap + 1);
}

void TestWorkspaceLedger::capProbe_learnedCapStillGivesAnEmptyScreenADonor()
{
    // B is part of the world but holds nothing; A holds {d1(occupied),
    // d2(empty)}. B's creates go unanswered until the probe concludes. The
    // pass that learns the cap must still run maintenance, because at the cap
    // the only way B gets a workspace is a donor transfer, and nothing else
    // would run it.
    WorkspaceMap candidate;
    candidate.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    WorkspaceEntry d1;
    d1.desktopId = id(1);
    candidate.insert(QStringLiteral("A"), 0, d1);
    WorkspaceEntry d2;
    d2.desktopId = id(2);
    candidate.insert(QStringLiteral("A"), 1, d2);

    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(rec.restoreState(candidate.toJson(1, {}, nullptr, true)));
    rec.onPopulationChanged(id(1), 1);
    QSignalSpy capSpy(&rec, &WorkspaceReconciler::capReached);
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(rec.adoptAll({id(1), id(2)}, current));
    QCOMPARE(createSpy.count(), 1); // B asks for its first desktop
    QSignalSpy mapSpy(&rec, &WorkspaceReconciler::mapChanged);
    const quint64 generation = rec.generation();

    QTRY_COMPARE_WITH_TIMEOUT(capSpy.count(), 1, ExpiriesMs);
    QCOMPARE(rec.map().ownerOf(id(2)), QStringLiteral("B"));
    // A transfer inside the expiry pass is a map change like any other.
    QVERIFY(rec.generation() > generation);
    QVERIFY(mapSpy.count() >= 1);
}

void TestWorkspaceLedger::capProbe_successClearsTheEvidence()
{
    // The daemon's OWN proof path: a create KWin actually answers says the
    // ceiling is not where the expiries pointed, so the accumulated evidence is
    // dropped and the next lost create starts a fresh episode rather than
    // completing the previous one.
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    rec.onScreenOrderChanged({QStringLiteral("A")});
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(rec.adoptAll({id(1), id(2)}, current));

    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy capSpy(&rec, &WorkspaceReconciler::capReached);

    rec.onPopulationChanged(id(1), 1);
    rec.onPopulationChanged(id(2), 1);
    QTRY_COMPARE_WITH_TIMEOUT(createSpy.count(), 2, ExpiriesMs);

    // KWin answers the retry with an id-only echo. The settled list has not
    // arrived, so the id list is still the one the first expiry was recorded
    // against — the case a count that ignored successes would mislearn on.
    rec.onKwinDesktopCreated(id(3));
    QCOMPARE(rec.map().ownerOf(id(3)), QStringLiteral("A"));

    // Occupy the new trailing empty and lose the next create too.
    rec.onPopulationChanged(id(3), 1);
    QCOMPARE(createSpy.count(), 3);
    QTRY_COMPARE_WITH_TIMEOUT(createSpy.count(), 4, ExpiriesMs);
    // One expiry since the success, so no ceiling is concluded.
    QCOMPARE(capSpy.count(), 0);
}

void TestWorkspaceLedger::removalRefusals_stopAtTheBudgetAndResumeOnPopulation()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec)); // A shows d1
    // Occupy A's trailing empty and realize the next one, so {d3} is a
    // mid-slice desktop the destroy sweep is allowed to reap.
    rec.onPopulationChanged(id(3), 1);
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)});

    QSignalSpy removeSpy(&rec, &WorkspaceReconciler::requestRemoveDesktop);
    QSignalSpy abandonedSpy(&rec, &WorkspaceReconciler::removalAbandoned);
    rec.onPopulationChanged(id(3), 0);

    // KWin never answers. Each expiry re-arms the destroy check once, and the
    // re-arms stop at the budget rather than looping for the session.
    const int budget = WorkspaceReconciler::MaxRemovalRefusals;
    QTRY_COMPARE_WITH_TIMEOUT(abandonedSpy.count(), budget, ExpiriesMs * 2);
    QCOMPARE(removeSpy.count(), budget);
    // Every abandoned removal is announced, so a race snapshot taken for it
    // can be dropped.
    QCOMPARE(abandonedSpy.last().first().toString(), id(3));
    QTest::qWait((FastLedgerMs + FastDebounceMs) * 3);
    QCOMPARE(removeSpy.count(), budget);
    // The desktop is still there, which is the accepted outcome.
    QCOMPARE(rec.map().ownerOf(id(3)), QStringLiteral("A"));

    // Maintenance re-arms the same check on every settle (here an unrelated
    // desktop arriving on B), so the budget must also hold at the moment the
    // check fires, not only where an expiry re-arms it.
    rec.setFocusedScreen(QStringLiteral("B"));
    rec.onPopulationChanged(id(9), 1);
    rec.onKwinDesktopCreated(id(9));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(9), id(4)});
    QTest::qWait(DebounceSettleMs);
    QCOMPARE(removeSpy.count(), budget);

    // A population change is the compositor telling us the situation changed,
    // so the budget comes back and the sweep gets to try again.
    rec.onPopulationChanged(id(3), 1);
    rec.onPopulationChanged(id(3), 0);
    QTRY_COMPARE_WITH_TIMEOUT(removeSpy.count(), budget + 1, DebounceSettleMs * 4);
}

void TestWorkspaceLedger::createRefusals_trailingRepairStopsAtTheBudget()
{
    // The post-expiry re-drive needs a terminator that does NOT depend on the
    // cap probe. The id list changes between expiries here (desktops coming
    // and going on B), which resets the probe before it can conclude, exactly
    // the machine where the probe alone never stops anything. Only A's create
    // budget can, at the requestCreateAt gate.
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    QStringList ids{id(1), id(3), id(2), id(4)};
    int nextId = 10;
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy capSpy(&rec, &WorkspaceReconciler::capReached);

    rec.onPopulationChanged(id(3), 1); // A wants a new trailing empty
    QCOMPARE(createSpy.count(), 1);

    // Expiry 1 spends one of the budget and re-asks (request 2); expiry 2 the
    // same (request 3); expiry 3 spends the last, and the re-drive it runs is
    // refused.
    const int budget = WorkspaceReconciler::MaxCreateRefusals;
    starveCreates(rec, ids, nextId, budget);
    QCOMPARE(createSpy.count(), budget);
    QTest::qWait(FastLedgerMs * 3);
    QCOMPARE(createSpy.count(), budget);
    QCOMPARE(capSpy.count(), 0);
}

void TestWorkspaceLedger::createRefusals_emptySliceTakesADonorOnceSpent()
{
    // A screen holding nothing whose creates KWin never answers. Once its
    // budget is spent no create will come, and the invariant that every
    // screen holds a workspace is kept by a donor transfer instead.
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec)); // A: {d1(occ), d3}
    QStringList ids{id(1), id(3), id(2), id(4)};
    int nextId = 10;
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);

    rec.onScreenAdded(QStringLiteral("C"));
    QCOMPARE(createSpy.count(), 1);
    starveCreates(rec, ids, nextId, WorkspaceReconciler::MaxCreateRefusals);
    // A's spare trailing empty is C's now. (A then asks for a trailing empty
    // of its own, on A's budget, which is the donor's maintenance at work.)
    QCOMPARE(sliceIds(rec, QStringLiteral("C")), QStringList({id(3)}));
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1)}));
}

void TestWorkspaceLedger::createRefusals_budgetComesBackWhenKWinAnswers()
{
    // KWin's ceiling is one count for all screens, so A's spent budget comes
    // back when ANY create lands, not only one credited to A.
    {
        WorkspaceReconciler rec;
        rec.setTimeouts(FastLedgerMs, FastDebounceMs);
        QVERIFY(adoptTwoScreens(rec));
        QStringList ids{id(1), id(3), id(2), id(4)};
        int nextId = 10;
        QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
        rec.onPopulationChanged(id(3), 1);
        starveCreates(rec, ids, nextId, WorkspaceReconciler::MaxCreateRefusals);
        QCOMPARE(createSpy.count(), WorkspaceReconciler::MaxCreateRefusals);

        // B occupies its trailing empty; KWin answers B's create.
        rec.onPopulationChanged(id(4), 1);
        QCOMPARE(createSpy.count(), WorkspaceReconciler::MaxCreateRefusals + 1);
        rec.onKwinDesktopCreated(id(50));
        ids.append(id(50));
        rec.onDesktopListSettled(ids); // maintenance runs, A asks again
        QCOMPARE(createSpy.count(), WorkspaceReconciler::MaxCreateRefusals + 2);
    }
    {
        // Fewer desktops is headroom coming back.
        WorkspaceReconciler rec;
        rec.setTimeouts(FastLedgerMs, FastDebounceMs);
        QVERIFY(adoptTwoScreens(rec));
        QStringList ids{id(1), id(3), id(2), id(4)};
        int nextId = 10;
        QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
        rec.onPopulationChanged(id(3), 1);
        starveCreates(rec, ids, nextId, WorkspaceReconciler::MaxCreateRefusals);
        const QString dropped = ids.takeLast();
        rec.onKwinDesktopRemoved(dropped);
        rec.onDesktopListSettled(ids);
        QCOMPARE(createSpy.count(), WorkspaceReconciler::MaxCreateRefusals + 1);
    }
    {
        // A screen that goes and comes back starts with a fresh budget.
        WorkspaceReconciler rec;
        rec.setTimeouts(FastLedgerMs, FastDebounceMs);
        QVERIFY(adoptTwoScreens(rec));
        QStringList ids{id(1), id(3), id(2), id(4)};
        int nextId = 10;
        QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
        rec.onPopulationChanged(id(4), 1); // B wants a trailing empty
        rec.setFocusedScreen(QStringLiteral("A"));
        QSignalSpy resyncSpy(&rec, &WorkspaceReconciler::resyncRequested);
        for (int pass = 1; pass <= WorkspaceReconciler::MaxCreateRefusals; ++pass) {
            QTRY_COMPARE_WITH_TIMEOUT(resyncSpy.count(), pass, ExpiriesMs);
            rec.onPopulationChanged(id(nextId), 1);
            rec.onKwinDesktopCreatedAt(id(nextId), static_cast<int>(ids.size()), QStringLiteral("added"));
            ids.append(id(nextId++));
            rec.onDesktopListSettled(ids);
        }
        QCOMPARE(createSpy.count(), WorkspaceReconciler::MaxCreateRefusals);
        rec.onScreenRemoved(QStringLiteral("B"));
        rec.onScreenAdded(QStringLiteral("B"));
        QCOMPARE(createSpy.count(), WorkspaceReconciler::MaxCreateRefusals + 1);
    }
}

void TestWorkspaceLedger::createRefusals_namedCreatesSpendTheirOwnBudget()
{
    // A's trailing-empty create and a workspace declared for A expire in the
    // SAME pass. That is one stall for A, not two, and the named create is
    // not A's trailing repair at all: A's budget is spent once per pass, so A
    // still gets every one of its re-drives.
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    QStringList ids{id(1), id(3), id(2), id(4)};
    int nextId = 10;
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    rec.onPopulationChanged(id(3), 1);
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))}, {});
    QCOMPARE(creates(createSpy, false), 1);
    QCOMPARE(creates(createSpy, true), 1);

    starveCreates(rec, ids, nextId, WorkspaceReconciler::MaxCreateRefusals);
    QCOMPARE(creates(createSpy, false), WorkspaceReconciler::MaxCreateRefusals);

    // A's unnamed budget is spent, and a newly declared workspace on A is
    // still created: it spends a budget of its own. And the reverse: named
    // creates expiring on their own never touch A's budget.
    rec.applyNamedWorkspaces(
        {named(QStringLiteral("n1"), QStringLiteral("chat")), named(QStringLiteral("n2"), QStringLiteral("mail"))}, {});
    QCOMPARE(createSpy.last().at(1).toString(), QStringLiteral("mail"));

    WorkspaceReconciler namedOnly;
    namedOnly.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(namedOnly));
    QStringList namedIds{id(1), id(3), id(2), id(4)};
    int namedNext = 10;
    QSignalSpy namedSpy(&namedOnly, &WorkspaceReconciler::requestCreateDesktop);
    // Pinned to A, so every re-drive is A's (the churn moves the focus to B).
    NamedWorkspace pinned = named(QStringLiteral("n1"), QStringLiteral("chat"));
    pinned.outputId = QStringLiteral("A");
    namedOnly.applyNamedWorkspaces({pinned}, {});
    starveCreates(namedOnly, namedIds, namedNext, WorkspaceReconciler::MaxCreateRefusals);
    QCOMPARE(creates(namedSpy, false), 0);
    namedOnly.onPopulationChanged(id(3), 1); // A's trailing repair, on an untouched budget
    QCOMPARE(creates(namedSpy, false), 1);
}

void TestWorkspaceLedger::namedCreate_expiredIsReDrivenWithinItsBudget()
{
    // A named create KWin never answered used to be forgotten: nothing calls
    // applyNamedWorkspaces again until a declaration or the list changes, so
    // the workspace the user declared simply never appeared. The expiry now
    // re-runs the declarations, within the declaration's own budget.
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy resyncSpy(&rec, &WorkspaceReconciler::resyncRequested);
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))}, {});
    QCOMPARE(creates(createSpy, true), 1);

    // Each expiry, with the list moving so the cap probe stays out of it.
    QStringList ids{id(1), id(3), id(2), id(4)};
    int nextId = 10;
    for (int pass = 1; pass <= WorkspaceReconciler::MaxCreateRefusals; ++pass) {
        QTRY_COMPARE_WITH_TIMEOUT(resyncSpy.count(), pass, ExpiriesMs);
        QCOMPARE(creates(createSpy, true), qMin(pass + 1, WorkspaceReconciler::MaxCreateRefusals));
        churnOnB(rec, ids, nextId++);
    }
    QTest::qWait(FastLedgerMs * 3);
    QCOMPARE(creates(createSpy, true), WorkspaceReconciler::MaxCreateRefusals);
}

void TestWorkspaceLedger::namePush_budgetStopsTheRepeats()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    const QList<NamedWorkspace> declarations{named(QStringLiteral("n1"), QStringLiteral("chat"))};
    const QStringList claimed{QStringLiteral("chat"), QStringLiteral("Desktop 2"), QStringLiteral("Desktop 3"),
                              QStringLiteral("Desktop 4")};
    rec.applyNamedWorkspaces(declarations, claimed);
    QSignalSpy renameSpy(&rec, &WorkspaceReconciler::requestSetDesktopName);

    // KWin keeps showing its default for the claimed desktop, push after push.
    // Each push window that lapses is a refusal, and after the budget the
    // desktop keeps the name it has: no push per settle for the session.
    const QStringList behind{QStringLiteral("Desktop 1"), QStringLiteral("Desktop 2"), QStringLiteral("Desktop 3"),
                             QStringLiteral("Desktop 4")};
    for (int round = 0; round < WorkspaceReconciler::MaxNamePushRefusals + 3; ++round) {
        rec.applyNamedWorkspaces(declarations, behind);
        QTest::qWait(FastLedgerMs + 20);
    }
    QCOMPARE(renameSpy.count(), WorkspaceReconciler::MaxNamePushRefusals);
}

void TestWorkspaceLedger::setCurrent_expiryReevaluatesWithinABudget()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    // The controller's half: answer every foreign switch with a snap-back.
    connect(&rec, &WorkspaceReconciler::foreignSwitchDetected, &rec, [&rec](const QString& screenId) {
        rec.snapBack(screenId);
    });
    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);
    QSignalSpy setCurrentSpy(&rec, &WorkspaceReconciler::requestSetCurrent);

    // A lands on B's desktop and the effect cannot perform the correction:
    // no echo ever comes. The resync re-pulls only the global list, so the
    // expiry itself re-evaluates A, a bounded number of times.
    rec.onScreenDesktopReport(QStringLiteral("A"), 3);
    const int budget = WorkspaceReconciler::MaxSetCurrentRefusals;
    QTRY_COMPARE_WITH_TIMEOUT(foreignSpy.count(), budget, ExpiriesMs * 2);
    QTest::qWait(FastLedgerMs * 4);
    QCOMPARE(foreignSpy.count(), budget);
    QCOMPARE(setCurrentSpy.count(), budget);

    // Any report from the screen is new information and restores the budget.
    rec.onScreenDesktopReport(QStringLiteral("A"), 4);
    QCOMPARE(foreignSpy.count(), budget + 1);
}

void TestWorkspaceLedger::expiry_resyncsAnnouncesAndFreesTheScreen()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy resyncSpy(&rec, &WorkspaceReconciler::resyncRequested);
    QSignalSpy quietSpy(&rec, &WorkspaceReconciler::structuralOpsSettled);

    // A switch that never echoes blocks the screen only until it expires.
    QVERIFY(rec.issueSetCurrent(QStringLiteral("A"), id(3)));
    QVERIFY(!rec.issueSetCurrent(QStringLiteral("A"), id(3)));
    QTRY_COMPARE_WITH_TIMEOUT(resyncSpy.count(), 1, ExpiriesMs);
    QVERIFY(rec.issueSetCurrent(QStringLiteral("A"), id(3)));

    // A create that never lands: its expiry closes the structural window, and
    // that is announced so deferred verbs do not wait on a map change.
    rec.onPopulationChanged(id(3), 1);
    QVERIFY(rec.hasPendingStructuralOps());
    QTRY_VERIFY_WITH_TIMEOUT(quietSpy.count() >= 1, ExpiriesMs);
}

void TestWorkspaceLedger::removalRace_signalledOnceForADoomedDesktop()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(WorkspaceReconciler::LedgerTimeoutMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    // Occupy A's trailing empty {d3}, realize the next one, then empty {d3}
    // so the destroy debounce issues its Remove.
    rec.onPopulationChanged(id(3), 1);
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)});
    QSignalSpy removeSpy(&rec, &WorkspaceReconciler::requestRemoveDesktop);
    QSignalSpy raceSpy(&rec, &WorkspaceReconciler::removalRaceDetected);
    rec.onPopulationChanged(id(3), 0);
    QTRY_COMPARE_WITH_TIMEOUT(removeSpy.count(), 1, DebounceSettleMs * 4);

    // A window maps onto {d3} while the Remove is in flight: the race is
    // surfaced with the owner so the controller can re-route afterwards.
    rec.onPopulationChanged(id(3), 1);
    QCOMPARE(raceSpy.count(), 1);
    QCOMPARE(raceSpy.first().at(0).toString(), id(3));
    QCOMPARE(raceSpy.first().at(1).toString(), QStringLiteral("A"));
    // An edge, not a level: more windows on the same doomed desktop stay
    // quiet, since the controller re-snapshots on every emission.
    rec.onPopulationChanged(id(3), 2);
    rec.onPopulationChanged(id(3), 3);
    QCOMPARE(raceSpy.count(), 1);
}

void TestWorkspaceLedger::foreign_pausedDuringRemovalThenReevaluatedAtSettle()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(WorkspaceReconciler::LedgerTimeoutMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    rec.onPopulationChanged(id(3), 1);
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)});
    QSignalSpy removeSpy(&rec, &WorkspaceReconciler::requestRemoveDesktop);
    rec.onPopulationChanged(id(3), 0);
    QTRY_COMPARE_WITH_TIMEOUT(removeSpy.count(), 1, DebounceSettleMs * 4);

    // While the Remove is open, reports must not trigger owner-wins policy
    // (they can read as foreign against the pre-removal numbering). B reports
    // position 3, which is A's d5 right now and position 2 once d3 is gone.
    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);
    rec.onScreenDesktopReport(QStringLiteral("B"), 3);
    QCOMPARE(foreignSpy.count(), 0);

    // The settled list closes the window and re-evaluates every screen: B is
    // genuinely on A's d5, still named correctly after the renumber.
    rec.onKwinDesktopRemoved(id(3));
    QCOMPARE(foreignSpy.count(), 0); // echoed, not yet settled
    rec.onDesktopListSettled({id(1), id(5), id(2), id(4)});
    QCOMPARE(foreignSpy.count(), 1);
    QCOMPARE(foreignSpy.first().at(0).toString(), QStringLiteral("B"));
    QCOMPARE(foreignSpy.first().at(1).toString(), id(5));
    QCOMPARE(foreignSpy.first().at(2).toString(), QStringLiteral("A"));
}

void TestWorkspaceLedger::adoption_keepsKnownPopulations()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));

    // A population report can precede the desktop's adoption (the census is
    // event-driven). Clearing it at settle would read the populated d9 as
    // empty, and the very maintenance that adopts it would reap it.
    rec.onPopulationChanged(id(9), 2);
    QSignalSpy removeSpy(&rec, &WorkspaceReconciler::requestRemoveDesktop);
    rec.onDesktopListSettled({id(1), id(3), id(9), id(2), id(4)});
    QCOMPARE(rec.map().ownerOf(id(9)), QStringLiteral("A"));
    QTest::qWait(DebounceSettleMs);
    QCOMPARE(removeSpy.count(), 0);

    // And the kept count is what makes the later 2→0 edge real.
    rec.onPopulationChanged(id(9), 0);
    QTRY_COMPARE_WITH_TIMEOUT(removeSpy.count(), 1, DebounceSettleMs * 4);
    QCOMPARE(removeSpy.first().first().toString(), id(9));
}

void TestWorkspaceLedger::setCurrent_untranslatableTargetNeverReachesTheLedger()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy switchSpy(&rec, &WorkspaceReconciler::requestSetCurrent);
    QSignalSpy resyncSpy(&rec, &WorkspaceReconciler::resyncRequested);

    // The real shape: KWin echoed a create, so the map owns {d9}, but the
    // settled list does not have it yet and the controller could not turn it
    // into a desktop number.
    rec.onPopulationChanged(id(3), 1);
    rec.onKwinDesktopCreated(id(9));
    QCOMPARE(rec.map().ownerOf(id(9)), QStringLiteral("A"));
    QVERIFY(!rec.issueSetCurrent(QStringLiteral("A"), id(9)));
    QCOMPARE(switchSpy.count(), 0);

    // The point of refusing BEFORE the ledger: nothing is left to expire. The
    // settle makes d9 live (so a ledgered entry for it would have stayed open
    // rather than been retired), and a full timeout passes with no resync.
    rec.onDesktopListSettled({id(1), id(3), id(9), id(2), id(4)});
    QTest::qWait(FastLedgerMs * 3);
    QCOMPARE(resyncSpy.count(), 0);

    // And the next switch for the screen goes straight through.
    QVERIFY(rec.issueSetCurrent(QStringLiteral("A"), id(9)));
    QCOMPARE(switchSpy.count(), 1);
}

void TestWorkspaceLedger::capProbe_stalledCreatesNeverTeachTheCap()
{
    // Every create times out at the bus (the manager reports Unanswered): a
    // stalled KWin, not a full one. However many of those expire with the id
    // list unchanged, the ceiling stays where it was.
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    rec.onScreenOrderChanged({QStringLiteral("A")});
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(rec.adoptAll({id(1), id(2)}, current));
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy capSpy(&rec, &WorkspaceReconciler::capReached);
    connect(&rec, &WorkspaceReconciler::requestCreateDesktop, &rec, [&rec](uint position, const QString& name) {
        QMetaObject::invokeMethod(
            &rec,
            [&rec, position, name]() {
                rec.onCreateDesktopAnswered(position, name, false);
            },
            Qt::QueuedConnection);
    });

    rec.onPopulationChanged(id(1), 1);
    rec.onPopulationChanged(id(2), 1);
    QTRY_COMPARE_WITH_TIMEOUT(createSpy.count(), WorkspaceReconciler::MaxCreateRefusals, ExpiriesMs);
    QTest::qWait(FastLedgerMs * 3);
    QCOMPARE(capSpy.count(), 0);
    QCOMPARE(rec.desktopCap(), WorkspaceReconciler::DefaultDesktopCap);
}

void TestWorkspaceLedger::named_revertedEmptyWorkspaceIsReaped()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))}, {});
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(5), id(3), id(2), id(4)}); // A: {d1, chat, d3}
    QSignalSpy removeSpy(&rec, &WorkspaceReconciler::requestRemoveDesktop);
    QTest::qWait(DebounceSettleMs);
    QCOMPARE(removeSpy.count(), 0); // named: exempt while declared

    // The declaration goes away. The empty mid-slice desktop is an ordinary
    // dynamic one now, and surplus.
    rec.applyNamedWorkspaces({}, {});
    QTRY_COMPARE_WITH_TIMEOUT(removeSpy.count(), 1, DebounceSettleMs * 4);
    QCOMPARE(removeSpy.first().first().toString(), id(5));
}

void TestWorkspaceLedger::removal_lostEchoIsClosedByTheSettle()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    rec.onPopulationChanged(id(3), 1);
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)});
    QSignalSpy removeSpy(&rec, &WorkspaceReconciler::requestRemoveDesktop);
    QSignalSpy resyncSpy(&rec, &WorkspaceReconciler::resyncRequested);
    QSignalSpy abandonedSpy(&rec, &WorkspaceReconciler::removalAbandoned);
    rec.onPopulationChanged(id(3), 0);
    QTRY_COMPARE_WITH_TIMEOUT(removeSpy.count(), 1, DebounceSettleMs * 4);

    // The desktopRemoved echo never arrives, but the list KWin settles on no
    // longer has d3: the removal worked. It must not linger to expire as a
    // refusal with a resync.
    rec.onDesktopListSettled({id(1), id(5), id(2), id(4)});
    QVERIFY(!rec.hasPendingStructuralOps());
    QTest::qWait(FastLedgerMs * 3);
    QCOMPARE(resyncSpy.count(), 0);
    QCOMPARE(abandonedSpy.count(), 0);
}

void TestWorkspaceLedger::expiry_ticksThatRetireNothingRaiseNothing()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);
    QSignalSpy resyncSpy(&rec, &WorkspaceReconciler::resyncRequested);

    // B stands on A's desktop (nobody corrects it here), and an unrelated
    // switch for A keeps the ledger timer ticking several times before it
    // expires. Those ticks retire nothing and must not re-raise B.
    rec.onScreenDesktopReport(QStringLiteral("B"), 1);
    QCOMPARE(foreignSpy.count(), 1);
    QVERIFY(rec.issueSetCurrent(QStringLiteral("A"), id(3)));
    QTest::qWait(FastLedgerMs * 3 / 4);
    QCOMPARE(resyncSpy.count(), 0);
    QCOMPARE(foreignSpy.count(), 1);
}

void TestWorkspaceLedger::namedCreate_reDriveReadsKWinsNamesAsTheyAreNow()
{
    // An expired named create re-runs the declarations. It used to re-run
    // them with KWin's names as the last apply had seen them, so a rename
    // that had landed since read as a fresh rename on KWin's side, and
    // adopting that "rename" put the workspace's old name back.
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    rec.onScreenOrderChanged({QStringLiteral("A")});
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(rec.adoptAll({id(1), id(2)}, current, {}, {QStringLiteral("work"), QStringLiteral("Desktop 2")}));
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("work"))},
                             {QStringLiteral("work"), QStringLiteral("Desktop 2")});
    QCOMPARE(rec.map().entryFor(id(1)).name, QStringLiteral("work"));

    QSignalSpy kwinRenameSpy(&rec, &WorkspaceReconciler::namedWorkspaceRenamedInKWin);
    QSignalSpy resyncSpy(&rec, &WorkspaceReconciler::resyncRequested);
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    // Renamed in settings, and a second workspace declared whose create KWin
    // never answers. KWin's names at this apply still say "work".
    rec.applyNamedWorkspaces(
        {named(QStringLiteral("n1"), QStringLiteral("office")), named(QStringLiteral("n2"), QStringLiteral("mail"))},
        {QStringLiteral("work"), QStringLiteral("Desktop 2")});
    QCOMPARE(createSpy.count(), 1);
    // KWin takes the new name: our own push echoing back.
    rec.onKwinDesktopRenamed(id(1), QStringLiteral("office"));
    QCOMPARE(kwinRenameSpy.count(), 0);

    // The create expires and the declarations are re-run.
    QTRY_COMPARE_WITH_TIMEOUT(resyncSpy.count(), 1, ExpiriesMs);
    QCOMPARE(createSpy.count(), 2);
    QCOMPARE(kwinRenameSpy.count(), 0);
    QCOMPARE(rec.map().entryFor(id(1)).name, QStringLiteral("office"));
}

void TestWorkspaceLedger::setCurrent_expiredUserSwitchesSpendNoCorrectionBudget()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(FastLedgerMs, FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec)); // A owns {d1, d3} and shows d1
    QSignalSpy resyncSpy(&rec, &WorkspaceReconciler::resyncRequested);
    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);

    // User presses the compositor never answers, as many as the correction
    // budget. Each is retried by the user's next press, not by owner-wins.
    for (int press = 1; press <= WorkspaceReconciler::MaxSetCurrentRefusals; ++press) {
        QVERIFY(rec.issueSetCurrentReplacing(QStringLiteral("A"), id(3)));
        QTRY_COMPARE_WITH_TIMEOUT(resyncSpy.count(), press, ExpiriesMs);
    }

    // A pin moves the desktop A shows to B, with no report from A in between
    // to hand a spent budget back. A is still corrected.
    NamedWorkspace pinned = named(QStringLiteral("n1"), QStringLiteral("web"));
    pinned.outputId = QStringLiteral("B");
    rec.applyNamedWorkspaces(
        {pinned},
        {QStringLiteral("web"), QStringLiteral("Desktop 2"), QStringLiteral("Desktop 3"), QStringLiteral("Desktop 4")});
    QCOMPARE(rec.map().ownerOf(id(1)), QStringLiteral("B"));
    QCOMPARE(foreignSpy.count(), 1);
    QCOMPARE(foreignSpy.first().at(0).toString(), QStringLiteral("A"));
}

QTEST_GUILESS_MAIN(TestWorkspaceLedger)
#include "test_workspace_ledger.moc"
