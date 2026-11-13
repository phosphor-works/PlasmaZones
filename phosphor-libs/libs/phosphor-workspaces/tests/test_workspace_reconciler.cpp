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
using WorkspaceTest::id;
using WorkspaceTest::openTwoConcurrentCreates;
using WorkspaceTest::sliceIds;

namespace {
NamedWorkspace named(const QString& declId, const QString& name, const QString& outputId = QString(), int position = -1)
{
    NamedWorkspace decl;
    decl.id = declId;
    decl.name = name;
    decl.outputId = outputId;
    decl.position = position;
    return decl;
}

QString label(int position)
{
    return QStringLiteral("Desktop %1").arg(position);
}

/// KWin's raw names for `count` desktops that all carry their positional
/// default, which is what KWin gives every desktop nobody named.
QStringList positionalNames(int count)
{
    QStringList names;
    for (int i = 1; i <= count; ++i) {
        names.append(label(i));
    }
    return names;
}
}

/// The structural behaviour of the reconciler: adoption, ownership, the verbs,
/// named workspaces and hotplug. Driven with scripted notification sequences
/// (no D-Bus): each case answers the reconciler's requests itself through the
/// created and settled inputs, the way VirtualDesktopManager would. The
/// timing-driven paths (the destroy debounce, ledger expiry, the cap probe and
/// the refusal budgets) live in test_workspace_ledger.
class TestWorkspaceReconciler : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void adoption_currentFirstThenKWinOrder();
    void adoption_unshownDesktopsFollowTheirWindows();
    void adoption_trailingEmptyIsUnnamedAndSettles();
    void adoption_refusesAnEmptyListOrNoScreen();
    void preAdoption_runsNoPolicy();
    void createOnOccupy_appendsNextEmpty();
    void externalCreation_adoptedByFocusedScreen();
    void externalCreation_shownByOneOtherScreenMovesThere();
    void externalRemoval_followedAndRepaired();
    void current_carriedByIdAcrossARenumber();
    void current_removedDesktopMovesToKWinsSuccessor();
    void report_pastTheListResolvedAtTheNextSettle();
    void report_malformedIsIgnored();
    void echo_ledgerSuppressesReactivePolicy();
    void echo_keepsTheStructuralWindowOpenUntilSettle();
    void foreign_everyUnmatchedReportSurfaces();
    void screenRemoved_sliceReassigned();
    void screenRemoved_keepsOpenCreates();
    void screenRemoved_lastScreenParksAndReplugTakesItBack();
    void screenRenamed_reKeysInPlace();
    void verbQueries_sliceScopedNoWrap();
    void issueSetCurrent_singleInFlightPerScreen();
    void issueSetCurrentReplacing_chainsButNeverPreemptsACorrection();
    void snapBack_correctsAndBreaksLoop();
    void setCurrent_refusingAnswerRetiresAndSpendsTheBudget();
    void setCurrent_refusedUserSwitchSpendsNoBudget();
    void reorderCurrentWorkspace_withinSliceAndBeforeTrailingEmpty();
    void transferCurrentWorkspace_reownsAndRepairs();
    void transferCurrentWorkspace_neverTheSoleDesktop();
    void hotplug_homeStampAndMigrateBack();
    void hotplug_daisyChainReturnsToTheOriginalHome();
    void hotplug_fosterShowingAMigratedDesktopIsCorrected();
    void donor_screenAtTheCapTakesASpareAndTheDonorIsCorrected();
    void restore_candidateReconciledAgainstReality();
    void restore_displacedEntryGoesHomeWhenItsScreenIsLive();
    void restore_canonicalizesStampsAndIsRefusedAfterAdoption();
    void named_createdPinnedAndExempt();
    void named_renameInSettingsRenamesTheDesktopInPlace();
    void named_claimByKWinName();
    void named_placeholderNameNeverClaims();
    void named_kwinBehindGetsTheNamePushedOnce();
    void named_kwinSideRenameIsReportedNotOverwritten();
    void named_unnamedRevertsToDynamic();
    void named_duplicateMapNamesCollapseToOne();
    void named_declarationTrimAndDuplicateSkip();
    void named_createEchoFifoMismatchHealedByKWinNames();
    void named_externalUserNamedCreateIsNotTakenForOurs();
    void named_pinTransfersRealizedWorkspaceAndClearsHome();
    void named_pinDeclinedWhenSourceHoldsItsLastDesktop();
    void named_declaredPositionMovesARealizedWorkspace();
    void named_positionsLaidOutOnceInRowOrder();
    void named_positionNotEnforcedOnAFosterScreen();
    void named_explicitPositionStaysBeforeTrailingEmpty();
    void named_verbMoveIsReported();
    void labels_stalePositionalLabelsRefreshedUserNamesKept();
    void settled_emptyListIsIgnored();
    void screenAdded_freshScreenGetsItsOwnDesktop();
    void screenAdded_bothHotplugSignalOrdersGetADesktop();
    void create_secondPendingSurvivesTheFirstsSettle();
    void create_settledBeforeEchoLandsOnRequestingScreen();
    void create_settledMatchesByPositionNotRequestOrder();
    void create_threeConcurrentSettleLandOnTheirOwnScreens();
    void create_positionalEchoPicksTheRightCreate();
    void create_landsAtItsIntentNotAFrozenSlot();
    void create_capGateCountsEchoedCreates();
    void answers_refusedCreateTeachesTheCapAtOnce();
    void answers_forALandedCreateChangeNothing();
    void answers_refusedRemoveIsAbandoned();
    void kwinRenameFeed_reportsUserNamesAndClosesOurPushes();
    void seed_isRecordedButNeverTakenForAnEcho();
    void issueSetCurrentReplacing_anEarlierAnswerNamingTheTargetIsNotItsOwn();
    void screenRenamed_recordsAlreadyUnderTheNewIdWin();
    void adoption_refreshesStalePositionalLabels();
};

void TestWorkspaceReconciler::adoption_currentFirstThenKWinOrder()
{
    WorkspaceReconciler rec;
    rec.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B")});

    // Four desktops; A shows d2, B shows d4. d1 precedes every owned desktop,
    // so the leading run goes to the first screen; d3 follows d2, so A.
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(2));
    current.insert(QStringLiteral("B"), id(4));
    QVERIFY(rec.adoptAll({id(1), id(2), id(3), id(4)}, current));
    QVERIFY(rec.isAdopted());

    // And in KWin's order within the slice: the shown d2 was placed first, so
    // inserting each later id before the trailing empty would have read d2,
    // d1, d3.
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(2), id(3)}));
    QCOMPARE(sliceIds(rec, QStringLiteral("B")), QStringList({id(4)}));
}

void TestWorkspaceReconciler::adoption_unshownDesktopsFollowTheirWindows()
{
    WorkspaceReconciler rec;
    rec.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B")});

    // KWin's global mode before the first adoption: both outputs show d1. The
    // windows on d2 and d4 are on B and those on d3 on A, so KWin order alone
    // (every desktop after d1 to A) would send B's windows across on the
    // owner-wins reunion. d5's preference names no screen in the order and
    // falls back to KWin order (it follows d4, so B).
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    current.insert(QStringLiteral("B"), id(1));
    QHash<QString, QString> preferred;
    preferred.insert(id(2), QStringLiteral("B"));
    preferred.insert(id(3), QStringLiteral("A"));
    preferred.insert(id(4), QStringLiteral("B"));
    preferred.insert(id(5), QStringLiteral("C"));
    QVERIFY(rec.adoptAll({id(1), id(2), id(3), id(4), id(5)}, current, preferred));

    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(3)}));
    QCOMPARE(sliceIds(rec, QStringLiteral("B")), QStringList({id(2), id(4), id(5)}));

    // The screen showing a desktop still wins over a preference for it.
    WorkspaceReconciler shown;
    shown.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B")});
    QHash<QString, QString> shownCurrent;
    shownCurrent.insert(QStringLiteral("A"), id(1));
    shownCurrent.insert(QStringLiteral("B"), id(2));
    QHash<QString, QString> wrong;
    wrong.insert(id(1), QStringLiteral("B"));
    QVERIFY(shown.adoptAll({id(1), id(2)}, shownCurrent, wrong));
    QCOMPARE(shown.map().ownerOf(id(1)), QStringLiteral("A"));
}

void TestWorkspaceReconciler::adoption_trailingEmptyIsUnnamedAndSettles()
{
    WorkspaceReconciler rec;
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QVERIFY(adoptTwoScreens(rec));

    // Each slice ends in the desktop KWin created for it, unnamed: the
    // trailing-empty invariant needs a DYNAMIC desktop there, since a named
    // one is destroy-exempt and could never play the role.
    QCOMPARE(rec.map().slice(QStringLiteral("A")).last().desktopId, id(3));
    QCOMPARE(rec.map().slice(QStringLiteral("B")).last().desktopId, id(4));
    QVERIFY(rec.map().slice(QStringLiteral("A")).last().name.isEmpty());
    QVERIFY(rec.map().slice(QStringLiteral("B")).last().name.isEmpty());

    // And it SETTLES: a later reply that is not the same list (KWin swapped
    // B's two desktops) runs maintenance again and asks for nothing more.
    QCOMPARE(createSpy.count(), 2);
    rec.onDesktopListSettled({id(1), id(3), id(4), id(2)});
    QCOMPARE(createSpy.count(), 2);
}

void TestWorkspaceReconciler::adoption_refusesAnEmptyListOrNoScreen()
{
    // A restored candidate must survive a failed read: an empty id list would
    // have repairAgainst drop every slice.
    WorkspaceMap candidate;
    candidate.setScreenOrder({QStringLiteral("A")});
    WorkspaceEntry chat;
    chat.desktopId = id(1);
    chat.name = QStringLiteral("chat");
    candidate.insert(QStringLiteral("A"), 0, chat);

    WorkspaceReconciler rec;
    QVERIFY(rec.restoreState(candidate.toJson(1, {}, nullptr, true)));
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(!rec.adoptAll({}, current));
    QVERIFY(!rec.isAdopted());
    QCOMPARE(rec.map().entryFor(id(1)).name, QStringLiteral("chat"));

    // No screen to adopt onto is the same kind of incomplete read.
    WorkspaceReconciler empty;
    QVERIFY(!empty.adoptAll({id(1)}, {}));
    QVERIFY(!empty.isAdopted());

    // The retry with a real read goes through.
    QVERIFY(rec.adoptAll({id(1)}, current));
    QVERIFY(rec.isAdopted());
}

void TestWorkspaceReconciler::preAdoption_runsNoPolicy()
{
    // The restore path: a stored screen C is not connected, so its slice is
    // fostered before adoption. Nothing may act on the candidate yet, so no
    // create for the screen it lands on, no reap, no correction.
    WorkspaceMap candidate;
    candidate.setScreenOrder({QStringLiteral("A"), QStringLiteral("C")});
    WorkspaceEntry d1;
    d1.desktopId = id(1);
    candidate.insert(QStringLiteral("A"), 0, d1);
    WorkspaceEntry d5;
    d5.desktopId = id(5);
    candidate.insert(QStringLiteral("C"), 0, d5);

    WorkspaceReconciler rec;
    QVERIFY(rec.restoreState(candidate.toJson(1, {}, nullptr, true)));
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);

    rec.onPopulationChanged(id(1), 1); // A's only desktop occupied
    rec.onScreenRemoved(QStringLiteral("C"));
    QCOMPARE(createSpy.count(), 0);
    QCOMPARE(rec.map().ownerOf(id(5)), QStringLiteral("A"));
    QCOMPARE(rec.map().entryFor(id(5)).homeScreenId, QStringLiteral("C"));
    rec.onScreenDesktopReport(QStringLiteral("A"), 1);
    QCOMPARE(foreignSpy.count(), 0);

    // Nor a donor transfer: with the cap at the candidate's size, a screen
    // joining with nothing would take A's spare empty desktop if maintenance
    // ran, and that is a move nobody has reconciled.
    WorkspaceReconciler capped;
    capped.setDesktopCap(2);
    QVERIFY(capped.restoreState(candidate.toJson(1, {}, nullptr, true)));
    capped.onScreenRemoved(QStringLiteral("C")); // A: {d1, d5}, d5 empty
    capped.onScreenAdded(QStringLiteral("B"));
    QVERIFY(!capped.map().hasScreen(QStringLiteral("B")));
    QCOMPARE(capped.map().ownerOf(id(5)), QStringLiteral("A"));
}

