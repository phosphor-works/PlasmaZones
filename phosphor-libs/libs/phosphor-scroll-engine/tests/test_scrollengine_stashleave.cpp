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

using namespace PhosphorScrollEngine;

using ScrollTestUtils::makeProviderEngine;
namespace Ax = ScrollTestUtils::Ax;

namespace {

const QString kS1 = QStringLiteral("S1");
const QString kS2 = QStringLiteral("S2");
const QString kA = QStringLiteral("app|a");
const QString kB = QStringLiteral("app|b");
const QString kC = QStringLiteral("app|c");
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
    // places the view (C44).
    void outputMoveCarriesThePannedViewOnlyToAnEqualLength()
    {
        QCOMPARE(movedStripViewDetached(Ax::defaultScreenRect()), true);
        QCOMPARE(movedStripViewDetached(Ax::t(QRect(0, 0, 900, ScrollTestUtils::kCrossExtent))), false);
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
    /// A four-column strip on S1 with the view panned off its focus, moved as
    /// a workspace to an S2 of @p targetRect, its windows re-arriving there.
    /// Whether the rebuilt strip still holds the panned (detached) view.
    static bool movedStripViewDetached(const QRect& targetRect)
    {
        QObject owner;
        ScrollEngine* engine = makeProviderEngine(&owner, {kS1, kS2}, [targetRect](const QString& screenId) {
            return screenId == kS2 ? targetRect : Ax::defaultScreenRect();
        });
        engine->setCurrentDesktopForScreen(kS1, 1);
        engine->setCurrentDesktopForScreen(kS2, 2);
        const QString kD = QStringLiteral("app|d");
        const QStringList riders{kA, kB, kC, kD};
        for (const QString& windowId : riders) {
            engine->windowOpened(windowId, kS1, 0, 0);
        }
        engine->windowFocused(kA, kS1);
        engine->scrollViewByPx(300, kS1);
        const auto* source = static_cast<ScrollState*>(engine->stateForScreen(kS1));
        if (!source || !source->strip().viewDetached()) {
            qWarning("movedStripViewDetached: the pan did not detach the source view");
            return false;
        }
        if (!engine->stashStripForOutputMove(kS1, kS2, 1, riders)) {
            qWarning("movedStripViewDetached: nothing was recorded");
            return false;
        }
        for (const QString& windowId : riders) {
            engine->handoffRelease(windowId);
            engine->releaseWindowOffScreen(windowId, kS2);
        }
        engine->setCurrentDesktopForScreen(kS2, 1);
        for (const QString& windowId : riders) {
            engine->windowOpened(windowId, kS2, 0, 0);
        }
        const auto* target = static_cast<ScrollState*>(engine->stateForScreen(kS2));
        return target && target->strip().viewDetached();
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
