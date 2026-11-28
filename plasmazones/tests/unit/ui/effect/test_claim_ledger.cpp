// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

// The claim ledger's release algorithms (kwin-effect/tilinghandler/claimledger.h)
// driven through a fake compositor (phosphor-works/workspace#26): releaseAllClaims
// for every ClaimScope, the retain arms of both maximize releases, the
// windowed-fullscreen release, the untrack funnel's monocle half and the
// evacuation scrub.

#include <QTest>

#include "tilinghandler/claimledger.h"

#include <map>
#include <memory>

using namespace PlasmaZones;
using ScrollDecisions::ClaimScope;

namespace {

/// One window as the fake compositor holds it.
struct FakeWindow
{
    bool deleted = false;
    bool hasClient = true;
    bool requestedFullScreen = false;
    bool fullScreen = false;
    bool gesture = false;
    bool requestedMaximized = true; ///< requested maximize mode is not Restore
    bool keepAbove = false;
    bool keepBelow = false;
};

/// A compositor that records every write, in order.
class FakeHost final : public ClaimHost
{
public:
    FakeWindow& add(const QString& id)
    {
        auto window = std::make_unique<FakeWindow>();
        FakeWindow& ref = *window;
        m_windows[id] = std::move(window);
        return ref;
    }
    ClaimWindow* handle(const QString& id)
    {
        const auto it = m_windows.find(id);
        return it == m_windows.end() ? nullptr : reinterpret_cast<ClaimWindow*>(it->second.get());
    }
    FakeWindow& window(ClaimWindow* w)
    {
        return *reinterpret_cast<FakeWindow*>(w);
    }

    ClaimWindow* findExact(const QString& id) override
    {
        return handle(id);
    }
    bool isDeleted(ClaimWindow* w) override
    {
        return window(w).deleted;
    }
    bool hasClient(ClaimWindow* w) override
    {
        return window(w).hasClient;
    }
    bool isRequestedFullScreen(ClaimWindow* w) override
    {
        return window(w).requestedFullScreen;
    }
    bool isFullScreen(ClaimWindow* w) override
    {
        return window(w).fullScreen;
    }
    bool inUserGesture(ClaimWindow* w) override
    {
        return window(w).gesture;
    }
    bool requestedMaximizeRestored(ClaimWindow* w) override
    {
        return !window(w).requestedMaximized;
    }
    void writeMaximizeRestore(ClaimWindow* w) override
    {
        writes.append(QStringLiteral("maximizeRestore"));
        window(w).requestedMaximized = false;
    }
    void writeFullScreenOff(ClaimWindow* w) override
    {
        writes.append(QStringLiteral("fullScreenOff"));
        // X11's order: both bits are clear by the time the next release runs.
        window(w).requestedFullScreen = false;
        window(w).fullScreen = false;
    }
    void setKeepFlags(ClaimWindow* w, bool keepAbove, bool keepBelow) override
    {
        writes.append(QStringLiteral("keepFlags"));
        window(w).keepAbove = keepAbove;
        window(w).keepBelow = keepBelow;
    }

    QStringList writes;

private:
    std::map<QString, std::unique_ptr<FakeWindow>> m_windows;
};

/// The ledgers a test owns, and a view of them.
struct World
{
    QSet<QString> maximizedToEdges;
    QSet<QString> monocleMaximized;
    QSet<QString> monocleRestoreOwed;
    QSet<QString> windowedFsClearInFlight;
    QHash<QString, QRect> windowedFullscreen;
    QHash<QString, WindowLayerSnapshot> layerSnapshots;
    FakeHost host;

    ClaimLedger ledger()
    {
        return ClaimLedger{maximizedToEdges,
                           monocleMaximized,
                           monocleRestoreOwed,
                           windowedFsClearInFlight,
                           windowedFullscreen,
                           layerSnapshots,
                           host};
    }

    /// A window holding all three claims, a layer snapshot and an armed
    /// clear marker, fullscreen and maximized in the compositor.
    FakeWindow& holdingEverything(const QString& id)
    {
        FakeWindow& w = host.add(id);
        w.requestedFullScreen = true;
        w.fullScreen = true;
        w.keepBelow = true;
        maximizedToEdges.insert(id);
        monocleMaximized.insert(id);
        windowedFullscreen.insert(id, QRect(0, 0, 100, 100));
        layerSnapshots.insert(id, WindowLayerSnapshot{true, false});
        windowedFsClearInFlight.insert(id);
        return w;
    }
};

const QString Id = QStringLiteral("app|w");

} // namespace