void TestWorkspaceReconciler::createOnOccupy_appendsNextEmpty()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);

    // A window lands on A's trailing empty {d3} → a new empty is appended.
    rec.onPopulationChanged(id(3), 1);
    QCOMPARE(createSpy.count(), 1);
    // Right after d3, which KWin has at 0-based position 1.
    QCOMPARE(createSpy.first().first().toUInt(), 2u);

    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)});
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(3), id(5)}));
}

void TestWorkspaceReconciler::externalCreation_adoptedByFocusedScreen()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.setFocusedScreen(QStringLiteral("B"));

    // Unmatched desktopCreated (Pager/System Settings): adopt onto B, before
    // its trailing empty.
    rec.onKwinDesktopCreated(id(9));
    QCOMPARE(rec.map().ownerOf(id(9)), QStringLiteral("B"));
    QCOMPARE(sliceIds(rec, QStringLiteral("B")), QStringList({id(2), id(9), id(4)}));

    // The pull provider outranks the pushed fallback, which is what lets the
    // daemon answer "focused" at the moment of the adoption.
    rec.setFocusedScreenProvider([]() {
        return QStringLiteral("A");
    });
    rec.onKwinDesktopCreated(id(8));
    QCOMPARE(rec.map().ownerOf(id(8)), QStringLiteral("A"));
    // A provider answering a screen the map does not know falls back.
    rec.setFocusedScreenProvider([]() {
        return QStringLiteral("Z");
    });
    rec.onKwinDesktopCreated(id(7));
    QCOMPARE(rec.map().ownerOf(id(7)), QStringLiteral("B"));
}

void TestWorkspaceReconciler::externalCreation_shownByOneOtherScreenMovesThere()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // focus A
    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);

    // A desktop made on B (Pager on B's panel) is adopted onto the focused A,
    // then B reports showing it. It is B's: snapping B away from the desktop
    // it was just created on would undo the user.
    rec.onKwinDesktopCreated(id(9));
    QCOMPARE(rec.map().ownerOf(id(9)), QStringLiteral("A"));
    rec.onDesktopListSettled({id(1), id(3), id(2), id(4), id(9)});
    rec.onScreenDesktopReport(QStringLiteral("B"), 5);
    QCOMPARE(rec.map().ownerOf(id(9)), QStringLiteral("B"));
    QCOMPARE(foreignSpy.count(), 0);

    // The other order: B's report arrives in the new numbering before the
    // list that defines it, and the settle hands the desktop over.
    WorkspaceReconciler rec2;
    QVERIFY(adoptTwoScreens(rec2));
    QSignalSpy foreignSpy2(&rec2, &WorkspaceReconciler::foreignSwitchDetected);
    rec2.onKwinDesktopCreated(id(9));
    rec2.onScreenDesktopReport(QStringLiteral("B"), 5);
    rec2.onDesktopListSettled({id(1), id(3), id(2), id(4), id(9)});
    QCOMPARE(rec2.map().ownerOf(id(9)), QStringLiteral("B"));
    QCOMPARE(foreignSpy2.count(), 0);
}

void TestWorkspaceReconciler::externalRemoval_followedAndRepaired()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);

    // External removal of B's trailing empty {d4}: follow the fact, then
    // maintenance restores B's trailing empty.
    rec.onKwinDesktopRemoved(id(4));
    QVERIFY(rec.map().ownerOf(id(4)).isEmpty());
    rec.onDesktopListSettled({id(1), id(3), id(2)});
    QCOMPARE(createSpy.count(), 1);
    // Right after d2, KWin's 0-based position 2.
    QCOMPARE(createSpy.first().first().toUInt(), 3u);
}

void TestWorkspaceReconciler::current_carriedByIdAcrossARenumber()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.onScreenDesktopReport(QStringLiteral("A"), 2); // d3
    rec.onScreenDesktopReport(QStringLiteral("B"), 4); // d4

    // d1 goes away (B's report for it never comes, the numbers just shift).
    // Every survivor slides down one, and the screens must still name the
    // desktops they show, not whatever slid into their old number.
    rec.onKwinDesktopRemoved(id(1));
    rec.onDesktopListSettled({id(3), id(2), id(4)});
    QCOMPARE(rec.currentDesktopIdOf(QStringLiteral("A")), id(3));
    QCOMPARE(rec.currentDesktopIdOf(QStringLiteral("B")), id(4));
    QCOMPARE(rec.currentIndexByScreen().value(QStringLiteral("A")), 1);
    QCOMPARE(rec.currentIndexByScreen().value(QStringLiteral("B")), 3);
}

void TestWorkspaceReconciler::current_removedDesktopMovesToKWinsSuccessor()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // A shows d1, B shows d2

    // B's shown desktop is removed. KWin moves the output to the desktop that
    // takes its number (d4), and reports nothing for B, since B's number did
    // not change. Forgetting B's current here left every verb on B dead.
    rec.onKwinDesktopRemoved(id(2));
    rec.onDesktopListSettled({id(1), id(3), id(4)});
    QCOMPARE(rec.currentDesktopIdOf(QStringLiteral("B")), id(4));

    // The last desktop has no next one, so the output moves back to the one
    // before it.
    rec.onScreenDesktopReport(QStringLiteral("B"), 3); // d4
    rec.onKwinDesktopRemoved(id(4));
    rec.onDesktopListSettled({id(1), id(3)});
    QCOMPARE(rec.currentDesktopIdOf(QStringLiteral("B")), id(3));
}

void TestWorkspaceReconciler::report_pastTheListResolvedAtTheNextSettle()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));

    // B reports desktop 5 while the settled list has four: the number belongs
    // to a list that has not arrived. It is neither dropped nor read against
    // the old list, and the settle that brings desktop 5 resolves it.
    rec.onKwinDesktopCreated(id(9));
    rec.onScreenDesktopReport(QStringLiteral("B"), 5);
    QVERIFY(rec.currentDesktopIdOf(QStringLiteral("B")).isEmpty());
    rec.onDesktopListSettled({id(1), id(3), id(2), id(4), id(9)});
    QCOMPARE(rec.currentDesktopIdOf(QStringLiteral("B")), id(9));
}

void TestWorkspaceReconciler::report_malformedIsIgnored()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.onScreenDesktopReport(QStringLiteral("A"), 2); // d3

    // A report with no screen, or a desktop below 1, is wire garbage: it must
    // neither record anything nor disturb what the screen already shows.
    QVERIFY(!rec.onScreenDesktopReport(QString(), 1));
    QVERIFY(!rec.onScreenDesktopReport(QStringLiteral("A"), 0));
    QVERIFY(!rec.onScreenDesktopReport(QStringLiteral("A"), -3));
    QCOMPARE(rec.currentDesktopIdOf(QStringLiteral("A")), id(3));
    QVERIFY(rec.currentDesktopIdOf(QString()).isEmpty());
}

void TestWorkspaceReconciler::echo_ledgerSuppressesReactivePolicy()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);

    // Our own create (from occupying the trailing empty) echoes back as
    // desktopCreated — matched, so NO adoption-by-focused-screen runs (it
    // realizes A's planned slice entry instead, even with focus on B).
    rec.setFocusedScreen(QStringLiteral("B"));
    rec.onPopulationChanged(id(3), 1);
    QCOMPARE(createSpy.count(), 1);
    rec.onKwinDesktopCreated(id(7));
    QCOMPARE(rec.map().ownerOf(id(7)), QStringLiteral("A"));
    rec.onDesktopListSettled({id(1), id(3), id(7), id(2), id(4)});

    // A switch WE asked for onto a desktop another screen owns: its echo is
    // matched and runs no owner-wins policy, where the same report unmatched
    // would be a foreign switch.
    QVERIFY(rec.issueSetCurrent(QStringLiteral("A"), id(2)));
    QVERIFY(rec.onScreenDesktopReport(QStringLiteral("A"), 4)); // d2
    QCOMPARE(foreignSpy.count(), 0);
}

void TestWorkspaceReconciler::echo_keepsTheStructuralWindowOpenUntilSettle()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy quietSpy(&rec, &WorkspaceReconciler::structuralOpsSettled);

    rec.onPopulationChanged(id(3), 1);
    QVERIFY(rec.hasPendingStructuralOps());
    // KWin has created and renumbered, but the list the verbs resolve against
    // is still the old one: deferred verbs must keep waiting.
    rec.onKwinDesktopCreated(id(5));
    QVERIFY(rec.hasPendingStructuralOps());
    QCOMPARE(quietSpy.count(), 0);
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)});
    QVERIFY(!rec.hasPendingStructuralOps());
    QCOMPARE(quietSpy.count(), 1);
    // A late copy of the echo neither reopens anything nor moves the desktop.
    rec.onKwinDesktopCreated(id(5));
    QVERIFY(!rec.hasPendingStructuralOps());
    QCOMPARE(rec.map().ownerOf(id(5)), QStringLiteral("A"));
}

void TestWorkspaceReconciler::foreign_everyUnmatchedReportSurfaces()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // positions: d1=1 d3=2 d2=3 d4=4

    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);
    QSignalSpy setCurrentSpy(&rec, &WorkspaceReconciler::requestSetCurrent);

    // A externally switches onto B's desktop {d2}.
    const bool matched = rec.onScreenDesktopReport(QStringLiteral("A"), 3);
    QVERIFY(!matched);
    QCOMPARE(foreignSpy.count(), 1);
    QCOMPARE(foreignSpy.first().at(0).toString(), QStringLiteral("A"));
    QCOMPARE(foreignSpy.first().at(1).toString(), id(2));
    QCOMPARE(foreignSpy.first().at(2).toString(), QStringLiteral("B"));

    // Detection is per report arrival: with no correction in flight, the next
    // external switch (onto B's other desktop, since the manager never
    // repeats an unchanged value) surfaces as well. What bounds the
    // corrections a sustained foreign switch draws is pinned by
    // snapBack_correctsAndBreaksLoop and
    // setCurrent_refusingAnswerRetiresAndSpendsTheBudget.
    rec.onScreenDesktopReport(QStringLiteral("A"), 4);
    QCOMPARE(foreignSpy.count(), 2);
    QCOMPARE(foreignSpy.last().at(1).toString(), id(4));
    // Detection alone issues nothing: the correction is the caller's call.
    QCOMPARE(setCurrentSpy.count(), 0);
}

void TestWorkspaceReconciler::screenRemoved_sliceReassigned()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // A: {d1,d3}, B: {d2,d4}

    rec.onScreenRemoved(QStringLiteral("B"));
    QVERIFY(!rec.map().hasScreen(QStringLiteral("B")));
    // B's entries joined A before A's trailing empty, in order.
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(2), id(4), id(3)}));
}

void TestWorkspaceReconciler::screenRemoved_keepsOpenCreates()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.onScreenAdded(QStringLiteral("C")); // asks KWin for C's first desktop
    QVERIFY(rec.hasPendingStructuralOps());

    // C goes away before KWin answers. The create is still KWin's to answer,
    // so it stays open: dropping it turned the echo into an unmatched
    // creation free to pair with some other screen's create.
    rec.onScreenRemoved(QStringLiteral("C"));
    QVERIFY(rec.hasPendingStructuralOps());
    rec.onKwinDesktopCreated(id(6));
    // Its planned screen is gone, so it goes to the adoption target.
    QCOMPARE(rec.map().ownerOf(id(6)), QStringLiteral("A"));
    rec.onDesktopListSettled({id(1), id(3), id(2), id(4), id(6)});
    QVERIFY(!rec.hasPendingStructuralOps());
}

void TestWorkspaceReconciler::screenRemoved_lastScreenParksAndReplugTakesItBack()
{
    WorkspaceReconciler rec;
    rec.onScreenOrderChanged({QStringLiteral("A")});
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(rec.adoptAll({id(1), id(2)}, current));
    rec.onPopulationChanged(id(1), 1); // A: {d1(occupied), d2(trailing empty)}
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))}, {QStringLiteral("chat"), label(2)});
    QCOMPARE(rec.map().entryFor(id(1)).name, QStringLiteral("chat"));
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);

    // The only screen goes away: KWin keeps its desktops, and so do we.
    rec.onScreenRemoved(QStringLiteral("A"));
    QVERIFY(rec.map().allDesktopIds().isEmpty());

    // Replug: the desktops come back to it, named one included, rather than
    // the screen asking KWin for a fresh one while both sit unowned.
    rec.onScreenAdded(QStringLiteral("A"));
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(2)}));
    QCOMPARE(rec.map().entryFor(id(1)).name, QStringLiteral("chat"));
    QCOMPARE(rec.map().entryFor(id(1)).namedId, QStringLiteral("n1"));
    QVERIFY(rec.map().entryFor(id(1)).homeScreenId.isEmpty());
    QCOMPARE(createSpy.count(), 0);
}

