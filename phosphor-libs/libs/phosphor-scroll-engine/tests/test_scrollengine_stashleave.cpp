// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// A window that left the context a mode-round-trip stash remembers gives up
// its column there (F648, F696, F714). The strip on S1 leaves scrolling, which
// stashes its structure; the window then moves to another monitor, desktop or
// activity, and the strip's return must not pull it back into its old column
// at its old width. A window still on the stashed context keeps its column.
// Headless, on the geometry-provider seam like the sibling engine suites.

#include <PhosphorEngine/WindowRegistry.h>
#include <PhosphorScrollEngine/ScrollEngine.h>
#include <PhosphorScrollEngine/ScrollState.h>

#include "scrollstriptestutils.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include <memory>

using namespace PhosphorScrollEngine;

using ScrollTestUtils::makeProviderEngine;
namespace Ax = ScrollTestUtils::Ax;

namespace {

const QString kS1 = QStringLiteral("S1");
const QString kS2 = QStringLiteral("S2");
const QString kA = QStringLiteral("app|a");
const QString kB = QStringLiteral("app|b");
const QString kC = QStringLiteral("app|c");
const QString kD = QStringLiteral("app|d");
/// The default column on the suite's 1200 px main extent.
constexpr int kDefaultMain = 600;
/// One adjustColumnWidth(10.0) on a default column: 600 + 10% of 1200.
constexpr int kWidenedMain = 720;

PhosphorEngine::DesktopSpanQuery spanOn(const QSet<int>& desktops, bool sticky = false)
{
    return [desktops, sticky](const QString&) {
        PhosphorEngine::DesktopSpan span;
        span.known = true;
        span.sticky = sticky;
        span.desktops = desktops;
        return span;
    };
}

/// The window ids the saved blob stashes under (S1, @p desktop).
QStringList savedIds(const ScrollEngine* engine, int desktop)
{
    QStringList ids;
    const QString key = kS1 + QLatin1Char('|') + QString::number(desktop) + QLatin1Char('|');
    const QJsonObject entry = engine->serializeStripState().value(key).toObject();
    for (const QJsonValue& column : entry.value(QLatin1String("columns")).toArray()) {
        for (const QJsonValue& tile : column.toObject().value(QLatin1String("tiles")).toArray()) {
            ids.append(tile.toObject().value(QLatin1String("windowId")).toString());
        }
    }
    return ids;
}

} // namespace