class TestClaimLedger : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ── releaseAllClaims, one row per ClaimScope ──────────────────────────
    void releaseAllClaims_eachScope_data()
    {
        QTest::addColumn<int>("scope");
        QTest::addColumn<bool>("windowedFullscreen");
        QTest::addColumn<bool>("monocle");
        QTest::addColumn<bool>("maximizedToEdges");
        const auto row = [](const char* name, ClaimScope scope, bool wfs, bool mono, bool edges) {
            QTest::newRow(name) << static_cast<int>(scope) << wfs << mono << edges;
        };
        row("StripExit", ClaimScope::StripExit, true, true, true);
        row("UntrackFunnel", ClaimScope::UntrackFunnel, true, true, true);
        row("LiveDeparture", ClaimScope::LiveDeparture, true, true, true);
        row("ModeFlip", ClaimScope::ModeFlip, true, true, true);
        row("Teardown", ClaimScope::Teardown, true, true, true);
        // A closing window's monocle membership is scrubbed bare by the funnel.
        row("Close", ClaimScope::Close, true, false, true);
        row("PassiveFloat", ClaimScope::PassiveFloat, true, false, true);
        // The window left fullscreen on its own: that claim is already gone.
        row("FullscreenExit", ClaimScope::FullscreenExit, false, true, true);
        // An evacuee keeps KWin's state; a snap placement handed it back.
        row("Evacuation", ClaimScope::Evacuation, false, false, false);
        row("SnapPlacement", ClaimScope::SnapPlacement, false, false, false);
    }
    void releaseAllClaims_eachScope()
    {
        QFETCH(int, scope);
        QFETCH(bool, windowedFullscreen);
        QFETCH(bool, monocle);
        QFETCH(bool, maximizedToEdges);
        World world;
        // The claims all held, the client no longer fullscreen in the
        // compositor, so each release answers its scope alone (the order with
        // a fullscreen client has its own case below).
        FakeWindow& w = world.holdingEverything(Id);
        w.requestedFullScreen = false;
        w.fullScreen = false;
        const ClaimReleaseResult result =
            world.ledger().releaseAllClaims(Id, world.host.handle(Id), static_cast<ClaimScope>(scope));

        QCOMPARE(result.windowedFullscreen, windowedFullscreen);
        QCOMPARE(world.windowedFullscreen.contains(Id), !windowedFullscreen);
        QCOMPARE(world.layerSnapshots.contains(Id), !windowedFullscreen);
        // The clear marker goes whenever the windowed-fullscreen claim answers
        // the scope, released or not.
        QCOMPARE(world.windowedFsClearInFlight.contains(Id), !windowedFullscreen);
        QCOMPARE(world.host.writes.contains(QStringLiteral("keepFlags")), windowedFullscreen);
        QVERIFY(!world.host.writes.contains(QStringLiteral("fullScreenOff")));

        QCOMPARE(result.monocle, monocle);
        QCOMPARE(world.monocleMaximized.contains(Id), !monocle);
        QCOMPARE(result.maximizedToEdges, maximizedToEdges);
        QCOMPARE(world.maximizedToEdges.contains(Id), !maximizedToEdges);
        // One write at most: the first maximize release restores the window,
        // and the second then finds it restored and sheds with no write.
        QCOMPARE(world.host.writes.count(QStringLiteral("maximizeRestore")), (monocle || maximizedToEdges) ? 1 : 0);
    }

    // Windowed fullscreen goes first: a window holding both gets a real
    // maximize restore after its fullscreen is off, not a fullscreen skip.
    void releaseAllClaims_fullscreenBeforeEitherMaximize()
    {
        World world;
        world.holdingEverything(Id);
        world.ledger().releaseAllClaims(Id, world.host.handle(Id), ClaimScope::Teardown);
        // The edges release then finds the window restored and writes nothing.
        QCOMPARE(world.host.writes,
                 QStringList({QStringLiteral("keepFlags"), QStringLiteral("fullScreenOff"),
                              QStringLiteral("maximizeRestore")}));
        QVERIFY(world.maximizedToEdges.isEmpty());
        QVERIFY(world.monocleMaximized.isEmpty());
    }

    // A lone layer snapshot still owes the release, and reports it.
    void releaseAllClaims_loneSnapshotStillReleases()
    {
        World world;
        FakeWindow& w = world.host.add(Id);
        w.keepBelow = true;
        world.layerSnapshots.insert(Id, WindowLayerSnapshot{true, false});
        const ClaimReleaseResult result =
            world.ledger().releaseAllClaims(Id, world.host.handle(Id), ClaimScope::StripExit);
        QVERIFY(result.windowedFullscreen);
        QVERIFY(!world.layerSnapshots.contains(Id));
        QVERIFY(w.keepAbove);
        QVERIFY(!w.keepBelow);
    }

    // The marker goes even with no membership and no snapshot.
    void releaseAllClaims_markerGoesWithNothingElse()
    {
        World world;
        world.host.add(Id);
        world.windowedFsClearInFlight.insert(Id);
        const ClaimReleaseResult result =
            world.ledger().releaseAllClaims(Id, world.host.handle(Id), ClaimScope::StripExit);
        QVERIFY(!result.windowedFullscreen);
        QVERIFY(!world.windowedFsClearInFlight.contains(Id));
    }

    // A window still requested fullscreen after the release (the client keeps
    // it) retains both maximize claims and reports neither as released.
    void releaseAllClaims_stillFullscreenRetainsBothMaximizes()
    {
        World world;
        FakeWindow& w = world.host.add(Id);
        w.requestedFullScreen = true;
        world.maximizedToEdges.insert(Id);
        world.monocleMaximized.insert(Id);
        const ClaimReleaseResult result =
            world.ledger().releaseAllClaims(Id, world.host.handle(Id), ClaimScope::Teardown);
        QVERIFY(!result.monocle);
        QVERIFY(!result.maximizedToEdges);
        QVERIFY(world.maximizedToEdges.contains(Id));
        QVERIFY(world.monocleMaximized.contains(Id));
        QVERIFY(world.host.writes.isEmpty());
    }

    // ── releaseMaximizedToEdges ───────────────────────────────────────────
    void edges_normalReleaseWritesOnce()
    {
        World world;
        world.host.add(Id);
        world.maximizedToEdges.insert(Id);
        QVERIFY(world.ledger().releaseMaximizedToEdges(Id, world.host.handle(Id)));
        QVERIFY(!world.maximizedToEdges.contains(Id));
        QCOMPARE(world.host.writes, QStringList({QStringLiteral("maximizeRestore")}));
    }
    void edges_notAMemberDoesNothing()
    {
        World world;
        world.host.add(Id);
        QVERIFY(!world.ledger().releaseMaximizedToEdges(Id, world.host.handle(Id)));
        QVERIFY(world.host.writes.isEmpty());
    }
    void edges_fullscreenRequestedRetains()
    {
        World world;
        world.host.add(Id).requestedFullScreen = true;
        world.maximizedToEdges.insert(Id);
        QVERIFY(!world.ledger().releaseMaximizedToEdges(Id, world.host.handle(Id)));
        QVERIFY(world.maximizedToEdges.contains(Id));
        QVERIFY(world.host.writes.isEmpty());
    }
    void edges_gestureRetainsWithNoOwedRestore()
    {
        World world;
        world.host.add(Id).gesture = true;
        world.maximizedToEdges.insert(Id);
        QVERIFY(!world.ledger().releaseMaximizedToEdges(Id, world.host.handle(Id)));
        QVERIFY(world.maximizedToEdges.contains(Id));
        QVERIFY(world.monocleRestoreOwed.isEmpty());
        QVERIFY(world.host.writes.isEmpty());
    }
    void edges_goneWindowSheds()
    {
        World world;
        world.maximizedToEdges.insert(Id);
        QVERIFY(!world.ledger().releaseMaximizedToEdges(Id, nullptr));
        QVERIFY(!world.maximizedToEdges.contains(Id));
        QVERIFY(world.host.writes.isEmpty());
    }
    void edges_noClientSheds()
    {
        World world;
        world.host.add(Id).hasClient = false;
        world.maximizedToEdges.insert(Id);
        QVERIFY(!world.ledger().releaseMaximizedToEdges(Id, world.host.handle(Id)));
        QVERIFY(!world.maximizedToEdges.contains(Id));
        QVERIFY(world.host.writes.isEmpty());
    }
    // A closing corpse is shed with no write, even with its client still there.
    void edges_deletedShedsEvenWithAClient()
    {
        World world;
        world.host.add(Id).deleted = true;
        world.maximizedToEdges.insert(Id);
        QVERIFY(!world.ledger().releaseMaximizedToEdges(Id, world.host.handle(Id)));
        QVERIFY(!world.maximizedToEdges.contains(Id));
        QVERIFY(world.host.writes.isEmpty());
    }
    void edges_alreadyRestoredShedsWithNoWrite()
    {
        World world;
        world.host.add(Id).requestedMaximized = false;
        world.maximizedToEdges.insert(Id);
        QVERIFY(!world.ledger().releaseMaximizedToEdges(Id, world.host.handle(Id)));
        QVERIFY(!world.maximizedToEdges.contains(Id));
        QVERIFY(world.host.writes.isEmpty());
    }

    // ── unmaximizeMonocle ─────────────────────────────────────────────────
    void monocle_normalReleaseWritesAndReportsTheWrite()
    {
        World world;
        world.host.add(Id);
        world.monocleMaximized.insert(Id);
        world.monocleRestoreOwed.insert(Id);
        QVERIFY(world.ledger().unmaximizeMonocle(Id));
        QVERIFY(!world.monocleMaximized.contains(Id));
        QVERIFY(!world.monocleRestoreOwed.contains(Id));
        QCOMPARE(world.host.writes, QStringList({QStringLiteral("maximizeRestore")}));
    }
    void monocle_fullscreenRequestedRetains()
    {
        World world;
        world.host.add(Id).requestedFullScreen = true;
        world.monocleMaximized.insert(Id);
        QVERIFY(!world.ledger().unmaximizeMonocle(Id));
        QVERIFY(world.monocleMaximized.contains(Id));
        QVERIFY(!world.monocleRestoreOwed.contains(Id));
        QVERIFY(world.host.writes.isEmpty());
    }
    // Mid-gesture the entry stays AND the restore is recorded as owed, so the
    // gesture end pays a restore rather than re-maximizing.
    void monocle_gestureRetainsAndOwesTheRestore()
    {
        World world;
        world.host.add(Id).gesture = true;
        world.monocleMaximized.insert(Id);
        QVERIFY(!world.ledger().unmaximizeMonocle(Id));
        QVERIFY(world.monocleMaximized.contains(Id));
        QVERIFY(world.monocleRestoreOwed.contains(Id));
        QVERIFY(world.host.writes.isEmpty());
    }
    void monocle_goneWindowShedsBothEntries()
    {
        World world;
        world.monocleMaximized.insert(Id);
        world.monocleRestoreOwed.insert(Id);
        QVERIFY(!world.ledger().unmaximizeMonocle(Id));
        QVERIFY(!world.monocleMaximized.contains(Id));
        QVERIFY(!world.monocleRestoreOwed.contains(Id));
        QVERIFY(world.host.writes.isEmpty());
    }
    void monocle_noClientShedsBothEntries()
    {
        World world;
        world.host.add(Id).hasClient = false;
        world.monocleMaximized.insert(Id);
        world.monocleRestoreOwed.insert(Id);
        QVERIFY(!world.ledger().unmaximizeMonocle(Id));
        QVERIFY(!world.monocleMaximized.contains(Id));
        QVERIFY(!world.monocleRestoreOwed.contains(Id));
    }
    // The monocle release resolves the window itself and does not test for a
    // corpse: a window marked deleted with its client still there is restored.
    // Pinned so a change to that is a decision, not an accident.
    void monocle_deletedWithAClientIsStillRestored()
    {
        World world;
        world.host.add(Id).deleted = true;
        world.monocleMaximized.insert(Id);
        QVERIFY(world.ledger().unmaximizeMonocle(Id));
        QCOMPARE(world.host.writes, QStringList({QStringLiteral("maximizeRestore")}));
    }
    // Already restored: membership goes, the write is called (it is a no-op
    // in KWin) and the answer says nothing was written, so no caller anchors
    // a maximize leg on it.
    void monocle_alreadyRestoredReportsNoWrite()
    {
        World world;
        world.host.add(Id).requestedMaximized = false;
        world.monocleMaximized.insert(Id);
        QVERIFY(!world.ledger().unmaximizeMonocle(Id));
        QVERIFY(!world.monocleMaximized.contains(Id));
    }

    // ── releaseWindowedFullscreenState ────────────────────────────────────
    void windowedFullscreen_goneWindowDropsTheSnapshotOnly()
    {
        World world;
        world.layerSnapshots.insert(Id, WindowLayerSnapshot{true, true});
        world.ledger().releaseWindowedFullscreenState(Id);
        QVERIFY(!world.layerSnapshots.contains(Id));
        QVERIFY(world.host.writes.isEmpty());
    }
    void windowedFullscreen_notFullscreenRestoresTheFlagsOnly()
    {
        World world;
        world.host.add(Id).keepBelow = true;
        world.layerSnapshots.insert(Id, WindowLayerSnapshot{false, false});
        world.ledger().releaseWindowedFullscreenState(Id);
        QCOMPARE(world.host.writes, QStringList({QStringLiteral("keepFlags")}));
    }
    // Inside our own enter round trip only the requested bit is set, and the
    // release must still turn fullscreen off.
    void windowedFullscreen_requestedOnlyIsTurnedOff()
    {
        World world;
        world.host.add(Id).requestedFullScreen = true;
        world.ledger().releaseWindowedFullscreenState(Id);
        QCOMPARE(world.host.writes, QStringList({QStringLiteral("fullScreenOff")}));
    }

    // ── The untrack funnel's monocle half ─────────────────────────────────
    void funnel_retainedMonocleSurvivesTheScrub()
    {
        World world;
        world.host.add(Id).gesture = true;
        world.monocleMaximized.insert(Id);
        ClaimLedger ledger = world.ledger();
        const bool retained = ledger.releaseMonocleBeforeScrub(Id, ClaimScope::UntrackFunnel);
        QVERIFY(retained);
        world.monocleMaximized.remove(Id); // the funnel's cleanupClosedWindowState
        ledger.settleMonocleAfterScrub(Id, retained);
        QVERIFY(world.monocleMaximized.contains(Id));
        QVERIFY(world.monocleRestoreOwed.contains(Id));
    }
    void funnel_closeScrubsMonocleBareAndDropsTheOwedRestore()
    {
        World world;
        world.host.add(Id);
        world.monocleMaximized.insert(Id);
        world.monocleRestoreOwed.insert(Id);
        ClaimLedger ledger = world.ledger();
        const bool retained = ledger.releaseMonocleBeforeScrub(Id, ClaimScope::Close);
        QVERIFY(!retained);
        QVERIFY(world.host.writes.isEmpty()); // Close does not release monocle
        world.monocleMaximized.remove(Id);
        ledger.settleMonocleAfterScrub(Id, retained);
        QVERIFY(!world.monocleMaximized.contains(Id));
        QVERIFY(!world.monocleRestoreOwed.contains(Id));
    }

    // ── The evacuation scrub ──────────────────────────────────────────────
    void evacuation_scrubsWithoutAMaximizeWrite()
    {
        World world;
        FakeWindow& w = world.holdingEverything(Id);
        world.ledger().scrubForEvacuation(Id, world.host.handle(Id));
        QVERIFY(!world.maximizedToEdges.contains(Id));
        QVERIFY(!world.windowedFullscreen.contains(Id));
        QVERIFY(!world.windowedFsClearInFlight.contains(Id));
        QVERIFY(!world.layerSnapshots.contains(Id));
        // The layer flags come back; KWin's fullscreen and maximize stay.
        QCOMPARE(world.host.writes, QStringList({QStringLiteral("keepFlags")}));
        QVERIFY(w.fullScreen);
        QVERIFY(w.requestedMaximized);
    }
};

QTEST_GUILESS_MAIN(TestClaimLedger)
#include "test_claim_ledger.moc"