void TestWorkspaceReconciler::screenRenamed_reKeysInPlace()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy setCurrentSpy(&rec, &WorkspaceReconciler::requestSetCurrent);

    // B's id flips spelling while it stays connected. The slice is the same
    // output's: same order slot, no home stamps, its current desktop known.
    rec.onScreenRenamed(QStringLiteral("B"), QStringLiteral("B/DP-2"));
    QCOMPARE(rec.map().screenOrder(), QStringList({QStringLiteral("A"), QStringLiteral("B/DP-2")}));
    QCOMPARE(sliceIds(rec, QStringLiteral("B/DP-2")), QStringList({id(2), id(4)}));
    QVERIFY(rec.map().entryFor(id(2)).homeScreenId.isEmpty());
    QCOMPARE(rec.currentDesktopIdOf(QStringLiteral("B/DP-2")), id(2));
    QCOMPARE(createSpy.count(), 0);
    QCOMPARE(setCurrentSpy.count(), 0);

    // The case a remove/add pair loses outright: the only screen renamed.
    WorkspaceReconciler single;
    single.onScreenOrderChanged({QStringLiteral("A")});
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(single.adoptAll({id(1)}, current));
    single.onScreenRenamed(QStringLiteral("A"), QStringLiteral("A/HDMI-1"));
    QCOMPARE(single.map().ownerOf(id(1)), QStringLiteral("A/HDMI-1"));
    QCOMPARE(single.currentDesktopIdOf(QStringLiteral("A/HDMI-1")), id(1));
}

void TestWorkspaceReconciler::verbQueries_sliceScopedNoWrap()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // positions: d1=1 d3=2 d2=3 d4=4; A shows d1
    rec.onScreenDesktopReport(QStringLiteral("A"), 1);

    QCOMPARE(rec.currentDesktopIdOf(QStringLiteral("A")), id(1));
    QCOMPARE(rec.desktopIdAtOffset(QStringLiteral("A"), 1), id(3));
    QVERIFY(rec.desktopIdAtOffset(QStringLiteral("A"), -1).isEmpty()); // top edge, no wrap
    QCOMPARE(rec.desktopIdAtOffsetFrom(QStringLiteral("A"), id(3), -1), id(1));
    QVERIFY(rec.desktopIdAtOffsetFrom(QStringLiteral("A"), id(2), 1).isEmpty()); // not A's
    QCOMPARE(rec.desktopIdAtSliceIndex(QStringLiteral("A"), 1), id(3));
    QVERIFY(rec.desktopIdAtSliceIndex(QStringLiteral("A"), 2).isEmpty());
    // A screen showing a FOREIGN desktop resolves no offsets.
    rec.onScreenDesktopReport(QStringLiteral("A"), 3); // {d2}, B's
    QVERIFY(rec.desktopIdAtOffset(QStringLiteral("A"), 1).isEmpty());
}

void TestWorkspaceReconciler::issueSetCurrent_singleInFlightPerScreen()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // A shows d1, B shows d2
    QSignalSpy setCurrentSpy(&rec, &WorkspaceReconciler::requestSetCurrent);

    QVERIFY(rec.issueSetCurrent(QStringLiteral("A"), id(3)));
    // Refused because one is open, not because of where A already is: the
    // target is neither A's current nor the open one's.
    QVERIFY(!rec.issueSetCurrent(QStringLiteral("A"), id(4)));
    QVERIFY(rec.issueSetCurrent(QStringLiteral("B"), id(4))); // other screens unaffected
    QCOMPARE(setCurrentSpy.count(), 2);
    QCOMPARE(rec.inFlightSetCurrentTarget(QStringLiteral("A")), id(3));

    // The echo (A now reports d3, position 2) retires the entry and frees the
    // screen for the next switch — which really is ISSUED.
    QVERIFY(rec.onScreenDesktopReport(QStringLiteral("A"), 2));
    QVERIFY(rec.inFlightSetCurrentTarget(QStringLiteral("A")).isEmpty());
    QVERIFY(rec.issueSetCurrent(QStringLiteral("A"), id(1)));
    QCOMPARE(setCurrentSpy.count(), 3);
    QCOMPARE(setCurrentSpy.last().at(0).toString(), QStringLiteral("A"));
    QCOMPARE(setCurrentSpy.last().at(1).toString(), id(1));
}

void TestWorkspaceReconciler::issueSetCurrentReplacing_chainsButNeverPreemptsACorrection()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.onPopulationChanged(id(3), 1);
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)}); // A: {d1, d3, d5}
    QSignalSpy setCurrentSpy(&rec, &WorkspaceReconciler::requestSetCurrent);

    // Two "next workspace" presses before the first echoes. The second
    // chains off the in-flight target and replaces it, instead of computing
    // the same target from the unchanged current and being refused.
    const auto next = [&rec]() {
        const QString inFlight = rec.inFlightSetCurrentTarget(QStringLiteral("A"));
        const QString base = inFlight.isEmpty() ? rec.currentDesktopIdOf(QStringLiteral("A")) : inFlight;
        return rec.issueSetCurrentReplacing(QStringLiteral("A"),
                                            rec.desktopIdAtOffsetFrom(QStringLiteral("A"), base, 1));
    };
    QVERIFY(next());
    QVERIFY(next());
    QCOMPARE(setCurrentSpy.count(), 2);
    QCOMPARE(setCurrentSpy.last().at(1).toString(), id(5));
    QCOMPARE(rec.inFlightSetCurrentTarget(QStringLiteral("A")), id(5));
    // The strict entry point is not loosened by the replaceable one.
    QVERIFY(!rec.issueSetCurrent(QStringLiteral("A"), id(1)));

    // KWin performs the first press and then the second: only the second is
    // still ledgered.
    QVERIFY(!rec.onScreenDesktopReport(QStringLiteral("A"), 2)); // d3
    QVERIFY(rec.onScreenDesktopReport(QStringLiteral("A"), 3)); // d5

    // An owner-wins correction is open: a navigation press must not replace
    // it, or a sustained foreign switch would loop.
    rec.onScreenDesktopReport(QStringLiteral("A"), 4); // B's d2
    QVERIFY(rec.snapBack(QStringLiteral("A")));
    QVERIFY(!rec.issueSetCurrentReplacing(QStringLiteral("A"), id(1)));
    QCOMPARE(rec.inFlightSetCurrentTarget(QStringLiteral("A")), id(5));
}

void TestWorkspaceReconciler::snapBack_correctsAndBreaksLoop()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // positions: d1=1 d3=2 d2=3 d4=4
    // A last showed d3, which is NOT its slice head, so the memo and the
    // fallback disagree and the target says which one snapBack used.
    rec.onScreenDesktopReport(QStringLiteral("A"), 2);

    QSignalSpy setCurrentSpy(&rec, &WorkspaceReconciler::requestSetCurrent);
    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);

    // A lands on B's {d2}: snap-back targets the last owned d3.
    rec.onScreenDesktopReport(QStringLiteral("A"), 3);
    QCOMPARE(foreignSpy.count(), 1);
    QVERIFY(rec.snapBack(QStringLiteral("A")));
    QCOMPARE(setCurrentSpy.count(), 1);
    QCOMPARE(setCurrentSpy.first().at(1).toString(), id(3));

    // Until the effect answers, the correction is the only one: a second
    // snapBack refuses, so a burst of foreign reports cannot stack switches.
    QVERIFY(!rec.snapBack(QStringLiteral("A")));
    QCOMPARE(setCurrentSpy.count(), 1);

    // The correction's echo retires the ledger entry; A is home, snapBack is
    // a no-op (not a fresh correction).
    QVERIFY(rec.onScreenDesktopReport(QStringLiteral("A"), 2));
    QVERIFY(!rec.snapBack(QStringLiteral("A")));
    QCOMPARE(setCurrentSpy.count(), 1);
    // A refused correction is bounded by the refusal budget instead:
    // setCurrent_refusingAnswerRetiresAndSpendsTheBudget.
}

void TestWorkspaceReconciler::setCurrent_refusingAnswerRetiresAndSpendsTheBudget()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // positions: d1=1 d3=2 d2=3 d4=4
    // The controller's half: answer every foreign switch with a snap-back.
    connect(&rec, &WorkspaceReconciler::foreignSwitchDetected, &rec, [&rec](const QString& screenId) {
        rec.snapBack(screenId);
    });
    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);
    QSignalSpy setCurrentSpy(&rec, &WorkspaceReconciler::requestSetCurrent);

    // A lands on B's {d2} and is corrected.
    rec.onScreenDesktopReport(QStringLiteral("A"), 3);
    QCOMPARE(setCurrentSpy.count(), 1);
    const QString home = rec.inFlightSetCurrentTarget(QStringLiteral("A"));
    QCOMPARE(rec.map().ownerOf(home), QStringLiteral("A"));

    // The effect answers every request once, and answers one it could not
    // perform with the desktop the output still shows. That answer retires
    // the entry at once (the screen is not blocked for a ledger timeout) and
    // the screen is evaluated again for what it shows, so the next
    // correction goes out straight away.
    QVERIFY(!rec.onScreenDesktopReport(QStringLiteral("A"), 3));
    QCOMPARE(foreignSpy.count(), 2);
    QCOMPARE(setCurrentSpy.count(), 2);

    // Each refusing answer spends the screen's budget like an expiry does, so
    // a correction the compositor always refuses stops instead of bouncing.
    const int budget = WorkspaceReconciler::MaxSetCurrentRefusals;
    for (int answer = 2; answer <= budget; ++answer) {
        QVERIFY(!rec.onScreenDesktopReport(QStringLiteral("A"), 3));
    }
    QCOMPARE(setCurrentSpy.count(), budget);
    QCOMPARE(foreignSpy.count(), budget);
    QVERIFY(rec.inFlightSetCurrentTarget(QStringLiteral("A")).isEmpty());

    // A report that answers nothing is the screen saying where it is: the
    // budget comes back and the screen is corrected again.
    rec.onScreenDesktopReport(QStringLiteral("A"), 4);
    QCOMPARE(foreignSpy.count(), budget + 1);
    QCOMPARE(setCurrentSpy.count(), budget + 1);
    // This time the switch happens: its report is the echo.
    const QStringList settled{id(1), id(3), id(2), id(4)};
    QVERIFY(settled.contains(home));
    QVERIFY(rec.onScreenDesktopReport(QStringLiteral("A"), static_cast<int>(settled.indexOf(home)) + 1));
    QVERIFY(rec.inFlightSetCurrentTarget(QStringLiteral("A")).isEmpty());
}

void TestWorkspaceReconciler::setCurrent_refusedUserSwitchSpendsNoBudget()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // positions: d1=1 d3=2 d2=3 d4=4; A shows d1
    rec.onScreenDesktopReport(QStringLiteral("A"), 1);
    connect(&rec, &WorkspaceReconciler::foreignSwitchDetected, &rec, [&rec](const QString& screenId) {
        rec.snapBack(screenId);
    });
    QSignalSpy setCurrentSpy(&rec, &WorkspaceReconciler::requestSetCurrent);

    // A user press the compositor refuses (KWin in global desktop mode): the
    // answer is A's unchanged desktop. It retires the press, which is not a
    // correction, so nothing is spent and nothing is corrected.
    const int budget = WorkspaceReconciler::MaxSetCurrentRefusals;
    for (int press = 0; press < budget + 1; ++press) {
        QVERIFY(rec.issueSetCurrentReplacing(QStringLiteral("A"), id(3)));
        QVERIFY(!rec.onScreenDesktopReport(QStringLiteral("A"), 1));
        QVERIFY(rec.inFlightSetCurrentTarget(QStringLiteral("A")).isEmpty());
    }
    QCOMPARE(setCurrentSpy.count(), budget + 1);

    // The correction budget is whole: a foreign landing is still corrected.
    rec.onScreenDesktopReport(QStringLiteral("A"), 3); // B's d2
    QCOMPARE(setCurrentSpy.count(), budget + 2);
}

