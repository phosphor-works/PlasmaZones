// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_wta_routing.cpp
 * @brief Unit tests for the WindowTrackingAdaptor open routing
 *        (RouteToWorkspace, RouteToScreen and RouteToDesktop, routed once per
 *        open), the workspace move verb, the per-output desktops report,
 *        guardedHandoff refusal recovery, cross-mode move gating and the
 *        snap-to-empty-zone hoist. Split from test_wta_convenience.cpp (shared
 *        fixture: wta_convenience_fixture.h).
 */

#include "wta_convenience_fixture.h"
#include "dbus/windowtrackingadaptor/lifecyclerelay.h"

#include <PhosphorEngine/EngineTypes.h>
#include <PhosphorProtocol/WindowTypes.h>

#include <QScopeGuard>

class TestWtaRouting : public QObject, protected WtaConvenienceFixture
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        initFixture();
    }
    void cleanup()
    {
        cleanupFixture();
    }

    // ── Focus relay: windowActivated arms the snap layer-focus memory ──
    void testWindowActivated_armsSnapLayerFocusMemory()
    {
        // windowActivated is the PRODUCTION entry point for
        // SnapEngine::windowFocused (snap is deliberately not in the
        // TilingAdaptor lifecycle vector), so this pins the wiring the
        // engine-level switch suites bypass by calling windowFocused
        // directly — the layer-switch candidate memories are dead in a live
        // session if this relay is dropped.
        const QString windowId = QStringLiteral("app|w1");
        const QString screen = QStringLiteral("DP-1");
        SnapState* state = m_snapEngine->stateForWindowOnScreen(windowId, screen);
        state->assignWindowToZone(windowId, QStringLiteral("zone-1"), screen, 1);
        QVERIFY(state->lastSnappedFocus().isEmpty());

        m_wta->windowActivated(windowId, screen);
        QCOMPARE(state->lastSnappedFocus(), windowId);
    }

    // ── Open routing: RouteToScreen / RouteToDesktop rules (RouteToWorkspace below) ──
    void testApplyOpenRoutingForTiling_routesToAutotileScreenAndDesktop()
    {
        // DP-2 is an AUTOTILE screen; DP-1 (the spawn screen) stays snapping (the
        // registry default).
        PhosphorZones::AssignmentEntry autotile;
        autotile.mode = PhosphorZones::AssignmentEntry::Autotile;
        autotile.tilingAlgorithm = QStringLiteral("dwindle");
        m_layoutManager->setAssignmentEntryDirect(QStringLiteral("DP-2"), 0, QString(), autotile);

        // Register the opening window's metadata so the rule query resolves its appId.
        auto* registry = new PhosphorEngine::WindowRegistry(m_parent);
        m_wta->setWindowRegistry(registry);
        m_wta->setWindowMetadata(QStringLiteral("inst1"), QStringLiteral("routeapp"), QString(), QString(), QString(),
                                 0, 0, QString(), 0, QVariantMap());

        // Rule: route "routeapp" onto DP-2 and onto virtual desktop 2.
        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("routeapp"));
        RuleAction route;
        route.type = QString(ActionType::RouteToScreen);
        route.params.insert(QString(ActionParam::TargetScreenId), QStringLiteral("DP-2"));
        RuleAction desk;
        desk.type = QString(ActionType::RouteToDesktop);
        desk.params.insert(QString(ActionParam::TargetDesktop), 2);
        rule.actions = {route, desk};

        RuleStore store(ConfigDefaults::rulesFilePath(), nullptr); // stack object: no QObject parent
        QVERIFY(store.addRule(rule));
        m_wta->setRuleStore(&store);
        // Detach on every exit, including a mid-test QVERIFY abort: the stack
        // store is destroyed before the adaptor that borrows it.
        const auto teardown = qScopeGuard([this] {
            m_wta->setRuleStore(nullptr);
            m_wta->setWindowRegistry(nullptr);
        });

        QSignalSpy outputSpy(m_wta, &WindowTrackingAdaptor::windowOutputMoveExpected);
        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);

        const QString routed =
            m_wta->applyOpenRoutingForTiling(QStringLiteral("routeapp|inst1"), QStringLiteral("DP-1"));

        // Redirected to the autotile target, with both the output- and desktop-move
        // signals emitted for the compositor to act on.
        QCOMPARE(routed, QStringLiteral("DP-2"));
        QCOMPARE(outputSpy.count(), 1);
        QCOMPARE(outputSpy.at(0).at(1).toString(), QStringLiteral("DP-2"));
        QCOMPARE(desktopSpy.count(), 1);
        QCOMPARE(desktopSpy.at(0).at(1).toInt(), 2);
    }

    void testRestorePolicy_scopedExclusionDoesNotCancelLowerPriorityRule()
    {
        // Consumer-level guard for the WTA evaluator's terminal-action scope
        // ({Exclude, ExcludePlacement}, set in ensureRuleEvaluator): a
        // higher-priority decoration-only opt-out must NOT stop the walk
        // before a lower-priority RestorePosition rule resolves. Deleting the
        // setTerminalActionScope call from ensureRuleEvaluator leaves every
        // primitive-level scope test green while this one fails — the
        // exclusion would terminate the walk and the global default (forced
        // FALSE below) would come back instead of the rule's TRUE.
        auto* registry = new PhosphorEngine::WindowRegistry(m_parent);
        m_wta->setWindowRegistry(registry);
        m_wta->setWindowMetadata(QStringLiteral("inst9"), QStringLiteral("deskflow"), QString(), QString(), QString(),
                                 0, 0, QString(), 0, QVariantMap());
        m_settings->setSnappingRestoreFloatedWindowsOnLogin(false);

        using namespace PhosphorRules;
        Rule decoRule;
        decoRule.id = QUuid::createUuid();
        decoRule.enabled = true;
        decoRule.priority = 500;
        decoRule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("deskflow"));
        RuleAction decoAction;
        decoAction.type = QString(ActionType::ExcludeDecorations);
        decoRule.actions = {decoAction};

        Rule restoreRule;
        restoreRule.id = QUuid::createUuid();
        restoreRule.enabled = true;
        restoreRule.priority = 100;
        restoreRule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("deskflow"));
        RuleAction restoreAction;
        restoreAction.type = QString(ActionType::RestorePosition);
        restoreAction.params.insert(QString(ActionParam::Value), true);
        restoreRule.actions = {restoreAction};

        RuleStore store(ConfigDefaults::rulesFilePath(), nullptr); // stack object: no QObject parent
        QVERIFY(store.addRule(decoRule));
        QVERIFY(store.addRule(restoreRule));
        m_wta->setRuleStore(&store);
        // Detach on every exit, including a mid-test QVERIFY abort: the stack
        // store is destroyed before the adaptor that borrows it.
        const auto teardown = qScopeGuard([this] {
            m_wta->setRuleStore(nullptr);
            m_wta->setWindowRegistry(nullptr);
            m_settings->setSnappingRestoreFloatedWindowsOnLogin(true);
        });

        QVERIFY(m_wta->shouldRestoreFloatedPosition(QStringLiteral("deskflow|inst9"),
                                                    PhosphorZones::AssignmentEntry::Mode::Snapping));
    }

    void testApplyOpenRoutingForTiling_acceptsScrollingTarget()
    {
        // DP-2 is a SCROLLING screen: org.phosphor_works.Placement.Tiling serves both
        // tiling-family engines, so a RouteToScreen rule targeting a
        // scrolling monitor must redirect exactly like an autotile target.
        PhosphorZones::AssignmentEntry scrolling;
        scrolling.mode = PhosphorZones::AssignmentEntry::Scrolling;
        m_layoutManager->setAssignmentEntryDirect(QStringLiteral("DP-2"), 0, QString(), scrolling);

        auto* registry = new PhosphorEngine::WindowRegistry(m_parent);
        m_wta->setWindowRegistry(registry);
        m_wta->setWindowMetadata(QStringLiteral("inst3"), QStringLiteral("scrollroute"), QString(), QString(),
                                 QString(), 0, 0, QString(), 0, QVariantMap());

        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("scrollroute"));
        RuleAction route;
        route.type = QString(ActionType::RouteToScreen);
        route.params.insert(QString(ActionParam::TargetScreenId), QStringLiteral("DP-2"));
        rule.actions = {route};

        RuleStore store(ConfigDefaults::rulesFilePath(), nullptr); // stack object: no QObject parent
        QVERIFY(store.addRule(rule));
        m_wta->setRuleStore(&store);
        // Detach on every exit, including a mid-test QVERIFY abort: the stack
        // store is destroyed before the adaptor that borrows it.
        const auto teardown = qScopeGuard([this] {
            m_wta->setRuleStore(nullptr);
            m_wta->setWindowRegistry(nullptr);
        });

        QSignalSpy outputSpy(m_wta, &WindowTrackingAdaptor::windowOutputMoveExpected);

        const QString routed =
            m_wta->applyOpenRoutingForTiling(QStringLiteral("scrollroute|inst3"), QStringLiteral("DP-1"));

        QCOMPARE(routed, QStringLiteral("DP-2"));
        QCOMPARE(outputSpy.count(), 1);
        QCOMPARE(outputSpy.at(0).at(1).toString(), QStringLiteral("DP-2"));
    }

    void testApplyOpenRoutingForTiling_declinesSnapModeTarget()
    {
        // DP-2 stays SNAPPING (registry default): autotile must NOT redirect onto a
        // snap-mode monitor (the snap placement path owns those), but RouteToDesktop
        // is engine-neutral and still fires.
        auto* registry = new PhosphorEngine::WindowRegistry(m_parent);
        m_wta->setWindowRegistry(registry);
        m_wta->setWindowMetadata(QStringLiteral("inst2"), QStringLiteral("snaproute"), QString(), QString(), QString(),
                                 0, 0, QString(), 0, QVariantMap());

        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("snaproute"));
        RuleAction route;
        route.type = QString(ActionType::RouteToScreen);
        route.params.insert(QString(ActionParam::TargetScreenId), QStringLiteral("DP-2"));
        RuleAction desk;
        desk.type = QString(ActionType::RouteToDesktop);
        desk.params.insert(QString(ActionParam::TargetDesktop), 3);
        rule.actions = {route, desk};

        RuleStore store(ConfigDefaults::rulesFilePath(), nullptr); // stack object: no QObject parent
        QVERIFY(store.addRule(rule));
        m_wta->setRuleStore(&store);
        // Detach on every exit, including a mid-test QVERIFY abort: the stack
        // store is destroyed before the adaptor that borrows it.
        const auto teardown = qScopeGuard([this] {
            m_wta->setRuleStore(nullptr);
            m_wta->setWindowRegistry(nullptr);
        });

        QSignalSpy outputSpy(m_wta, &WindowTrackingAdaptor::windowOutputMoveExpected);
        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);

        const QString routed =
            m_wta->applyOpenRoutingForTiling(QStringLiteral("snaproute|inst2"), QStringLiteral("DP-1"));

        QVERIFY2(routed.isEmpty(), "a snap-mode RouteToScreen target must not be an autotile redirect");
        QCOMPARE(outputSpy.count(), 0);
        QCOMPARE(desktopSpy.count(), 1); // desktop routing is engine-neutral
        QCOMPARE(desktopSpy.at(0).at(1).toInt(), 3);
    }

    // The snap open path's RouteToDesktop emit (applyOpenDesktopRouting, called from
    // SnapAdaptor::resolveWindowRestore) emits windowDesktopMoveRequested when a
    // matched rule pins a desktop, and stays silent otherwise.
    void testApplyOpenDesktopRouting_emitsDesktopMoveForMatch()
    {
        auto* registry = new PhosphorEngine::WindowRegistry(m_parent);
        m_wta->setWindowRegistry(registry);
        m_wta->setWindowMetadata(QStringLiteral("inst3"), QStringLiteral("deskapp"), QString(), QString(), QString(), 0,
                                 0, QString(), 0, QVariantMap());

        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("deskapp"));
        RuleAction desk;
        desk.type = QString(ActionType::RouteToDesktop);
        desk.params.insert(QString(ActionParam::TargetDesktop), 4);
        rule.actions = {desk};

        RuleStore store(ConfigDefaults::rulesFilePath(), nullptr); // stack object: no QObject parent
        QVERIFY(store.addRule(rule));
        m_wta->setRuleStore(&store);
        // Detach on every exit, including a mid-test QVERIFY abort: the stack
        // store is destroyed before the adaptor that borrows it.
        const auto teardown = qScopeGuard([this] {
            m_wta->setRuleStore(nullptr);
            m_wta->setWindowRegistry(nullptr);
        });

        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);
        // The RETURN is asserted as well as the signal. No production caller
        // reads it today, so these tests are what holds the "reports a match"
        // half of the contract; asserting only the spy would leave it untested.
        QVERIFY2(m_wta->applyOpenDesktopRouting(QStringLiteral("deskapp|inst3"), QStringLiteral("DP-1")),
                 "a matched RouteToDesktop must report the match to its caller");
        QCOMPARE(desktopSpy.count(), 1);
        QCOMPARE(desktopSpy.at(0).at(1).toInt(), 4);

        // A window with no matching rule emits nothing and reports no match.
        m_wta->setWindowMetadata(QStringLiteral("inst4"), QStringLiteral("nomatch"), QString(), QString(), QString(), 0,
                                 0, QString(), 0, QVariantMap());
        QSignalSpy quietSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);
        QVERIFY(!m_wta->applyOpenDesktopRouting(QStringLiteral("nomatch|inst4"), QStringLiteral("DP-1")));
        QCOMPARE(quietSpy.count(), 0);
    }

    // The snap open path runs RouteToDesktop for a FIRST placement only. An
    // Open and a PendingSweep (a window that opened before the daemon was
    // ready) route; the unminimize re-drive, the daemon-restart sweep, the
    // desktop-arrival continuation and the desktop re-apply act on a window
    // that is already where it belongs, and re-routing it would yank it back
    // on every re-announce. Each reason drives its OWN window: routing runs
    // once per open, so a second reason on the same window would read zero
    // whether or not the first-placement gate held.
    void testResolveWindowRestore_routesToDesktopOnFirstPlacementOnly()
    {
        auto* registry = new PhosphorEngine::WindowRegistry(m_parent);
        m_wta->setWindowRegistry(registry);

        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("deskapp"));
        RuleAction desk;
        desk.type = QString(ActionType::RouteToDesktop);
        desk.params.insert(QString(ActionParam::TargetDesktop), 3);
        rule.actions = {desk};

        RuleStore store(ConfigDefaults::rulesFilePath(), nullptr);
        QVERIFY(store.addRule(rule));
        m_wta->setRuleStore(&store);
        const auto teardown = qScopeGuard([this] {
            m_wta->setRuleStore(nullptr);
            m_wta->setWindowRegistry(nullptr);
        });

        using PhosphorEngine::RestoreReason;
        const auto drive = [this](RestoreReason reason, const QString& instance) {
            m_wta->setWindowMetadata(instance, QStringLiteral("deskapp"), QString(), QString(), QString(), 0, 0,
                                     QString(), 0, QVariantMap());
            QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);
            int x = 0;
            int y = 0;
            int w = 0;
            int h = 0;
            bool shouldSnap = false;
            m_snapAdaptor->resolveWindowRestore(QStringLiteral("deskapp|") + instance, QStringLiteral("DP-1"), false,
                                                static_cast<int>(PhosphorEngine::WindowKind::Unknown),
                                                static_cast<int>(reason), 0, 0, x, y, w, h, shouldSnap);
            return desktopSpy.count();
        };
        QCOMPARE(drive(RestoreReason::Open, QStringLiteral("open")), 1);
        QCOMPARE(drive(RestoreReason::PendingSweep, QStringLiteral("sweep")), 1);
        QCOMPARE(drive(RestoreReason::Unminimize, QStringLiteral("unminimize")), 0);
        QCOMPARE(drive(RestoreReason::DaemonRestartSweep, QStringLiteral("restart")), 0);
        QCOMPARE(drive(RestoreReason::DesktopArrival, QStringLiteral("arrival")), 0);
        QCOMPARE(drive(RestoreReason::DesktopReapply, QStringLiteral("reapply")), 0);

        // The latch: the same window's pending sweep after its open is the
        // same open, so it moves nothing. Closing the window drops the latch,
        // and a reused window id routes again on its next open.
        QCOMPARE(drive(RestoreReason::PendingSweep, QStringLiteral("open")), 0);
        m_wta->windowClosed(QStringLiteral("deskapp|open"), static_cast<int>(PhosphorEngine::WindowKind::Unknown),
                            QStringLiteral("DP-1"));
        QCOMPARE(drive(RestoreReason::Open, QStringLiteral("open")), 1);
    }

    // The reopen contract on the snap channel: a record that tiles the app on
    // ANOTHER output is not a reason to move the opening window, nor to hand
    // it to that output's tiling engine. The window opens on its own snap
    // screen, falls to the snap float default there, and still inherits the
    // free size of a closed sibling recorded on that screen (#1106). Every
    // record in the bucket is left as it was.
    void testRecordTiledOnAnotherOutput_doesNotMoveTheOpeningWindow()
    {
        using PhosphorEngine::WindowPlacement;
        const QString appId = QStringLiteral("reopenapp");
        const QString opener = QStringLiteral("reopenapp|second");

        auto* registry = new PhosphorEngine::WindowRegistry(m_parent);
        m_wta->setWindowRegistry(registry);
        m_wta->setWindowMetadata(QStringLiteral("second"), appId, QString(), QString(), QString(), 0, 0, QString(), 0,
                                 QVariantMap());
        int routedOpens = 0;
        m_wta->setRoutedOpenDispatcher([&routedOpens](const PhosphorProtocol::WindowOpenedEntry&) {
            ++routedOpens;
        });
        const auto teardown = qScopeGuard([this] {
            m_wta->setRoutedOpenDispatcher({});
            m_wta->setWindowRegistry(nullptr);
        });

        PhosphorZones::AssignmentEntry autotile;
        autotile.mode = PhosphorZones::AssignmentEntry::Autotile;
        autotile.tilingAlgorithm = QStringLiteral("dwindle");
        m_layoutManager->setAssignmentEntryDirect(QStringLiteral("DP-2"), 0, QString(), autotile);

        PhosphorEngine::WindowPlacementStore& store = m_wta->service()->placementStore();
        WindowPlacement tiledElsewhere;
        tiledElsewhere.windowId = QStringLiteral("reopenapp|old");
        tiledElsewhere.appId = appId;
        tiledElsewhere.screenId = QStringLiteral("DP-2");
        PhosphorEngine::EngineSlot tiledSlot;
        tiledSlot.state = QString(WindowPlacement::stateTiled());
        tiledElsewhere.engines.insert(QString(WindowPlacement::autotileEngineId()), tiledSlot);
        QVERIFY(store.record(tiledElsewhere));
        const QRect siblingFree(80, 60, 910, 620);
        WindowPlacement closedSibling;
        closedSibling.windowId = QStringLiteral("reopenapp|closed");
        closedSibling.appId = appId;
        closedSibling.screenId = m_screenId;
        closedSibling.freeGeometryByScreen.insert(m_screenId, siblingFree);
        QVERIFY(store.record(closedSibling));
        WindowPlacement snappedElsewhere;
        snappedElsewhere.windowId = QStringLiteral("reopenapp|snapped");
        snappedElsewhere.appId = appId;
        snappedElsewhere.screenId = QStringLiteral("DP-2");
        PhosphorEngine::EngineSlot snapSlot;
        snapSlot.state = QString(WindowPlacement::stateSnapped());
        snapSlot.zoneIds = {m_zoneIds[0]};
        snappedElsewhere.engines.insert(QString(WindowPlacement::snapEngineId()), snapSlot);
        QVERIFY(store.record(snappedElsewhere));
        const WindowPlacement tiledBefore = *store.peekExact(tiledElsewhere.windowId);
        const WindowPlacement snappedBefore = *store.peekExact(snappedElsewhere.windowId);

        QSignalSpy sizeSpy(m_snapEngine, &PhosphorEngine::PlacementEngineBase::sizeRestoreRequested);
        QSignalSpy geomSpy(m_wta, &WindowTrackingAdaptor::applyGeometryRequested);
        QSignalSpy outputMoveSpy(m_wta, &WindowTrackingAdaptor::windowOutputMoveExpected);
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        bool shouldSnap = false;
        m_snapAdaptor->resolveWindowRestore(
            opener, m_screenId, false, static_cast<int>(PhosphorEngine::WindowKind::Unknown),
            static_cast<int>(PhosphorEngine::RestoreReason::Open), 0, 0, x, y, w, h, shouldSnap);

        QVERIFY(!shouldSnap);
        QVERIFY2(m_snapEngine->isFloating(opener), "the opening screen's snap float default places the window");
        QCOMPARE(sizeSpy.count(), 1);
        const QList<QVariant> args = sizeSpy.takeFirst();
        QCOMPARE(args.at(0).toString(), opener);
        QCOMPARE(args.at(1).toSize(), siblingFree.size());
        for (const QList<QVariant>& call : std::as_const(geomSpy)) {
            QVERIFY2(call.at(6).toString() != QStringLiteral("DP-2"),
                     "the window must not be sent to the other output");
        }
        QCOMPARE(outputMoveSpy.count(), 0);
        QCOMPARE(routedOpens, 0);
        const auto tiledAfter = store.peekExact(tiledElsewhere.windowId);
        const auto snappedAfter = store.peekExact(snappedElsewhere.windowId);
        QVERIFY(tiledAfter.has_value());
        QVERIFY(snappedAfter.has_value());
        QVERIFY2(tiledAfter->sameContentAs(tiledBefore), "the tiled record is left as it was");
        QVERIFY2(snappedAfter->sameContentAs(snappedBefore), "the snapped record is left as it was");
    }

    void testEmitOpenRouting_matchedButUnusableDesktopStillReportsAMatch()
    {
        // A matched RouteToDesktop owns this window's desktop whether or not its
        // target is usable, so both open channels must suppress the remembered
        // desktop either way. Reporting no-match here would let the rule and the
        // recorded desktop fight over one window on one open.
        //
        // Stored rules cannot produce such a target — the descriptor validator
        // rejects it on both routes into the store — so the slot is filled
        // DIRECTLY here. That is the in-process producer the guard exists for,
        // and it is why this branch is testable even though it is unreachable
        // from a rules.json.
        PhosphorRules::RuleAction desk;
        desk.type = QString(PhosphorRules::ActionType::RouteToDesktop);
        desk.params.insert(QString(PhosphorRules::ActionParam::TargetDesktop), 0);

        PhosphorRules::ResolvedActions resolved;
        QVERIFY(resolved.fillSlot(QString(PhosphorRules::ActionSlot::RouteDesktop), desk));

        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);
        QVERIFY2(m_wta->emitOpenRoutingIfMatched(resolved, QStringLiteral("deskapp|inst")),
                 "a matched rule with an unusable target still owns this window's desktop");
        QCOMPARE(desktopSpy.count(), 0);
    }

    void testEmitOpenRouting_noRouteSlotReportsNoMatch()
    {
        // The complementary half: an empty verdict must report no match, or
        // every window would have its remembered desktop suppressed.
        PhosphorRules::ResolvedActions resolved;
        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);
        QVERIFY(!m_wta->emitOpenRoutingIfMatched(resolved, QStringLiteral("deskapp|inst")));
        QCOMPARE(desktopSpy.count(), 0);
    }

    void testApplyOpenDesktopRouting_reportsNoMatchWithoutARuleStore()
    {
        // The defensive guard. false is the "no rule named a desktop" answer, so
        // an inverted sense would have every window look rule-routed.
        auto* registry = new PhosphorEngine::WindowRegistry(m_parent);
        m_wta->setWindowRegistry(registry);
        m_wta->setWindowMetadata(QStringLiteral("inst10"), QStringLiteral("deskapp"), QString(), QString(), QString(),
                                 0, 0, QString(), 0, QVariantMap());
        const auto teardown = qScopeGuard([this] {
            m_wta->setWindowRegistry(nullptr);
        });

        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);
        QVERIFY(!m_wta->applyOpenDesktopRouting(QStringLiteral("deskapp|inst10"), QStringLiteral("DP-1")));
        QCOMPARE(desktopSpy.count(), 0);
    }

    // The wire clamp, asserted directly. Its consumers are the reason gates in
    // SnapAdaptor::resolveWindowRestore (sibling borrowing and placement
    // rules), so a garbled value reading as anything but Open
    // would silently skip a genuine restore. Every named reason must survive the
    // round trip, and everything else must land on Open rather than on whichever
    // enumerator happens to sit at that ordinal.
    void testClampRestoreReasonFromWire_unknownValuesReadAsOpen()
    {
        using PhosphorEngine::RestoreReason;
        QCOMPARE(PhosphorEngine::clampRestoreReasonFromWire(static_cast<int>(RestoreReason::Open)),
                 RestoreReason::Open);
        QCOMPARE(PhosphorEngine::clampRestoreReasonFromWire(static_cast<int>(RestoreReason::Unminimize)),
                 RestoreReason::Unminimize);
        QCOMPARE(PhosphorEngine::clampRestoreReasonFromWire(static_cast<int>(RestoreReason::PendingSweep)),
                 RestoreReason::PendingSweep);
        QCOMPARE(PhosphorEngine::clampRestoreReasonFromWire(static_cast<int>(RestoreReason::DesktopArrival)),
                 RestoreReason::DesktopArrival);
        QCOMPARE(PhosphorEngine::clampRestoreReasonFromWire(static_cast<int>(RestoreReason::DaemonRestartSweep)),
                 RestoreReason::DaemonRestartSweep);
        QCOMPARE(PhosphorEngine::clampRestoreReasonFromWire(static_cast<int>(RestoreReason::DesktopReapply)),
                 RestoreReason::DesktopReapply);

        QCOMPARE(PhosphorEngine::clampRestoreReasonFromWire(9999), RestoreReason::Open);
        QCOMPARE(PhosphorEngine::clampRestoreReasonFromWire(-1), RestoreReason::Open);
    }

    // A BARE RouteToScreen rule (no SnapToZone) must move the opening window to the
    // target monitor on the snap open path: applyOpenScreenRouting translates the
    // window's frame onto the target screen's available area and emits the
    // output-move marker plus a FREE applyGeometryRequested (empty zone id). A local
    // WTA wired to a deterministic two-screen ScreenManager is used because the
    // shared fixture has a null ScreenManager.
    void testApplyOpenScreenRouting_movesBareRouteToTargetMonitor()
    {
        PhosphorScreens::FakePhysicalScreenSource fake;
        fake.addScreen(QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), QStringLiteral("DP-1"));
        fake.addScreen(QStringLiteral("DP-2"), QRect(1920, 0, 1920, 1080), QStringLiteral("DP-2"));
        PhosphorScreens::ScreenManager screenMgr(
            PhosphorScreens::ScreenManagerConfig{.physicalScreenSource = &fake, .useGeometrySensors = false});
        screenMgr.start();

        QObject parent;
        auto* wta = new WindowTrackingAdaptor(m_layoutManager, m_zoneDetector, &screenMgr, m_settings, nullptr, nullptr,
                                              &parent);
        // Owned here and declared after the parent, so it goes before the
        // adaptor whose service it was built on, as in cleanupFixture.
        const auto snap =
            std::make_unique<SnapEngine>(m_layoutManager, wta->service(), m_zoneDetector, nullptr, nullptr);
        wta->service()->setSnapState(snap->snapState());
        wta->service()->setSnapEngine(snap.get());
        wta->setEngines(snap.get(), nullptr, nullptr);

        auto* registry = new PhosphorEngine::WindowRegistry(&parent);
        wta->setWindowRegistry(registry);
        wta->setWindowMetadata(QStringLiteral("inst1"), QStringLiteral("routeapp"), QString(), QString(), QString(), 0,
                               0, QString(), 0, QVariantMap());

        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("routeapp"));
        RuleAction route;
        route.type = QString(ActionType::RouteToScreen);
        route.params.insert(QString(ActionParam::TargetScreenId), QStringLiteral("DP-2"));
        rule.actions = {route}; // bare RouteToScreen, NO SnapToZone
        RuleStore store(ConfigDefaults::rulesFilePath(), &parent);
        QVERIFY(store.addRule(rule));
        wta->setRuleStore(&store);
        // Detach on every exit, including a mid-test QVERIFY abort: the stack
        // store is destroyed before the adaptor that borrows it.
        const auto teardown = qScopeGuard([wta] {
            wta->setRuleStore(nullptr);
            wta->setWindowRegistry(nullptr);
            wta->service()->setSnapEngine(nullptr);
            wta->service()->setSnapState(nullptr);
        });

        // Window opened on DP-1 at a known frame.
        const QString w = QStringLiteral("routeapp|inst1");
        wta->setFrameGeometry(w, 100, 100, 800, 600);

        QSignalSpy outputSpy(wta, &WindowTrackingAdaptor::windowOutputMoveExpected);
        QSignalSpy geomSpy(wta, &WindowTrackingAdaptor::applyGeometryRequested);

        QVERIFY2(wta->applyOpenScreenRouting(w, QStringLiteral("DP-1")),
                 "a matched bare route must report the directive as matched");

        // Output-move marker for DP-2.
        QCOMPARE(outputSpy.count(), 1);
        QCOMPARE(outputSpy.at(0).at(1).toString(), QStringLiteral("DP-2"));
        // Free placement on DP-2: applyGeometryRequested(windowId, x, y, w, h, zoneId, screenId, sizeOnly).
        QCOMPARE(geomSpy.count(), 1);
        const auto args = geomSpy.at(0);
        QCOMPARE(args.at(0).toString(), w);
        QVERIFY2(args.at(5).toString().isEmpty(), "a bare route must be a free placement (empty zone id)");
        QCOMPARE(args.at(6).toString(), QStringLiteral("DP-2"));
        const int x = args.at(1).toInt();
        QVERIFY2(x >= 1920 && x < 3840, "the window must land within DP-2's geometry");
    }

    // The open path may have asked the effect for a free SIZE moments ago,
    // inside this very resolve, and the frame shadow cannot know yet. The
    // route must translate the RESTORED size onto the target monitor.
    //
    // The case that matters is shadow-EMPTY: the snap handler seeds the
    // shadow only on an Open, so a pending sweep or a desktop arrival reaches
    // the route with nothing there, the registry metadata supplies the rect,
    // and that metadata still carries the un-restored managed size. Applying
    // the pending size only when the shadow was already populated left those
    // drivers routing the very size the restore had just replaced.
    void testApplyOpenScreenRouting_pendingFreeSizeWinsOverMetadata()
    {
        PhosphorScreens::FakePhysicalScreenSource fake;
        fake.addScreen(QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), QStringLiteral("DP-1"));
        fake.addScreen(QStringLiteral("DP-2"), QRect(1920, 0, 1920, 1080), QStringLiteral("DP-2"));
        PhosphorScreens::ScreenManager screenMgr(
            PhosphorScreens::ScreenManagerConfig{.physicalScreenSource = &fake, .useGeometrySensors = false});
        screenMgr.start();

        QObject parent;
        auto* wta = new WindowTrackingAdaptor(m_layoutManager, m_zoneDetector, &screenMgr, m_settings, nullptr, nullptr,
                                              &parent);
        // Owned here and declared after the parent, so it goes before the
        // adaptor whose service it was built on, as in cleanupFixture.
        const auto snap =
            std::make_unique<SnapEngine>(m_layoutManager, wta->service(), m_zoneDetector, nullptr, nullptr);
        wta->service()->setSnapState(snap->snapState());
        wta->service()->setSnapEngine(snap.get());
        wta->setEngines(snap.get(), nullptr, nullptr);

        auto* registry = new PhosphorEngine::WindowRegistry(&parent);
        wta->setWindowRegistry(registry);
        // Metadata carries the ZONE-sized frame the app opened at — the size
        // the free-size restore exists to undo. It reaches the registry
        // through the extended map, the same way the effect pushes it.
        namespace MetaKey = PhosphorProtocol::Service::WindowMetadataKey;
        QVariantMap frame;
        frame.insert(QString(MetaKey::PositionX), 100);
        frame.insert(QString(MetaKey::PositionY), 100);
        frame.insert(QString(MetaKey::Width), 960);
        frame.insert(QString(MetaKey::Height), 1040);
        wta->setWindowMetadata(QStringLiteral("inst7"), QStringLiteral("sizeapp"), QString(), QString(), QString(), 0,
                               0, QString(), 0, frame);

        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("sizeapp"));
        RuleAction route;
        route.type = QString(ActionType::RouteToScreen);
        route.params.insert(QString(ActionParam::TargetScreenId), QStringLiteral("DP-2"));
        rule.actions = {route};
        RuleStore store(ConfigDefaults::rulesFilePath(), &parent);
        QVERIFY(store.addRule(rule));
        wta->setRuleStore(&store);
        const auto teardown = qScopeGuard([wta] {
            wta->setRuleStore(nullptr);
            wta->setWindowRegistry(nullptr);
            wta->service()->setSnapEngine(nullptr);
            wta->service()->setSnapState(nullptr);
        });

        // No setFrameGeometry: this is the driver that pushes no seed. The
        // engine's size restore lands through the ordinary relay first.
        const QString w = QStringLiteral("sizeapp|inst7");
        const QSize restored(880, 640);
        Q_EMIT snap->sizeRestoreRequested(w, restored, QStringLiteral("DP-1"));

        QSignalSpy geomSpy(wta, &WindowTrackingAdaptor::applyGeometryRequested);
        QVERIFY(wta->applyOpenScreenRouting(w, QStringLiteral("DP-1")));

        // The relay's own size-only apply, then the route's free placement.
        QVERIFY(geomSpy.count() >= 1);
        const auto args = geomSpy.last();
        QCOMPARE(args.at(0).toString(), w);
        QCOMPARE(args.at(6).toString(), QStringLiteral("DP-2"));
        QVERIFY2(!args.at(7).toBool(), "the route's own apply is a full placement, not size-only");
        QCOMPARE(QSize(args.at(3).toInt(), args.at(4).toInt()), restored);
    }

    // pruneStaleWindows is the backstop for a window that died without a close
    // signal, and it must reach the two open-path pending maps as well: their
    // normal clears are the next frame report and windowClosed, neither of
    // which arrives for such a window. Driven through the observable the maps
    // feed, the open route's size translation. The last leg pins the first of
    // those normal clears: a frame report supersedes a pending rect.
    void testPruneStaleWindows_dropsThePendingOpenMaps()
    {
        PhosphorScreens::FakePhysicalScreenSource fake;
        fake.addScreen(QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), QStringLiteral("DP-1"));
        fake.addScreen(QStringLiteral("DP-2"), QRect(1920, 0, 1920, 1080), QStringLiteral("DP-2"));
        PhosphorScreens::ScreenManager screenMgr(
            PhosphorScreens::ScreenManagerConfig{.physicalScreenSource = &fake, .useGeometrySensors = false});
        screenMgr.start();

        QObject parent;
        auto* wta = new WindowTrackingAdaptor(m_layoutManager, m_zoneDetector, &screenMgr, m_settings, nullptr, nullptr,
                                              &parent);
        // Owned here and declared after the parent, so it goes before the
        // adaptor whose service it was built on, as in cleanupFixture.
        const auto snap =
            std::make_unique<SnapEngine>(m_layoutManager, wta->service(), m_zoneDetector, nullptr, nullptr);
        wta->service()->setSnapState(snap->snapState());
        wta->service()->setSnapEngine(snap.get());
        wta->setEngines(snap.get(), nullptr, nullptr);

        auto* registry = new PhosphorEngine::WindowRegistry(&parent);
        wta->setWindowRegistry(registry);
        namespace MetaKey = PhosphorProtocol::Service::WindowMetadataKey;
        QVariantMap frame;
        frame.insert(QString(MetaKey::PositionX), 100);
        frame.insert(QString(MetaKey::PositionY), 100);
        frame.insert(QString(MetaKey::Width), 960);
        frame.insert(QString(MetaKey::Height), 1040);
        wta->setWindowMetadata(QStringLiteral("inst8"), QStringLiteral("pruneapp"), QString(), QString(), QString(), 0,
                               0, QString(), 0, frame);

        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("pruneapp"));
        RuleAction route;
        route.type = QString(ActionType::RouteToScreen);
        route.params.insert(QString(ActionParam::TargetScreenId), QStringLiteral("DP-2"));
        rule.actions = {route};
        RuleStore store(ConfigDefaults::rulesFilePath(), &parent);
        QVERIFY(store.addRule(rule));
        wta->setRuleStore(&store);
        const auto teardown = qScopeGuard([wta] {
            wta->setRuleStore(nullptr);
            wta->setWindowRegistry(nullptr);
            wta->service()->setSnapEngine(nullptr);
            wta->service()->setSnapState(nullptr);
        });

        const QString w = QStringLiteral("pruneapp|inst8");
        const QSize restored(880, 640);
        Q_EMIT snap->sizeRestoreRequested(w, restored, QStringLiteral("DP-1"));

        // Present: the route carries the restored size.
        QSignalSpy before(wta, &WindowTrackingAdaptor::applyGeometryRequested);
        QVERIFY(wta->applyOpenScreenRouting(w, QStringLiteral("DP-1")));
        QVERIFY(before.count() >= 1);
        QCOMPARE(QSize(before.last().at(3).toInt(), before.last().at(4).toInt()), restored);

        // The window dies with no close signal, and the sweep runs for a set
        // that does not name it. The same sweep also drops the registry
        // metadata, which the rule query needs, so that half is re-seeded
        // afterwards: this test is about the pending maps, and without their
        // sweep the restored size would survive and be routed again.
        wta->pruneStaleWindows(QStringList{QStringLiteral("someone-else")});
        wta->setWindowMetadata(QStringLiteral("inst8"), QStringLiteral("pruneapp"), QString(), QString(), QString(), 0,
                               0, QString(), 0, frame);
        QSignalSpy after(wta, &WindowTrackingAdaptor::applyGeometryRequested);
        QVERIFY(wta->applyOpenScreenRouting(w, QStringLiteral("DP-1")));
        QVERIFY(after.count() >= 1);
        QCOMPARE(QSize(after.last().at(3).toInt(), after.last().at(4).toInt()), QSize(960, 1040));

        // The open path asks for a float position, then the effect reports
        // where the window is: the route translates the reported frame.
        Q_EMIT snap->geometryRestoreRequested(w, QRect(100, 100, 880, 640), QStringLiteral("DP-1"));
        wta->setFrameGeometry(w, 200, 150, 700, 500);
        QSignalSpy reported(wta, &WindowTrackingAdaptor::applyGeometryRequested);
        QVERIFY(wta->applyOpenScreenRouting(w, QStringLiteral("DP-1")));
        QVERIFY(reported.count() >= 1);
        QCOMPARE(QSize(reported.last().at(3).toInt(), reported.last().at(4).toInt()), QSize(700, 500));
    }

    // A rule carrying BOTH SnapToZone and RouteToScreen is still moved by
    // applyOpenScreenRouting. Its only caller reaches it exclusively under
    // !shouldSnap — the placement directive already declined (a non-snapping
    // target, no layout, no surviving ordinal) — so suppressing the move on the
    // Placement slot's mere presence left the window neither snapped nor routed
    // (the #921 fix). The verdict is still "matched": the rule owns the
    // window's monitor either way.
    void testApplyOpenScreenRouting_snapToZonePresent_stillHonoursRoute()
    {
        PhosphorScreens::FakePhysicalScreenSource fake;
        fake.addScreen(QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), QStringLiteral("DP-1"));
        fake.addScreen(QStringLiteral("DP-2"), QRect(1920, 0, 1920, 1080), QStringLiteral("DP-2"));
        PhosphorScreens::ScreenManager screenMgr(
            PhosphorScreens::ScreenManagerConfig{.physicalScreenSource = &fake, .useGeometrySensors = false});
        screenMgr.start();

        QObject parent;
        auto* wta = new WindowTrackingAdaptor(m_layoutManager, m_zoneDetector, &screenMgr, m_settings, nullptr, nullptr,
                                              &parent);
        // Owned here and declared after the parent, so it goes before the
        // adaptor whose service it was built on, as in cleanupFixture.
        const auto snap =
            std::make_unique<SnapEngine>(m_layoutManager, wta->service(), m_zoneDetector, nullptr, nullptr);
        wta->service()->setSnapState(snap->snapState());
        wta->service()->setSnapEngine(snap.get());
        wta->setEngines(snap.get(), nullptr, nullptr);

        auto* registry = new PhosphorEngine::WindowRegistry(&parent);
        wta->setWindowRegistry(registry);
        wta->setWindowMetadata(QStringLiteral("inst2"), QStringLiteral("snaproute"), QString(), QString(), QString(), 0,
                               0, QString(), 0, QVariantMap());

        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("snaproute"));
        RuleAction route;
        route.type = QString(ActionType::RouteToScreen);
        route.params.insert(QString(ActionParam::TargetScreenId), QStringLiteral("DP-2"));
        RuleAction snapTo;
        snapTo.type = QString(ActionType::SnapToZone);
        snapTo.params.insert(QString(ActionParam::Zones), QJsonArray{1});
        rule.actions = {route, snapTo};
        RuleStore store(ConfigDefaults::rulesFilePath(), &parent);
        QVERIFY(store.addRule(rule));
        wta->setRuleStore(&store);
        // Detach on every exit, including a mid-test QVERIFY abort: the stack
        // store is destroyed before the adaptor that borrows it.
        const auto teardown = qScopeGuard([wta] {
            wta->setRuleStore(nullptr);
            wta->setWindowRegistry(nullptr);
            wta->service()->setSnapEngine(nullptr);
            wta->service()->setSnapState(nullptr);
        });

        const QString w = QStringLiteral("snaproute|inst2");
        wta->setFrameGeometry(w, 100, 100, 800, 600);

        QSignalSpy outputSpy(wta, &WindowTrackingAdaptor::windowOutputMoveExpected);
        QSignalSpy geomSpy(wta, &WindowTrackingAdaptor::applyGeometryRequested);

        QVERIFY2(wta->applyOpenScreenRouting(w, QStringLiteral("DP-1")),
                 "a matched route+snap rule must report the directive as matched");

        // The route is honoured even though the Placement slot rode along:
        // reaching this call at all means the snap did not happen.
        QCOMPARE(outputSpy.count(), 1);
        QCOMPARE(outputSpy.at(0).at(1).toString(), QStringLiteral("DP-2"));
        QCOMPARE(geomSpy.count(), 1);
        QCOMPARE(geomSpy.at(0).at(6).toString(), QStringLiteral("DP-2"));
    }

    // The bool verdict of applyOpenScreenRouting and the directiveMatched
    // out-param of applyOpenRoutingForTiling both report "a directive
    // matched" for the cases where a rule OWNS the window's monitor but no
    // move is needed or possible. The tiling channel's answer vetoes its
    // reopen claim (dispatchOpenToClaimingEngine); the snap channel's bool
    // gates no claim and is pinned so the two channels keep one reading of
    // the same rule.
    void testRoutingVerdicts_matchedButNoMove_reportMatchedOnBothChannels()
    {
        PhosphorScreens::FakePhysicalScreenSource fake;
        fake.addScreen(QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), QStringLiteral("DP-1"));
        PhosphorScreens::ScreenManager screenMgr(
            PhosphorScreens::ScreenManagerConfig{.physicalScreenSource = &fake, .useGeometrySensors = false});
        screenMgr.start();

        QObject parent;
        auto* wta = new WindowTrackingAdaptor(m_layoutManager, m_zoneDetector, &screenMgr, m_settings, nullptr, nullptr,
                                              &parent);
        auto* registry = new PhosphorEngine::WindowRegistry(&parent);
        wta->setWindowRegistry(registry);
        wta->setWindowMetadata(QStringLiteral("inst3"), QStringLiteral("pinapp"), QString(), QString(), QString(), 0, 0,
                               QString(), 0, QVariantMap());

        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("pinapp"));
        RuleAction route;
        route.type = QString(ActionType::RouteToScreen);
        route.params.insert(QString(ActionParam::TargetScreenId), QStringLiteral("DP-1"));
        rule.actions = {route};
        RuleStore store(ConfigDefaults::rulesFilePath(), &parent);
        QVERIFY(store.addRule(rule));
        wta->setRuleStore(&store);
        // Detach on every exit, including a mid-test QVERIFY abort: the stack
        // store is destroyed before the adaptor that borrows it.
        const auto teardown = qScopeGuard([wta] {
            wta->setRuleStore(nullptr);
            wta->setWindowRegistry(nullptr);
        });

        const QString w = QStringLiteral("pinapp|inst3");
        wta->setFrameGeometry(w, 100, 100, 800, 600);

        // ALREADY ON TARGET. The snap channel returns true...
        QVERIFY2(wta->applyOpenScreenRouting(w, QStringLiteral("DP-1")),
                 "a rule pinning the window to the screen it already occupies still owns that monitor");
        // ...and the tiling channel must agree, with an EMPTY redirect (no
        // move needed) but directiveMatched set — the two answers are
        // deliberately separate.
        bool matched = false;
        const QString routed = wta->applyOpenRoutingForTiling(w, QStringLiteral("DP-1"), &matched);
        QVERIFY2(routed.isEmpty(), "already on target: no redirect");
        QVERIFY2(matched, "…but the directive matched, so the tiling channel's claim is vetoed");

        // NO RULE AT ALL: both channels report unmatched, so the tiling claim runs.
        wta->setWindowMetadata(QStringLiteral("inst4"), QStringLiteral("otherapp"), QString(), QString(), QString(), 0,
                               0, QString(), 0, QVariantMap());
        const QString other = QStringLiteral("otherapp|inst4");
        wta->setFrameGeometry(other, 100, 100, 800, 600);
        QVERIFY2(!wta->applyOpenScreenRouting(other, QStringLiteral("DP-1")), "no rule → no directive");
        bool otherMatched = true;
        QVERIFY(wta->applyOpenRoutingForTiling(other, QStringLiteral("DP-1"), &otherMatched).isEmpty());
        QVERIFY2(!otherMatched, "no rule → no directive on the tiling channel either");
    }

    // The ownership verdict both channels share (hasValidPlacementTarget) is a
    // PAYLOAD-SHAPE judgement over the SnapToZone targets, and the zone-NAME
    // form (discussion #924) must count: a names-only rule owns the window's
    // target on both channels even though no zone in the test layout carries
    // that name (no engine is wired here; in the daemon the engine would then
    // decline the snap and the remembered-placement fallback stays suppressed,
    // as for an ordinal the layout lacks). The control is a window no rule
    // matches, which both channels must report as unowned. (A payload with no
    // valid target cannot be built through the store: Rule::isValid runs the
    // descriptor validator, which needs at least one valid entry across the
    // two lists.)
    void testRoutingVerdicts_namesOnlyPlacement_ownsOnBothChannels()
    {
        PhosphorScreens::FakePhysicalScreenSource fake;
        fake.addScreen(QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), QStringLiteral("DP-1"));
        PhosphorScreens::ScreenManager screenMgr(
            PhosphorScreens::ScreenManagerConfig{.physicalScreenSource = &fake, .useGeometrySensors = false});
        screenMgr.start();

        QObject parent;
        auto* wta = new WindowTrackingAdaptor(m_layoutManager, m_zoneDetector, &screenMgr, m_settings, nullptr, nullptr,
                                              &parent);
        auto* registry = new PhosphorEngine::WindowRegistry(&parent);
        wta->setWindowRegistry(registry);
        wta->setWindowMetadata(QStringLiteral("inst5"), QStringLiteral("namedapp"), QString(), QString(), QString(), 0,
                               0, QString(), 0, QVariantMap());
        wta->setWindowMetadata(QStringLiteral("inst6"), QStringLiteral("otherapp"), QString(), QString(), QString(), 0,
                               0, QString(), 0, QVariantMap());

        using namespace PhosphorRules;
        Rule named;
        named.id = QUuid::createUuid();
        named.enabled = true;
        named.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("namedapp"));
        RuleAction snapByName;
        snapByName.type = QString(ActionType::SnapToZone);
        snapByName.params.insert(QString(ActionParam::ZoneNames), QJsonArray{QStringLiteral("Editor")});
        named.actions = {snapByName};
        RuleStore store(ConfigDefaults::rulesFilePath(), nullptr); // stack object: no QObject parent
        QVERIFY(store.addRule(named));
        wta->setRuleStore(&store);
        // Detach on every exit: `store` is declared after `parent`, so on a
        // mid-test QVERIFY abort it dies first while the WTA still holds it.
        const auto teardown = qScopeGuard([wta] {
            wta->setRuleStore(nullptr);
            wta->setWindowRegistry(nullptr);
        });

        const QString w = QStringLiteral("namedapp|inst5");
        wta->setFrameGeometry(w, 100, 100, 800, 600);
        QVERIFY2(wta->applyOpenScreenRouting(w, QStringLiteral("DP-1")),
                 "a names-only SnapToZone rule owns the window's target on the snap channel");
        bool matched = false;
        QVERIFY(wta->applyOpenRoutingForTiling(w, QStringLiteral("DP-1"), &matched).isEmpty());
        QVERIFY2(matched, "…and on the tiling channel");

        const QString other = QStringLiteral("otherapp|inst6");
        wta->setFrameGeometry(other, 100, 100, 800, 600);
        QVERIFY2(!wta->applyOpenScreenRouting(other, QStringLiteral("DP-1")), "no rule → no ownership");
        bool otherMatched = true;
        QVERIFY(wta->applyOpenRoutingForTiling(other, QStringLiteral("DP-1"), &otherMatched).isEmpty());
        QVERIFY2(!otherMatched, "no rule → no ownership on the tiling channel either");
    }

    void testGuardedHandoff_refusalRehomesIntoSource()
    {
        QObject owner;
        auto* source = new PhosphorScrollEngine::ScrollEngine(nullptr, nullptr, &owner);
        source->setActiveScreens({QStringLiteral("S1")});
        source->windowOpened(QStringLiteral("app|gh"), QStringLiteral("S1"), 0, 0);
        QVERIFY(source->isWindowTracked(QStringLiteral("app|gh")));

        auto* dest = new PhosphorScrollEngine::ScrollEngine(nullptr, nullptr, &owner);
        // dest claims NOTHING → handoffReceive refuses.
        PhosphorEngine::IPlacementEngine::HandoffContext ctx;
        ctx.windowId = QStringLiteral("app|gh");
        ctx.toScreenId = QStringLiteral("S2");
        ctx.fromEngineId = source->engineId();
        const bool adopted = WindowTrackingInternal::guardedHandoff(source, dest, ctx, QStringLiteral("S1"));
        QVERIFY(!adopted);
        QVERIFY(!dest->isWindowTracked(QStringLiteral("app|gh")));
        // Re-homed: still managed by the source on the recovery screen.
        QVERIFY(source->isWindowTracked(QStringLiteral("app|gh")));

        // Pre-tracked destination (source == dest): a refused move to an
        // unclaimed screen must report NOT adopted even though
        // isWindowTracked stays true throughout.
        PhosphorEngine::IPlacementEngine::HandoffContext sameCtx;
        sameCtx.windowId = QStringLiteral("app|gh");
        sameCtx.toScreenId = QStringLiteral("S9");
        sameCtx.fromEngineId = source->engineId();
        const bool sameAdopted = WindowTrackingInternal::guardedHandoff(source, source, sameCtx, QStringLiteral("S1"));
        QVERIFY(!sameAdopted);
        QVERIFY(source->isWindowTracked(QStringLiteral("app|gh")));
        QCOMPARE(source->screenForTrackedWindow(QStringLiteral("app|gh")), QStringLiteral("S1"));
    }

    /// Cross-desktop refusal: the recovery must land on the window's SOURCE
    /// desktop, not the recovery screen's current one. A cross-desktop
    /// crossing passes recoverDesktop for exactly this reason — without it
    /// the re-home silently teleports the window onto whatever desktop is
    /// visible, and the user finds it on the wrong desktop after a refusal.
    void testGuardedHandoff_crossDesktopRefusalRecoversSourceDesktop()
    {
        QObject owner;
        auto* source = new PhosphorScrollEngine::ScrollEngine(nullptr, nullptr, &owner);
        source->setActiveScreens({QStringLiteral("S1")});
        source->windowOpened(QStringLiteral("app|xd"), QStringLiteral("S1"), 0, 0);
        QVERIFY(source->isWindowTracked(QStringLiteral("app|xd")));

        auto* dest = new PhosphorScrollEngine::ScrollEngine(nullptr, nullptr, &owner);
        // dest claims nothing → the cross-desktop receive is refused.
        PhosphorEngine::IPlacementEngine::HandoffContext ctx;
        ctx.windowId = QStringLiteral("app|xd");
        ctx.toScreenId = QStringLiteral("S2");
        ctx.toDesktop = 4;
        ctx.fromEngineId = source->engineId();
        const bool adopted =
            WindowTrackingInternal::guardedHandoff(source, dest, ctx, QStringLiteral("S1"), /*recoverDesktop=*/7);
        QVERIFY(!adopted);
        QVERIFY(source->isWindowTracked(QStringLiteral("app|xd")));

        const auto recovered = source->capturePlacement(QStringLiteral("app|xd"));
        QVERIFY(recovered.has_value());
        QCOMPARE(recovered->screenId, QStringLiteral("S1"));
        QCOMPARE(recovered->virtualDesktop, 7);
    }

    /// Cross-mode MOVE adoption gating at the adaptor level: when the target
    /// engine refuses the arrival (its screen set does not claim the release
    /// screen), handleCrossModeMove must leave the window managed by the
    /// source and must NOT arm the daemon-owned output-move marker — a marker
    /// for a move that never happens swallows the window's next genuine
    /// outputChanged.
    void testCrossModeMove_refusedByTarget_rehomesAndArmsNoMarker()
    {
        QObject owner;
        auto* scroll = new PhosphorScrollEngine::ScrollEngine(nullptr, nullptr, &owner);
        // The engine claims NOTHING, so its handoffReceive refuses — while the
        // registry still routes DP-2 to it by mode (the mid-flip shape).
        m_wta->setEngines(m_snapEngine, nullptr, scroll);
        // Restored on every exit: `scroll` dies with `owner` at the end of this
        // test, before cleanupFixture runs.
        const auto restoreEngines = qScopeGuard([this] {
            m_wta->setEngines(m_snapEngine, nullptr, nullptr);
        });
        PhosphorZones::AssignmentEntry scrolling;
        scrolling.mode = PhosphorZones::AssignmentEntry::Scrolling;
        m_layoutManager->setAssignmentEntryDirect(QStringLiteral("DP-2"), 0, QString(), scrolling);
        m_layoutManager->assignLayout(m_screenId, m_layoutManager->currentVirtualDesktop(), QString(), m_testLayout);

        const QString w = QStringLiteral("app|crossmove");
        m_snapEngine->commitSnap(w, m_zoneIds[0], m_screenId);
        QVERIFY(m_snapEngine->isWindowTracked(w));

        QSignalSpy markerSpy(m_wta, &WindowTrackingAdaptor::windowOutputMoveExpected);
        Q_EMIT m_snapEngine->crossModeMoveRequested(w, QStringLiteral("DP-2"), 0, QStringLiteral("right"));

        QVERIFY2(!scroll->isWindowTracked(w), "the refusing target must not end up tracking the window");
        QVERIFY2(m_snapEngine->isWindowTracked(w), "a refused crossing must re-home the window into the source");
        QCOMPARE(m_snapEngine->screenForTrackedWindow(w), m_screenId);
        QVERIFY2(markerSpy.isEmpty(), "no output-move marker may be armed for a refused crossing");
        // Nothing left the source monitor, so the screen-leave release never
        // ran: the re-homed window keeps its zone there.
        QCOMPARE(m_snapEngine->zoneForWindow(w), m_zoneIds[0]);
    }

    /// Auto-assign open placement skips OCCUPIED zones: snapToEmptyZone must
    /// hoist the window into a free zone rather than stacking it onto the
    /// first zone in the layout, and must decline once every zone is taken.
    void testSnapToEmptyZone_skipsOccupiedZonesThenDeclines()
    {
        m_layoutManager->assignLayout(m_screenId, m_layoutManager->currentVirtualDesktop(), QString(), m_testLayout);
        m_settings->setAutoAssignAllLayouts(true); // the layout's own flag is off by default

        m_snapEngine->commitSnap(QStringLiteral("app1|occupant"), m_zoneIds[0], m_screenId);

        const QString w = QStringLiteral("app2|arrival");
        int x = 0, y = 0, width = 0, height = 0;
        bool shouldSnap = false;
        m_snapAdaptor->snapToEmptyZone(w, m_screenId, false, x, y, width, height, shouldSnap);

        QVERIFY(shouldSnap);
        QVERIFY(width > 0 && height > 0);
        const QString landed = m_wta->getWindowState(w).zoneId;
        QVERIFY2(landed != m_zoneIds[0], "the occupied zone must not be reused while empty zones remain");
        QVERIFY(landed == m_zoneIds[1] || landed == m_zoneIds[2]);

        // Fill the layout: with no empty zone left there is nothing to hoist into.
        m_snapEngine->commitSnap(QStringLiteral("app3|filler-a"), m_zoneIds[1], m_screenId);
        m_snapEngine->commitSnap(QStringLiteral("app4|filler-b"), m_zoneIds[2], m_screenId);

        bool lateSnap = true;
        m_snapAdaptor->snapToEmptyZone(QStringLiteral("app5|late"), m_screenId, false, x, y, width, height, lateSnap);
        QVERIFY2(!lateSnap, "a full layout must decline the empty-zone hoist");
    }

    // ── Open routing runs once per open ──

    // The first routing pass for an open latches its verdict until the window
    // closes. A second pass (the other open channel, a re-announce) moves
    // nothing and still reports the match; windowClosed drops the latch so a
    // reused window id routes again.
    void testOpenRouting_routesOncePerOpenUntilClose()
    {
        auto* registry = new PhosphorEngine::WindowRegistry(m_parent);
        m_wta->setWindowRegistry(registry);
        m_wta->setWindowMetadata(QStringLiteral("once"), QStringLiteral("deskapp"), QString(), QString(), QString(), 0,
                                 0, QString(), 0, QVariantMap());

        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("deskapp"));
        rule.actions = {desktopRoute(4)};
        RuleStore store(ConfigDefaults::rulesFilePath(), nullptr); // stack object: no QObject parent
        QVERIFY(store.addRule(rule));
        m_wta->setRuleStore(&store);
        const auto teardown = qScopeGuard([this] {
            m_wta->setRuleStore(nullptr);
            m_wta->setWindowRegistry(nullptr);
        });

        const QString w = QStringLiteral("deskapp|once");
        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);
        QVERIFY(m_wta->applyOpenDesktopRouting(w, QStringLiteral("DP-1")));
        QVERIFY2(m_wta->applyOpenDesktopRouting(w, QStringLiteral("DP-1")),
                 "the second pass re-reports the latched match");
        QCOMPARE(desktopSpy.count(), 1);

        m_wta->windowClosed(w, static_cast<int>(PhosphorEngine::WindowKind::Unknown), QStringLiteral("DP-1"));
        m_wta->setWindowMetadata(QStringLiteral("once"), QStringLiteral("deskapp"), QString(), QString(), QString(), 0,
                                 0, QString(), 0, QVariantMap());
        QVERIFY(m_wta->applyOpenDesktopRouting(w, QStringLiteral("DP-1")));
        QCOMPARE(desktopSpy.count(), 2);
        QCOMPARE(desktopSpy.at(1).at(1).toInt(), 4);
    }

    // The tiling channel routes on a first placement only: a genuine open
    // always, a re-announce only for a window the placement store has no
    // record of.
    void testIsFirstTilingPlacement_openOrUnrecordedOnly()
    {
        PhosphorProtocol::WindowOpenedEntry opened;
        opened.windowId = QStringLiteral("tileapp|open");
        opened.focusEligible = true;
        QVERIFY(m_wta->isFirstTilingPlacement(opened));

        PhosphorProtocol::WindowOpenedEntry unrecorded;
        unrecorded.windowId = QStringLiteral("tileapp|fresh");
        unrecorded.focusEligible = false;
        QVERIFY2(m_wta->isFirstTilingPlacement(unrecorded), "a window this daemon never placed routes on re-announce");

        PhosphorEngine::WindowPlacement record;
        record.windowId = QStringLiteral("tileapp|placed");
        record.appId = QStringLiteral("tileapp");
        record.screenId = m_screenId;
        QVERIFY(m_wta->service()->placementStore().record(record));
        PhosphorProtocol::WindowOpenedEntry reannounced;
        reannounced.windowId = record.windowId;
        reannounced.focusEligible = false;
        QVERIFY2(!m_wta->isFirstTilingPlacement(reannounced), "a placed window's re-announce does not route again");

        // A genuine open of that same window is still a first placement.
        reannounced.focusEligible = true;
        QVERIFY(m_wta->isFirstTilingPlacement(reannounced));
    }

    // ── Open routing: RouteToWorkspace ──

    // Without a resolver (dynamic workspaces off) the workspace slot is inert:
    // alone it reports no match, beside a RouteToDesktop the positional route
    // applies.
    void testEmitOpenRouting_workspaceRouteWithoutResolverIsInert()
    {
        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);

        PhosphorRules::ResolvedActions alone;
        QVERIFY(
            alone.fillSlot(QString(PhosphorRules::ActionSlot::RouteWorkspace), workspaceRoute(QStringLiteral("Work"))));
        QVERIFY(!m_wta->emitOpenRoutingIfMatched(alone, QStringLiteral("wsapp|alone")));
        QCOMPARE(desktopSpy.count(), 0);

        PhosphorRules::ResolvedActions withDesktop;
        QVERIFY(withDesktop.fillSlot(QString(PhosphorRules::ActionSlot::RouteWorkspace),
                                     workspaceRoute(QStringLiteral("Work"))));
        QVERIFY(withDesktop.fillSlot(QString(PhosphorRules::ActionSlot::RouteDesktop), desktopRoute(3)));
        QVERIFY(m_wta->emitOpenRoutingIfMatched(withDesktop, QStringLiteral("wsapp|withdesk")));
        QCOMPARE(desktopSpy.count(), 1);
        QCOMPARE(desktopSpy.at(0).at(1).toInt(), 3);
    }

    // A realized workspace decides the desktop over a RouteToDesktop in the
    // same cascade: the resolver moves the window, so no positional move is
    // emitted. The resolver is asked with the TRIMMED id and name, with the
    // output leg on, and the owner it names is where the open lands.
    void testEmitOpenRouting_realizedWorkspaceWinsOverDesktopRoute()
    {
        QList<ResolverCall> calls;
        installResolver(calls, [](const QString&) {
            return ResolverAnswer{2, QStringLiteral("DP-2")};
        });
        const auto teardown = qScopeGuard([this] {
            m_wta->setWorkspaceRouteResolver({});
        });

        const QString entryId = QUuid::createUuid().toString();
        PhosphorRules::ResolvedActions resolved;
        QVERIFY(resolved.fillSlot(QString(PhosphorRules::ActionSlot::RouteWorkspace),
                                  workspaceRoute(QStringLiteral("  Work  "), QStringLiteral("  ") + entryId)));
        QVERIFY(resolved.fillSlot(QString(PhosphorRules::ActionSlot::RouteDesktop), desktopRoute(3)));

        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);
        const QString w = QStringLiteral("wsapp|realized");
        QVERIFY(m_wta->emitOpenRoutingIfMatched(resolved, w));
        QCOMPARE(desktopSpy.count(), 0);

        QCOMPARE(calls.size(), 1);
        const ResolverCall call = calls.constFirst();
        QCOMPARE(call.entryId, entryId);
        QCOMPARE(call.name, QStringLiteral("Work"));
        QCOMPARE(call.windowId, w);
        QVERIFY2(call.moveOutput, "a declared workspace decides the monitor too");
        QCOMPARE(m_wta->openRouteLandingScreen(w, QStringLiteral("DP-1")), QStringLiteral("DP-2"));
    }

    // An undeclared workspace (the resolver answers 0) leaves the positional
    // route in charge, and with none beside it nothing matched. A route
    // whose id and name are both blank after trimming never reaches the
    // resolver.
    void testEmitOpenRouting_undeclaredWorkspaceFallsThrough()
    {
        QList<ResolverCall> calls;
        installResolver(calls, [](const QString&) {
            return ResolverAnswer{0, QString()};
        });
        const auto teardown = qScopeGuard([this] {
            m_wta->setWorkspaceRouteResolver({});
        });
        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);

        PhosphorRules::ResolvedActions withDesktop;
        QVERIFY(withDesktop.fillSlot(QString(PhosphorRules::ActionSlot::RouteWorkspace),
                                     workspaceRoute(QStringLiteral("Elsewhere"))));
        QVERIFY(withDesktop.fillSlot(QString(PhosphorRules::ActionSlot::RouteDesktop), desktopRoute(3)));
        QVERIFY(m_wta->emitOpenRoutingIfMatched(withDesktop, QStringLiteral("wsapp|undeclared")));
        QCOMPARE(calls.size(), 1);
        QCOMPARE(desktopSpy.count(), 1);
        QCOMPARE(desktopSpy.at(0).at(1).toInt(), 3);
        QCOMPARE(m_wta->openRouteLandingScreen(QStringLiteral("wsapp|undeclared"), QStringLiteral("DP-1")),
                 QStringLiteral("DP-1"));

        PhosphorRules::ResolvedActions alone;
        QVERIFY(alone.fillSlot(QString(PhosphorRules::ActionSlot::RouteWorkspace),
                               workspaceRoute(QStringLiteral("Elsewhere"))));
        QVERIFY(!m_wta->emitOpenRoutingIfMatched(alone, QStringLiteral("wsapp|undeclared-alone")));
        QCOMPARE(calls.size(), 2);

        PhosphorRules::ResolvedActions blank;
        QVERIFY(blank.fillSlot(QString(PhosphorRules::ActionSlot::RouteWorkspace),
                               workspaceRoute(QStringLiteral("   "), QStringLiteral("  "))));
        QVERIFY(blank.fillSlot(QString(PhosphorRules::ActionSlot::RouteDesktop), desktopRoute(5)));
        QVERIFY(m_wta->emitOpenRoutingIfMatched(blank, QStringLiteral("wsapp|blank")));
        QCOMPARE(calls.size(), 2);
        QCOMPARE(desktopSpy.count(), 2);
        QCOMPARE(desktopSpy.at(1).at(1).toInt(), 5);
    }

    // Declared but momentarily unresolvable (the resolver answers < 0): the
    // route is parked, the open still belongs to the workspace, and neither
    // the positional desktop nor any monitor applies meanwhile.
    void testEmitOpenRouting_parkedWorkspaceClaimsWithoutMoving()
    {
        QList<ResolverCall> calls;
        installResolver(calls, [](const QString&) {
            return ResolverAnswer{-1, QString()};
        });
        const auto teardown = qScopeGuard([this] {
            m_wta->setWorkspaceRouteResolver({});
        });

        PhosphorRules::ResolvedActions resolved;
        QVERIFY(resolved.fillSlot(QString(PhosphorRules::ActionSlot::RouteWorkspace),
                                  workspaceRoute(QStringLiteral("Work"))));
        QVERIFY(resolved.fillSlot(QString(PhosphorRules::ActionSlot::RouteDesktop), desktopRoute(3)));

        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);
        const QString w = QStringLiteral("wsapp|parked");
        QVERIFY2(m_wta->emitOpenRoutingIfMatched(resolved, w), "a parked workspace route still owns the open");
        QCOMPARE(desktopSpy.count(), 0);
        QCOMPARE(calls.size(), 1);
        QCOMPARE(m_wta->openRouteLandingScreen(w, QStringLiteral("DP-1")), QStringLiteral("DP-1"));
    }

    // The snap placement directive follows the open's workspace verdict, not
    // the rule's RouteToDesktop: parked names no desktop, a sticky window
    // names none either (it is on every desktop and the resolver moved
    // nothing), a realized route names its desktop and owner. The verdict is
    // single-use: a later builder call that is no open (the Meta+F unfloat)
    // reads the rule's own positional route instead.
    void testPlacementDirective_followsTheWorkspaceVerdict()
    {
        auto* registry = new PhosphorEngine::WindowRegistry(m_parent);
        m_wta->setWindowRegistry(registry);
        for (const QString& instance :
             {QStringLiteral("parked"), QStringLiteral("sticky"), QStringLiteral("realized")}) {
            m_wta->setWindowMetadata(instance, QStringLiteral("wsapp"), QString(), QString(), QString(), 0, 0,
                                     QString(), 0, QVariantMap());
        }

        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("wsapp"));
        rule.actions = {workspaceRoute(QStringLiteral("Work")), desktopRoute(3)};
        RuleStore store(ConfigDefaults::rulesFilePath(), nullptr); // stack object: no QObject parent
        QVERIFY(store.addRule(rule));
        m_wta->setRuleStore(&store);

        QList<ResolverCall> calls;
        installResolver(calls, [](const QString& windowId) {
            if (windowId.endsWith(QLatin1String("|parked"))) {
                return ResolverAnswer{-1, QString()};
            }
            return ResolverAnswer{2, QStringLiteral("DP-1")};
        });
        const auto teardown = qScopeGuard([this] {
            m_wta->setWorkspaceRouteResolver({});
            m_wta->setRuleStore(nullptr);
            m_wta->setWindowRegistry(nullptr);
        });

        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);

        const QString parked = QStringLiteral("wsapp|parked");
        QVERIFY(m_wta->applyOpenDesktopRouting(parked, QStringLiteral("DP-1")));
        const PhosphorSnapEngine::PlacementDirective parkedDirective =
            m_wta->placementZonesByRule(parked, QStringLiteral("DP-1"));
        QCOMPARE(parkedDirective.targetDesktop, 0);
        QVERIFY(parkedDirective.targetScreenId.isEmpty());

        const QString sticky = QStringLiteral("wsapp|sticky");
        m_wta->service()->setWindowSticky(sticky, true);
        QVERIFY(m_wta->applyOpenDesktopRouting(sticky, QStringLiteral("DP-1")));
        const PhosphorSnapEngine::PlacementDirective stickyDirective =
            m_wta->placementZonesByRule(sticky, QStringLiteral("DP-1"));
        QCOMPARE(stickyDirective.targetDesktop, 0);
        QVERIFY(stickyDirective.targetScreenId.isEmpty());

        const QString realized = QStringLiteral("wsapp|realized");
        QVERIFY(m_wta->applyOpenDesktopRouting(realized, QStringLiteral("DP-1")));
        const PhosphorSnapEngine::PlacementDirective realizedDirective =
            m_wta->placementZonesByRule(realized, QStringLiteral("DP-1"));
        QCOMPARE(realizedDirective.targetDesktop, 2);
        QCOMPARE(realizedDirective.targetScreenId, QStringLiteral("DP-1"));
        const PhosphorSnapEngine::PlacementDirective laterDirective =
            m_wta->placementZonesByRule(realized, QStringLiteral("DP-1"));
        QCOMPARE(laterDirective.targetDesktop, 3);

        QCOMPARE(calls.size(), 3);
        QCOMPARE(desktopSpy.count(), 0);
    }

    // route_precedence on both channels: a realized workspace decides the
    // monitor, so a RouteToScreen in the same cascade stands down. On the snap
    // channel that means no move at all; on the tiling channel the redirect
    // names the workspace owner (empty when the owner is the spawn screen),
    // and the directive is matched either way.
    void testWorkspaceRoute_overridesRouteToScreenOnBothChannels()
    {
        PhosphorScreens::FakePhysicalScreenSource fake;
        fake.addScreen(QStringLiteral("DP-1"), QRect(0, 0, 1920, 1080), QStringLiteral("DP-1"));
        fake.addScreen(QStringLiteral("DP-2"), QRect(1920, 0, 1920, 1080), QStringLiteral("DP-2"));
        PhosphorScreens::ScreenManager screenMgr(
            PhosphorScreens::ScreenManagerConfig{.physicalScreenSource = &fake, .useGeometrySensors = false});
        screenMgr.start();

        PhosphorZones::AssignmentEntry autotile;
        autotile.mode = PhosphorZones::AssignmentEntry::Autotile;
        autotile.tilingAlgorithm = QStringLiteral("dwindle");
        m_layoutManager->setAssignmentEntryDirect(QStringLiteral("DP-2"), 0, QString(), autotile);

        QObject parent;
        auto* wta = new WindowTrackingAdaptor(m_layoutManager, m_zoneDetector, &screenMgr, m_settings, nullptr, nullptr,
                                              &parent);
        auto* registry = new PhosphorEngine::WindowRegistry(&parent);
        wta->setWindowRegistry(registry);
        for (const QString& instance : {QStringLiteral("snap"), QStringLiteral("tile"), QStringLiteral("home")}) {
            wta->setWindowMetadata(instance, QStringLiteral("wsroute"), QString(), QString(), QString(), 0, 0,
                                   QString(), 0, QVariantMap());
        }

        using namespace PhosphorRules;
        Rule rule;
        rule.id = QUuid::createUuid();
        rule.enabled = true;
        rule.match = MatchExpression::makeLeaf(Field::AppId, Operator::AppIdMatches, QStringLiteral("wsroute"));
        RuleAction screenRoute;
        screenRoute.type = QString(ActionType::RouteToScreen);
        screenRoute.params.insert(QString(ActionParam::TargetScreenId), QStringLiteral("DP-1"));
        rule.actions = {workspaceRoute(QStringLiteral("Work")), screenRoute};
        RuleStore store(ConfigDefaults::rulesFilePath(), nullptr); // stack object: no QObject parent
        QVERIFY(store.addRule(rule));
        wta->setRuleStore(&store);
        wta->setWorkspaceRouteResolver(
            [](const QString&, const QString&, const QString& windowId, bool, QString* ownerScreenOut) {
                if (ownerScreenOut) {
                    *ownerScreenOut =
                        windowId.endsWith(QLatin1String("|home")) ? QStringLiteral("DP-1") : QStringLiteral("DP-2");
                }
                return 2;
            });
        const auto teardown = qScopeGuard([wta] {
            wta->setWorkspaceRouteResolver({});
            wta->setRuleStore(nullptr);
            wta->setWindowRegistry(nullptr);
        });

        // Snap channel. The window spawned on DP-2, the workspace owner, while
        // the RouteToScreen names DP-1: honouring the screen route would move
        // the window off the monitor its workspace lives on.
        const QString snapWindow = QStringLiteral("wsroute|snap");
        wta->setFrameGeometry(snapWindow, 2020, 100, 800, 600);
        QSignalSpy outputSpy(wta, &WindowTrackingAdaptor::windowOutputMoveExpected);
        QSignalSpy geomSpy(wta, &WindowTrackingAdaptor::applyGeometryRequested);
        QVERIFY(wta->applyOpenDesktopRouting(snapWindow, QStringLiteral("DP-2")));
        QVERIFY2(wta->applyOpenScreenRouting(snapWindow, QStringLiteral("DP-2")),
                 "the workspace owns the window's monitor");
        QCOMPARE(geomSpy.count(), 0);
        QCOMPARE(outputSpy.count(), 0);

        // Tiling channel. Spawned on DP-1, which the RouteToScreen names, but
        // the workspace owner is DP-2: the redirect goes there.
        bool matched = false;
        const QString tileWindow = QStringLiteral("wsroute|tile");
        QCOMPARE(wta->applyOpenRoutingForTiling(tileWindow, QStringLiteral("DP-1"), &matched), QStringLiteral("DP-2"));
        QVERIFY(matched);
        QCOMPARE(outputSpy.count(), 1);
        QCOMPARE(outputSpy.at(0).at(1).toString(), QStringLiteral("DP-2"));

        // The owner is the spawn screen: no redirect, still matched.
        bool homeMatched = false;
        QVERIFY(wta->applyOpenRoutingForTiling(QStringLiteral("wsroute|home"), QStringLiteral("DP-1"), &homeMatched)
                    .isEmpty());
        QVERIFY(homeMatched);
        QCOMPARE(outputSpy.count(), 1);
    }

    // ── Workspace move verb ──

    // A window no engine places moves the way KWin's own desktop move would:
    // ONE windowWorkspaceMoveRequested carrying both legs, never the plain
    // desktop move. Without the output leg the window keeps its output. A
    // sticky window is refused outright.
    void testMoveWindowToWorkspaceVerb_untrackedWindowRidesOneWorkspaceMove()
    {
        QSignalSpy workspaceSpy(m_wta, &WindowTrackingAdaptor::windowWorkspaceMoveRequested);
        QSignalSpy desktopSpy(m_wta, &WindowTrackingAdaptor::windowDesktopMoveRequested);
        const QString desktopId = QUuid::createUuid().toString(QUuid::WithoutBraces);

        const QString loose = QStringLiteral("looseapp|one");
        m_wta->moveWindowToWorkspaceVerb(loose, QStringLiteral("DP-2"), 3, desktopId, QString());
        QCOMPARE(workspaceSpy.count(), 1);
        QCOMPARE(desktopSpy.count(), 0);
        const QList<QVariant> withOutput = workspaceSpy.at(0);
        QCOMPARE(withOutput.at(0).toString(), loose);
        QCOMPARE(withOutput.at(1).toString(), desktopId);
        QCOMPARE(withOutput.at(2).toInt(), 3);
        QCOMPARE(withOutput.at(3).toString(), QStringLiteral("DP-2"));

        m_wta->moveWindowToWorkspaceVerb(QStringLiteral("looseapp|two"), QStringLiteral("DP-2"), 3, desktopId,
                                         QString(), /*moveOutput=*/false);
        QCOMPARE(workspaceSpy.count(), 2);
        QVERIFY2(workspaceSpy.at(1).at(3).toString().isEmpty(), "no output leg: the window keeps its output");

        const QString sticky = QStringLiteral("looseapp|sticky");
        m_wta->service()->setWindowSticky(sticky, true);
        m_wta->moveWindowToWorkspaceVerb(sticky, QStringLiteral("DP-2"), 3, desktopId, QString());
        QCOMPARE(workspaceSpy.count(), 2);
        QCOMPARE(desktopSpy.count(), 0);
    }

    // ── Per-output desktops report ──

    // The effect's mode report is cached and relayed in-process once per
    // change: a repeat of the cached value is dropped, so the workspace gate
    // is not re-run for nothing.
    void testReportPerOutputDesktopsMode_cachesAndRelaysChangesOnly()
    {
        QVERIFY(!m_wta->perOutputDesktopsMode().has_value());
        QSignalSpy relaySpy(m_wta->lifecycleRelay(), &WindowLifecycleRelay::perOutputDesktopsModeReported);

        m_wta->reportPerOutputDesktopsMode(true);
        QVERIFY(m_wta->perOutputDesktopsMode() == std::optional<bool>(true));
        QCOMPARE(relaySpy.count(), 1);
        QCOMPARE(relaySpy.at(0).at(0).toBool(), true);

        m_wta->reportPerOutputDesktopsMode(true);
        QCOMPARE(relaySpy.count(), 1);

        m_wta->reportPerOutputDesktopsMode(false);
        QVERIFY(m_wta->perOutputDesktopsMode() == std::optional<bool>(false));
        QCOMPARE(relaySpy.count(), 2);
        QCOMPARE(relaySpy.at(1).at(0).toBool(), false);
    }