class TestScrollEngineStashLeave : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        AX_GUARD_SUITE();
    }

    // b moves to another monitor while S1 is in another mode. The release
    // the daemon sends off the kept screen gives up its column on S1.
    void leftScreenGivesUpItsColumn()
    {
        QObject owner;
        ScrollEngine* engine = stripWithWidenedB(&owner, {kS1});
        engine->setActiveScreens({});

        engine->releaseWindowOffScreen(kB, kS2);

        reopenS1(engine);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kB)), kDefaultMain);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kA)), kDefaultMain);
    }

    // b opens on another scrolling monitor and c is handed to it: scroll is
    // the arriving engine there, and no release reaches it, so the arrival
    // gives up the column on S1 itself.
    void arrivalOnAnotherScrollScreen()
    {
        QObject owner;
        ScrollEngine* engine = stripWithWidenedB(&owner, {kS1, kS2});
        engine->setCurrentDesktopForScreen(kS2, 1);
        engine->windowFocused(kC, kS1);
        engine->adjustColumnWidth(10.0, kS1);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kC)), kWidenedMain);
        engine->setActiveScreens({kS2});

        engine->windowOpened(kB, kS2, 0, 0);
        PhosphorEngine::IPlacementEngine::HandoffContext ctx;
        ctx.windowId = kC;
        ctx.fromEngineId = QStringLiteral("snap");
        ctx.toScreenId = kS2;
        engine->handoffReceive(ctx);
        // Both come back to S1 while it is still in the other mode.
        engine->handoffRelease(kB);
        engine->handoffRelease(kC);

        engine->setActiveScreens({kS1, kS2});
        reopenS1(engine);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kB)), kDefaultMain);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kC)), kDefaultMain);
    }

    // b moves to desktop 2 while S1 is in another mode, and back. The
    // reconcile for its new desktop set reaches the stash although no strip
    // tracks b any more (F696).
    void leftDesktopGivesUpItsColumn()
    {
        QObject owner;
        ScrollEngine* engine = stripWithWidenedB(&owner, {kS1});
        engine->setActiveScreens({});

        engine->reconcileWindowMemberships(kB, spanOn({2}));
        engine->reconcileWindowMemberships(kB, spanOn({1}));

        reopenS1(engine);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kB)), kDefaultMain);
    }

    // a is still open, on a monitor in another mode. A new window of the same
    // app does not take its stashed column (F648).
    void fuzzyClaimSkipsALiveWindow()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1});
        auto* registry = new PhosphorEngine::WindowRegistry(&owner);
        engine->setWindowRegistry(registry);
        engine->setCurrentDesktopForScreen(kS1, 1);
        engine->windowOpened(QStringLiteral("other|x"), kS1, 0, 0);
        engine->windowOpened(kA, kS1, 0, 0);
        engine->windowFocused(kA, kS1);
        engine->adjustColumnWidth(10.0, kS1);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kA)), kWidenedMain);
        engine->setActiveScreens({});
        PhosphorEngine::WindowMetadata live;
        live.appId = QStringLiteral("app");
        registry->upsert(QStringLiteral("a"), live);

        engine->setActiveScreens({kS1});
        engine->windowOpened(QStringLiteral("other|x"), kS1, 0, 0);
        engine->windowOpened(QStringLiteral("app|n"), kS1, 0, 0);

        QCOMPARE(Ax::mainLen(engine->lastManagedRect(QStringLiteral("app|n"))), kDefaultMain);
    }

    // F714: a sticky window has left no desktop, so it keeps its column.
    void stickyKeepsItsColumn()
    {
        QObject owner;
        ScrollEngine* engine = stripWithWidenedB(&owner, {kS1});
        engine->setActiveScreens({});

        engine->reconcileWindowMemberships(kB, spanOn({}, /*sticky=*/true));

        reopenS1(engine);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kB)), kWidenedMain);
    }

    // F714: b has a stashed column on desktops 1 and 2, widened on both, and
    // is then on desktop 2 only. Desktop 2 keeps its column; desktop 1, which
    // it left, gives its column up.
    void multiDesktopKeepsTheDesktopItIsOn()
    {
        QObject owner;
        ScrollEngine* engine = stripWithWidenedB(&owner, {kS1});
        engine->setActiveScreens({});
        engine->setCurrentDesktopForScreen(kS1, 2);
        engine->setActiveScreens({kS1});
        engine->windowOpened(kB, kS1, 0, 0);
        engine->windowFocused(kB, kS1);
        engine->adjustColumnWidth(10.0, kS1);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kB)), kWidenedMain);
        engine->setActiveScreens({});

        engine->reconcileWindowMemberships(kB, spanOn({2}));

        engine->setActiveScreens({kS1});
        engine->windowOpened(kB, kS1, 0, 0);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kB)), kWidenedMain);
        engine->setActiveScreens({});
        engine->setCurrentDesktopForScreen(kS1, 1);
        reopenS1(engine);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kB)), kDefaultMain);
    }

    // A column given up is not saved, so a restart does not bring it back.
    void givenUpColumnIsNotPersisted()
    {
        QObject owner;
        ScrollEngine* engine = stripWithWidenedB(&owner, {kS1});
        engine->setActiveScreens({});
        QCOMPARE(savedIds(engine, 1), (QStringList{kA, kB, kC}));

        engine->releaseWindowOffScreen(kB, kS2);

        QCOMPARE(savedIds(engine, 1), (QStringList{kA, kC}));
    }

    // A daemon restart keeps window ids, so the staged strip names live
    // windows. b was moved to S2 while the daemon was down: the frame the
    // registry holds for it lies there, and the bring-up prune gives up its
    // column on S1. a is still on S1 and keeps its.
    void restartMoveGivesUpTheStagedColumn()
    {
        QObject owner;
        ScrollEngine* seed = stripWithWidenedB(&owner, {kS1});
        const QJsonObject blob = seed->serializeStripState();
        const QRect s1 = Ax::defaultScreenRect();
        const QRect s2 = s1.translated(s1.width(), 0);
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1}, [s1, s2](const QString& screenId) {
            return screenId == kS2 ? s2 : s1;
        });
        engine->setAllScreenGeometriesProvider([s1, s2]() {
            return QList<QRect>{s1, s2};
        });
        auto* registry = new PhosphorEngine::WindowRegistry(&owner);
        engine->setWindowRegistry(registry);
        registry->upsert(QStringLiteral("a"), frameAt(s1.center()));
        registry->upsert(QStringLiteral("b"), frameAt(s2.center()));
        engine->restoreStripState(blob);
        QCOMPARE(savedIds(engine, 1), (QStringList{kA, kB, kC}));

        engine->pruneStaleWindows({kA, kB, kC});

        QCOMPARE(savedIds(engine, 1), (QStringList{kA, kC}));
    }

    // The move-column-to-workspace verb. a and b share a widened column on
    // desktop 1; the verb records the column for desktop 2 and releases each
    // window, and each re-arrives on its own once S1 shows desktop 2. They
    // land as ONE column at the width it had, opened right of the column in
    // focus there (the default insert position), not one column per window.
    void columnMoveRejoinsAsOneColumn()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1});
        engine->setCurrentDesktopForScreen(kS1, 1);
        for (const QString& windowId : {kA, kB, kC}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kA, kS1);
        engine->consumeWindowIntoColumn(kS1); // b joins a's column
        engine->adjustColumnWidth(10.0, kS1);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kA)), kWidenedMain);
        const auto* source = static_cast<ScrollState*>(engine->stateForScreen(kS1));
        QVERIFY(source && source->strip().activeColumn());
        const ColumnWidth widened = source->strip().activeColumn()->width;
        ScrollEngine::FocusedColumnRefusal reason = ScrollEngine::FocusedColumnRefusal::NotScrolling;
        const QStringList column = engine->focusedColumnWindows(kS1, &reason);
        QCOMPARE(reason, ScrollEngine::FocusedColumnRefusal::None);
        QCOMPARE(column, (QStringList{kA, kB}));

        QVERIFY(engine->stashColumnForDesktopMove(kS1, column, 2));
        for (const QString& windowId : column) {
            engine->handoffRelease(windowId);
        }
        engine->setCurrentDesktopForScreen(kS1, 2);
        const QString resident = QStringLiteral("other|x");
        engine->windowOpened(resident, kS1, 0, 0);
        engine->windowOpened(kA, kS1, 0, 0);
        engine->windowOpened(kB, kS1, 0, 0);

        const int aColumn = engine->columnIndexForWindow(kS1, kA);
        QCOMPARE(aColumn, 1);
        QCOMPARE(engine->columnIndexForWindow(kS1, kB), aColumn);
        QCOMPARE(engine->columnIndexForWindow(kS1, resident), 0);
        // The width INTENT, not the applied rect: two columns overflow the
        // suite's work area, and the apply clamps a straddler at the edge.
        const auto* target = static_cast<ScrollState*>(engine->stateForScreen(kS1));
        QVERIFY(target && target != source);
        QVERIFY(target->strip().columns().at(aColumn).width == widened);
    }

    // The move-workspace-to-monitor verb. Desktop 1's strip on S1 holds a
    // widened [a, b] column and [c], with c in focus; the workspace moves to
    // S2, which keeps the desktop. Each window is released and re-arrives on
    // S2 on its own, in no particular order, and the strip comes back whole:
    // the same columns in the same order and the shared column at its width.
    // d stays behind (it does not ride) and is not recorded.
    void outputMoveRebuildsTheStripOnTheTarget()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1, kS2});
        engine->setCurrentDesktopForScreen(kS1, 1);
        engine->setCurrentDesktopForScreen(kS2, 2);
        const QString kD = QStringLiteral("app|d");
        for (const QString& windowId : {kA, kB, kC, kD}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kA, kS1);
        engine->consumeWindowIntoColumn(kS1); // b joins a's column
        engine->adjustColumnWidth(10.0, kS1);
        const auto* source = static_cast<ScrollState*>(engine->stateForScreen(kS1));
        QVERIFY(source && source->strip().activeColumn());
        const ColumnWidth widened = source->strip().activeColumn()->width;
        engine->windowFocused(kC, kS1);

        // Refusals record nothing.
        QVERIFY(!engine->stashStripForOutputMove(kS1, kS1, 1, {kA}));
        QVERIFY(!engine->stashStripForOutputMove(kS1, kS2, 0, {kA}));
        QVERIFY(!engine->stashStripForOutputMove(kS1, kS2, 1, {QStringLiteral("app|none")}));

        const QStringList riders{kA, kB, kC};
        QVERIFY(engine->stashStripForOutputMove(kS1, kS2, 1, riders));
        for (const QString& windowId : riders) {
            engine->handoffRelease(windowId);
            // What the daemon sends once the window is on the other monitor:
            // the record lives under S2, so it survives.
            engine->releaseWindowOffScreen(windowId, kS2);
        }
        engine->setCurrentDesktopForScreen(kS2, 1);
        engine->windowOpened(kC, kS2, 0, 0);
        engine->windowOpened(kB, kS2, 0, 0);
        engine->windowOpened(kA, kS2, 0, 0);

        const int shared = engine->columnIndexForWindow(kS2, kA);
        QCOMPARE(shared, 0);
        QCOMPARE(engine->columnIndexForWindow(kS2, kB), shared);
        QCOMPARE(engine->columnIndexForWindow(kS2, kC), 1);
        const auto* target = static_cast<ScrollState*>(engine->stateForScreen(kS2));
        QVERIFY(target && target != source);
        QCOMPARE(target->strip().columns().size(), 2);
        QVERIFY(target->strip().columns().at(shared).width == widened);
        QCOMPARE(engine->columnIndexForWindow(kS1, kD), 0);
    }

    // The column read answers for the EXACT strip key. A monitor split into
    // sub-screens has no single column for its physical id, so the physical
    // id is refused even when one of the sub-screens holds focus, and even
    // when only one of them scrolls.
    void focusedColumnAnswersForTheExactScreenOnly()
    {
        const QString left = QStringLiteral("DP-1/vs:0");
        const QString right = QStringLiteral("DP-1/vs:1");
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {left, right});
        engine->windowOpened(kA, left, 0, 0);
        engine->windowOpened(kB, right, 0, 0);
        engine->windowFocused(kA, left);

        ScrollEngine::FocusedColumnRefusal reason = ScrollEngine::FocusedColumnRefusal::None;
        QVERIFY(engine->focusedColumnWindows(QStringLiteral("DP-1"), &reason).isEmpty());
        QCOMPARE(reason, ScrollEngine::FocusedColumnRefusal::NotScrolling);
        QCOMPARE(engine->focusedColumnWindows(left), QStringList{kA});
        QCOMPARE(engine->focusedColumnWindows(right), QStringList{kB});
        QVERIFY(!engine->stashColumnForDesktopMove(QStringLiteral("DP-1"), {kA}, 2));

        engine->setActiveScreens({left});
        QVERIFY(engine->focusedColumnWindows(QStringLiteral("DP-1"), &reason).isEmpty());
        QCOMPARE(reason, ScrollEngine::FocusedColumnRefusal::NotScrolling);
    }

    // A column's minimized member is out of the strip, floating, and still
    // part of the column: it travels with it, at its slot. A member the user
    // floated left the column and does not.
    void focusedColumnCarriesItsMinimizedMemberOnly()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1});
        auto* registry = new PhosphorEngine::WindowRegistry(&owner);
        engine->setWindowRegistry(registry);
        engine->setCurrentDesktopForScreen(kS1, 1);
        const QString kD = QStringLiteral("app|d");
        for (const QString& windowId : {kA, kB, kC, kD}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kA, kS1);
        engine->consumeWindowIntoColumn(kS1); // b joins a's column
        engine->consumeWindowIntoColumn(kS1); // and c after it
        engine->windowFocused(kA, kS1);
        PhosphorEngine::WindowMetadata minimized;
        minimized.appId = QStringLiteral("app");
        minimized.isMinimized = true;
        registry->upsert(QStringLiteral("b"), minimized);
        PhosphorEngine::WindowMetadata shown;
        shown.appId = QStringLiteral("app");
        shown.isMinimized = false;
        registry->upsert(QStringLiteral("c"), shown);
        engine->setWindowFloat(kB, true, kS1);
        engine->setWindowFloat(kC, true, kS1);
        auto* state = static_cast<ScrollState*>(engine->stateForScreen(kS1));
        QVERIFY(state);
        QVERIFY(!state->floatingHasFocus());
        QVERIFY(state->isFloating(kB));
        QVERIFY(state->isFloating(kC));

        QCOMPARE(engine->focusedColumnWindows(kS1), (QStringList{kA, kB}));
    }

    // A refused move-column-to-workspace (C41). The column is recorded for
    // desktop 2 before the move, the move does not happen, and the windows
    // are placed again on desktop 1. A later move of a alone to desktop 2
    // lands it as a fresh column at the default width: the record of the
    // refused move went when a arrived somewhere else.
    void refusedColumnMoveLeavesNoRecord()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1});
        engine->setCurrentDesktopForScreen(kS1, 1);
        for (const QString& windowId : {kA, kB, kC}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kA, kS1);
        engine->consumeWindowIntoColumn(kS1); // b joins a's column
        engine->adjustColumnWidth(10.0, kS1);
        const auto* source = static_cast<ScrollState*>(engine->stateForScreen(kS1));
        QVERIFY(source && source->strip().activeColumn());
        const ColumnWidth widened = source->strip().activeColumn()->width;
        QVERIFY(engine->stashColumnForDesktopMove(kS1, {kA, kB}, 2));

        // Refused: both windows are released and placed again where they are.
        for (const QString& windowId : {kA, kB}) {
            engine->handoffRelease(windowId);
            engine->windowOpened(windowId, kS1, 0, 0);
        }

        engine->handoffRelease(kA);
        engine->setCurrentDesktopForScreen(kS1, 2);
        engine->windowOpened(kA, kS1, 0, 0);

        const auto* target = static_cast<ScrollState*>(engine->stateForScreen(kS1));
        QVERIFY(target && target != source);
        const int aColumn = engine->columnIndexForWindow(kS1, kA);
        QVERIFY(aColumn >= 0);
        QVERIFY2(!(target->strip().columns().at(aColumn).width == widened),
                 "the refused move's record must not size the later arrival");
    }

    // A column's minimized members go back in the order they left (C43). b
    // and then c leave a's column from the same slot, so the column they
    // return to reads a, b, c, not the order of their ids.
    void hiddenMembersReturnInLeaveOrder()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1});
        auto* registry = new PhosphorEngine::WindowRegistry(&owner);
        engine->setWindowRegistry(registry);
        engine->setCurrentDesktopForScreen(kS1, 1);
        for (const QString& windowId : {kA, kB, kC}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kA, kS1);
        engine->consumeWindowIntoColumn(kS1); // b joins a's column
        engine->consumeWindowIntoColumn(kS1); // and c after it
        engine->windowFocused(kA, kS1);
        PhosphorEngine::WindowMetadata minimized;
        minimized.appId = QStringLiteral("app");
        minimized.isMinimized = true;
        registry->upsert(QStringLiteral("b"), minimized);
        registry->upsert(QStringLiteral("c"), minimized);
        // b leaves slot 1 of [a, b, c], then c leaves slot 1 of [a, c].
        engine->setWindowFloat(kB, true, kS1);
        engine->setWindowFloat(kC, true, kS1);
        auto* state = static_cast<ScrollState*>(engine->stateForScreen(kS1));
        QVERIFY(state);
        QVERIFY(state->isFloating(kB));
        QVERIFY(state->isFloating(kC));
        QVERIFY(!state->floatingHasFocus());

        QCOMPARE(engine->focusedColumnWindows(kS1), (QStringList{kA, kB, kC}));
    }

    // A moved column whose shown tab stays behind falls back to the first
    // tile that is on the strip, never to a hidden member (C45). c is the
    // shown tab and does not move; a, minimized, is the column's first
    // member and comes back floating, so the record names b.
    void movedColumnFallsBackToALiveTab()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1});
        auto* registry = new PhosphorEngine::WindowRegistry(&owner);
        engine->setWindowRegistry(registry);
        engine->setCurrentDesktopForScreen(kS1, 1);
        for (const QString& windowId : {kA, kB, kC}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kA, kS1);
        engine->consumeWindowIntoColumn(kS1); // b joins a's column
        engine->consumeWindowIntoColumn(kS1); // and c after it
        engine->windowFocused(kC, kS1);
        PhosphorEngine::WindowMetadata minimized;
        minimized.appId = QStringLiteral("app");
        minimized.isMinimized = true;
        registry->upsert(QStringLiteral("a"), minimized);
        engine->setWindowFloat(kA, true, kS1);
        QCOMPARE(engine->focusedColumnWindows(kS1), (QStringList{kA, kB, kC}));

        QVERIFY(engine->stashColumnForDesktopMove(kS1, {kA, kB}, 2));
        engine->handoffRelease(kA);
        engine->handoffRelease(kB);

        const QJsonObject entry = engine->serializeStripState().value(kS1 + QLatin1String("|2|")).toObject();
        const QJsonArray columns = entry.value(QLatin1String("columns")).toArray();
        QCOMPARE(columns.size(), 1);
        QCOMPARE(columns.at(0).toObject().value(QLatin1String("activeWindow")).toString(), kB);
    }

    // A strip moved to a monitor whose work area is as long as its own keeps
    // the view the user had panned to. On a monitor of another length the
    // anchor would name another view, so the focus goes alone and the policy
    // places the view (C44). The record then carries no view at all: with
    // the focus on the last column, the rebuilt strip holds the anchor
    // focusWindow derives for it, not a zero that would put that column at
    // the leading edge with nothing behind it (C72).
    void outputMoveCarriesThePannedViewOnlyToAnEqualLength()
    {
        const QRect shorter = Ax::t(QRect(0, 0, 900, ScrollTestUtils::kCrossExtent));
        const MovedView sameLength = movedStripView(Ax::defaultScreenRect(), kA, 300);
        QVERIFY(sameLength.built);
        QCOMPARE(sameLength.detached, true);
        const MovedView otherLength = movedStripView(shorter, kA, 300);
        QVERIFY(otherLength.built);
        QCOMPARE(otherLength.detached, false);

        const MovedView lastFocused = movedStripView(shorter, kD, 0);
        QVERIFY(lastFocused.built);
        QCOMPARE(lastFocused.focused, kD);
        // The derived view keeps the last column off the leading edge, so the
        // compare below cannot pass on a zero anchor.
        QVERIFY(lastFocused.derivedAnchor != 0);
        QCOMPARE(lastFocused.anchor, lastFocused.derivedAnchor);

        // The same when the focused window lands FIRST (C94). Its own arrival
        // derives a view on a one-column strip, and every later arrival hands
        // that view back over a longer strip: kept raw, it would leave the
        // last column at the leading edge with dead space after it.
        const MovedView focusedFirst = movedStripView(shorter, kD, 0, {kD, kA, kB, kC});
        QVERIFY(focusedFirst.built);
        QCOMPARE(focusedFirst.focused, kD);
        QVERIFY(focusedFirst.derivedAnchor != 0);
        QCOMPARE(focusedFirst.anchor, focusedFirst.derivedAnchor);
    }

    // A late exact-id claim on a record that carries no view hands back the
    // view the user has now. The user centered the last column, which leaves
    // the view past the strip's trailing end on purpose, and the claim that
    // opens a column before it keeps that overhang rather than pulling the
    // column flush to the trailing edge (C115).
    void lateClaimKeepsACenteredLastColumn()
    {
        QObject owner;
        const QRect shorter = Ax::t(QRect(0, 0, 900, ScrollTestUtils::kCrossExtent));
        ScrollEngine* engine = movedStripEngine(&owner, shorter, kD);
        QVERIFY(engine);
        for (const QString& windowId : {kA, kB, kD}) {
            engine->windowOpened(windowId, kS2, 0, 0);
        }
        const auto* target = static_cast<ScrollState*>(engine->stateForScreen(kS2));
        QVERIFY(target);
        QCOMPARE(target->strip().columnCount(), 3);
        QCOMPARE(target->strip().activeWindowId(), kD);
        engine->centerColumn(kS2);
        const int centered = target->strip().viewAnchor();
        // Centered on a 900 px view: the 450 px column starts 225 px in.
        QCOMPARE(centered, 225);

        engine->windowOpened(kC, kS2, 0, 0); // claims its slot before d
        QCOMPARE(target->strip().columnCount(), 4);
        QCOMPARE(engine->columnIndexForWindow(kS2, kC), 2);
        QCOMPARE(target->strip().activeWindowId(), kD);
        QCOMPARE(target->strip().viewAnchor(), centered);
    }

    // The lone-column rule centers the focused window's column while it is
    // the only one, which leaves the view before the strip's leading edge.
    // Once its neighbours land the rule no longer holds, and the rebuilt
    // strip shows the first column at the leading edge instead of keeping
    // the dead space before it (C116).
    void loneColumnCenteringEndsWhenNeighboursLand()
    {
        QObject owner;
        const QRect shorter = Ax::t(QRect(0, 0, 900, ScrollTestUtils::kCrossExtent));
        ScrollEngine* engine = movedStripEngine(&owner, shorter, kA);
        QVERIFY(engine);
        QVariantMap loneCentered;
        loneCentered.insert(ScrollPerScreenKeys::alwaysCenterSingleColumn(), true);
        engine->applyPerScreenConfig(kS2, loneCentered);
        engine->windowOpened(kA, kS2, 0, 0);
        const auto* target = static_cast<ScrollState*>(engine->stateForScreen(kS2));
        QVERIFY(target);
        QVERIFY2(target->strip().viewAnchor() > 0, "precondition: the lone column is centered");
        for (const QString& windowId : {kB, kC, kD}) {
            engine->windowOpened(windowId, kS2, 0, 0);
        }
        QCOMPARE(target->strip().columnCount(), 4);
        QCOMPARE(target->strip().activeWindowId(), kA);
        QCOMPARE(target->strip().viewAnchor(), 0);
    }

    // Under the Always policy a view the user panned is DETACHED, and the
    // policy never re-centers it. Its overhang before the first column is the
    // pan's, so a late claim keeps it as it keeps any user overhang, rather
    // than clamping it as the policy's (C125).
    void lateClaimKeepsAPannedOverhangUnderAlways()
    {
        QObject owner;
        const QRect shorter = Ax::t(QRect(0, 0, 900, ScrollTestUtils::kCrossExtent));
        ScrollEngine* engine = movedStripEngine(&owner, shorter, kA);
        QVERIFY(engine);
        QVariantMap always;
        always.insert(ScrollPerScreenKeys::centerFocusedColumn(), static_cast<int>(CenterFocusedColumn::Always));
        engine->applyPerScreenConfig(kS2, always);
        for (const QString& windowId : {kA, kB, kD}) {
            engine->windowOpened(windowId, kS2, 0, 0);
        }
        const auto* target = static_cast<ScrollState*>(engine->stateForScreen(kS2));
        QVERIFY(target);
        QCOMPARE(target->strip().columnCount(), 3);
        QCOMPARE(target->strip().activeWindowId(), kA);
        // Centered on a 900 px view: the 450 px first column starts 225 px in.
        QCOMPARE(target->strip().viewAnchor(), 225);
        // A partial pan back toward the strip: still 125 px of dead space.
        engine->scrollViewByPx(100, kS2);
        QVERIFY(target->strip().viewDetached());
        const int panned = target->strip().viewAnchor();
        QCOMPARE(panned, 125);

        engine->windowOpened(kC, kS2, 0, 0); // claims its slot before d
        QCOMPARE(target->strip().columnCount(), 4);
        QCOMPARE(target->strip().activeWindowId(), kA);
        QVERIFY(target->strip().viewDetached());
        QCOMPARE(target->strip().viewAnchor(), panned);
    }

    // A mode round trip across a rotation drops the stashed view, which was
    // captured on the other axis. When the focus lands first the rebuilt
    // strip still ends on the view the policy derives for it, as a record
    // with no view does (C94).
    void rotatedRoundTripDerivesTheViewWhenTheFocusLandsFirst()
    {
        QObject owner;
        auto geometry = std::make_shared<QRect>(Ax::defaultScreenRect());
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1}, [geometry](const QString&) {
            return *geometry;
        });
        engine->setCurrentDesktopForScreen(kS1, 1);
        for (const QString& windowId : {kA, kB, kC, kD}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kD, kS1);
        engine->setActiveScreens({});

        // Rotated: the same main-axis length, on the other axis.
        *geometry = Ax::t(QRect(0, 0, ScrollTestUtils::kCrossExtent, ScrollTestUtils::kMainExtent));
        engine->setActiveScreens({kS1});
        for (const QString& windowId : {kD, kA, kB, kC}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        const auto* state = static_cast<ScrollState*>(engine->stateForScreen(kS1));
        QVERIFY(state && state->hasResolvedAxis());
        const StripAxis rotated = Ax::stripAxis().transposed();
        QCOMPARE(state->resolvedAxis().axis(), rotated.axis());
        QCOMPARE(state->strip().columnCount(), 4);
        QCOMPARE(state->strip().activeWindowId(), kD);
        const int derived = derivedAnchorFor(state->strip(), *geometry, kD, rotated);
        QVERIFY(derived != 0);
        QCOMPARE(state->strip().viewAnchor(), derived);
    }

    // A record that carries no view keeps carrying none across a restart.
    // The save writes no anchor for it, and the staged entry restores the
    // focus with the view the policy derives, as the in-session record does
    // (C72).
    void outputMoveRecordWithoutAViewSurvivesARestart()
    {
        QObject owner;
        const QRect shorter = Ax::t(QRect(0, 0, 900, ScrollTestUtils::kCrossExtent));
        const auto geometry = [shorter](const QString& screenId) {
            return screenId == kS2 ? shorter : Ax::defaultScreenRect();
        };
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1, kS2}, geometry);
        engine->setCurrentDesktopForScreen(kS1, 1);
        engine->setCurrentDesktopForScreen(kS2, 2);
        const QStringList riders{kA, kB, kC, kD};
        for (const QString& windowId : riders) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kD, kS1);
        QVERIFY(engine->stashStripForOutputMove(kS1, kS2, 1, riders));
        for (const QString& windowId : riders) {
            engine->handoffRelease(windowId);
            engine->releaseWindowOffScreen(windowId, kS2);
        }
        const QJsonObject blob = engine->serializeStripState();
        const QJsonObject entry = blob.value(kS2 + QLatin1String("|1|")).toObject();
        QCOMPARE(entry.value(QLatin1String("columns")).toArray().size(), 4);
        QCOMPARE(entry.value(QLatin1String("focusedWindow")).toString(), kD);
        QVERIFY(!entry.contains(QLatin1String("viewAnchor")));

        ScrollEngine* restarted = makeProviderEngine(&owner, {kS1, kS2}, geometry);
        restarted->setCurrentDesktopForScreen(kS1, 1);
        restarted->setCurrentDesktopForScreen(kS2, 1);
        restarted->restoreStripState(blob);
        for (const QString& windowId : riders) {
            restarted->windowOpened(windowId, kS2, 0, 0);
        }
        const auto* target = static_cast<ScrollState*>(restarted->stateForScreen(kS2));
        QVERIFY(target);
        QCOMPARE(target->strip().columnCount(), 4);
        QCOMPARE(target->strip().activeWindowId(), kD);
        const int derived = derivedAnchorFor(target->strip(), shorter, kD);
        QVERIFY(derived != 0);
        QCOMPARE(target->strip().viewAnchor(), derived);
    }

    // The equal-length gate measures the target side in the context the strip
    // lands in, not the one the target monitor shows at the press (C73). S2
    // shows desktop 2, whose outer gaps shorten its work area, and the strip
    // moves to S2's desktop 1, which has none and is as long as S1's.
    void outputMoveMeasuresTheDestinationContext()
    {
        int s2Shown = 2;
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1, kS2});
        engine->setContextGapProvider([&s2Shown](const QString& screenId, int desktop, const QString&) {
            QVariantMap gaps;
            if (screenId == kS2 && (desktop > 0 ? desktop : s2Shown) == 2) {
                gaps.insert(QStringLiteral("OuterGap"), 50);
            }
            return gaps;
        });
        engine->setCurrentDesktopForScreen(kS1, 1);
        engine->setCurrentDesktopForScreen(kS2, 2);
        const QStringList riders{kA, kB, kC, kD};
        for (const QString& windowId : riders) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kA, kS1);
        engine->scrollViewByPx(300, kS1);
        const auto* source = static_cast<ScrollState*>(engine->stateForScreen(kS1));
        QVERIFY(source && source->strip().viewDetached());
        QVERIFY(engine->stashStripForOutputMove(kS1, kS2, 1, riders));
        for (const QString& windowId : riders) {
            engine->handoffRelease(windowId);
            engine->releaseWindowOffScreen(windowId, kS2);
        }

        s2Shown = 1;
        engine->setCurrentDesktopForScreen(kS2, 1);
        for (const QString& windowId : riders) {
            engine->windowOpened(windowId, kS2, 0, 0);
        }
        const auto* target = static_cast<ScrollState*>(engine->stateForScreen(kS2));
        QVERIFY(target);
        QCOMPARE(target->strip().columnCount(), 4);
        QVERIFY2(target->strip().viewDetached(), "the panned view must reach a destination of the same length");
    }

    // The destination desktop has never been shown on S2, so no override map
    // names it yet. The gate reads S2's screen-level rules from the desktop
    // S2 shows, here smart gaps, which drop the outer gaps around a lone
    // column. The global defaults would keep them and read a shorter work
    // area than the source's, and the view would be dropped (C95).
    void outputMoveReadsTheTargetRulesForAnUnseenDesktop()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1, kS2});
        engine->setContextGapProvider([](const QString&, int, const QString&) {
            QVariantMap gaps;
            gaps.insert(QStringLiteral("OuterGap"), 50);
            return gaps;
        });
        engine->setCurrentDesktopForScreen(kS1, 1);
        engine->setCurrentDesktopForScreen(kS2, 2);
        QVariantMap smartGaps;
        smartGaps.insert(ScrollPerScreenKeys::smartGaps(), true);
        engine->applyPerScreenConfig(kS1, smartGaps);
        engine->applyPerScreenConfig(kS2, smartGaps);
        engine->windowOpened(kA, kS1, 0, 0);
        QVERIFY(engine->stashStripForOutputMove(kS1, kS2, 1, {kA}));
        engine->handoffRelease(kA);
        engine->releaseWindowOffScreen(kA, kS2);

        const QJsonObject entry = engine->serializeStripState().value(kS2 + QLatin1String("|1|")).toObject();
        QCOMPARE(entry.value(QLatin1String("focusedWindow")).toString(), kA);
        QVERIFY2(entry.contains(QLatin1String("viewAnchor")), "a destination of the same length carries the view");
    }

    // A refused workspace move re-homes its window into the background
    // desktop it was released from. That desktop scrolls while the one S1
    // shows does not, and the receive is judged by the receiving desktop's
    // own mode, so the window is held there again instead of by no engine
    // (C101). A desktop that does not scroll by the daemon's resolution, or
    // the desktop in view of a screen this engine does not scroll, still
    // refuses it.
    void backgroundReceiveFollowsTheReceivingDesktopsMode()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1});
        engine->setCurrentDesktopForScreen(kS1, 2);
        engine->windowOpened(kA, kS1, 0, 0);
        engine->handoffRelease(kA);
        QVERIFY(!engine->isWindowTracked(kA));
        engine->setCurrentDesktopForScreen(kS1, 1);
        engine->setActiveScreens({});

        PhosphorEngine::IPlacementEngine::HandoffContext ctx;
        ctx.windowId = kA;
        ctx.fromEngineId = QStringLiteral("snap");
        ctx.toScreenId = kS1;

        // The desktop in view does not scroll: refused.
        engine->handoffReceive(ctx);
        QVERIFY(!engine->isWindowTracked(kA));

        // Desktop 2 is given another mode: refused.
        ctx.toDesktop = 2;
        engine->setScrollingModeResolver([](const QString&, int desktop, const QString&) {
            return desktop != 2;
        });
        engine->handoffReceive(ctx);
        QVERIFY(!engine->isWindowTracked(kA));

        // Desktop 2 scrolls: held there.
        engine->setScrollingModeResolver([](const QString&, int, const QString&) {
            return true;
        });
        engine->handoffReceive(ctx);
        const std::optional<PhosphorEngine::PlacementStateKey> held = engine->heldKeyForWindow(kA);
        QVERIFY(held.has_value());
        QCOMPARE(held->screenId, kS1);
        QCOMPARE(held->desktop, 2);
    }

    // A background desktop of a screen this engine does not scroll has no
    // strip unless one was kept from when it scrolled, and none is built for
    // it. A receive into such a desktop is refused before it gives up
    // anything the window still has elsewhere: its stashed column on S2 is
    // still there when S2 scrolls again (C117).
    void refusedBackgroundReceiveKeepsTheWindowsRecords()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1, kS2});
        engine->setCurrentDesktopForScreen(kS1, 1);
        engine->setCurrentDesktopForScreen(kS2, 1);
        engine->windowOpened(kA, kS2, 0, 0);
        engine->windowOpened(kB, kS2, 0, 0);
        engine->windowFocused(kA, kS2);
        engine->adjustColumnWidth(10.0, kS2);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kA)), kWidenedMain);
        engine->setActiveScreens({});

        PhosphorEngine::IPlacementEngine::HandoffContext ctx;
        ctx.windowId = kA;
        ctx.fromEngineId = QStringLiteral("snap");
        ctx.toScreenId = kS1;
        ctx.toDesktop = 3;
        engine->handoffReceive(ctx);
        QVERIFY(!engine->isWindowTracked(kA));

        engine->setActiveScreens({kS2});
        engine->windowOpened(kA, kS2, 0, 0);
        engine->windowOpened(kB, kS2, 0, 0);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kA)), kWidenedMain);
    }

    // A window on two desktops is adopted into the second while its moved
    // column is still waiting for a column-mate on the first. The adoption is
    // no arrival elsewhere for that record: the window still holds the
    // desktop the move went to, and its tile there is how the column-mate
    // finds the column (C74).
    void adoptionKeepsTheMoveRecordOfAHeldDesktop()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1});
        engine->setCurrentDesktopForScreen(kS1, 1);
        for (const QString& windowId : {kA, kB, kC}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kA, kS1);
        engine->consumeWindowIntoColumn(kS1); // b joins a's column
        QVERIFY(engine->stashColumnForDesktopMove(kS1, {kA, kB}, 2));
        engine->handoffRelease(kA);
        engine->handoffRelease(kB);

        // a lands on desktop 2 first, then also goes on desktop 1, where the
        // membership pass adopts it.
        engine->setCurrentDesktopForScreen(kS1, 2);
        engine->windowOpened(kA, kS1, 0, 0);
        engine->setCurrentDesktopForScreen(kS1, 1);
        const PhosphorEngine::MembershipReconcileResult adopted =
            engine->reconcileWindowMemberships(kA, spanOn({1, 2}));
        QCOMPARE(adopted.adopted.size(), 1);

        // b lands on desktop 2 and joins a's column there.
        engine->setCurrentDesktopForScreen(kS1, 2);
        engine->windowOpened(kB, kS1, 0, 0);
        const int aColumn = engine->columnIndexForWindow(kS1, kA);
        QVERIFY(aColumn >= 0);
        QCOMPARE(engine->columnIndexForWindow(kS1, kB), aColumn);
    }

    // The move record is persisted as one (C77). A move still pending at a
    // restart that keeps the window ids is refused after it, and the arrival
    // back on the source desktop gives the record up as it does in session:
    // a later move of a alone lands at the default width.
    void refusedColumnMoveLeavesNoRecordAcrossARestart()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1});
        engine->setCurrentDesktopForScreen(kS1, 1);
        for (const QString& windowId : {kA, kB, kC}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kA, kS1);
        engine->consumeWindowIntoColumn(kS1); // b joins a's column
        engine->adjustColumnWidth(10.0, kS1);
        QCOMPARE(Ax::mainLen(engine->lastManagedRect(kA)), kWidenedMain);
        QVERIFY(engine->stashColumnForDesktopMove(kS1, {kA, kB}, 2));
        engine->handoffRelease(kA);
        engine->handoffRelease(kB);
        const QJsonObject blob = engine->serializeStripState();
        const QJsonArray recorded =
            blob.value(kS1 + QLatin1String("|2|")).toObject().value(QLatin1String("columns")).toArray();
        QCOMPARE(recorded.size(), 1);
        QVERIFY(recorded.at(0).toObject().value(QLatin1String("arrivesAsMove")).toBool(false));

        ScrollEngine* restarted = makeProviderEngine(&owner, {kS1});
        restarted->setCurrentDesktopForScreen(kS1, 1);
        restarted->restoreStripState(blob);
        // Refused: both windows are placed again on desktop 1.
        restarted->windowOpened(kC, kS1, 0, 0);
        restarted->windowOpened(kA, kS1, 0, 0);
        restarted->windowOpened(kB, kS1, 0, 0);

        restarted->handoffRelease(kA);
        restarted->setCurrentDesktopForScreen(kS1, 2);
        restarted->windowOpened(kA, kS1, 0, 0);
        QCOMPARE(Ax::mainLen(restarted->lastManagedRect(kA)), kDefaultMain);
    }

    // A strip that leaves scrolling a second time before every window came
    // back folds the pending tiles into the fresh capture. A pending tile
    // whose column-mate did come back stays in that column-mate's column, so
    // its late arrival rejoins it rather than opening a column of its own.
    void secondLeaveKeepsAPendingTileWithItsColumnMate()
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1});
        engine->setCurrentDesktopForScreen(kS1, 1);
        for (const QString& windowId : {kA, kB, kC}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kA, kS1);
        engine->consumeWindowIntoColumn(kS1); // b joins a's column
        engine->setActiveScreens({});
        engine->setActiveScreens({kS1});
        engine->windowOpened(kA, kS1, 0, 0); // only a comes back
        engine->setActiveScreens({});

        reopenS1(engine);

        QCOMPARE(engine->columnIndexForWindow(kS1, kB), engine->columnIndexForWindow(kS1, kA));
        QVERIFY(engine->columnIndexForWindow(kS1, kC) != engine->columnIndexForWindow(kS1, kA));
    }