void TestWorkspaceReconciler::reorderCurrentWorkspace_withinSliceAndBeforeTrailingEmpty()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.onPopulationChanged(id(3), 1);
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)}); // A: {d1, d3, d5(trailing)}
    rec.onScreenDesktopReport(QStringLiteral("A"), 1);
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);

    QVERIFY(rec.reorderCurrentWorkspace(QStringLiteral("A"), 1));
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(3), id(1), id(5)}));
    // One more would put d1 behind the trailing empty: refused, not a reap
    // and recreate of the empty one.
    QVERIFY(!rec.reorderCurrentWorkspace(QStringLiteral("A"), 1));
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(3), id(1), id(5)}));
    QVERIFY(rec.reorderCurrentWorkspace(QStringLiteral("A"), -1));
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(3), id(5)}));
    QVERIFY(!rec.reorderCurrentWorkspace(QStringLiteral("A"), -1)); // top edge

    // The trailing empty itself never moves.
    rec.onScreenDesktopReport(QStringLiteral("A"), 3); // d5
    QVERIFY(!rec.reorderCurrentWorkspace(QStringLiteral("A"), -1));
    QCOMPARE(createSpy.count(), 0);
}

void TestWorkspaceReconciler::transferCurrentWorkspace_reownsAndRepairs()
{
    // A: {d1, d5 (displaced from an unplugged C), d3(trailing)}, B: {d2, d4}.
    WorkspaceMap candidate;
    candidate.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    for (const auto& [screen, desktop] : {std::pair{QStringLiteral("A"), id(1)}, std::pair{QStringLiteral("A"), id(5)},
                                          std::pair{QStringLiteral("A"), id(3)}, std::pair{QStringLiteral("B"), id(2)},
                                          std::pair{QStringLiteral("B"), id(4)}}) {
        WorkspaceEntry entry;
        entry.desktopId = desktop;
        candidate.insert(screen, candidate.sliceSize(screen), entry);
    }
    candidate.setHomeScreen(id(5), QStringLiteral("C"));
    WorkspaceReconciler rec;
    QVERIFY(rec.restoreState(candidate.toJson(1, {}, nullptr, true)));
    rec.onPopulationChanged(id(1), 1);
    rec.onPopulationChanged(id(5), 1);
    rec.onPopulationChanged(id(2), 1);
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(5));
    current.insert(QStringLiteral("B"), id(2));
    QVERIFY(rec.adoptAll({id(1), id(5), id(3), id(2), id(4)}, current));

    QSignalSpy setCurrentSpy(&rec, &WorkspaceReconciler::requestSetCurrent);
    const QString moved = rec.transferCurrentWorkspace(QStringLiteral("A"), QStringLiteral("B"));
    QCOMPARE(moved, id(5));
    // Inserted before B's trailing empty.
    QCOMPARE(sliceIds(rec, QStringLiteral("B")), QStringList({id(2), id(5), id(4)}));
    // A deliberate move ends the displacement: replugging C must not pull it
    // off the screen the user just put it on.
    QVERIFY(rec.map().entryFor(id(5)).homeScreenId.isEmpty());
    // The source lands on the workspace that took the moved one's slot (d3),
    // not on its slice head (d1).
    QCOMPARE(setCurrentSpy.count(), 1);
    QCOMPARE(setCurrentSpy.first().at(0).toString(), QStringLiteral("A"));
    QCOMPARE(setCurrentSpy.first().at(1).toString(), id(3));

    // The trailing empty is not a workspace to hand over.
    QVERIFY(rec.onScreenDesktopReport(QStringLiteral("A"), 3)); // d3, the echo
    QVERIFY(rec.transferCurrentWorkspace(QStringLiteral("A"), QStringLiteral("B")).isEmpty());
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(3)}));
}

void TestWorkspaceReconciler::transferCurrentWorkspace_neverTheSoleDesktop()
{
    // A holds one OCCUPIED desktop, so the trailing-empty guard cannot be what
    // refuses: only the last-desktop rule can.
    WorkspaceReconciler rec;
    rec.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B")});
    rec.setDesktopCap(2); // so A cannot be given a trailing empty
    rec.onPopulationChanged(id(1), 1);
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    current.insert(QStringLiteral("B"), id(2));
    QVERIFY(rec.adoptAll({id(1), id(2)}, current));
    QCOMPARE(rec.map().sliceSize(QStringLiteral("A")), 1);
    QVERIFY(rec.transferCurrentWorkspace(QStringLiteral("A"), QStringLiteral("B")).isEmpty());
    QCOMPARE(rec.map().ownerOf(id(1)), QStringLiteral("A"));
}

void TestWorkspaceReconciler::hotplug_homeStampAndMigrateBack()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // A: {d1(occ), d3}, B: {d2(occ), d4}

    // Unplug B: its slice fosters onto A, home-stamped.
    rec.onScreenRemoved(QStringLiteral("B"));
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(2), id(4), id(3)}));
    QCOMPARE(rec.map().entryFor(id(2)).homeScreenId, QStringLiteral("B"));
    QCOMPARE(rec.map().entryFor(id(4)).homeScreenId, QStringLiteral("B"));
    // A's own entries carry no home stamp.
    QVERIFY(rec.map().entryFor(id(1)).homeScreenId.isEmpty());

    // Replug B: the displaced entries migrate home in order, stamps cleared,
    // and A keeps exactly its own slice.
    rec.onScreenAdded(QStringLiteral("B"));
    QCOMPARE(sliceIds(rec, QStringLiteral("B")), QStringList({id(2), id(4)}));
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(3)}));
    QVERIFY(rec.map().entryFor(id(2)).homeScreenId.isEmpty());
    QVERIFY(rec.map().entryFor(id(4)).homeScreenId.isEmpty());
}

void TestWorkspaceReconciler::hotplug_daisyChainReturnsToTheOriginalHome()
{
    WorkspaceReconciler rec;
    rec.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    current.insert(QStringLiteral("B"), id(2));
    current.insert(QStringLiteral("C"), id(3));
    QVERIFY(rec.adoptAll({id(1), id(2), id(3)}, current));

    // C goes, its desktop fostered on A. Then A goes too: the fostered entry
    // moves on to B but keeps C, its ORIGINAL home.
    rec.onScreenRemoved(QStringLiteral("C"));
    QCOMPARE(rec.map().entryFor(id(3)).homeScreenId, QStringLiteral("C"));
    rec.onScreenRemoved(QStringLiteral("A"));
    QCOMPARE(rec.map().ownerOf(id(3)), QStringLiteral("B"));
    QCOMPARE(rec.map().entryFor(id(3)).homeScreenId, QStringLiteral("C"));
    QCOMPARE(rec.map().entryFor(id(1)).homeScreenId, QStringLiteral("A"));

    rec.onScreenAdded(QStringLiteral("C"));
    QCOMPARE(rec.map().ownerOf(id(3)), QStringLiteral("C"));
    rec.onScreenAdded(QStringLiteral("A"));
    QCOMPARE(rec.map().ownerOf(id(1)), QStringLiteral("A"));
}

void TestWorkspaceReconciler::hotplug_fosterShowingAMigratedDesktopIsCorrected()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.onScreenRemoved(QStringLiteral("B"));
    // A's slice is now {d1, d2, d4, d3}; A switches onto the fostered d2.
    rec.onScreenDesktopReport(QStringLiteral("A"), 3);
    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);

    // B returns and takes d2 home, so A is left on a desktop it no longer
    // owns and the owner-wins correction is raised for it.
    rec.onScreenAdded(QStringLiteral("B"));
    QCOMPARE(foreignSpy.count(), 1);
    QCOMPARE(foreignSpy.first().at(0).toString(), QStringLiteral("A"));
    QCOMPARE(foreignSpy.first().at(1).toString(), id(2));
    QCOMPARE(foreignSpy.first().at(2).toString(), QStringLiteral("B"));
}

void TestWorkspaceReconciler::donor_screenAtTheCapTakesASpareAndTheDonorIsCorrected()
{
    WorkspaceReconciler rec;
    rec.setDesktopCap(4);
    QVERIFY(adoptTwoScreens(rec)); // four desktops: at the cap
    rec.onScreenDesktopReport(QStringLiteral("A"), 2); // A shows its trailing empty d3
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);
    QSignalSpy mapSpy(&rec, &WorkspaceReconciler::mapChanged);

    // C arrives with KWin at its ceiling: no create, a spare from a donor.
    rec.onScreenAdded(QStringLiteral("C"));
    QCOMPARE(createSpy.count(), 0);
    QCOMPARE(rec.map().ownerOf(id(3)), QStringLiteral("C"));
    QVERIFY(mapSpy.count() >= 1);
    // The donor was showing the desktop it gave away.
    QCOMPARE(foreignSpy.count(), 1);
    QCOMPARE(foreignSpy.first().at(0).toString(), QStringLiteral("A"));
    QCOMPARE(foreignSpy.first().at(2).toString(), QStringLiteral("C"));
}

void TestWorkspaceReconciler::restore_candidateReconciledAgainstReality()
{
    // Previous session: A owned {d1(named chat), d2}; B owned {d9}.
    WorkspaceMap candidate;
    candidate.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    WorkspaceEntry chat;
    chat.desktopId = id(1);
    chat.name = QStringLiteral("chat");
    candidate.insert(QStringLiteral("A"), 0, chat);
    WorkspaceEntry d2;
    d2.desktopId = id(2);
    candidate.insert(QStringLiteral("A"), 1, d2);
    WorkspaceEntry d9;
    d9.desktopId = id(9);
    candidate.insert(QStringLiteral("B"), 0, d9);

    WorkspaceReconciler rec;
    QVERIFY(rec.restoreState(candidate.toJson(1, {}, nullptr, /*includeState=*/true)));
    // The restore carries the screen order itself; asserted HERE, because the
    // reorder below would hand it the same order and mask a parse that lost it.
    QCOMPARE(rec.map().screenOrder(), QStringList({QStringLiteral("A"), QStringLiteral("B")}));
    rec.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B")});

    // Reality: {d9} vanished while the daemon was down; {d5} is new.
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    current.insert(QStringLiteral("B"), id(5));
    QVERIFY(rec.adoptAll({id(1), id(2), id(5)}, current));

    QCOMPARE(rec.map().entryFor(id(1)).name, QStringLiteral("chat")); // kept
    QCOMPARE(rec.map().ownerOf(id(2)), QStringLiteral("A")); // kept
    QCOMPARE(rec.map().ownerOf(id(5)), QStringLiteral("B")); // adopted (B shows it)
    QVERIFY(rec.map().ownerOf(id(9)).isEmpty()); // vanished id dropped
}

void TestWorkspaceReconciler::restore_displacedEntryGoesHomeWhenItsScreenIsLive()
{
    // Last session ended with B unplugged, so d2 sat on A stamped home B. B is
    // connected this session from the start, which produces no screenAdded:
    // adoption is the only thing that can bring d2 home.
    WorkspaceMap candidate;
    candidate.setScreenOrder({QStringLiteral("A")});
    WorkspaceEntry d1;
    d1.desktopId = id(1);
    candidate.insert(QStringLiteral("A"), 0, d1);
    WorkspaceEntry d2;
    d2.desktopId = id(2);
    d2.homeScreenId = QStringLiteral("B");
    candidate.insert(QStringLiteral("A"), 1, d2);

    WorkspaceReconciler rec;
    QVERIFY(rec.restoreState(candidate.toJson(1, {}, nullptr, true)));
    rec.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B")});
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    current.insert(QStringLiteral("B"), id(3));
    QVERIFY(rec.adoptAll({id(1), id(2), id(3)}, current));

    QCOMPARE(rec.map().ownerOf(id(2)), QStringLiteral("B"));
    QVERIFY(rec.map().entryFor(id(2)).homeScreenId.isEmpty());
    QCOMPARE(sliceIds(rec, QStringLiteral("B")), QStringList({id(2), id(3)}));
}

