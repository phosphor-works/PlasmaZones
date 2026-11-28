// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Maximize to edges on a tiling screen (AutotileEngine::toggleMaximizeToEdges,
// phosphor-works/workspace#28). The engine applies the maximize after the
// algorithm has run, so every algorithm gets it: the window keeps its tile and
// its place in the order, its own entry takes the screen's available area with
// the maximizedToEdges flag, and the other entries keep their rects. Runs the
// real retile pipeline with real screen geometry and the bundled Luau
// algorithms.

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <PhosphorScreens/Manager.h>
#include <PhosphorTileEngine/AutotileEngine.h>
#include <PhosphorTiles/TilingState.h>

#include "FakePhysicalScreenSource.h"

#include "helpers/AutotileTestHelpers.h"
#include "helpers/ScriptedAlgoTestSetup.h"

using PhosphorTileEngine::AutotileEngine;

namespace {

const QString Screen = QStringLiteral("DP-1");

/// The last batch's entries, keyed by window id.
QHash<QString, QJsonObject> lastBatch(const QSignalSpy& spy)
{
    QHash<QString, QJsonObject> out;
    if (spy.isEmpty()) {
        return out;
    }
    const QJsonArray arr = QJsonDocument::fromJson(spy.last().first().toString().toUtf8()).array();
    for (const QJsonValue& v : arr) {
        const QJsonObject obj = v.toObject();
        out.insert(obj.value(QLatin1String("windowId")).toString(), obj);
    }
    return out;
}

QRect rectOf(const QJsonObject& obj)
{
    return QRect(obj.value(QLatin1String("x")).toInt(), obj.value(QLatin1String("y")).toInt(),
                 obj.value(QLatin1String("width")).toInt(), obj.value(QLatin1String("height")).toInt());
}

bool maximizedFlag(const QJsonObject& obj)
{
    return obj.value(QLatin1String("maximizedToEdges")).toBool(false);
}

} // namespace

class TestAutotileMaximize : public QObject
{
    Q_OBJECT

    PlasmaZones::TestHelpers::ScriptedAlgoTestSetup m_scriptSetup;

    struct World
    {
        PhosphorScreens::FakePhysicalScreenSource provider;
        std::unique_ptr<PhosphorScreens::ScreenManager> manager;
        std::unique_ptr<AutotileEngine> engine;
    };