private:
    struct ResolverCall
    {
        QString entryId;
        QString name;
        QString windowId;
        bool moveOutput = false;
    };
    struct ResolverAnswer
    {
        int desktop = 0;
        QString ownerScreen;
    };

    /// A fake workspace resolver on the fixture adaptor: records every call in
    /// @p calls and answers what @p answer returns for the window. The caller
    /// clears it again with a scope guard, since @p calls lives on its stack.
    void installResolver(QList<ResolverCall>& calls, std::function<ResolverAnswer(const QString&)> answer)
    {
        m_wta->setWorkspaceRouteResolver(
            [&calls, answer = std::move(answer)](const QString& entryId, const QString& name, const QString& windowId,
                                                 bool moveOutput, QString* ownerScreenOut) {
                calls.append(ResolverCall{entryId, name, windowId, moveOutput});
                const ResolverAnswer a = answer(windowId);
                if (ownerScreenOut) {
                    *ownerScreenOut = a.ownerScreen;
                }
                return a.desktop;
            });
    }

    static PhosphorRules::RuleAction workspaceRoute(const QString& name, const QString& entryId = QString())
    {
        PhosphorRules::RuleAction action;
        action.type = QString(PhosphorRules::ActionType::RouteToWorkspace);
        action.params.insert(QString(PhosphorRules::ActionParam::TargetWorkspaceName), name);
        if (!entryId.isNull()) {
            action.params.insert(QString(PhosphorRules::ActionParam::TargetWorkspaceId), entryId);
        }
        return action;
    }

    static PhosphorRules::RuleAction desktopRoute(int desktop)
    {
        PhosphorRules::RuleAction action;
        action.type = QString(PhosphorRules::ActionType::RouteToDesktop);
        action.params.insert(QString(PhosphorRules::ActionParam::TargetDesktop), desktop);
        return action;
    }
};

QTEST_MAIN(TestWtaRouting)
#include "test_wta_routing.moc"