private:
    /// What a strip moved to another monitor holds once its windows re-arrived.
    struct MovedView
    {
        bool built = false;
        bool detached = false;
        QString focused;
        int anchor = 0;
        /// The anchor focusWindow derives on the rebuilt strip for the
        /// focused window, reached from the first column (derivedAnchorFor).
        int derivedAnchor = 0;
    };

    /// The anchor focusWindow derives on a copy of @p strip, laid out on
    /// @p targetRect, for @p windowId reached from the first column, whose
    /// view sits at the strip's start whatever came before.
    static int derivedAnchorFor(const ScrollStrip& strip, const QRect& targetRect, const QString& windowId,
                                StripAxis axis = Ax::stripAxis())
    {
        ScrollLayoutParams params = ScrollTestUtils::engineParams();
        params.workArea = targetRect;
        params.axis = axis;
        ScrollStrip probe = strip;
        probe.focusWindow(kA, params);
        probe.focusWindow(windowId, params);
        return probe.viewAnchor();
    }

    /// A four-column strip on S1 focused on @p focused, recorded for S2's
    /// desktop 1 on an S2 of @p targetRect and released, with S2 then showing
    /// desktop 1. Nothing has arrived on S2 yet. Null when nothing was
    /// recorded.
    static ScrollEngine* movedStripEngine(QObject* owner, const QRect& targetRect, const QString& focused)
    {
        ScrollEngine* engine = makeProviderEngine(owner, {kS1, kS2}, [targetRect](const QString& screenId) {
            return screenId == kS2 ? targetRect : Ax::defaultScreenRect();
        });
        engine->setCurrentDesktopForScreen(kS1, 1);
        engine->setCurrentDesktopForScreen(kS2, 2);
        const QStringList riders{kA, kB, kC, kD};
        for (const QString& windowId : riders) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(focused, kS1);
        if (!engine->stashStripForOutputMove(kS1, kS2, 1, riders)) {
            qWarning("movedStripEngine: nothing was recorded");
            return nullptr;
        }
        for (const QString& windowId : riders) {
            engine->handoffRelease(windowId);
            engine->releaseWindowOffScreen(windowId, kS2);
        }
        engine->setCurrentDesktopForScreen(kS2, 1);
        return engine;
    }

    /// A four-column strip on S1 focused on @p focused, its view panned by
    /// @p panPx (0 for none), moved as a workspace to an S2 of @p targetRect,
    /// its windows re-arriving there in @p arrivals order (strip order when
    /// empty).
    static MovedView movedStripView(const QRect& targetRect, const QString& focused, int panPx,
                                    const QStringList& arrivals = {})
    {
        MovedView view;
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1, kS2}, [targetRect](const QString& screenId) {
            return screenId == kS2 ? targetRect : Ax::defaultScreenRect();
        });
        engine->setCurrentDesktopForScreen(kS1, 1);
        engine->setCurrentDesktopForScreen(kS2, 2);
        const QStringList riders{kA, kB, kC, kD};
        for (const QString& windowId : riders) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(focused, kS1);
        if (panPx != 0) {
            engine->scrollViewByPx(panPx, kS1);
            const auto* source = static_cast<ScrollState*>(engine->stateForScreen(kS1));
            if (!source || !source->strip().viewDetached()) {
                qWarning("movedStripView: the pan did not detach the source view");
                return view;
            }
        }
        if (!engine->stashStripForOutputMove(kS1, kS2, 1, riders)) {
            qWarning("movedStripView: nothing was recorded");
            return view;
        }
        for (const QString& windowId : riders) {
            engine->handoffRelease(windowId);
            engine->releaseWindowOffScreen(windowId, kS2);
        }
        engine->setCurrentDesktopForScreen(kS2, 1);
        for (const QString& windowId : arrivals.isEmpty() ? riders : arrivals) {
            engine->windowOpened(windowId, kS2, 0, 0);
        }
        const auto* target = static_cast<ScrollState*>(engine->stateForScreen(kS2));
        if (!target || target->strip().columnCount() != riders.size()) {
            qWarning("movedStripView: the strip was not rebuilt on the target");
            return view;
        }
        view.built = true;
        view.detached = target->strip().viewDetached();
        view.focused = target->strip().activeWindowId();
        view.anchor = target->strip().viewAnchor();
        view.derivedAnchor = derivedAnchorFor(target->strip(), targetRect, view.focused);
        return view;
    }

    /// a, b and c on desktop 1 of S1, b's column widened.
    static ScrollEngine* stripWithWidenedB(QObject* owner, const QSet<QString>& screens)
    {
        ScrollEngine* engine = makeProviderEngine(owner, screens);
        engine->setCurrentDesktopForScreen(kS1, 1);
        for (const QString& windowId : {kA, kB, kC}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kB, kS1);
        engine->adjustColumnWidth(10.0, kS1);
        return engine;
    }

    /// S1 back in scrolling, its three windows announced again.
    static void reopenS1(ScrollEngine* engine)
    {
        if (!engine->isActiveOnScreen(kS1)) {
            engine->setActiveScreens({kS1});
        }
        for (const QString& windowId : {kA, kB, kC}) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
    }

    /// Metadata whose frame is centred on @p centre.
    static PhosphorEngine::WindowMetadata frameAt(const QPoint& centre)
    {
        PhosphorEngine::WindowMetadata meta;
        meta.appId = QStringLiteral("app");
        meta.width = 200;
        meta.height = 100;
        meta.positionX = centre.x() - 100;
        meta.positionY = centre.y() - 50;
        return meta;
    }
};

QTEST_GUILESS_MAIN(TestScrollEngineStashLeave)
#include "test_scrollengine_stashleave.moc"