void TestWorkspaceReconciler::restore_canonicalizesStampsAndIsRefusedAfterAdoption()
{
    WorkspaceMap candidate;
    candidate.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    WorkspaceEntry d1;
    d1.desktopId = id(1);
    d1.homeScreenId = QStringLiteral("DP-1"); // connector spelling of A itself
    candidate.insert(QStringLiteral("A"), 0, d1);
    WorkspaceEntry d2;
    d2.desktopId = id(2);
    d2.homeScreenId = QStringLiteral("DP-3"); // connector spelling of absent C
    candidate.insert(QStringLiteral("B"), 0, d2);

    WorkspaceReconciler rec;
    QSignalSpy mapSpy(&rec, &WorkspaceReconciler::mapChanged);
    const auto canonical = [](const QString& screen) {
        return screen == QStringLiteral("DP-1") ? QStringLiteral("A")
                                                : (screen == QStringLiteral("DP-3") ? QStringLiteral("C") : screen);
    };
    QVERIFY(rec.restoreState(candidate.toJson(1, {}, nullptr, true), canonical));
    QCOMPARE(mapSpy.count(), 1);
    // A stamp naming the entry's own owner is no displacement.
    QVERIFY(rec.map().entryFor(id(1)).homeScreenId.isEmpty());
    QCOMPARE(rec.map().entryFor(id(2)).homeScreenId, QStringLiteral("C"));
    QVERIFY(!rec.restoreState(QStringLiteral("not json")));
    QCOMPARE(rec.map().ownerOf(id(1)), QStringLiteral("A"));

    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    current.insert(QStringLiteral("B"), id(2));
    QVERIFY(rec.adoptAll({id(1), id(2)}, current));
    // After adoption the map is the reconciled truth and is not replaced.
    QVERIFY(!rec.restoreState(candidate.toJson(1, {}, nullptr, true)));
    QCOMPARE(rec.map().entryFor(id(2)).homeScreenId, QStringLiteral("C"));
}

void TestWorkspaceReconciler::named_createdPinnedAndExempt()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // A: {d1(occ), d3}, B: {d2(occ), d4}

    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    const NamedWorkspace chat = named(QStringLiteral("n1"), QStringLiteral("chat"), QStringLiteral("B"));
    rec.applyNamedWorkspaces({chat}, {});

    QCOMPARE(createSpy.count(), 1);
    QCOMPARE(createSpy.first().at(1).toString(), QStringLiteral("chat"));
    // A second apply while the create is pending requests nothing extra.
    rec.applyNamedWorkspaces({chat}, {});
    QCOMPARE(createSpy.count(), 1);

    // Realize it: lands in B's slice before the trailing empty, named, and
    // carrying the declaration's id.
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(3), id(2), id(5), id(4)});
    QCOMPARE(sliceIds(rec, QStringLiteral("B")), QStringList({id(2), id(5), id(4)}));
    QCOMPARE(rec.map().entryFor(id(5)).name, QStringLiteral("chat"));
    QCOMPARE(rec.map().entryFor(id(5)).namedId, QStringLiteral("n1"));

    // Named + empty: destroy-exempt (it is mid-slice and empty).
    rec.setTimeouts(WorkspaceReconciler::LedgerTimeoutMs, WorkspaceTest::FastDebounceMs);
    QSignalSpy removeSpy(&rec, &WorkspaceReconciler::requestRemoveDesktop);
    rec.onPopulationChanged(id(5), 1);
    rec.onPopulationChanged(id(5), 0);
    QTest::qWait(WorkspaceTest::FastDebounceMs * 5);
    QCOMPARE(removeSpy.count(), 0);
}

void TestWorkspaceReconciler::named_renameInSettingsRenamesTheDesktopInPlace()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))}, {});
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(5), id(3), id(2), id(4)});
    const QStringList kwinNames{label(1), QStringLiteral("chat"), label(3), label(4), label(5)};
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))}, kwinNames);

    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy renameSpy(&rec, &WorkspaceReconciler::requestSetDesktopName);
    QSignalSpy kwinRenameSpy(&rec, &WorkspaceReconciler::namedWorkspaceRenamedInKWin);

    // Same declaration id, new name: the same desktop takes the new name. No
    // new desktop, no revert of the old one, and KWin is told the new name so
    // its own list (the identity a stateless restart claims by) follows.
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("talk"))}, kwinNames);
    QCOMPARE(createSpy.count(), 0);
    QCOMPARE(rec.map().entryFor(id(5)).name, QStringLiteral("talk"));
    QCOMPARE(rec.map().entryFor(id(5)).namedId, QStringLiteral("n1"));
    QCOMPARE(renameSpy.count(), 1);
    QCOMPARE(renameSpy.first().at(0).toString(), id(5));
    QCOMPARE(renameSpy.first().at(1).toString(), QStringLiteral("talk"));
    // KWin still shows the old name until the push lands, and that is not a
    // rename on KWin's side.
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("talk"))}, kwinNames);
    QCOMPARE(kwinRenameSpy.count(), 0);
    QCOMPARE(renameSpy.count(), 1);
}

void TestWorkspaceReconciler::named_claimByKWinName()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // last ids: {d1} {d3} {d2} {d4}

    // Restart-without-state-file shape: KWin still carries the name we
    // stamped last session on {d1} (a NON-trailing desktop — claiming a
    // trailing empty would legitimately trigger a trailing-empty repair
    // create); the declaration claims it by NAME, the only identity KWin
    // keeps, and stamps its id on the claimed entry.
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))},
                             {QStringLiteral("chat"), label(2), label(3), label(4)});
    QCOMPARE(createSpy.count(), 0);
    QCOMPARE(rec.map().entryFor(id(1)).name, QStringLiteral("chat"));
    QCOMPARE(rec.map().entryFor(id(1)).namedId, QStringLiteral("n1"));
}

void TestWorkspaceReconciler::named_placeholderNameNeverClaims()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // last ids: {d1} {d3} {d2} {d4}

    // KWin names every desktop nobody named after its position, so the
    // second desktop really is called "Desktop 2" in KWin's raw list. A
    // workspace a user happened to call that must NOT claim it: the label is
    // generated, not an identity. The declaration is created instead.
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), label(2))}, positionalNames(4));

    QCOMPARE(createSpy.count(), 1);
    QCOMPARE(createSpy.first().at(1).toString(), label(2));
    for (const QString& desktopId : {id(1), id(2), id(3), id(4)}) {
        QVERIFY(rec.map().entryFor(desktopId).name.isEmpty());
    }
}

void TestWorkspaceReconciler::named_kwinBehindGetsTheNamePushedOnce()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))},
                             {QStringLiteral("chat"), label(2), label(3), label(4)});
    QSignalSpy renameSpy(&rec, &WorkspaceReconciler::requestSetDesktopName);

    // KWin lost the name (its default came back), so the map is right and
    // KWin is behind: push it, once per push window, however often the
    // declarations are re-applied meanwhile.
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))}, positionalNames(4));
    QCOMPARE(renameSpy.count(), 1);
    QCOMPARE(renameSpy.first().at(0).toString(), id(1));
    QCOMPARE(renameSpy.first().at(1).toString(), QStringLiteral("chat"));
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))}, positionalNames(4));
    QCOMPARE(renameSpy.count(), 1);

    // Once KWin agrees, the record is gone, so a later loss pushes again at
    // once rather than waiting out a stale window.
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))},
                             {QStringLiteral("chat"), label(2), label(3), label(4)});
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))}, positionalNames(4));
    QCOMPARE(renameSpy.count(), 2);
}

void TestWorkspaceReconciler::named_kwinSideRenameIsReportedNotOverwritten()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))},
                             {QStringLiteral("chat"), label(2), label(3), label(4)});
    QSignalSpy renameSpy(&rec, &WorkspaceReconciler::requestSetDesktopName);
    QSignalSpy kwinRenameSpy(&rec, &WorkspaceReconciler::namedWorkspaceRenamedInKWin);

    // The user renamed d1 in the Pager. That is a name they chose, not a
    // default and not ours, so it is reported for the controller to adopt
    // into the declaration, and NOT pushed back over.
    const QStringList renamed{QStringLiteral("work"), label(2), label(3), label(4)};
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))}, renamed);
    QCOMPARE(renameSpy.count(), 0);
    QCOMPARE(kwinRenameSpy.count(), 1);
    QCOMPARE(kwinRenameSpy.first().at(0).toString(), QStringLiteral("n1"));
    QCOMPARE(kwinRenameSpy.first().at(1).toString(), id(1));
    QCOMPARE(kwinRenameSpy.first().at(2).toString(), QStringLiteral("work"));
    // Once per rename, not once per settle.
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))}, renamed);
    QCOMPARE(kwinRenameSpy.count(), 1);

    // The controller adopts it: same id, the KWin name. Nothing to push.
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("work"))}, renamed);
    QCOMPARE(rec.map().entryFor(id(1)).name, QStringLiteral("work"));
    QCOMPARE(renameSpy.count(), 1); // the in-place rename's own push
    QCOMPARE(kwinRenameSpy.count(), 1);
}

void TestWorkspaceReconciler::named_unnamedRevertsToDynamic()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))},
                             {QStringLiteral("chat"), label(2), label(3), label(4)});
    QCOMPARE(rec.map().entryFor(id(1)).name, QStringLiteral("chat"));

    QSignalSpy renameSpy(&rec, &WorkspaceReconciler::requestSetDesktopName);
    rec.applyNamedWorkspaces({}, {QStringLiteral("chat"), label(2), label(3), label(4)});
    QVERIFY(rec.map().entryFor(id(1)).name.isEmpty());
    QVERIFY(rec.map().entryFor(id(1)).namedId.isEmpty());
    // KWin keeps an empty name verbatim (a blank label in its UI and config),
    // so unnamed is pushed as KWin's own positional default.
    QCOMPARE(renameSpy.count(), 1);
    QCOMPARE(renameSpy.first().at(0).toString(), id(1));
    QCOMPARE(renameSpy.first().at(1).toString(), label(1));
}

void TestWorkspaceReconciler::named_duplicateMapNamesCollapseToOne()
{
    // Two restored entries both carry "chat" (a state file written across a
    // bad session). One declaration claims one of them; the other must revert
    // rather than stay destroy-exempt with nothing in settings to explain it.
    WorkspaceMap candidate;
    candidate.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    WorkspaceEntry first;
    first.desktopId = id(1);
    first.name = QStringLiteral("chat");
    candidate.insert(QStringLiteral("A"), 0, first);
    WorkspaceEntry second;
    second.desktopId = id(2);
    second.name = QStringLiteral("chat");
    candidate.insert(QStringLiteral("B"), 0, second);

    WorkspaceReconciler rec;
    QVERIFY(rec.restoreState(candidate.toJson(1, {}, nullptr, true)));
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    current.insert(QStringLiteral("B"), id(2));
    QVERIFY(rec.adoptAll({id(1), id(2)}, current));
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))}, {});

    QCOMPARE(rec.map().entryFor(id(1)).name, QStringLiteral("chat"));
    QVERIFY(rec.map().entryFor(id(2)).name.isEmpty());
}

void TestWorkspaceReconciler::named_declarationTrimAndDuplicateSkip()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);

    // Padded names are trimmed, a blank one and a repeat of a name (or of an
    // id) already declared are skipped: one create, for "chat".
    rec.applyNamedWorkspaces(
        {named(QStringLiteral("n1"), QStringLiteral("  chat ")), named(QStringLiteral("n2"), QStringLiteral("   ")),
         named(QStringLiteral("n3"), QStringLiteral("chat")), named(QStringLiteral("n1"), QStringLiteral("mail"))},
        {});
    QCOMPARE(createSpy.count(), 1);
    QCOMPARE(createSpy.first().at(1).toString(), QStringLiteral("chat"));
}

void TestWorkspaceReconciler::named_createEchoFifoMismatchHealedByKWinNames()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.setFocusedScreen(QStringLiteral("A"));

    const QList<NamedWorkspace> declarations{named(QStringLiteral("n1"), QStringLiteral("chat"))};
    rec.applyNamedWorkspaces(declarations, positionalNames(4));

    // An EXTERNAL creation races our named create and steals the FIFO match
    // (id-only echoes): {d7} is realized under the declared name while OUR
    // desktop {d8} (the one KWin actually created with the name) adopts as
    // external.
    rec.onKwinDesktopCreated(id(7));
    rec.onKwinDesktopCreated(id(8));
    rec.onDesktopListSettled({id(1), id(3), id(7), id(2), id(4), id(8)});
    QCOMPARE(rec.map().entryFor(id(7)).name, QStringLiteral("chat"));

    // Re-verifying against KWin's OWN name list (what the controller does
    // after every settle) moves the identity, id included, to the desktop
    // that really carries the name. d7 carries its positional default.
    const QStringList kwinNames{label(1), label(2), label(3), label(4), label(5), QStringLiteral("chat")};
    rec.applyNamedWorkspaces(declarations, kwinNames);
    QCOMPARE(rec.map().entryFor(id(8)).name, QStringLiteral("chat"));
    QCOMPARE(rec.map().entryFor(id(8)).namedId, QStringLiteral("n1"));
    QVERIFY(rec.map().entryFor(id(7)).name.isEmpty());
}

