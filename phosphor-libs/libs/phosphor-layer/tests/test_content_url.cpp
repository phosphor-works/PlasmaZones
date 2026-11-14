// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorLayer/PhosphorLayer.h>

#include "mocks/mockscreenprovider.h"
#include "mocks/mocktransport.h"
#include "mocks/testroles.h"

#include <QDir>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlNetworkAccessManagerFactory>
#include <QQuickItem>
#include <QRegularExpression>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

using namespace PhosphorLayer;
using PhosphorLayer::Testing::MockScreenProvider;
using PhosphorLayer::Testing::MockTransport;

namespace {

/// The QML a test serves over kAsyncScheme, keyed by URL path.
using AsyncSources = QHash<QString, QByteArray>;
constexpr QLatin1StringView kAsyncScheme("pltest");

/// A reply that delivers its body (or a not-found error) on a later event-loop
/// turn, the way a real network reply does.
class AsyncReply : public QNetworkReply
{
public:
    AsyncReply(QNetworkAccessManager::Operation op, const QNetworkRequest& request, std::optional<QByteArray> body,
               QObject* parent)
        : QNetworkReply(parent)
        , m_body(body.value_or(QByteArray()))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        const bool found = body.has_value();
        if (found) {
            setHeader(QNetworkRequest::ContentLengthHeader, m_body.size());
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        } else {
            setError(QNetworkReply::ContentNotFoundError, QStringLiteral("not served by the test"));
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 404);
        }
        QTimer::singleShot(10, this, [this, found] {
            setFinished(true);
            if (!found) {
                Q_EMIT errorOccurred(error());
            } else {
                Q_EMIT readyRead();
            }
            Q_EMIT finished();
        });
    }

    void abort() override
    {
    }
    bool isSequential() const override
    {
        return true;
    }
    qint64 bytesAvailable() const override
    {
        return m_body.size() - m_offset + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        const qint64 n = qMin(maxSize, qint64(m_body.size()) - m_offset);
        if (n <= 0) {
            return -1;
        }
        memcpy(data, m_body.constData() + m_offset, size_t(n));
        m_offset += n;
        return n;
    }

private:
    QByteArray m_body;
    qint64 m_offset = 0;
};

class AsyncNam : public QNetworkAccessManager
{
public:
    AsyncNam(AsyncSources sources, QObject* parent)
        : QNetworkAccessManager(parent)
        , m_sources(std::move(sources))
    {
    }

protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request, QIODevice*) override
    {
        const auto it = m_sources.constFind(request.url().path());
        return new AsyncReply(op, request,
                              it == m_sources.cend() ? std::nullopt : std::optional<QByteArray>(it.value()), this);
    }

private:
    const AsyncSources m_sources;
};

/// The type loader calls create() from its own thread, so the sources are
/// copied into each manager rather than shared.
class AsyncNamFactory : public QQmlNetworkAccessManagerFactory
{
public:
    explicit AsyncNamFactory(AsyncSources sources)
        : m_sources(std::move(sources))
    {
    }
    QNetworkAccessManager* create(QObject* parent) override
    {
        return new AsyncNam(m_sources, parent);
    }

private:
    const AsyncSources m_sources;
};

} // namespace

/**
 * @brief Exercises the contentUrl loading path (QQmlComponent).
 *
 * The other Surface tests pin content via contentItem to keep the state
 * machine fully synchronous. These tests write real .qml files and use
 * contentUrl so QQmlComponent actually loads, parses, and instantiates.
 * A local file loads synchronously inside drive(); the tests that need the
 * asynchronous Warming → Hidden transition (statusChanged firing later)
 * serve their QML over kAsyncScheme through a shared engine whose network
 * replies arrive on a later event-loop turn.
 */
