// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorLayer/PhosphorLayer.h>

#include "mocks/mockscreenprovider.h"
#include "mocks/mocktransport.h"
#include "mocks/testroles.h"

#include <QEventLoop>
#include <QFile>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>

using namespace PhosphorLayer;
using PhosphorLayer::Testing::MockScreenProvider;
using PhosphorLayer::Testing::MockTransport;

class TestSurface : public QObject
{
    Q_OBJECT

private:
    // Helper: build a SurfaceConfig with a bare QQuickItem content (so the
    // state machine doesn't block on async QML loading).
    static SurfaceConfig buildConfig(QScreen* screen, QString debugName = QStringLiteral("test"))
    {
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentItem = std::make_unique<QQuickItem>();
        cfg.screen = screen;
        cfg.debugName = std::move(debugName);
        return cfg;
    }

private Q_SLOTS:
    void initialState()
    {
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto* surface = f.create(buildConfig(s.primary()));
        QVERIFY(surface);
        QCOMPARE(surface->state(), Surface::State::Constructed);
        QCOMPARE(surface->window(), nullptr);
        QCOMPARE(surface->transport(), nullptr);
    }

    void showTransitionsThroughWarmingToShown()
    {
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto* surface = f.create(buildConfig(s.primary()));
        QSignalSpy spy(surface, &Surface::stateChanged);

        surface->show();
        // Inline content (contentItem) — the state machine should reach Shown
        // synchronously in one drive() pass.
        QCOMPARE(surface->state(), Surface::State::Shown);
        QVERIFY(surface->window() != nullptr);
        QVERIFY(surface->transport() != nullptr);
        QCOMPARE(t.m_attachCount, 1);
        QCOMPARE(t.m_lastArgs.layer, Layer::Top); // Modal fixture
        QCOMPARE(t.m_lastArgs.keyboard, KeyboardInteractivity::Exclusive);

        // Expect: Constructed → Warming → Hidden → Shown
        QCOMPARE(spy.count(), 3);
        QCOMPARE(spy.at(0).at(0).value<Surface::State>(), Surface::State::Warming);
        QCOMPARE(spy.at(1).at(0).value<Surface::State>(), Surface::State::Hidden);
        QCOMPARE(spy.at(2).at(0).value<Surface::State>(), Surface::State::Shown);
    }

    void keyboardPolicyUpdatesWithoutRecreatingTheSurface()
    {
        MockTransport transport;
        MockScreenProvider screens;
        SurfaceFactory factory(PhosphorLayer::Testing::makeDeps(&transport, &screens));
        auto* surface = factory.create(buildConfig(screens.primary()));
        surface->setKeyboardInteractivity(KeyboardInteractivity::OnDemand);
        surface->show();
        QCOMPARE(transport.m_lastArgs.keyboard, KeyboardInteractivity::OnDemand);
        auto* window = surface->window();
        surface->setKeyboardInteractivity(KeyboardInteractivity::Exclusive);
        QCOMPARE(transport.m_lastHandle->m_keyboard, KeyboardInteractivity::Exclusive);
        surface->setKeyboardInteractivity(KeyboardInteractivity::OnDemand);
        QCOMPARE(transport.m_lastHandle->m_keyboard, KeyboardInteractivity::OnDemand);
        QCOMPARE(surface->config().effectiveKeyboard(), KeyboardInteractivity::OnDemand);
        QCOMPARE(transport.m_attachCount, 1);
        QCOMPARE(surface->window(), window);
    }