void TestWorkspaceReconciler::named_externalUserNamedCreateIsNotTakenForOurs()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy renameSpy(&rec, &WorkspaceReconciler::requestSetDesktopName);
    rec.onPopulationChanged(id(3), 1); // our unnamed create for A, at position 2

    // The user adds "Work" in System Settings at the same moment. Its echo
    // carries a name a user chose, which an unnamed create of ours never
    // gets from KWin, so it is external and our create stays open.
    rec.onKwinDesktopCreatedAt(id(9), 4, QStringLiteral("Work"));
    QVERIFY(rec.hasPendingStructuralOps());
    rec.onKwinDesktopCreatedAt(id(5), 2, label(3));
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(3), id(9), id(5)}));

    // And the user's name is never overwritten with a positional label.
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(9), id(4)},
                             {label(1), label(2), label(3), label(3), QStringLiteral("Work"), label(4)});
    for (const QList<QVariant>& push : std::as_const(renameSpy)) {
        QVERIFY(push.at(0).toString() != id(9));
    }
}

void TestWorkspaceReconciler::named_pinTransfersRealizedWorkspaceAndClearsHome()
{
    // "chat" is realized on A, displaced there from an unplugged C, and A is
    // SHOWING it.
    WorkspaceMap candidate;
    candidate.setScreenOrder({QStringLiteral("A"), QStringLiteral("B")});
    const auto add = [&candidate](const QString& screen, const QString& desktopId) {
        WorkspaceEntry entry;
        entry.desktopId = desktopId;
        candidate.insert(screen, candidate.sliceSize(screen), entry);
    };
    add(QStringLiteral("A"), id(1));
    add(QStringLiteral("A"), id(5));
    add(QStringLiteral("A"), id(3));
    add(QStringLiteral("B"), id(2));
    add(QStringLiteral("B"), id(4));
    candidate.setName(id(5), QStringLiteral("chat"));
    candidate.setNamedId(id(5), QStringLiteral("n1"));
    candidate.setHomeScreen(id(5), QStringLiteral("C"));

    WorkspaceReconciler rec;
    QVERIFY(rec.restoreState(candidate.toJson(1, {}, nullptr, true)));
    rec.onPopulationChanged(id(1), 1);
    rec.onPopulationChanged(id(2), 1);
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(5));
    current.insert(QStringLiteral("B"), id(2));
    QVERIFY(rec.adoptAll({id(1), id(5), id(3), id(2), id(4)}, current));

    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"), QStringLiteral("B"))}, {});

    QCOMPARE(rec.map().ownerOf(id(5)), QStringLiteral("B"));
    // A pin is a deliberate placement, so it outranks hotplug memory: leaving
    // the stamp would yank the workspace back off B on the next replug of C.
    QVERIFY(rec.map().entryFor(id(5)).homeScreenId.isEmpty());
    // A was left sitting on a desktop it no longer owns, which is exactly the
    // owner-wins case the verb-side transfer path also raises.
    QCOMPARE(foreignSpy.count(), 1);
    QCOMPARE(foreignSpy.first().at(0).toString(), QStringLiteral("A"));
    QCOMPARE(foreignSpy.first().at(1).toString(), id(5));
    QCOMPARE(foreignSpy.first().at(2).toString(), QStringLiteral("B"));
}

void TestWorkspaceReconciler::named_pinDeclinedWhenSourceHoldsItsLastDesktop()
{
    WorkspaceReconciler rec;
    rec.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B")});
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    current.insert(QStringLiteral("B"), id(2));
    QVERIFY(rec.adoptAll({id(1), id(2)}, current)); // one desktop each

    // Claim {d1} by the name KWin still carries for it.
    NamedWorkspace chat = named(QStringLiteral("n1"), QStringLiteral("chat"));
    rec.applyNamedWorkspaces({chat}, {QStringLiteral("chat"), label(2)});
    QCOMPARE(rec.map().entryFor(id(1)).name, QStringLiteral("chat"));

    // Pinning it to B would leave A with no workspace at all, so the transfer
    // is declined and the workspace stays put.
    chat.outputId = QStringLiteral("B");
    rec.applyNamedWorkspaces({chat}, {QStringLiteral("chat"), label(2)});
    QCOMPARE(rec.map().ownerOf(id(1)), QStringLiteral("A"));
    QCOMPARE(rec.map().sliceSize(QStringLiteral("A")), 1);
}

void TestWorkspaceReconciler::named_declaredPositionMovesARealizedWorkspace()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // A: {d1(occ), d3}

    NamedWorkspace chat = named(QStringLiteral("n1"), QStringLiteral("chat"), QStringLiteral("A"));
    rec.applyNamedWorkspaces({chat}, {});
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(5), id(3), id(2), id(4)});
    QCOMPARE(rec.map().sliceIndexOf(id(5)), 1);

    // The declared position applies to a workspace that is ALREADY realized on
    // the screen it belongs to, not only at creation and pin time — that gap
    // is what made the settings Position control do nothing.
    chat.position = 0;
    rec.applyNamedWorkspaces({chat}, {});
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(5), id(1), id(3)}));

    // Idempotent: re-applying the same declaration moves nothing and announces
    // nothing, so this cannot become a per-settle reorder loop.
    QSignalSpy mapSpy(&rec, &WorkspaceReconciler::mapChanged);
    rec.applyNamedWorkspaces({chat}, {});
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(5), id(1), id(3)}));
    QCOMPARE(mapSpy.count(), 0);

    // An out-of-range position clamps to the last slot BEFORE the trailing
    // empty; landing behind it would hand the trailing role to a destroy-exempt
    // workspace and the invariant would never recover.
    chat.position = 9;
    rec.applyNamedWorkspaces({chat}, {});
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(5), id(3)}));
}

void TestWorkspaceReconciler::named_positionsLaidOutOnceInRowOrder()
{
    // A: {d1, d5, d6, d7, d8, d3(trailing)}, four named workspaces on it.
    WorkspaceMap candidate;
    candidate.setScreenOrder({QStringLiteral("A")});
    for (const int n : {1, 5, 6, 7, 8, 3}) {
        WorkspaceEntry entry;
        entry.desktopId = id(n);
        candidate.insert(QStringLiteral("A"), candidate.sliceSize(QStringLiteral("A")), entry);
    }
    WorkspaceReconciler rec;
    QVERIFY(rec.restoreState(candidate.toJson(1, {}, nullptr, true)));
    for (const int n : {1, 5, 6, 7, 8}) {
        rec.onPopulationChanged(id(n), 1);
    }
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(rec.adoptAll({id(1), id(5), id(6), id(7), id(8), id(3)}, current));
    const QStringList kwinNames{
        label(1), QStringLiteral("u1"), QStringLiteral("u2"), QStringLiteral("e1"), QStringLiteral("e2"), label(6)};

    // Two unpositioned workspaces (rows 0 and 1) and two that both ask for
    // slot 0 (rows 2 and 3). The equal pair keeps its ROW order, the
    // unpositioned pair stays where it is relative to the rest, and laying it
    // out again moves nothing.
    const QList<NamedWorkspace> declarations{named(QStringLiteral("u1"), QStringLiteral("u1")),
                                             named(QStringLiteral("u2"), QStringLiteral("u2")),
                                             named(QStringLiteral("e1"), QStringLiteral("e1"), QString(), 0),
                                             named(QStringLiteral("e2"), QStringLiteral("e2"), QString(), 0)};
    rec.applyNamedWorkspaces(declarations, kwinNames);
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(7), id(8), id(1), id(5), id(6), id(3)}));

    QSignalSpy mapSpy(&rec, &WorkspaceReconciler::mapChanged);
    rec.applyNamedWorkspaces(declarations, kwinNames);
    rec.applyNamedWorkspaces(declarations, kwinNames);
    QCOMPARE(mapSpy.count(), 0);
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(7), id(8), id(1), id(5), id(6), id(3)}));
}

void TestWorkspaceReconciler::named_positionNotEnforcedOnAFosterScreen()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    // "chat" pinned to B at slot 0.
    const NamedWorkspace chat = named(QStringLiteral("n1"), QStringLiteral("chat"), QStringLiteral("B"), 0);
    rec.applyNamedWorkspaces({chat}, {});
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)});
    QCOMPARE(sliceIds(rec, QStringLiteral("B")), QStringList({id(5), id(2), id(4)}));

    // B is unplugged: its slice is fostered on A behind A's own workspaces.
    // The pin's position is B's, and enforcing it on A would shove chat to the
    // front of a screen it does not belong to.
    rec.onScreenRemoved(QStringLiteral("B"));
    const QStringList fostered = sliceIds(rec, QStringLiteral("A"));
    rec.applyNamedWorkspaces({chat}, {});
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), fostered);
    QCOMPARE(fostered.indexOf(id(5)), 1);

    // The same without a home stamp: a workspace pinned to an output that has
    // not been connected this session lives on whatever screen took it, and
    // the position it declares is for the pinned output, not that one.
    WorkspaceMap candidate;
    candidate.setScreenOrder({QStringLiteral("A")});
    for (const int n : {1, 6, 7, 3}) {
        WorkspaceEntry entry;
        entry.desktopId = id(n);
        candidate.insert(QStringLiteral("A"), candidate.sliceSize(QStringLiteral("A")), entry);
    }
    candidate.setName(id(7), QStringLiteral("mail"));
    candidate.setNamedId(id(7), QStringLiteral("n2"));
    WorkspaceReconciler unplugged;
    QVERIFY(unplugged.restoreState(candidate.toJson(1, {}, nullptr, true)));
    unplugged.onPopulationChanged(id(1), 1);
    unplugged.onPopulationChanged(id(6), 1);
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(unplugged.adoptAll({id(1), id(6), id(7), id(3)}, current));
    unplugged.applyNamedWorkspaces({named(QStringLiteral("n2"), QStringLiteral("mail"), QStringLiteral("C"), 0)}, {});
    QCOMPARE(sliceIds(unplugged, QStringLiteral("A")), QStringList({id(1), id(6), id(7), id(3)}));
}

void TestWorkspaceReconciler::named_explicitPositionStaysBeforeTrailingEmpty()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // A: {d1(occ), d3(trailing empty)}

    // An out-of-range explicit position must clamp to the slot BEFORE the
    // trailing empty. Landing behind it would demote the empty desktop to
    // mid-slice (maintenance then reaps it) and hand the trailing role to a
    // named workspace, which is destroy-exempt — the invariant would never
    // recover.
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"), QStringLiteral("A"), 9)}, {});
    QCOMPARE(createSpy.count(), 1);
    // The slot before A's trailing empty is right after d1, KWin position 1.
    QCOMPARE(createSpy.first().first().toUInt(), 1u);

    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(5), id(3), id(2), id(4)});
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(5), id(3)}));
    QVERIFY(rec.map().slice(QStringLiteral("A")).last().name.isEmpty());
}

void TestWorkspaceReconciler::named_verbMoveIsReported()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"), QStringLiteral("A"), 1)}, {});
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(5), id(3), id(2), id(4)});
    rec.onScreenDesktopReport(QStringLiteral("A"), 2); // A shows chat
    QSignalSpy movedSpy(&rec, &WorkspaceReconciler::namedWorkspaceMoved);

    // The verb moved a positioned workspace: the declaration is the
    // controller's to rewrite, or the next apply would move it straight back.
    QVERIFY(rec.reorderCurrentWorkspace(QStringLiteral("A"), -1));
    QCOMPARE(movedSpy.count(), 1);
    QCOMPARE(movedSpy.first().at(0).toString(), QStringLiteral("n1"));
    QCOMPARE(movedSpy.first().at(1).toString(), QStringLiteral("chat"));
    QCOMPARE(movedSpy.first().at(2).toString(), QStringLiteral("A"));
    QCOMPARE(movedSpy.first().at(3).toInt(), 0);

    QCOMPARE(rec.transferCurrentWorkspace(QStringLiteral("A"), QStringLiteral("B")), id(5));
    QCOMPARE(movedSpy.count(), 2);
    QCOMPARE(movedSpy.last().at(2).toString(), QStringLiteral("B"));
    QCOMPARE(movedSpy.last().at(3).toInt(), 1);
}

