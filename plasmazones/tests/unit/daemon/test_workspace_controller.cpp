// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// WorkspaceController against a real reconciler, a real window registry, a
// real ScreenManager over synthetic outputs and a VirtualDesktopManager fed
// desktop lists directly (no KWin, no D-Bus). Covers the adoption gate and
// its timeout, the census across desktop-list changes, the desktop-switch OSD
// suppression around snap-back, the RouteToWorkspace resolver, screen
// removal, first adoption by window location, and the strip stash of a
// workspace moved to another monitor.

#include "daemon/controllers/workspacecontroller.h"

#include "daemon/controllers/shortcutmanager.h"

#include <PhosphorEngine/WindowRegistry.h>
#include <PhosphorScreens/Manager.h>
#include <PhosphorWorkspaces/VirtualDesktopManager.h>
#include <PhosphorWorkspaces/WorkspaceReconciler.h>

#include "FakePhysicalScreenSource.h"
#include "config/configdefaults.h"

#include <QFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QUuid>

#include <memory>

using PlasmaZones::WorkspaceController;

namespace {

/// Commits desktop lists and replays KWin's desktop signals the way the
/// manager's D-Bus handlers do once a message is parsed.
class ListDriver : public PhosphorWorkspaces::VirtualDesktopManager
{
public:
    using VirtualDesktopManager::DesktopEntry;

    void commit(const QStringList& ids, const QStringList& names = {})
    {
        QList<DesktopEntry> entries;
        for (int i = 0; i < ids.size(); ++i) {
            entries.append(DesktopEntry{i, ids.at(i), names.value(i)});
        }
        QVERIFY(commitDesktopList(entries, ids.value(0), 1));
    }
    void removed(const QString& desktopId)
    {
        QVERIFY(QMetaObject::invokeMethod(this, "onKWinDesktopRemoved", Q_ARG(QString, desktopId)));
    }
    void count(uint count)
    {
        QVERIFY(QMetaObject::invokeMethod(this, "onNumberOfDesktopsChanged", Q_ARG(uint, count)));
    }
};

PhosphorEngine::WindowMetadata onDesktop(int desktop)
{
    PhosphorEngine::WindowMetadata meta;
    meta.virtualDesktop = desktop;
    meta.virtualDesktops = {desktop};
    meta.isSticky = false;
    return meta;
}

/// One world per test: outputs, screen manager, desktops, registry and the
/// controller under test. Ledger and debounce timings are shortened.
struct World
{
    PhosphorScreens::FakePhysicalScreenSource screens;
    std::unique_ptr<PhosphorScreens::ScreenManager> screenManager;
    ListDriver vdm;
    PhosphorEngine::WindowRegistry registry;
    std::unique_ptr<WorkspaceController> controller;

    void addScreen(const QString& id, int x)
    {
        screens.addScreen(id, QRect(x, 0, 1920, 1080), id);
    }
    void build()
    {
        screenManager = std::make_unique<PhosphorScreens::ScreenManager>(
            PhosphorScreens::ScreenManagerConfig{.physicalScreenSource = &screens, .useGeometrySensors = false});
        screenManager->start();
        controller = std::make_unique<WorkspaceController>(&vdm, &registry, screenManager.get());
        controller->reconciler().setTimeouts(/*ledgerTimeoutMs=*/400, /*destroyDebounceMs=*/50);
    }
    PhosphorWorkspaces::WorkspaceReconciler& reconciler()
    {
        return controller->reconciler();
    }
};

const QString ScreenA = QStringLiteral("PZTEST:A:1");
const QString ScreenB = QStringLiteral("PZTEST:B:2");

} // namespace