    /// A consumer may drive the handle directly through transport() as well
    /// (ShellHost does), so the setter cannot treat the config as proof of
    /// what the compositor holds: setting the config's own value again must
    /// still reach the handle.
    void keyboardPolicyReachesTheHandleAfterADirectHandleWrite()
    {
        MockTransport transport;
        MockScreenProvider screens;
        SurfaceFactory factory(PhosphorLayer::Testing::makeDeps(&transport, &screens));
        auto* surface = factory.create(buildConfig(screens.primary()));
        surface->setKeyboardInteractivity(KeyboardInteractivity::None);
        surface->show();
        surface->transport()->setKeyboardInteractivity(KeyboardInteractivity::Exclusive);
        QCOMPARE(transport.m_lastHandle->m_keyboard, KeyboardInteractivity::Exclusive);
        surface->setKeyboardInteractivity(KeyboardInteractivity::None);
        QCOMPARE(transport.m_lastHandle->m_keyboard, KeyboardInteractivity::None);
    }

    void keyboardPolicyRejectsAnOutOfRangeValue()
    {
        MockTransport transport;
        MockScreenProvider screens;
        SurfaceFactory factory(PhosphorLayer::Testing::makeDeps(&transport, &screens));
        auto* surface = factory.create(buildConfig(screens.primary()));
        surface->show();
        surface->setKeyboardInteractivity(KeyboardInteractivity::OnDemand);
        QCOMPARE(transport.m_lastHandle->m_keyboard, KeyboardInteractivity::OnDemand);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("out-of-range value")));
        surface->setKeyboardInteractivity(static_cast<KeyboardInteractivity>(5));
        QCOMPARE(transport.m_lastHandle->m_keyboard, KeyboardInteractivity::OnDemand);
        QCOMPARE(surface->config().effectiveKeyboard(), KeyboardInteractivity::OnDemand);
    }

    /// A contentItem surface hosts an item its caller built in its own
    /// engine, so it asks no provider for one (and builds none of its own).
    void contentItemSurfaceTakesNoEngine()
    {
        struct CountingProvider : public IQmlEngineProvider
        {
            QQmlEngine* engineForSurface(const SurfaceConfig&) override
            {
                ++m_requests;
                return &m_engine;
            }
            void releaseEngine(QQmlEngine*) override
            {
            }
            QQmlEngine m_engine;
            int m_requests = 0;
        };
        MockTransport t;
        MockScreenProvider s;
        CountingProvider p;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s, &p));
        auto* surface = f.create(buildConfig(s.primary()));
        surface->warmUp();
        QCOMPARE(surface->state(), Surface::State::Hidden);
        QCOMPARE(p.m_requests, 0);
    }

    /// contextProperties cannot reach a contentItem (it lives in its caller's
    /// context), so the factory refuses the combination instead of dropping
    /// them silently.
    void contextPropertiesWithContentItemAreRefused()
    {
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto cfg = buildConfig(s.primary());
        cfg.contextProperties = {{QStringLiteral("tag"), 1}};
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("contentUrl content only")));
        QCOMPARE(f.create(std::move(cfg)), nullptr);
    }

    void warmUpStaysAtHidden()
    {
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto* surface = f.create(buildConfig(s.primary()));
        surface->warmUp();
        QCOMPARE(surface->state(), Surface::State::Hidden);
        QVERIFY(surface->window() != nullptr);
        QCOMPARE(t.m_attachCount, 1);
        // Now show is cheap: Hidden → Shown only.
        QSignalSpy spy(surface, &Surface::stateChanged);
        surface->show();
        QCOMPARE(surface->state(), Surface::State::Shown);
        QCOMPARE(spy.count(), 1);
    }

    void hideReturnsToHidden()
    {
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto* surface = f.create(buildConfig(s.primary()));
        surface->show();
        QCOMPARE(surface->state(), Surface::State::Shown);
        surface->hide();
        QCOMPARE(surface->state(), Surface::State::Hidden);
        // Re-show doesn't re-attach — transport attach count stays at 1.
        surface->show();
        QCOMPARE(surface->state(), Surface::State::Shown);
        QCOMPARE(t.m_attachCount, 1);
    }

    void rejectedAttachTransitionsToFailed()
    {
        MockTransport t;
        MockScreenProvider s;
        t.rejectNextAttach();
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto* surface = f.create(buildConfig(s.primary(), QStringLiteral("reject-test")));
        QSignalSpy failSpy(surface, &Surface::failed);

        surface->show();
        // state() is synchronous (m_state is written inside failWith); the
        // `failed` signal itself is deferred via QueuedConnection so consumer
        // slots can `delete surface` without re-entry. QTRY pumps the loop.
        QCOMPARE(surface->state(), Surface::State::Failed);
        QTRY_COMPARE(failSpy.count(), 1);
        QVERIFY(failSpy.at(0).at(0).toString().contains(QStringLiteral("attach")));
    }

    void failedSurfaceIgnoresFurtherCalls()
    {
        MockTransport t;
        MockScreenProvider s;
        t.rejectNextAttach();
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto* surface = f.create(buildConfig(s.primary()));
        surface->show();
        QCOMPARE(surface->state(), Surface::State::Failed);

        QSignalSpy spy(surface, &Surface::stateChanged);
        surface->show();
        surface->hide();
        surface->warmUp();
        QCOMPARE(spy.count(), 0); // no state changes
        QCOMPARE(surface->state(), Surface::State::Failed);
    }

    void missingContentSourceFails()
    {
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.screen = s.primary();
        // Neither contentUrl nor contentItem set — SurfaceFactory should
        // refuse before we even reach the state machine.
        auto* surface = f.create(std::move(cfg));
        QCOMPARE(surface, nullptr);
    }

    void bothContentSourcesRefused()
    {
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.screen = s.primary();
        cfg.contentUrl = QUrl(QStringLiteral("qrc:/nonexistent.qml"));
        cfg.contentItem = std::make_unique<QQuickItem>();
        auto* surface = f.create(std::move(cfg));
        QCOMPARE(surface, nullptr);
    }

    void unsupportedTransportRefuses()
    {
        MockTransport t;
        MockScreenProvider s;
        t.setSupported(false);
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto* surface = f.create(buildConfig(s.primary()));
        QCOMPARE(surface, nullptr);
    }

    void windowPropertiesLandOnWrapperWindow()
    {
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto cfg = buildConfig(s.primary());
        cfg.windowProperties = {{QStringLiteral("myTag"), QStringLiteral("set")}, {QStringLiteral("myCount"), 7}};
        auto* surface = f.create(std::move(cfg));
        surface->warmUp();
        QCOMPARE(surface->state(), Surface::State::Hidden);
        QVERIFY(surface->window() != nullptr);
        QCOMPARE(surface->window()->property("myTag").toString(), QStringLiteral("set"));
        QCOMPARE(surface->window()->property("myCount").toInt(), 7);
    }

    void attachArgsReflectRoleAndOverrides()
    {
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        const Role hud = Testing::makeHudRole();
        auto cfg = buildConfig(s.primary());
        cfg.role = hud;
        cfg.marginsOverride = QMargins(5, 10, 15, 20);
        cfg.exclusiveZoneOverride = 42;
        auto* surface = f.create(std::move(cfg));
        surface->show();

        QCOMPARE(t.m_lastArgs.layer, Layer::Overlay);
        QCOMPARE(t.m_lastArgs.anchors, AnchorAll);
        QCOMPARE(t.m_lastArgs.margins, QMargins(5, 10, 15, 20));
        QCOMPARE(t.m_lastArgs.exclusiveZone, 42);
        QCOMPARE(t.m_lastArgs.screen, s.primary());
        // Assert against the literal scope prefix the Hud fixture ships
        // (not against `hud.scopePrefix`) so a regression where the
        // factory constructs its own empty/derived scope would be caught
        // rather than silently passing through.
        QCOMPARE(t.m_lastArgs.scope, QStringLiteral("test-hud"));
    }

    void engineProviderReleaseOrderingDefersAfterWindowDelete()
    {
        // Bug regression: when an engineProvider is injected, the previous
        // dtor called releaseEngine() SYNCHRONOUSLY while the window's
        // deleteLater was still queued — a provider that `delete engine` in
        // releaseEngine would free the engine before the window's QML
        // children had torn down. The fix queues releaseEngine on the window's
        // destroyed() signal, with the engine as context, so it runs only once
        // the window is gone. Merely queueing it after the window's
        // deleteLater is not enough: a DeferredDelete runs only back at the
        // loop level it was posted from, while a queued call runs in any
        // nested loop. The teardown below spins one to split the two.
        struct TrackingProvider : public IQmlEngineProvider
        {
            QQmlEngine* engineForSurface(const SurfaceConfig&) override
            {
                m_engine = new QQmlEngine();
                return m_engine;
            }
            void releaseEngine(QQmlEngine* e) override
            {
                QVERIFY(m_windowDestroyed); // window must have torn down first
                ++m_releaseCalls;
                m_engine = nullptr;
                e->deleteLater();
            }
            QQmlEngine* m_engine = nullptr;
            bool m_windowDestroyed = false;
            int m_releaseCalls = 0;
        };

        // contentUrl content: only a surface that loads its own QML takes an
        // engine from the provider, so only it has one to release.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile qml(dir.filePath(QStringLiteral("provider.qml")));
        QVERIFY(qml.open(QIODevice::WriteOnly | QIODevice::Text));
        qml.write("import QtQuick 2.15\nItem { width: 10; height: 10 }\n");
        qml.close();

        MockTransport t;
        MockScreenProvider s;
        TrackingProvider p;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s, &p));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = QUrl::fromLocalFile(qml.fileName());
        cfg.screen = s.primary();
        auto* surface = f.create(std::move(cfg));
        surface->warmUp();
        QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Hidden, 2000);
        QVERIFY(p.m_engine != nullptr);

        QPointer<QQuickWindow> winWatch(surface->window());
        QVERIFY(winWatch);
        QObject::connect(winWatch.data(), &QObject::destroyed, [&p] {
            // Window's destroyed handler fires during DeferredDelete
            // processing — before releaseEngine runs, per the ordering
            // contract.
            p.m_windowDestroyed = true;
        });

        // Destroy the Surface from an event handler running under an event
        // loop, then spin a nested loop before returning, the shape a modal
        // dialog or a QEventLoop-based wait in a consumer's slot produces. The
        // window's DeferredDelete is held until the handler returns, while a
        // plain queued release would run inside the nested loop with the
        // window still alive (the QVERIFY in releaseEngine catches that).
        int releasesDuringNestedLoop = -1;
        QEventLoop outer;
        QTimer::singleShot(0, &outer, [&] {
            delete surface;
            QEventLoop nested;
            QTimer::singleShot(50, &nested, &QEventLoop::quit);
            nested.exec();
            releasesDuringNestedLoop = p.m_releaseCalls;
            outer.quit();
        });
        outer.exec();
        // Wait for the deferred-delete chain to settle rather than pumping
        // a fixed number of passes — the exact depth depends on Qt internals
        // and is not a behaviour this test needs to pin. Polling via
        // QTRY_VERIFY keeps it robust as Qt / the Surface teardown graph
        // evolve.
        QTRY_VERIFY_WITH_TIMEOUT(winWatch.isNull(), 2000);
        QTRY_COMPARE_WITH_TIMEOUT(p.m_releaseCalls, 1, 2000);
        QCOMPARE(releasesDuringNestedLoop, 0);
        QVERIFY(p.m_windowDestroyed);
        QCOMPARE(p.m_releaseCalls, 1);
    }

    void attachRecordsNonZeroSizeAtAttach()
    {
        // Anti-regression for the partial-anchor zero-size-commit bug
        // (commit 4630da9d): for a wlr-layer-shell surface that is NOT
        // doubly-anchored on both axes, the initial commit MUST carry a
        // non-zero size or the compositor echoes back 0×0 and the surface
        // stays stuck. This pins the contentItem path, where
        // ensureWrapperWindow sizes the wrapper window before the attach;
        // test_content_url's windowRootAttachesHiddenSizedAndBeforeComplete
        // pins the Window-rooted path in instantiateFromComponent.
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto cfg = buildConfig(s.primary());
        // Top|Left anchor: non-doubly-anchored on either axis — the case
        // the regression affected.
        cfg.anchorsOverride = Anchors{Anchor::Top, Anchor::Left};
        auto* surface = f.create(std::move(cfg));
        surface->warmUp();

        QCOMPARE(t.m_attachRecords.size(), 1);
        const auto& rec = t.m_attachRecords.first();
        QVERIFY2(!rec.sizeAtAttach.isEmpty(),
                 qPrintable(QStringLiteral("size at attach was %1x%2")
                                .arg(rec.sizeAtAttach.width())
                                .arg(rec.sizeAtAttach.height())));
        QVERIFY(rec.sizeAtAttach.width() > 0);
        QVERIFY(rec.sizeAtAttach.height() > 0);
    }

    void initialSizeSizesWindowAtWarmUp()
    {
        // Anti-regression for SurfaceConfig::initialSize (PR #381). Callers
        // that opt into a content-sized warm-up to avoid full-screen Vulkan
        // swapchain allocation must see the wrapper QQuickWindow attach at
        // exactly the requested size, not the target screen's geometry.
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto cfg = buildConfig(s.primary());
        cfg.initialSize = QSize(240, 70);
        auto* surface = f.create(std::move(cfg));
        surface->warmUp();

        QCOMPARE(t.m_attachRecords.size(), 1);
        QCOMPARE(t.m_attachRecords.first().sizeAtAttach, QSize(240, 70));
        QVERIFY(surface->window());
        QCOMPARE(surface->window()->size(), QSize(240, 70));
    }

    void initialSizeEmptyFallsBackToScreenGeometry()
    {
        // The default `QSize{}` is the "unset" sentinel — pre-PR-#381
        // behaviour (warm up at the target screen's full geometry) MUST
        // remain the default for callers that do not opt in.
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto cfg = buildConfig(s.primary());
        // Default-constructed initialSize → isEmpty() → fall through.
        QVERIFY(cfg.initialSize.isEmpty());
        const QSize screenSize = s.primary()->geometry().size();
        auto* surface = f.create(std::move(cfg));
        surface->warmUp();

        QCOMPARE(t.m_attachRecords.size(), 1);
        QCOMPARE(t.m_attachRecords.first().sizeAtAttach, screenSize);
    }

    void initialSizePartiallyZeroIsTreatedAsUnset()
    {
        // Documented contract on SurfaceConfig::initialSize: any non-positive
        // dimension counts as "unset" (matches QSize::isEmpty semantics).
        // A partially-zero size silently falls back to screen geometry —
        // pin that contract so a future refactor doesn't accidentally
        // commit (0×N) to the wl_surface.
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        auto cfg = buildConfig(s.primary());
        cfg.initialSize = QSize(0, 100); // isEmpty() → true
        const QSize screenSize = s.primary()->geometry().size();
        auto* surface = f.create(std::move(cfg));
        surface->warmUp();

        QCOMPARE(t.m_attachRecords.size(), 1);
        QCOMPARE(t.m_attachRecords.first().sizeAtAttach, screenSize);
    }

    /// A screen lost before the surface attaches is still reported, and the
    /// later attach falls back to the provider's primary rather than the
    /// screen that went away.
    void screenLostBeforeAttachFallsBackToPrimary()
    {
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        QScreen* primary = s.primary();
        QVERIFY(primary);

        auto* surface = f.create(buildConfig(primary));
        QSignalSpy lostSpy(surface, &Surface::screenLost);
        QCOMPARE(surface->state(), Surface::State::Constructed);

        s.setScreens({});
        QVERIFY(lostSpy.wait(1000));
        QCOMPARE(lostSpy.count(), 1);
        QCOMPARE(surface->config().screen, nullptr);

        s.setScreens({primary});
        surface->warmUp();
        QCOMPARE(surface->state(), Surface::State::Hidden);
        QCOMPARE(t.m_attachRecords.size(), 1);
        QCOMPARE(t.m_attachRecords.first().args.screen, primary);
        QCOMPARE(lostSpy.count(), 1);
        delete surface;
    }

    /// A provider that drops a screen from its list before it signals (a
    /// filtering provider) is still reported: the attach notices the screen is
    /// unlisted, emits screenLost and falls back to the primary.
    void screenDroppedWithoutASignalIsReportedAtAttach()
    {
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        const QList<QScreen*> all = s.screens();
        QScreen* primary = s.primary();
        QVERIFY(primary);

        auto* surface = f.create(buildConfig(primary));
        QSignalSpy lostSpy(surface, &Surface::screenLost);
        // Drop it from the list without emitting screensChanged.
        s.m_screens = {};
        s.m_primary = nullptr;
        surface->warmUp();
        QVERIFY(lostSpy.wait(1000));
        QCOMPARE(lostSpy.count(), 1);
        s.m_screens = all;
        s.m_primary = primary;
        delete surface;
    }

    void screenRemovalNullsStaleScreenReference()
    {
        // Anti-regression for the stale-QScreen* hazard (review L3).
        //
        // Three observable contracts:
        //   1. When the attached screen is removed from the provider, the
        //      Surface emits `screenLost` exactly once (so consumers can
        //      decide to destroy+rebuild). State is preserved — the library
        //      does not auto-transition to Failed.
        //   2. Nulling is idempotent: a second setScreens({}) with the
        //      screen already removed does NOT re-emit screenLost.
        //   3. The factory's own fallback kicks in on subsequent create()
        //      calls with a null cfg.screen: it routes to the provider's
        //      current primary rather than propagating a stale pointer.
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));

        QScreen* primary = s.primary();
        QVERIFY(primary);

        auto* surface = f.create(buildConfig(primary));
        QSignalSpy lostSpy(surface, &Surface::screenLost);

        surface->warmUp();
        QCOMPARE(surface->state(), Surface::State::Hidden);
        QCOMPARE(t.m_attachRecords.size(), 1);
        QCOMPARE(t.m_attachRecords.first().args.screen, primary);

        // Remove the only screen. Library emits screenLost and nulls the
        // internal config.screen so the next re-attach won't pass the
        // dangling pointer.
        //
        // screenLost is emitted via Qt::QueuedConnection (so consumer
        // slots can `delete surface` safely) — wait for the event loop
        // to deliver before checking the spy.
        s.setScreens({});
        QCOMPARE(surface->state(), Surface::State::Hidden);
        QVERIFY(lostSpy.wait(/*timeoutMs=*/1000));
        QCOMPARE(lostSpy.count(), 1);

        // Re-emitting setScreens({}) with no screen to lose: no new signal.
        // Spin the event loop briefly to confirm no spurious queued emit
        // lands.
        s.setScreens({});
        QTest::qWait(50);
        QCOMPARE(lostSpy.count(), 1);

        // Restore a screen. Build a fresh surface and verify the factory
        // routes through the provider's current primary.
        s.setScreens({primary});
        t.m_attachRecords.clear();
        auto* surface2 = f.create(buildConfig(/*screen=*/nullptr));
        QVERIFY(surface2);
        surface2->warmUp();
        QCOMPARE(surface2->state(), Surface::State::Hidden);
        QCOMPARE(t.m_attachRecords.size(), 1);
        QCOMPARE(t.m_attachRecords.first().args.screen, primary);
    }
};

QTEST_MAIN(TestSurface)
#include "test_surface.moc"