    static std::unique_ptr<World> makeWorld(const QString& algorithm, const QStringList& windows)
    {
        auto world = std::make_unique<World>();
        world->provider.addScreen(Screen, QRect(0, 0, 1920, 1080));
        world->manager = std::make_unique<PhosphorScreens::ScreenManager>(
            PhosphorScreens::ScreenManagerConfig{.physicalScreenSource = &world->provider,
                                                 .useGeometrySensors = false});
        world->manager->start();
        world->engine = std::make_unique<AutotileEngine>(nullptr, nullptr, world->manager.get(),
                                                         PlasmaZones::TestHelpers::testRegistry());
        world->engine->setAutotileScreens({Screen});
        world->engine->setAlgorithm(algorithm);
        for (const QString& id : windows) {
            world->engine->windowOpened(id, Screen);
        }
        return world;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_scriptSetup.init());
    }

    // The maximized window takes the available area with the flag. Its
    // neighbours keep the rects the algorithm gave them, and the order is
    // unchanged.
    void toggle_maximizesOneWindowAndKeepsTheOthers()
    {
        auto world = makeWorld(QStringLiteral("master-stack"), {QStringLiteral("a"), QStringLiteral("b")});
        AutotileEngine& engine = *world->engine;
        PhosphorTiles::TilingState* state = engine.tilingStateForScreen(Screen);
        QTRY_COMPARE(state->calculatedZones().size(), 2);

        QSignalSpy tiledSpy(&engine, &AutotileEngine::windowsTiled);
        engine.retile(Screen);
        QTRY_VERIFY(!tiledSpy.isEmpty());
        const QHash<QString, QJsonObject> before = lastBatch(tiledSpy);
        QCOMPARE(before.size(), 2);
        QVERIFY(!maximizedFlag(before.value(QStringLiteral("a"))));
        const QStringList orderBefore = state->tiledWindows();

        tiledSpy.clear();
        QVERIFY(engine.toggleMaximizeToEdges(Screen, QStringLiteral("b")));
        QVERIFY(engine.isMaximizedToEdges(QStringLiteral("b")));
        QTRY_VERIFY(!tiledSpy.isEmpty());
        const QHash<QString, QJsonObject> after = lastBatch(tiledSpy);
        QCOMPARE(after.size(), 2);

        const QRect area = world->manager->screenAvailableGeometry(Screen);
        QVERIFY(area.isValid());
        QVERIFY(maximizedFlag(after.value(QStringLiteral("b"))));
        QCOMPARE(rectOf(after.value(QStringLiteral("b"))), area);
        QVERIFY(!after.value(QStringLiteral("b")).contains(QLatin1String("monocle")));

        QVERIFY(!maximizedFlag(after.value(QStringLiteral("a"))));
        QCOMPARE(rectOf(after.value(QStringLiteral("a"))), rectOf(before.value(QStringLiteral("a"))));
        QCOMPARE(state->tiledWindows(), orderBefore);
    }

    // A second toggle restores the tile the algorithm gives the window.
    void toggleTwice_restoresTheTile()
    {
        auto world = makeWorld(QStringLiteral("columns"), {QStringLiteral("a"), QStringLiteral("b")});
        AutotileEngine& engine = *world->engine;
        PhosphorTiles::TilingState* state = engine.tilingStateForScreen(Screen);
        QTRY_COMPARE(state->calculatedZones().size(), 2);

        QSignalSpy tiledSpy(&engine, &AutotileEngine::windowsTiled);
        engine.retile(Screen);
        QTRY_VERIFY(!tiledSpy.isEmpty());
        const QRect tileBefore = rectOf(lastBatch(tiledSpy).value(QStringLiteral("a")));

        QVERIFY(engine.toggleMaximizeToEdges(Screen, QStringLiteral("a")));
        tiledSpy.clear();
        QVERIFY(engine.toggleMaximizeToEdges(Screen, QStringLiteral("a")));
        QVERIFY(!engine.isMaximizedToEdges(QStringLiteral("a")));
        QTRY_VERIFY(!tiledSpy.isEmpty());
        const QJsonObject entry = lastBatch(tiledSpy).value(QStringLiteral("a"));
        QVERIFY(!maximizedFlag(entry));
        QCOMPARE(rectOf(entry), tileBefore);
    }

    // An empty window id names the screen's focused window.
    void emptyWindowId_namesTheFocusedWindow()
    {
        auto world = makeWorld(QStringLiteral("columns"), {QStringLiteral("a"), QStringLiteral("b")});
        AutotileEngine& engine = *world->engine;
        QTRY_COMPARE(engine.tilingStateForScreen(Screen)->calculatedZones().size(), 2);
        engine.windowFocused(QStringLiteral("a"), Screen);

        QVERIFY(engine.toggleMaximizeToEdges(Screen, QString()));
        QVERIFY(engine.isMaximizedToEdges(QStringLiteral("a")));
        QVERIFY(!engine.isMaximizedToEdges(QStringLiteral("b")));
    }

    // Refusals change nothing: an unknown window, a floating one, and a window
    // in a monocle stack, whose maximize the monocle flag already owns.
    void refusals_changeNothing()
    {
        auto world = makeWorld(QStringLiteral("columns"), {QStringLiteral("a"), QStringLiteral("b")});
        AutotileEngine& engine = *world->engine;
        QTRY_COMPARE(engine.tilingStateForScreen(Screen)->calculatedZones().size(), 2);

        QVERIFY(!engine.toggleMaximizeToEdges(Screen, QStringLiteral("nobody")));

        engine.setWindowFloat(QStringLiteral("b"), true, Screen);
        QVERIFY(!engine.toggleMaximizeToEdges(Screen, QStringLiteral("b")));
        QVERIFY(!engine.isMaximizedToEdges(QStringLiteral("b")));

        auto mono = makeWorld(QStringLiteral("monocle"), {QStringLiteral("m1"), QStringLiteral("m2")});
        QTRY_COMPARE(mono->engine->tilingStateForScreen(Screen)->calculatedZones().size(), 2);
        QVERIFY(!mono->engine->toggleMaximizeToEdges(Screen, QStringLiteral("m1")));
        QVERIFY(!mono->engine->isMaximizedToEdges(QStringLiteral("m1")));
    }

    // Floating a maximized window sets its maximize aside, and the unfloat
    // brings it back maximized, as a scrolling column's float record does.
    void float_setsTheMaximizeAside()
    {
        auto world = makeWorld(QStringLiteral("columns"), {QStringLiteral("a"), QStringLiteral("b")});
        AutotileEngine& engine = *world->engine;
        QTRY_COMPARE(engine.tilingStateForScreen(Screen)->calculatedZones().size(), 2);

        QVERIFY(engine.toggleMaximizeToEdges(Screen, QStringLiteral("a")));
        engine.setWindowFloat(QStringLiteral("a"), true, Screen);
        QTRY_VERIFY(!engine.isMaximizedToEdges(QStringLiteral("a")));

        QSignalSpy tiledSpy(&engine, &AutotileEngine::windowsTiled);
        engine.setWindowFloat(QStringLiteral("a"), false, Screen);
        QTRY_VERIFY(lastBatch(tiledSpy).contains(QStringLiteral("a")));
        QVERIFY(maximizedFlag(lastBatch(tiledSpy).value(QStringLiteral("a"))));
        QVERIFY(engine.isMaximizedToEdges(QStringLiteral("a")));
    }

    // An own-workspace trip (PlasmaZones#1201): with a hold, the handoff on
    // the way out keeps the maximize, and the return to the origin puts the
    // window back between the windows it was between.
    void ownWorkspaceHold_keepsMaximizeAndPlace()
    {
        auto world =
            makeWorld(QStringLiteral("columns"), {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
        AutotileEngine& engine = *world->engine;
        PhosphorTiles::TilingState* state = engine.tilingStateForScreen(Screen);
        QTRY_COMPARE(state->calculatedZones().size(), 3);
        QCOMPARE(state->tiledWindows(), QStringList({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}));

        QVERIFY(engine.toggleMaximizeToEdges(Screen, QStringLiteral("b")));
        engine.holdForOwnWorkspace(QStringLiteral("b"));
        engine.handoffRelease(QStringLiteral("b"));
        QVERIFY(engine.isMaximizedToEdges(QStringLiteral("b")));
        QCOMPARE(state->tiledWindows(), QStringList({QStringLiteral("a"), QStringLiteral("c")}));

        engine.windowOpened(QStringLiteral("b"), Screen);
        QTRY_COMPARE(state->tiledWindows(),
                     QStringList({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}));
    }

    // Without a hold the handoff ends the maximize, as every per-mode state
    // ends when the window leaves the engine.
    void handoffWithoutHold_endsTheMaximize()
    {
        auto world = makeWorld(QStringLiteral("columns"), {QStringLiteral("a"), QStringLiteral("b")});
        AutotileEngine& engine = *world->engine;
        QTRY_COMPARE(engine.tilingStateForScreen(Screen)->calculatedZones().size(), 2);

        QVERIFY(engine.toggleMaximizeToEdges(Screen, QStringLiteral("b")));
        engine.handoffRelease(QStringLiteral("b"));
        QVERIFY(!engine.isMaximizedToEdges(QStringLiteral("b")));
    }

    // Closing a maximized window ends its maximize, so a window that later
    // reuses the id is not maximized.
    void close_endsTheMaximize()
    {
        auto world = makeWorld(QStringLiteral("columns"), {QStringLiteral("a"), QStringLiteral("b")});
        AutotileEngine& engine = *world->engine;
        QTRY_COMPARE(engine.tilingStateForScreen(Screen)->calculatedZones().size(), 2);

        QVERIFY(engine.toggleMaximizeToEdges(Screen, QStringLiteral("a")));
        engine.windowClosed(QStringLiteral("a"));
        QVERIFY(!engine.isMaximizedToEdges(QStringLiteral("a")));
    }
};

QTEST_MAIN(TestAutotileMaximize)
#include "test_autotile_maximize.moc"