class TestContentUrl : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_tmpDir;

    QUrl writeQml(QStringView filename, QStringView body)
    {
        QFile f(m_tmpDir.filePath(filename.toString()));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qFatal("Could not write temp QML: %s", qPrintable(f.errorString()));
        }
        QTextStream(&f) << body;
        return QUrl::fromLocalFile(f.fileName());
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_tmpDir.isValid());
    }

    void validQmlTransitionsToShown()
    {
        const QUrl url = writeQml(u"valid.qml", u"import QtQuick 2.15\nItem { width: 100; height: 100 }\n");

        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = url;
        cfg.screen = s.primary();

        auto* surface = f.create(std::move(cfg));
        QVERIFY(surface);
        QSignalSpy spy(surface, &Surface::stateChanged);

        surface->show();
        // QQmlComponent loads local files synchronously by default, so we
        // typically reach Shown before this returns — but also accept an
        // event-loop pump if Qt defers (it does for `import` resolution).
        if (surface->state() != Surface::State::Shown) {
            QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Shown, 2000);
        }
        QVERIFY(surface->window() != nullptr);
        QCOMPARE(t.m_attachCount, 1);
    }

    void invalidQmlSyntaxTransitionsToFailed()
    {
        const QUrl url = writeQml(u"broken.qml", u"this is not QML\n");

        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = url;
        cfg.screen = s.primary();
        cfg.debugName = QStringLiteral("broken-qml");

        auto* surface = f.create(std::move(cfg));
        QSignalSpy failSpy(surface, &Surface::failed);

        surface->show();
        if (surface->state() != Surface::State::Failed) {
            QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Failed, 2000);
        }
        // `failed` is deferred via QueuedConnection — pump the loop.
        QTRY_COMPARE(failSpy.count(), 1);
    }

    void rootObjectMustBeQuickItem()
    {
        // QtObject is a valid QML root but not a QQuickItem — Surface should
        // reject it as unsupported content.
        const QUrl url = writeQml(u"object.qml", u"import QtQml 2.15\nQtObject { property int x: 5 }\n");

        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = url;
        cfg.screen = s.primary();

        auto* surface = f.create(std::move(cfg));
        QSignalSpy failSpy(surface, &Surface::failed);

        surface->show();
        if (surface->state() != Surface::State::Failed) {
            QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Failed, 2000);
        }
        QTRY_COMPARE(failSpy.count(), 1);
        QVERIFY(failSpy.at(0).at(0).toString().contains(QStringLiteral("QQuickItem")));
    }

    void windowRootedQmlIsAdopted()
    {
        // Many consumers (Phosphor's overlays, panel apps, notification
        // daemons) ship QML where the ROOT type is a Window — they manage
        // size/visibility declaratively and expect the C++ side to adopt
        // the window rather than create its own wrapper. Verify PhosphorLayer
        // supports that pattern.
        const QUrl url = writeQml(u"window.qml",
                                  u"import QtQuick 2.15\nimport QtQuick.Window 2.15\n"
                                  u"Window { width: 200; height: 100; property string tag: 'window-root' }\n");

        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = url;
        cfg.screen = s.primary();
        cfg.debugName = QStringLiteral("window-root");

        auto* surface = f.create(std::move(cfg));
        surface->show();
        if (surface->state() != Surface::State::Shown) {
            QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Shown, 2000);
        }
        auto* win = surface->window();
        QVERIFY(win);
        // The Surface's window IS the QML root (not a wrapper around it).
        QCOMPARE(win->property("tag").toString(), QStringLiteral("window-root"));
    }

    void windowPropertiesInjectIntoWindowRoot()
    {
        // createWithInitialProperties is used when windowProperties is non-empty;
        // for Window-rooted QML the properties land on the Window itself.
        const QUrl url = writeQml(u"window-init.qml",
                                  u"import QtQuick.Window 2.15\nWindow { property string injected: 'default' }\n");

        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = url;
        cfg.screen = s.primary();
        cfg.windowProperties = {{QStringLiteral("injected"), QStringLiteral("from-config")}};

        auto* surface = f.create(std::move(cfg));
        surface->show();
        if (surface->state() != Surface::State::Shown) {
            QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Shown, 2000);
        }
        QCOMPARE(surface->window()->property("injected").toString(), QStringLiteral("from-config"));
    }

    /// The Window-rooted invariants instantiateFromComponent exists for: the
    /// window reaches the transport HIDDEN, with no platform window yet, with a
    /// non-zero size, and before componentComplete. Attaching after
    /// componentComplete lets Qt create an xdg_toplevel first and fixes the
    /// wl_surface's role, and a 0x0 first commit leaves a partial-anchor layer
    /// surface stuck.
    void windowRootAttachesHiddenSizedAndBeforeComplete()
    {
        const QUrl url =
            writeQml(u"window-attach.qml",
                     u"import QtQuick.Window 2.15\n"
                     u"Window { property bool completed: false; Component.onCompleted: completed = true }\n");

        MockTransport t;
        t.m_capturePropertiesAtAttach = {QStringLiteral("completed")};
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = url;
        cfg.screen = s.primary();
        cfg.anchorsOverride = Anchors{Anchor::Top, Anchor::Left};

        auto* surface = f.create(std::move(cfg));
        surface->warmUp();
        QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Hidden, 2000);

        QCOMPARE(t.m_attachRecords.size(), 1);
        const auto& rec = t.m_attachRecords.first();
        QVERIFY(!rec.sizeAtAttach.isEmpty());
        QVERIFY(!rec.visibleAtAttach);
        QVERIFY(!rec.hadPlatformWindowAtAttach);
        QCOMPARE(rec.propertiesAtAttach.value(QStringLiteral("completed")).toBool(), false);
        QVERIFY(surface->window()->property("completed").toBool());
    }

    /// A show() from a stateChanged(Hidden) slot is latched until the current
    /// drive() frame finishes. On the Window-rooted path that Hidden is emitted
    /// before completeCreate, so without the latch the window would map before
    /// the QML had completed. The load is asynchronous so the instantiation
    /// runs from onComponentStatus, outside drive(), which is the frame the
    /// latch has to cover itself.
    void showFromHiddenSlotWaitsForComponentComplete()
    {
        // Declared before the engine, which does not own it and must not
        // outlive it.
        AsyncNamFactory namFactory(
            {{QStringLiteral("/window-latch.qml"),
              QByteArrayLiteral("import QtQuick.Window 2.15\n"
                                "Window {\n"
                                "  property bool completed: false\n"
                                "  property bool shownBeforeComplete: false\n"
                                "  Component.onCompleted: completed = true\n"
                                "  onVisibleChanged: if (visible && !completed) shownBeforeComplete = true\n"
                                "}\n")}});
        QQmlEngine engine;
        engine.setNetworkAccessManagerFactory(&namFactory);

        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = QUrl(kAsyncScheme + QStringLiteral(":/window-latch.qml"));
        cfg.screen = s.primary();
        cfg.sharedEngine = &engine;

        auto* surface = f.create(std::move(cfg));
        QObject::connect(surface, &Surface::stateChanged, surface, [surface](Surface::State state) {
            if (state == Surface::State::Hidden) {
                surface->show();
            }
        });
        surface->warmUp();
        // The load really is pending: the instantiation will come from
        // onComponentStatus, not from inside this warmUp().
        QCOMPARE(surface->state(), Surface::State::Warming);
        QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Shown, 2000);
        QVERIFY(surface->window()->property("completed").toBool());
        QVERIFY(!surface->window()->property("shownBeforeComplete").toBool());
        delete surface;
        // The window is released through the event loop; drain it before the
        // engine it hangs off goes out of scope.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    /// A screen lost while an asynchronous load is pending nulls the bound
    /// screen. When the load lands, the warm-up geometry and the attach both
    /// take the provider's primary instead of attaching with no size.
    void screenLostDuringAsyncLoadFallsBackToPrimary()
    {
        AsyncNamFactory namFactory(
            {{QStringLiteral("/item.qml"), QByteArrayLiteral("import QtQuick 2.15\nItem {}\n")}});
        QQmlEngine engine;
        engine.setNetworkAccessManagerFactory(&namFactory);

        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        QScreen* primary = s.primary();
        QVERIFY(primary);
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = QUrl(kAsyncScheme + QStringLiteral(":/item.qml"));
        cfg.screen = primary;
        cfg.sharedEngine = &engine;
        cfg.anchorsOverride = Anchors{Anchor::Top, Anchor::Left};

        auto* surface = f.create(std::move(cfg));
        QSignalSpy lostSpy(surface, &Surface::screenLost);
        surface->warmUp();
        QCOMPARE(surface->state(), Surface::State::Warming);

        // The screen goes away and comes back while the QML is in flight.
        s.setScreens({});
        QVERIFY(lostSpy.wait(1000));
        QCOMPARE(surface->config().screen, nullptr);
        s.setScreens({primary});

        QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Hidden, 2000);
        QCOMPARE(t.m_attachRecords.size(), 1);
        QCOMPARE(t.m_attachRecords.first().args.screen, primary);
        QVERIFY(!t.m_attachRecords.first().sizeAtAttach.isEmpty());
        delete surface;
        // The window is released through the event loop; drain it before the
        // engine it hangs off goes out of scope.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    /// windowProperties belong to the window: a key the Window root declares
    /// takes the config's value, and an undeclared key lands as a dynamic
    /// property. Plain behaviour coverage. Both routes give the same result
    /// here, since setInitialProperties drops an undeclared key silently
    /// rather than warning.
    void windowPropertiesSplitBetweenDeclaredAndDynamic()
    {
        const QUrl url = writeQml(u"window-split.qml",
                                  u"import QtQuick.Window 2.15\nWindow { property string injected: 'default' }\n");

        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = url;
        cfg.screen = s.primary();
        cfg.windowProperties = {{QStringLiteral("injected"), QStringLiteral("from-config")},
                                {QStringLiteral("dynamicOnly"), 5}};

        auto* surface = f.create(std::move(cfg));
        surface->warmUp();
        QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Hidden, 2000);
        QCOMPARE(surface->window()->property("injected").toString(), QStringLiteral("from-config"));
        QCOMPARE(surface->window()->property("dynamicOnly").toInt(), 5);
    }

    /// For Item-rooted QML the window properties go to the wrapper window
    /// only. A same-named property the root ITEM declares keeps its own value.
    void windowPropertiesDoNotReachAnItemRoot()
    {
        const QUrl url =
            writeQml(u"item-tag.qml",
                     u"import QtQuick 2.15\nItem { width: 10; height: 10; property string myTag: 'item-own' }\n");

        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = url;
        cfg.screen = s.primary();
        cfg.windowProperties = {{QStringLiteral("myTag"), QStringLiteral("window-value")}};

        auto* surface = f.create(std::move(cfg));
        surface->warmUp();
        QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Hidden, 2000);
        QCOMPARE(surface->window()->property("myTag").toString(), QStringLiteral("window-value"));
        const auto children = surface->window()->contentItem()->childItems();
        QCOMPARE(children.size(), 1);
        QCOMPARE(children.first()->property("myTag").toString(), QStringLiteral("item-own"));
    }

    /// A required property the content never received is an error, as
    /// QQmlComponent::create would report it, not a live surface with the
    /// property at its default.
    void unsetRequiredPropertyFails()
    {
        const QUrl url = writeQml(
            u"required.qml", u"import QtQuick 2.15\nItem { width: 10; height: 10; required property string needed }\n");

        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = url;
        cfg.screen = s.primary();

        auto* surface = f.create(std::move(cfg));
        QSignalSpy failedSpy(surface, &Surface::failed);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("failed:.*needed")));
        surface->warmUp();
        QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Failed, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 2000);
    }

    /// Two surfaces sharing one engine, each with its own value for the same
    /// context property: each sees its own, and neither leaks onto the shared
    /// root context.
    void sharedEngineContextPropertiesStayPerSurface()
    {
        const QUrl url =
            writeQml(u"ctx-shared.qml",
                     u"import QtQuick 2.15\nItem { width: 10; height: 10; property string tag: perSurfaceTag }\n");

        QQmlEngine engine;
        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        const auto make = [&](const QString& value) {
            SurfaceConfig cfg;
            cfg.role = Testing::makeModalRole();
            cfg.contentUrl = url;
            cfg.screen = s.primary();
            cfg.sharedEngine = &engine;
            cfg.contextProperties = {{QStringLiteral("perSurfaceTag"), value}};
            auto* surface = f.create(std::move(cfg));
            surface->warmUp();
            return surface;
        };
        auto* first = make(QStringLiteral("first"));
        auto* second = make(QStringLiteral("second"));
        QTRY_COMPARE_WITH_TIMEOUT(first->state(), Surface::State::Hidden, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(second->state(), Surface::State::Hidden, 2000);
        const auto rootOf = [](Surface* surface) {
            return surface->window()->contentItem()->childItems().value(0);
        };
        QCOMPARE(rootOf(first)->property("tag").toString(), QStringLiteral("first"));
        QCOMPARE(rootOf(second)->property("tag").toString(), QStringLiteral("second"));
        QVERIFY(!engine.rootContext()->contextProperty(QStringLiteral("perSurfaceTag")).isValid());
        delete first;
        delete second;
        // The windows and their child contexts are released through the event
        // loop; drain it before the engine they hang off goes out of scope.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void rejectedAttachOnContentUrlTransitionsToFailed()
    {
        // Complementary to the existing contentItem rejectedAttach test.
        // Covers the async-load path: QML loads successfully, then the
        // transport rejects attach(). Surface must end in Failed and emit
        // `failed` exactly once. Without this case, a regression that
        // split the rejection handling between the sync-ready and async-
        // ready branches would only be caught on the contentItem side.
        const QUrl url = writeQml(u"valid-rejected.qml", u"import QtQuick 2.15\nItem { width: 1; height: 1 }\n");

        MockTransport t;
        t.rejectNextAttach();
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = url;
        cfg.screen = s.primary();

        auto* surface = f.create(std::move(cfg));
        QSignalSpy failSpy(surface, &Surface::failed);
        surface->show();
        QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Failed, 2000);
        // failed() is deferred via QueuedConnection; QTRY pumps the event
        // loop so by the time state == Failed the signal has landed.
        QTRY_COMPARE(failSpy.count(), 1);
    }

    void screenRemovedWhileWarming()
    {
        // Async load path + screen removal mid-Warming. The Surface has
        // no observable "stuck in Warming" failure mode — we verify that
        // it eventually reaches Shown or Failed, never wedges in Warming.
        const QUrl url = writeQml(u"warm-race.qml", u"import QtQuick 2.15\nItem { width: 10; height: 10 }\n");

        MockTransport t;
        MockScreenProvider s;
        QScreen* primary = s.primary();
        QVERIFY(primary);
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = url;
        cfg.screen = primary;

        auto* surface = f.create(std::move(cfg));
        surface->show();
        // Drop the primary while the QML component loads. setScreens
        // triggers screensChanged; the Surface's notifier slot nulls
        // m_config.screen but does NOT advance state.
        s.setScreens({});
        // Restore the primary so finishAttach (when load completes) can
        // resolve a real target via its fallback path.
        s.setScreens({primary});

        QTRY_VERIFY_WITH_TIMEOUT(
            surface->state() == Surface::State::Shown || surface->state() == Surface::State::Failed, 2000);
    }

    void contextPropertiesAreVisibleToQml()
    {
        const QUrl url = writeQml(u"ctx.qml", u"import QtQuick 2.15\nItem { property string tag: injectedTag }\n");

        MockTransport t;
        MockScreenProvider s;
        SurfaceFactory f(PhosphorLayer::Testing::makeDeps(&t, &s));
        SurfaceConfig cfg;
        cfg.role = Testing::makeModalRole();
        cfg.contentUrl = url;
        cfg.screen = s.primary();
        cfg.contextProperties = {{QStringLiteral("injectedTag"), QStringLiteral("hello-phosphorlayer")}};

        auto* surface = f.create(std::move(cfg));
        surface->show();
        if (surface->state() != Surface::State::Shown) {
            QTRY_COMPARE_WITH_TIMEOUT(surface->state(), Surface::State::Shown, 2000);
        }

        // Verify the QML actually saw the context property: dig into the
        // root item's "tag" property. Safer than reaching for engine->
        // rootContext() because that's been cleared by the time we query.
        auto* win = surface->window();
        QVERIFY(win);
        QVERIFY(win->contentItem());
        QQuickItem* root = nullptr;
        const auto children = win->contentItem()->childItems();
        for (QObject* child : children) {
            if (auto* qi = qobject_cast<QQuickItem*>(child)) {
                root = qi;
                break;
            }
        }
        QVERIFY2(
            root,
            qPrintable(
                QStringLiteral("content root not found in contentItem childItems (count=%1)").arg(children.size())));
        QCOMPARE(root->property("tag").toString(), QStringLiteral("hello-phosphorlayer"));
    }
};

QTEST_MAIN(TestContentUrl)
#include "test_content_url.moc"