class TestWorkspaceController : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        // The controller restores last session's map from this file and
        // writes it back on destruction; every case starts from no file.
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::StateLocation)
                      + QStringLiteral("/workspaces.json"));
    }

    /// With no per-output report at all the compositor is not there, nothing
    /// has pushed the window set, and adopting would reap every desktop that
    /// holds windows. The timeout waits instead. Once a screen reports, the
    /// timeout adopts, seeding only the screens that reported.
    void adoptionTimeout_waitsForCompositor_andSeedsOnlyReportedScreens()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.addScreen(ScreenB, 1920);
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3")});
        world.build();
        world.controller->start();

        QTest::qWait(3300);
        QVERIFY(!world.reconciler().isAdopted());

        world.vdm.updateScreenDesktop(ScreenA, 1);
        QVERIFY(!world.reconciler().isAdopted()); // B has not reported
        QTRY_VERIFY_WITH_TIMEOUT(world.reconciler().isAdopted(), 4000);
        QCOMPARE(world.reconciler().currentDesktopIdOf(ScreenA), QStringLiteral("d1"));
        // B never reported: the global current is not B's to claim.
        QVERIFY(world.reconciler().currentDesktopIdOf(ScreenB).isEmpty());
    }

    /// KWin moves the windows of a removed desktop to its neighbour, which
    /// takes the removed desktop's number, so the registry's numbers do not
    /// change and no metadata event recounts the windows. The post-commit
    /// reseed counts them on the desktop they landed on: here the trailing
    /// empty becomes occupied and a new trailing empty is asked for.
    void externalRemoval_recountsTheWindowsKWinMoved()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3")});
        world.registry.upsert(QStringLiteral("w"), onDesktop(2));
        world.build();
        world.controller->start();
        world.vdm.updateScreenDesktop(ScreenA, 1);
        QVERIFY(world.reconciler().isAdopted());
        QTest::qWait(150);

        QSignalSpy creates(&world.reconciler(), &PhosphorWorkspaces::WorkspaceReconciler::requestCreateDesktop);
        world.vdm.removed(QStringLiteral("d2"));
        world.vdm.count(2);
        QCOMPARE(world.vdm.desktopIds(), (QStringList{QStringLiteral("d1"), QStringLiteral("d3")}));
        QTRY_VERIFY_WITH_TIMEOUT(!creates.isEmpty(), 1000);
    }

    /// A window whose desktop number is past the end of the list when it is
    /// pushed (the list carrying that desktop has not committed yet) is
    /// counted once the list commits, so the desktop is not reaped as empty.
    void windowPushedAheadOfItsList_isCountedWhenTheListCommits()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2")});
        world.build();
        world.controller->start();
        world.vdm.updateScreenDesktop(ScreenA, 1);
        QVERIFY(world.reconciler().isAdopted());
        QTest::qWait(150);

        QSignalSpy removes(&world.reconciler(), &PhosphorWorkspaces::WorkspaceReconciler::requestRemoveDesktop);
        world.registry.upsert(QStringLiteral("w"), onDesktop(3));
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3")});
        QTest::qWait(400);
        for (const QList<QVariant>& call : std::as_const(removes)) {
            QVERIFY2(call.at(0).toString() != QStringLiteral("d3"), "a desktop holding a window was reaped");
        }
    }

    /// A window placed on several desktops (not all of them) keeps each one
    /// of them: it counts toward every desktop it is on.
    void windowOnSeveralDesktops_keepsEachOfThem()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3"), QStringLiteral("d4")});
        PhosphorEngine::WindowMetadata meta = onDesktop(2);
        meta.virtualDesktops = {2, 3};
        world.registry.upsert(QStringLiteral("w"), meta);
        world.build();
        QSignalSpy removes(&world.reconciler(), &PhosphorWorkspaces::WorkspaceReconciler::requestRemoveDesktop);
        world.controller->start();
        world.vdm.updateScreenDesktop(ScreenA, 1);
        QVERIFY(world.reconciler().isAdopted());
        QTest::qWait(300);
        for (const QList<QVariant>& call : std::as_const(removes)) {
            const QString id = call.at(0).toString();
            QVERIFY2(id != QStringLiteral("d2") && id != QStringLiteral("d3"), "a desktop the window is on was reaped");
        }
    }

    /// A report that lands a screen on another screen's workspace keeps the
    /// desktop-switch card quiet, owner-wins sends a CORRECTION switch back,
    /// and the report answering it is quiet too, exactly once.
    void foreignReport_snapsBackAsCorrection_andSilencesBothCards()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.addScreen(ScreenB, 1920);
        // Each slice ends in an empty desktop (A: d1 d2, B: d3 d4), so
        // adoption asks for no create and no structural op holds owner-wins.
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3"), QStringLiteral("d4")});
        world.build();
        world.controller->start();
        world.vdm.updateScreenDesktop(ScreenA, 1);
        world.vdm.updateScreenDesktop(ScreenB, 3);
        QVERIFY(world.reconciler().isAdopted());
        QVERIFY(!world.reconciler().hasPendingStructuralOps());
        QCOMPARE(world.reconciler().map().ownerOf(QStringLiteral("d3")), ScreenB);

        QVERIFY(!world.controller->takeDesktopSwitchOsdSuppression(ScreenA, 1));
        QVERIFY(world.controller->takeDesktopSwitchOsdSuppression(ScreenA, 3));

        QSignalSpy switches(world.controller.get(), &WorkspaceController::screenDesktopSwitchRequested);
        world.vdm.updateScreenDesktop(ScreenA, 3);
        QCOMPARE(switches.count(), 1);
        QCOMPARE(switches.at(0).at(0).toString(), ScreenA);
        QCOMPARE(switches.at(0).at(1).toString(), QStringLiteral("d1"));
        QCOMPARE(switches.at(0).at(2).value<PhosphorProtocol::ScreenDesktopSwitchPurpose>(),
                 PhosphorProtocol::ScreenDesktopSwitchPurpose::Correction);

        QVERIFY(world.controller->takeDesktopSwitchOsdSuppression(ScreenA, 1));
        QVERIFY(!world.controller->takeDesktopSwitchOsdSuppression(ScreenA, 1));
    }

    /// The effect answers a switch it did not make with a report of the live
    /// desktop, which VirtualDesktopManager swallows when it equals what it
    /// already holds. Such a switch must not hold the screen until its ledger
    /// entry expires: one to the desktop already shown is retired by that
    /// answer as soon as it arrives, and an unanswered user switch gives way
    /// to the user's next press.
    void unansweredSwitch_doesNotHoldTheScreen()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3")});
        world.registry.upsert(QStringLiteral("w"), onDesktop(2));
        world.build();
        world.controller->start();
        world.vdm.updateScreenDesktop(ScreenA, 1);
        QVERIFY(world.reconciler().isAdopted());

        world.controller->focusWorkspaceAt(ScreenA, 0);
        QCOMPARE(world.reconciler().inFlightSetCurrentTarget(ScreenA), QStringLiteral("d1"));
        // The effect's forced answer, which the adaptor relays ahead of
        // VirtualDesktopManager's dedup.
        world.controller->noteScreenDesktopReport(ScreenA, 1);
        QVERIFY(world.reconciler().inFlightSetCurrentTarget(ScreenA).isEmpty());

        // A user switch the compositor has not answered does not eat the
        // user's next press: the next one replaces it.
        world.controller->focusWorkspaceAt(ScreenA, 1);
        QCOMPARE(world.reconciler().inFlightSetCurrentTarget(ScreenA), QStringLiteral("d2"));
        world.controller->focusWorkspaceAt(ScreenA, 2);
        QCOMPARE(world.reconciler().inFlightSetCurrentTarget(ScreenA), QStringLiteral("d3"));
    }

    /// A switch to the desktop the output already shows is answered by the
    /// effect and by nothing else. An answer made up daemon-side came on top
    /// of the effect's own, and that second answer was then read as the
    /// answer to the user's next press, retiring it as refused.
    void switchToTheShownDesktop_isAnsweredOnlyByTheEffect()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3")});
        world.registry.upsert(QStringLiteral("w"), onDesktop(2));
        world.build();
        world.controller->start();
        world.vdm.updateScreenDesktop(ScreenA, 1);
        QVERIFY(world.reconciler().isAdopted());

        world.controller->focusWorkspaceAt(ScreenA, 0); // d1, already shown
        QTest::qWait(50);
        QCOMPARE(world.reconciler().inFlightSetCurrentTarget(ScreenA), QStringLiteral("d1"));

        // The user's next press replaces it before the effect has answered.
        world.controller->focusWorkspaceAt(ScreenA, 1);
        QCOMPARE(world.reconciler().inFlightSetCurrentTarget(ScreenA), QStringLiteral("d2"));
        // The effect answers the first press: A is still on d1. That is the
        // first press's answer, not a refusal of the second.
        world.controller->noteScreenDesktopReport(ScreenA, 1);
        QCOMPARE(world.reconciler().inFlightSetCurrentTarget(ScreenA), QStringLiteral("d2"));
        // Then the second: the switch itself.
        world.controller->noteScreenDesktopReport(ScreenA, 2);
        world.vdm.updateScreenDesktop(ScreenA, 2);
        QVERIFY(world.reconciler().inFlightSetCurrentTarget(ScreenA).isEmpty());
        QCOMPARE(world.reconciler().currentDesktopIdOf(ScreenA), QStringLiteral("d2"));
    }

    /// A snap-back the compositor refuses is answered with the foreign desktop
    /// the screen still shows, a report VirtualDesktopManager swallows. That
    /// answer still settles the snap-back, so a later switch the user makes by
    /// hand to the workspace the snap-back aimed at gets its card.
    void refusedSnapBack_leavesNothingToSilenceALaterSwitch()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.addScreen(ScreenB, 1920);
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3"), QStringLiteral("d4")});
        world.build();
        world.controller->start();
        world.vdm.updateScreenDesktop(ScreenA, 1);
        world.vdm.updateScreenDesktop(ScreenB, 3);
        QVERIFY(world.reconciler().isAdopted());

        world.vdm.updateScreenDesktop(ScreenA, 3); // B's d3: corrected toward d1
        QCOMPARE(world.reconciler().inFlightSetCurrentTarget(ScreenA), QStringLiteral("d1"));
        QVERIFY(world.controller->takeDesktopSwitchOsdSuppression(ScreenA, 3));

        // Refused: the answer is the desktop A still shows.
        world.controller->noteScreenDesktopReport(ScreenA, 3);
        QVERIFY(world.reconciler().inFlightSetCurrentTarget(ScreenA).isEmpty());

        // The user goes to d1 by hand: an ordinary switch, with its card.
        QVERIFY(!world.controller->takeDesktopSwitchOsdSuppression(ScreenA, 1));
    }

    /// A screen-id rename carries the snap-back state with it: a correction
    /// refused inside the cooldown is not let through early by the rename,
    /// and the retry waiting for the cooldown runs under the new id instead
    /// of asking about an id the map no longer knows.
    void screenIdRename_keepsTheSnapBackCooldownAndItsRetry()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.addScreen(ScreenB, 1920);
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3"), QStringLiteral("d4")});
        world.build();
        world.controller->start();
        world.vdm.updateScreenDesktop(ScreenA, 1);
        world.vdm.updateScreenDesktop(ScreenB, 3);
        QVERIFY(world.reconciler().isAdopted());

        QSignalSpy switches(world.controller.get(), &WorkspaceController::screenDesktopSwitchRequested);
        world.vdm.updateScreenDesktop(ScreenA, 3); // foreign: corrected at once
        QCOMPARE(switches.count(), 1);
        // Refused. A is still on B's desktop, and the next correction waits
        // out the cooldown.
        world.controller->noteScreenDesktopReport(ScreenA, 3);
        QCOMPARE(switches.count(), 1);

        const QString renamed = ScreenA + QStringLiteral("/DP-1");
        world.screens.setIdentifier(ScreenA, renamed);
        Q_EMIT world.screenManager->screenIdentifierChanged(ScreenA, renamed);
        QCOMPARE(world.reconciler().map().ownerOf(QStringLiteral("d1")), renamed);
        QCOMPARE(switches.count(), 1);

        QTRY_COMPARE_WITH_TIMEOUT(switches.count(), 2, 2500);
        QCOMPARE(switches.last().at(0).toString(), renamed);
        QCOMPARE(switches.last().at(1).toString(), QStringLiteral("d1"));
    }

    /// The rule resolver finds the declaration by its entry id (any spelling
    /// of the UUID) and falls back to the name. A target no declaration
    /// carries is Unrealized, before adoption too, so the positional route
    /// stays available for it.
    void routeResolver_matchesEntryIdThenName_andLeavesUndeclaredUnrealized()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2")});
        world.registry.upsert(QStringLiteral("w"), onDesktop(1));
        world.build();

        const QUuid uuid = QUuid::createUuid();
        QVariantMap entry;
        entry.insert(PlasmaZones::ConfigDefaults::namedEntryIdField(), uuid.toString());
        entry.insert(PlasmaZones::ConfigDefaults::namedEntryNameField(), QStringLiteral("Web"));
        entry.insert(PlasmaZones::ConfigDefaults::namedEntryOutputField(), QString());
        entry.insert(PlasmaZones::ConfigDefaults::namedEntryPositionField(), -1);
        world.controller->applyNamedDeclarations({entry});
        world.controller->start();

        using Verdict = WorkspaceController::WorkspaceRouteVerdict;
        QCOMPARE(world.controller->routeWindowToNamedWorkspace(QString(), QStringLiteral("Nowhere"),
                                                               QStringLiteral("w"), true),
                 static_cast<int>(Verdict::Unrealized));
        QCOMPARE(
            world.controller->routeWindowToNamedWorkspace(QString(), QStringLiteral("Web"), QStringLiteral("w"), true),
            static_cast<int>(Verdict::Unresolvable));

        // Adoption realizes the declaration: no desktop carries the name, so
        // one is created for it. KWin's answer is a list with the new desktop
        // at the position asked for.
        QSignalSpy creates(&world.reconciler(), &PhosphorWorkspaces::WorkspaceReconciler::requestCreateDesktop);
        world.vdm.updateScreenDesktop(ScreenA, 1);
        QVERIFY(world.reconciler().isAdopted());
        QTRY_VERIFY(!creates.isEmpty());
        QCOMPARE(creates.first().at(1).toString(), QStringLiteral("Web"));
        QVERIFY(world.reconciler().hasPendingStructuralOps());
        QCOMPARE(
            world.controller->routeWindowToNamedWorkspace(QString(), QStringLiteral("Web"), QStringLiteral("w"), true),
            static_cast<int>(Verdict::Unresolvable));

        QSignalSpy moves(world.controller.get(), &WorkspaceController::windowWorkspaceMoveRequested);
        QStringList ids{QStringLiteral("d1"), QStringLiteral("d2")};
        ids.insert(static_cast<int>(creates.first().at(0).toUInt()), QStringLiteral("dw"));
        world.vdm.commit(ids);
        QCOMPARE(world.reconciler().map().entryFor(QStringLiteral("dw")).name, QStringLiteral("Web"));
        QCOMPARE(world.reconciler().map().entryFor(QStringLiteral("dw")).namedId, uuid.toString());
        // The route parked during the create re-issues once it has settled.
        QTRY_VERIFY(!moves.isEmpty());
        QCOMPARE(moves.last().at(3).toString(), QStringLiteral("dw"));
        QCOMPARE(moves.last().at(6).toBool(), true);

        // By id, in another spelling of the same UUID, whatever name the rule
        // still carries; then by name when the id names no entry.
        const int expected = world.vdm.desktopIndexOf(QStringLiteral("dw"));
        QString owner;
        const QString otherSpelling = uuid.toString(QUuid::WithoutBraces).toUpper();
        QCOMPARE(world.controller->routeWindowToNamedWorkspace(otherSpelling, QStringLiteral("Renamed since"),
                                                               QStringLiteral("w"), true, &owner),
                 expected);
        QCOMPARE(owner, ScreenA);
        QCOMPARE(world.controller->routeWindowToNamedWorkspace(QUuid::createUuid().toString(), QStringLiteral("Web"),
                                                               QStringLiteral("w"), true),
                 expected);
    }

    /// First adoption hands a desktop no screen shows to the screen its
    /// windows are on, not to the screen before it in KWin's order. Both
    /// screens show d1 here, and d2's only window sits on B.
    void firstAdoption_givesAnUnshownDesktopToTheScreenItsWindowsAreOn()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.addScreen(ScreenB, 1920);
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3")});
        world.registry.upsert(QStringLiteral("w"), onDesktop(2));
        world.build();
        world.controller->setWindowScreenResolver([](const QString& windowId) {
            return windowId == QStringLiteral("w") ? ScreenB : QString();
        });
        world.controller->start();
        world.vdm.updateScreenDesktop(ScreenA, 1);
        world.vdm.updateScreenDesktop(ScreenB, 1);
        QVERIFY(world.reconciler().isAdopted());

        // KWin order alone would give d2 to A, the owner before it.
        QCOMPARE(world.reconciler().map().ownerOf(QStringLiteral("d2")), ScreenB);
    }

    /// Moving a workspace to another monitor records its scrolling strips for
    /// that monitor before any rider moves: each rider is released from its
    /// strip on its own move, so a record taken after the first one would
    /// read a strip that has already lost a column.
    void workspaceToMonitor_stashesTheStripsBeforeTheFirstRiderMoves()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.addScreen(ScreenB, 1920);
        // A: d1 d2, B: d3 d4. The rider sits on A's current desktop.
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3"), QStringLiteral("d4")});
        world.registry.upsert(QStringLiteral("w"), onDesktop(1));
        world.build();

        struct StashCall
        {
            QStringList windowIds;
            QString targetScreenId;
            int desktop = 0;
            int movesBefore = -1;
        };
        QList<StashCall> stashes;
        int moves = 0;
        world.controller->setStripStashHandler(
            [&stashes, &moves](const QStringList& windowIds, const QString& targetScreenId, int desktop) {
                stashes.append(StashCall{windowIds, targetScreenId, desktop, moves});
            });
        QObject::connect(world.controller.get(), &WorkspaceController::windowWorkspaceMoveRequested,
                         world.controller.get(), [&moves]() {
                             ++moves;
                         });

        world.controller->start();
        world.vdm.updateScreenDesktop(ScreenA, 1);
        world.vdm.updateScreenDesktop(ScreenB, 3);
        QVERIFY(world.reconciler().isAdopted());
        QTRY_VERIFY(!world.reconciler().hasPendingStructuralOps());
        const int movesBeforeVerb = moves;

        world.controller->moveWorkspaceToOutput(ScreenA, PlasmaZones::NavigationDirection::Right);
        QTRY_VERIFY(moves > movesBeforeVerb);

        QCOMPARE(stashes.size(), 1);
        QCOMPARE(stashes.first().windowIds, QStringList{QStringLiteral("w")});
        QCOMPARE(stashes.first().targetScreenId, ScreenB);
        QCOMPARE(stashes.first().desktop, world.vdm.desktopIndexOf(QStringLiteral("d1")));
        QCOMPARE(stashes.first().movesBefore, movesBeforeVerb);
        QCOMPARE(world.reconciler().map().ownerOf(QStringLiteral("d1")), ScreenB);
    }

    /// An unplugged output is known by the identifier it carried: its
    /// connector name no longer resolves once the output is gone, and a
    /// removal under that name left the dead screen holding its slice.
    void screenRemoval_usesTheScreensIdentifier()
    {
        World world;
        world.addScreen(ScreenA, 0);
        world.screens.addScreen(QStringLiteral("PZTEST-B"), QRect(1920, 0, 1920, 1080), ScreenB);
        world.vdm.commit({QStringLiteral("d1"), QStringLiteral("d2"), QStringLiteral("d3"), QStringLiteral("d4")});
        world.build();
        world.controller->start();
        world.vdm.updateScreenDesktop(ScreenA, 1);
        world.vdm.updateScreenDesktop(ScreenB, 3);
        QVERIFY(world.reconciler().isAdopted());
        QVERIFY(world.reconciler().map().hasScreen(ScreenB));

        world.screens.removeScreen(QStringLiteral("PZTEST-B"));
        QVERIFY(!world.reconciler().map().hasScreen(ScreenB));
        QCOMPARE(world.reconciler().map().ownerOf(QStringLiteral("d3")), ScreenA);
    }
};

QTEST_MAIN(TestWorkspaceController)
#include "test_workspace_controller.moc"