void TestWorkspaceReconciler::labels_stalePositionalLabelsRefreshedUserNamesKept()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy renameSpy(&rec, &WorkspaceReconciler::requestSetDesktopName);

    // d1 was removed. KWin never renames, so the survivors keep the labels of
    // the positions they used to hold: d3 still says "Desktop 2" at position
    // 1. Those are ours to correct. d4 carries a name the user gave it, which
    // is theirs.
    rec.onKwinDesktopRemoved(id(1));
    rec.onDesktopListSettled({id(3), id(2), id(4)}, {label(2), label(3), QStringLiteral("music")});
    QStringList pushed;
    for (const QList<QVariant>& push : std::as_const(renameSpy)) {
        pushed.append(push.at(0).toString() + QLatin1Char('=') + push.at(1).toString());
    }
    QCOMPARE(pushed, QStringList({id(3) + QStringLiteral("=") + label(1), id(2) + QStringLiteral("=") + label(2)}));

    // A names-only change with the ids unchanged is still seen; a label
    // already right is left alone.
    renameSpy.clear();
    rec.onDesktopListSettled({id(3), id(2), id(4)}, {label(1), label(2), label(9)});
    QCOMPARE(renameSpy.count(), 1);
    QCOMPARE(renameSpy.first().at(0).toString(), id(4));
    QCOMPARE(renameSpy.first().at(1).toString(), label(3));
}

void TestWorkspaceReconciler::settled_emptyListIsIgnored()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // A: {d1,d3}, B: {d2,d4}

    // A failed upstream read surfaces as an empty list. KWin never has zero
    // desktops, so acting on it would drop every slice and have the engines
    // reap all per-desktop state. Refuse it and ask for a resync instead.
    QSignalSpy resyncSpy(&rec, &WorkspaceReconciler::resyncRequested);
    QSignalSpy mapSpy(&rec, &WorkspaceReconciler::mapChanged);
    rec.onDesktopListSettled({});

    QCOMPARE(mapSpy.count(), 0);
    QCOMPARE(resyncSpy.count(), 1);
    QCOMPARE(rec.map().slice(QStringLiteral("A")).size(), 2);
    QCOMPARE(rec.map().slice(QStringLiteral("B")).size(), 2);
    QCOMPARE(rec.map().ownerOf(id(1)), QStringLiteral("A"));
    // And the verbs still resolve against the kept list.
    rec.onScreenDesktopReport(QStringLiteral("A"), 1);
    QCOMPARE(rec.currentDesktopIdOf(QStringLiteral("A")), id(1));
}

void TestWorkspaceReconciler::screenAdded_freshScreenGetsItsOwnDesktop()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));

    // A monitor with no hotplug history joins: it owns nothing, so the
    // slice-never-empty invariant asks for a desktop of its own.
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    rec.onScreenAdded(QStringLiteral("C"));
    QCOMPARE(createSpy.count(), 1);
    QVERIFY(rec.hasPendingStructuralOps()); // the Create is open

    rec.onKwinDesktopCreated(id(6));
    rec.onDesktopListSettled({id(1), id(3), id(2), id(4), id(6)});
    QCOMPARE(rec.map().ownerOf(id(6)), QStringLiteral("C"));
    QVERIFY(!rec.hasPendingStructuralOps()); // and retired once it landed
}

void TestWorkspaceReconciler::screenAdded_bothHotplugSignalOrdersGetADesktop()
{
    // A hotplug reaches the reconciler as two calls, and which one arrives
    // first depends on how the controller recomputes its screen order. Both
    // orders must end with the new output holding a workspace of its own.
    {
        // Order 1: the recomputed order already names the new screen.
        WorkspaceReconciler rec;
        QVERIFY(adoptTwoScreens(rec));
        QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
        rec.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
        rec.onScreenAdded(QStringLiteral("C"));
        QCOMPARE(createSpy.count(), 1);
        // After everything A and B hold.
        QCOMPARE(createSpy.first().first().toUInt(), 4u);

        rec.onKwinDesktopCreated(id(6));
        rec.onDesktopListSettled({id(1), id(3), id(2), id(4), id(6)});
        QCOMPARE(rec.map().ownerOf(id(6)), QStringLiteral("C"));
    }
    {
        // Order 2: screenAdded first, the order recomputed afterwards.
        WorkspaceReconciler rec;
        QVERIFY(adoptTwoScreens(rec));
        QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
        rec.onScreenAdded(QStringLiteral("C"));
        rec.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
        QCOMPARE(createSpy.count(), 1);

        rec.onKwinDesktopCreated(id(6));
        rec.onDesktopListSettled({id(1), id(3), id(2), id(4), id(6)});
        QCOMPARE(rec.map().ownerOf(id(6)), QStringLiteral("C"));
    }
}

void TestWorkspaceReconciler::create_secondPendingSurvivesTheFirstsSettle()
{
    // Two Creates open at once, one landing. The settle must retire ONLY the
    // op the landed desktop answered: retiring a second by arithmetic (the id
    // count grew by one, so "one create landed") left B's echo with no ledger
    // entry, which adopted B's desktop onto the FOCUSED screen and let
    // maintainScreen request a duplicate in the meantime.
    WorkspaceReconciler rec;
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QVERIFY(openTwoConcurrentCreates(rec, createSpy));

    // A's create lands: echo first, then the settled list.
    rec.onKwinDesktopCreated(id(3));
    QCOMPARE(rec.map().ownerOf(id(3)), QStringLiteral("A"));
    rec.onDesktopListSettled({id(1), id(3), id(2)});

    // B's Create is still open: no duplicate request, and B's slice is
    // untouched while it waits.
    QCOMPARE(createSpy.count(), 2);
    QCOMPARE(rec.map().slice(QStringLiteral("B")).size(), 1);

    // B's echo lands on the screen that ASKED, not on the focused screen.
    rec.onKwinDesktopCreated(id(4));
    QCOMPARE(rec.map().ownerOf(id(4)), QStringLiteral("B"));
    rec.onDesktopListSettled({id(1), id(3), id(2), id(4)});
    QCOMPARE(rec.map().ownerOf(id(4)), QStringLiteral("B"));
    QCOMPARE(createSpy.count(), 2);
}

void TestWorkspaceReconciler::create_settledBeforeEchoLandsOnRequestingScreen()
{
    // The echo-loss recovery order: both creates land and the settled list
    // arrives with no id-only echo at all. Each new id consumes the open
    // Create of matching RANK among the requested global positions, so each
    // desktop lands on the screen that asked for it instead of being adopted
    // onto the focused one. Here the two rankings happen to agree;
    // create_settledMatchesByPositionNotRequestOrder is the case where they do
    // not.
    WorkspaceReconciler rec;
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QVERIFY(openTwoConcurrentCreates(rec, createSpy));

    rec.onDesktopListSettled({id(1), id(3), id(2), id(4)});

    QCOMPARE(rec.map().ownerOf(id(3)), QStringLiteral("A"));
    QCOMPARE(rec.map().ownerOf(id(4)), QStringLiteral("B"));
    QCOMPARE(rec.map().slice(QStringLiteral("A")).size(), 2);
    QCOMPARE(rec.map().slice(QStringLiteral("B")).size(), 2);
    // Both ops were consumed, so maintenance is quiet again.
    QCOMPARE(createSpy.count(), 2);

    // The lost echo turns up late after all. The desktop is already owned, so
    // it matches nothing and moves nothing.
    QSignalSpy mapSpy(&rec, &WorkspaceReconciler::mapChanged);
    rec.onKwinDesktopCreated(id(3));
    QCOMPARE(rec.map().ownerOf(id(3)), QStringLiteral("A"));
    QCOMPARE(mapSpy.count(), 0);
}

void TestWorkspaceReconciler::create_settledMatchesByPositionNotRequestOrder()
{
    // Same settle-beats-the-echo race, but B's population change fires first,
    // so the ledger reads [Create(B), Create(A)] while KWin's list still puts
    // A's new desktop (d3, 0-based position 1) ahead of B's (d4, position 3).
    // Matching the oldest request first would hand d3 to B and d4 to A — each
    // desktop in the wrong screen's slice, and invisible afterwards because
    // both screens still end in a trailing empty. The match is by requested
    // POSITION.
    WorkspaceReconciler rec;
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QVERIFY(openTwoConcurrentCreates(rec, createSpy, /*populateBFirst=*/true));

    rec.onDesktopListSettled({id(1), id(3), id(2), id(4)});

    QCOMPARE(rec.map().ownerOf(id(3)), QStringLiteral("A"));
    QCOMPARE(rec.map().ownerOf(id(4)), QStringLiteral("B"));
    QCOMPARE(rec.map().slice(QStringLiteral("A")).size(), 2);
    QCOMPARE(rec.map().slice(QStringLiteral("B")).size(), 2);
    QCOMPARE(createSpy.count(), 2);
}

void TestWorkspaceReconciler::create_threeConcurrentSettleLandOnTheirOwnScreens()
{
    // Three screens repairing their trailing empty in ONE maintenance pass,
    // all answered by a single settle with no echoes. KWin performs the three
    // in request order, so each later request is placed counting the earlier
    // creates KWin will already have inserted ahead of it: the requested
    // positions are 1, 3 and 5, exactly where the settled list puts the new
    // desktops.
    WorkspaceReconciler rec;
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);

    rec.onScreenOrderChanged({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
    rec.setFocusedScreen(QStringLiteral("A"));

    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    current.insert(QStringLiteral("B"), id(2));
    current.insert(QStringLiteral("C"), id(3));
    QVERIFY(rec.adoptAll({id(1), id(2), id(3)}, current));
    QCOMPARE(createSpy.count(), 0);

    rec.onPopulationChanged(id(1), 1);
    rec.onPopulationChanged(id(2), 1);
    rec.onPopulationChanged(id(3), 1);
    QCOMPARE(createSpy.count(), 3);
    QCOMPARE(createSpy.at(0).first().toUInt(), 1u);
    QCOMPARE(createSpy.at(1).first().toUInt(), 3u);
    QCOMPARE(createSpy.at(2).first().toUInt(), 5u);

    rec.onDesktopListSettled({id(1), id(4), id(2), id(5), id(3), id(6)});

    QCOMPARE(rec.map().ownerOf(id(4)), QStringLiteral("A"));
    QCOMPARE(rec.map().ownerOf(id(5)), QStringLiteral("B"));
    QCOMPARE(rec.map().ownerOf(id(6)), QStringLiteral("C"));
    QCOMPARE(rec.map().slice(QStringLiteral("A")).size(), 2);
    QCOMPARE(rec.map().slice(QStringLiteral("B")).size(), 2);
    QCOMPARE(rec.map().slice(QStringLiteral("C")).size(), 2);
    // Every op was consumed, so maintenance asks for nothing more.
    QCOMPARE(createSpy.count(), 3);
}

void TestWorkspaceReconciler::create_positionalEchoPicksTheRightCreate()
{
    // Two creates open, A's at position 1 and B's at position 3. KWin's
    // echo carries the position, so B's landing is B's even though A's
    // request is older and an id-only echo would have gone to it.
    WorkspaceReconciler rec;
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QVERIFY(openTwoConcurrentCreates(rec, createSpy));
    const int positionB = static_cast<int>(createSpy.at(1).first().toUInt());

    rec.onKwinDesktopCreatedAt(id(4), positionB, label(positionB + 1));
    QCOMPARE(rec.map().ownerOf(id(4)), QStringLiteral("B"));
    rec.onKwinDesktopCreatedAt(id(3), 1, label(2));
    QCOMPARE(rec.map().ownerOf(id(3)), QStringLiteral("A"));
}

void TestWorkspaceReconciler::create_landsAtItsIntentNotAFrozenSlot()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    // A: {d1, d3}. Occupying d3 asks for A's next trailing empty, which was
    // slot 2 when requested.
    rec.onPopulationChanged(id(3), 1);
    // Before it lands, an external desktop is adopted onto A (added at the
    // end of KWin's list, where System Settings puts one). A has no trailing
    // empty right now, so the adoption appends.
    rec.onKwinDesktopCreatedAt(id(9), 4, QStringLiteral("Work"));
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(3), id(9)}));

    // The landing is A's new trailing empty, so it goes LAST. Slot 2 frozen
    // at request time would wedge it before d9, mid-slice, where an empty
    // dynamic desktop is reaped.
    rec.onKwinDesktopCreatedAt(id(5), 2, label(3));
    QCOMPARE(sliceIds(rec, QStringLiteral("A")), QStringList({id(1), id(3), id(9), id(5)}));
}

void TestWorkspaceReconciler::create_capGateCountsEchoedCreates()
{
    WorkspaceReconciler rec;
    rec.setDesktopCap(5);
    QVERIFY(adoptTwoScreens(rec)); // four desktops
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy capSpy(&rec, &WorkspaceReconciler::capReached);

    rec.onPopulationChanged(id(3), 1);
    QCOMPARE(createSpy.count(), 1);
    // KWin echoed it: the fifth desktop exists, though the settled list still
    // says four and the request is no longer open.
    rec.onKwinDesktopCreated(id(5));
    // B now wants a sixth. Counting only the settled list and the open
    // requests would let it through past the ceiling.
    rec.onPopulationChanged(id(4), 1);
    QCOMPARE(createSpy.count(), 1);
    QCOMPARE(capSpy.count(), 1);
}

void TestWorkspaceReconciler::answers_refusedCreateTeachesTheCapAtOnce()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy capSpy(&rec, &WorkspaceReconciler::capReached);
    QSignalSpy quietSpy(&rec, &WorkspaceReconciler::structuralOpsSettled);
    rec.onPopulationChanged(id(3), 1);
    QCOMPARE(createSpy.count(), 1);

    // KWin returned from createDesktop and no desktopCreated came first: it
    // declined, which it only does at its maximum. No probe and no waiting.
    rec.onCreateDesktopAnswered(createSpy.first().at(0).toUInt(), createSpy.first().at(1).toString(), true);
    QCOMPARE(rec.desktopCap(), 4);
    QCOMPARE(capSpy.count(), 1);
    QVERIFY(!rec.hasPendingStructuralOps());
    QCOMPARE(quietSpy.count(), 1);
    // And the next want is refused at the gate rather than asked again.
    rec.onPopulationChanged(id(4), 1);
    QCOMPARE(createSpy.count(), 1);
}

void TestWorkspaceReconciler::answers_forALandedCreateChangeNothing()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy createSpy(&rec, &WorkspaceReconciler::requestCreateDesktop);
    QSignalSpy capSpy(&rec, &WorkspaceReconciler::capReached);
    rec.onPopulationChanged(id(3), 1);
    const uint position = createSpy.first().at(0).toUInt();
    const QString name = createSpy.first().at(1).toString();

    // The answer of a create that landed arrives after its echo and its
    // settle. It is the success it looks like.
    rec.onKwinDesktopCreated(id(5));
    rec.onCreateDesktopAnswered(position, name, true);
    QCOMPARE(rec.map().ownerOf(id(5)), QStringLiteral("A"));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)});
    rec.onCreateDesktopAnswered(position, name, true);
    QCOMPARE(rec.desktopCap(), WorkspaceReconciler::DefaultDesktopCap);
    QCOMPARE(capSpy.count(), 0);

    // A stall keeps the create open: its echo can still land.
    rec.onPopulationChanged(id(5), 1);
    QCOMPARE(createSpy.count(), 2);
    rec.onCreateDesktopAnswered(createSpy.last().at(0).toUInt(), createSpy.last().at(1).toString(), false);
    QVERIFY(rec.hasPendingStructuralOps());
    rec.onKwinDesktopCreated(id(6));
    QCOMPARE(rec.map().ownerOf(id(6)), QStringLiteral("A"));
}

void TestWorkspaceReconciler::answers_refusedRemoveIsAbandoned()
{
    WorkspaceReconciler rec;
    rec.setTimeouts(WorkspaceReconciler::LedgerTimeoutMs, WorkspaceTest::FastDebounceMs);
    QVERIFY(adoptTwoScreens(rec));
    rec.onPopulationChanged(id(3), 1);
    rec.onKwinDesktopCreated(id(5));
    rec.onDesktopListSettled({id(1), id(3), id(5), id(2), id(4)});
    QSignalSpy removeSpy(&rec, &WorkspaceReconciler::requestRemoveDesktop);
    QSignalSpy abandonedSpy(&rec, &WorkspaceReconciler::removalAbandoned);
    rec.onPopulationChanged(id(3), 0);
    QTRY_COMPARE_WITH_TIMEOUT(removeSpy.count(), 1, WorkspaceTest::FastDebounceMs * 20);

    // KWin returned and kept d3. The removal is over a timeout sooner than
    // its expiry would say so, and a race snapshot for it is stale.
    rec.onRemoveDesktopAnswered(id(3), true);
    QCOMPARE(abandonedSpy.count(), 1);
    QCOMPARE(abandonedSpy.first().first().toString(), id(3));
    QVERIFY(!rec.hasPendingStructuralOps());
    QCOMPARE(rec.map().ownerOf(id(3)), QStringLiteral("A"));
}

void TestWorkspaceReconciler::kwinRenameFeed_reportsUserNamesAndClosesOurPushes()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("chat"))},
                             {QStringLiteral("chat"), label(2), label(3), label(4)});
    QSignalSpy kwinRenameSpy(&rec, &WorkspaceReconciler::namedWorkspaceRenamedInKWin);
    QSignalSpy renameSpy(&rec, &WorkspaceReconciler::requestSetDesktopName);

    // Renamed in settings: the push goes out, and KWin's echo of it is ours.
    rec.applyNamedWorkspaces({named(QStringLiteral("n1"), QStringLiteral("talk"))},
                             {QStringLiteral("chat"), label(2), label(3), label(4)});
    QCOMPARE(renameSpy.count(), 1);
    rec.onKwinDesktopRenamed(id(1), QStringLiteral("talk"));
    QCOMPARE(kwinRenameSpy.count(), 0);

    // Then the user renames it in the Pager: reported once, with the id.
    rec.onKwinDesktopRenamed(id(1), QStringLiteral("music"));
    rec.onKwinDesktopRenamed(id(1), QStringLiteral("music"));
    QCOMPARE(kwinRenameSpy.count(), 1);
    QCOMPARE(kwinRenameSpy.first().at(0).toString(), QStringLiteral("n1"));
    QCOMPARE(kwinRenameSpy.first().at(2).toString(), QStringLiteral("music"));
    // A positional label is KWin's default coming back, not a user's name.
    rec.onKwinDesktopRenamed(id(1), label(1));
    QCOMPARE(kwinRenameSpy.count(), 1);
}

void TestWorkspaceReconciler::seed_isRecordedButNeverTakenForAnEcho()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec));
    QSignalSpy foreignSpy(&rec, &WorkspaceReconciler::foreignSwitchDetected);

    // Our switch for A to d3 is open. A seed that happens to name d3 is the
    // output's position being re-stated, not KWin performing the switch, so
    // the entry stays open for the real echo.
    QVERIFY(rec.issueSetCurrent(QStringLiteral("A"), id(3)));
    rec.onScreenDesktopSeeded(QStringLiteral("A"), 2);
    QCOMPARE(rec.currentDesktopIdOf(QStringLiteral("A")), id(3));
    QCOMPARE(rec.inFlightSetCurrentTarget(QStringLiteral("A")), id(3));

    // A seed on another screen's desktop is owner-wins material like any
    // report: a replugged output sitting on a desktop it does not own.
    rec.onScreenDesktopSeeded(QStringLiteral("B"), 1);
    QCOMPARE(foreignSpy.count(), 1);
    QCOMPARE(foreignSpy.first().at(0).toString(), QStringLiteral("B"));
}

void TestWorkspaceReconciler::issueSetCurrentReplacing_anEarlierAnswerNamingTheTargetIsNotItsOwn()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // positions: d1=1 d3=2 d2=3 d4=4; A shows d1, owns {d1, d3}

    // next, prev, next before anything answers: each press chains off the
    // one in flight and replaces it, so the entry owes two answers to the
    // presses it replaced before its own.
    const auto press = [&rec](int delta) {
        const QString inFlight = rec.inFlightSetCurrentTarget(QStringLiteral("A"));
        const QString base = inFlight.isEmpty() ? rec.currentDesktopIdOf(QStringLiteral("A")) : inFlight;
        return rec.issueSetCurrentReplacing(QStringLiteral("A"),
                                            rec.desktopIdAtOffsetFrom(QStringLiteral("A"), base, delta));
    };
    QVERIFY(press(1));
    QVERIFY(press(-1));
    QVERIFY(press(1));
    QCOMPARE(rec.inFlightSetCurrentTarget(QStringLiteral("A")), id(3));

    // The first press's answer names d3, the same desktop the last press is
    // for. It is still the first press's answer.
    rec.onScreenDesktopReport(QStringLiteral("A"), 2);
    QCOMPARE(rec.inFlightSetCurrentTarget(QStringLiteral("A")), id(3));
    rec.onScreenDesktopReport(QStringLiteral("A"), 1); // the second press: back to d1
    QCOMPARE(rec.inFlightSetCurrentTarget(QStringLiteral("A")), id(3));
    // Only the last press's own answer retires the entry.
    QVERIFY(rec.onScreenDesktopReport(QStringLiteral("A"), 2));
    QVERIFY(rec.inFlightSetCurrentTarget(QStringLiteral("A")).isEmpty());
}

void TestWorkspaceReconciler::screenRenamed_recordsAlreadyUnderTheNewIdWin()
{
    WorkspaceReconciler rec;
    QVERIFY(adoptTwoScreens(rec)); // positions: d1=1 d3=2 d2=3 d4=4; A shows d1
    const QString renamed = QStringLiteral("A/DP-1");

    // An older switch under the old spelling, then the effect already
    // reporting and being switched under the new one before the rename lands.
    QVERIFY(rec.issueSetCurrent(QStringLiteral("A"), id(3)));
    rec.onScreenDesktopReport(renamed, 2); // d3
    QVERIFY(rec.issueSetCurrent(renamed, id(1)));

    rec.onScreenRenamed(QStringLiteral("A"), renamed);
    // The new id's own records are the fresher ones: its current desktop and
    // its switch. The old id's switch is dropped, not kept beside it, so the
    // screen has one switch open.
    QCOMPARE(rec.map().ownerOf(id(1)), renamed);
    QCOMPARE(rec.currentDesktopIdOf(renamed), id(3));
    QCOMPARE(rec.inFlightSetCurrentTarget(renamed), id(1));
    // The kept switch owes no answer for the dropped one: the new id's
    // switch means the effect had already re-spelled the output, and nothing
    // it sent under the old spelling can arrive after that. Its own first
    // answer under the new id retires it.
    QVERIFY(rec.onScreenDesktopReport(renamed, 1));
    QVERIFY(rec.inFlightSetCurrentTarget(renamed).isEmpty());
}

void TestWorkspaceReconciler::adoption_refreshesStalePositionalLabels()
{
    // KWin labels a desktop once, when it creates it. A desktop removed
    // before the feature came up left the survivors behind it with the label
    // of a position they no longer hold, and adoption is the only pass that
    // sees them until the list next changes.
    WorkspaceReconciler rec;
    rec.onScreenOrderChanged({QStringLiteral("A")});
    QSignalSpy renameSpy(&rec, &WorkspaceReconciler::requestSetDesktopName);
    QHash<QString, QString> current;
    current.insert(QStringLiteral("A"), id(1));
    QVERIFY(rec.adoptAll({id(1), id(3), id(4)}, current, {}, {label(1), label(3), QStringLiteral("music")}));

    // d3 is corrected; the name a user gave d4 is theirs and stays.
    QCOMPARE(renameSpy.count(), 1);
    QCOMPARE(renameSpy.first().at(0).toString(), id(3));
    QCOMPARE(renameSpy.first().at(1).toString(), label(2));
}

QTEST_GUILESS_MAIN(TestWorkspaceReconciler)
#include "test_workspace_reconciler.moc"
