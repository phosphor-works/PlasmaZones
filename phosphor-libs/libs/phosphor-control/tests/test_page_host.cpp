// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The chrome's page QML against a live ApplicationController, with page
// Loaders incubating asynchronously as they do in a window. PageHost: the
// page it shows, and the controller it hands that page, follow the registry
// when a page is bound to a new controller or QML, whether the page is on
// screen or only cached. A page that passes context is created with its
// controller and settingsApp already set, and is not built again for a
// registry change that leaves its QML alone. A page that does not pass
// context is created plain and assigned its controller, and one that passes
// context waits for a live controller. The page area says when a page is
// still loading and when it cannot be shown. A deep link waits for a page that
// can reveal it, and a hidden cached page keeps its size until it is shown
// again. FacetModulePage, hosted as FacetHost hosts it:
// it shows each state its placeholder controller reports, and follows a
// change of that state.

#include "simple_page.h"

#include <PhosphorControl/ApplicationController.h>
#include <PhosphorControl/FacetHost.h>
#include <PhosphorControl/ModulePlaceholderPage.h>
#include <PhosphorControl/PageController.h>

#include <QCoreApplication>
#include <QFile>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlIncubationController>
#include <QQuickItem>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <utility>

using namespace PhosphorControl;
using PhosphorControlTest::SimplePage;

namespace {

// A placeholder controller as FacetHost has one per module page, in a state
// the test sets.
class PlaceholderController : public ModulePlaceholderPage
{
    Q_OBJECT
public:
    using ModulePlaceholderPage::ModulePlaceholderPage;

    void setState(bool failed, bool disabledAfterCrash, QString reason, bool canRetry)
    {
        m_failed = failed;
        m_disabledAfterCrash = disabledAfterCrash;
        m_reason = std::move(reason);
        m_canRetry = canRetry;
        Q_EMIT moduleStateChanged();
    }
    QString moduleName() const override
    {
        return QStringLiteral("Mod");
    }
    bool failed() const override
    {
        return m_failed;
    }
    bool disabledAfterCrash() const override
    {
        return m_disabledAfterCrash;
    }
    QString reason() const override
    {
        return m_reason;
    }
    bool canRetry() const override
    {
        return m_canRetry;
    }
    void retry() override
    {
        ++m_retries;
    }
    int retries() const
    {
        return m_retries;
    }

private:
    bool m_failed = false;
    bool m_disabledAfterCrash = false;
    QString m_reason;
    bool m_canRetry = false;
    int m_retries = 0;
};

} // namespace

// Named, not anonymous: TestPageHost holds one as a member, and a member of an
// anonymous-namespace type would give TestPageHost internal linkage.
namespace PhosphorControlTest {
// Runs the engine's asynchronous incubation from the event loop, as a
// QQuickWindow's controller does. Without one the engine finishes an
// asynchronous Loader's item in the same turn of the loop that compiled its
// QML, so PageHost's loading state never lasts. While held, every incubating
// page stays Loading.
class TestIncubator : public QObject, public QQmlIncubationController
{
public:
    TestIncubator()
    {
        startTimer(1);
    }
    void setHeld(bool held)
    {
        m_held = held;
    }

protected:
    void timerEvent(QTimerEvent*) override
    {
        if (!m_held) {
            incubateFor(5);
        }
    }

private:
    bool m_held = false;
};
} // namespace PhosphorControlTest

namespace {

// PageHost's page Loaders are Repeater delegates, which are no QObject's
// children, so QObject::findChild reaches neither them nor the pages in
// them. This walks the visual tree instead: the first item under @p root,
// depth first, that @p matches accepts, or null.
template<typename Predicate>
QQuickItem* findItem(QQuickItem* root, const Predicate& matches)
{
    if (!root) {
        return nullptr;
    }
    const QList<QQuickItem*> children = root->childItems();
    for (QQuickItem* child : children) {
        if (matches(child)) {
            return child;
        }
        if (QQuickItem* found = findItem(child, matches)) {
            return found;
        }
    }
    return nullptr;
}

QQuickItem* itemNamed(QQuickItem* root, const QString& objectName)
{
    return findItem(root, [&objectName](const QQuickItem* item) {
        return item->objectName() == objectName;
    });
}

// The controller PageHost injected into the page item named @p objectName,
// or null when there is no such item.
QObject* controllerOf(QQuickItem* root, const QString& objectName)
{
    QQuickItem* const item = itemNamed(root, objectName);
    return item ? item->property("controller").value<QObject*>() : nullptr;
}

// What the page item named @p objectName, a fixture that records its context
// once in Component.onCompleted, held in @p property then. Null when there is
// no such item.
QObject* seenAtCompletion(QQuickItem* root, const QString& objectName, const char* property)
{
    QQuickItem* const item = itemNamed(root, objectName);
    return item ? item->property(property).value<QObject*>() : nullptr;
}

// The page item PageHost gave @p controller, or null.
QQuickItem* pageControlledBy(QQuickItem* root, const QObject* controller)
{
    return findItem(root, [controller](const QQuickItem* item) {
        return item->property("controller").value<QObject*>() == controller;
    });
}

// The first Kirigami PlaceholderMessage under @p root, or null.
QQuickItem* placeholderMessage(QQuickItem* root)
{
    return findItem(root, [](const QQuickItem* item) {
        return item->metaObject()->indexOfProperty("helpfulAction") >= 0;
    });
}

// PageHost's PageLoadingIndicator, or null.
QQuickItem* loadingIndicator(QQuickItem* root)
{
    return findItem(root, [](const QQuickItem* item) {
        return item->metaObject()->indexOfProperty("graceDelay") >= 0;
    });
}

// Whether a visible item under @p root shows @p text.
bool showsText(QQuickItem* root, const QString& text)
{
    return findItem(root,
                    [&text](const QQuickItem* item) {
                        return item->isVisible() && item->property("text").toString() == text;
                    })
        != nullptr;
}

} // namespace

class TestPageHost : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        m_engine.setIncubationController(&m_incubator);
        // Pages that do not pass context but declare both, as a module page
        // does whose entry lost the flag.
        for (const char* name : {"first", "second"}) {
            writeQml(QString::fromLatin1(name),
                     QByteArray("import QtQuick\nItem { objectName: \"") + name
                         + "\"; property QtObject controller: null; property QtObject settingsApp: null }\n");
        }
        // A page that does not pass context and declares its controller
        // alone, as an app page may. Creating it with settingsApp set warns.
        writeQml(QStringLiteral("plain"),
                 "import QtQuick\nItem { objectName: \"plain\"; property QtObject controller: null }\n");
        // Module pages: both properties required, and what each held when
        // the page was completed recorded then, so a value assigned after
        // creation does not show up as one it was created with.
        for (const char* name : {"required", "required2"}) {
            writeQml(QString::fromLatin1(name),
                     QByteArray("import QtQuick\nItem {\n    objectName: \"") + name
                         + "\"\n"
                           "    required property QtObject controller\n"
                           "    required property QtObject settingsApp\n"
                           "    property QtObject controllerAtCompletion\n"
                           "    property QtObject settingsAppAtCompletion\n"
                           "    Component.onCompleted: {\n"
                           "        controllerAtCompletion = controller;\n"
                           "        settingsAppAtCompletion = settingsApp;\n"
                           "    }\n}\n");
        }
        // A page that takes a deep link: it records the anchor it was handed.
        writeQml(QStringLiteral("reveal"),
                 "import QtQuick\n"
                 "Item {\n"
                 "    objectName: \"reveal\"\n"
                 "    property QtObject controller: null\n"
                 "    property QtObject settingsApp: null\n"
                 "    property string revealed\n"
                 "    function revealAnchor(anchor) { revealed = anchor }\n"
                 "}\n");
        // A page whose QML does not compile.
        writeQml(QStringLiteral("broken"), "import QtQuick\nItem { this is not QML }\n");
    }

    void init()
    {
        // Fails the test on every warning the QML engine reports against a
        // location, which its message text carries as `<url>:<line>`: a .qml
        // or .js file, or <Unknown File> for a component built from data and
        // for a page created with a property it does not declare. Also on a
        // TypeError a script reports, and on the warning injectIfAssignable
        // logs for a property it could not set, neither of which need carry
        // a location.
        QTest::failOnWarning(
            QRegularExpression(QStringLiteral("(\\.(qml|js)|<Unknown File>):\\d+|TypeError|injectIfAssignable")));
        m_incubator.setHeld(false);
    }

    void showsTheNewQmlWhenTheCurrentPageIsBound()
    {
        ApplicationController controller;
        auto* const placeholder = new SimplePage(QStringLiteral("p"), &controller);
        controller.registerPage(placeholder, QString(), QStringLiteral("P"), url(QStringLiteral("first")));
        controller.setCurrentPageId(QStringLiteral("p"));
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(controllerOf(host.get(), QStringLiteral("first")) == placeholder);

        // A module that loads while its placeholder page is on screen binds
        // the real page into it. The new page requires its context, so it is
        // built only when PageHost creates it with the real controller.
        auto* const real = new SimplePage(QStringLiteral("p"), &controller);
        QVERIFY(controller.bindPage(QStringLiteral("p"), real, url(QStringLiteral("required"))));
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("required")));
        QCOMPARE(seenAtCompletion(host.get(), QStringLiteral("required"), "controllerAtCompletion"), real);
        QCOMPARE(seenAtCompletion(host.get(), QStringLiteral("required"), "settingsAppAtCompletion"), &controller);
        QVERIFY(!itemNamed(host.get(), QStringLiteral("first")));
    }

    void createsThePageWithItsContextWhenItsOwnControllerIsBoundToNewQml()
    {
        ApplicationController controller;
        auto* const page = new SimplePage(QStringLiteral("p"), &controller);
        controller.registerPage(page, QString(), QStringLiteral("P"), url(QStringLiteral("first")));
        controller.setCurrentPageId(QStringLiteral("p"));
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(controllerOf(host.get(), QStringLiteral("first")) == page);

        QVERIFY(controller.bindPage(QStringLiteral("p"), page, url(QStringLiteral("required"))));
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("required")));
        QCOMPARE(seenAtCompletion(host.get(), QStringLiteral("required"), "controllerAtCompletion"), page);
        QCOMPARE(seenAtCompletion(host.get(), QStringLiteral("required"), "settingsAppAtCompletion"), &controller);
    }

    void aRebindOfWhatIsBoundStillPassesContext()
    {
        // A page registered plain, then bound to the controller and QML it
        // already has: nothing changes but the flag, and the page is built
        // with its context on its first visit.
        ApplicationController controller;
        auto* const page = new SimplePage(QStringLiteral("p"), &controller);
        controller.registerPage(page, QString(), QStringLiteral("P"), url(QStringLiteral("required")));
        controller.registerPage(new SimplePage(QStringLiteral("q"), &controller), QString(), QStringLiteral("Q"),
                                url(QStringLiteral("first")));
        controller.setCurrentPageId(QStringLiteral("q"));
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("first")));

        QVERIFY(controller.bindPage(QStringLiteral("p"), page, url(QStringLiteral("required"))));
        controller.setCurrentPageId(QStringLiteral("p"));
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("required")));
        QCOMPARE(seenAtCompletion(host.get(), QStringLiteral("required"), "controllerAtCompletion"), page);
    }

    void handsTheNewControllerToAPageWhoseQmlStays()
    {
        ApplicationController controller;
        auto* const placeholder = new SimplePage(QStringLiteral("p"), &controller);
        controller.registerPage(placeholder, QString(), QStringLiteral("P"), url(QStringLiteral("first")));
        controller.setCurrentPageId(QStringLiteral("p"));
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(controllerOf(host.get(), QStringLiteral("first")) == placeholder);
        const QPointer<QQuickItem> item = itemNamed(host.get(), QStringLiteral("first"));

        auto* const real = new SimplePage(QStringLiteral("p"), &controller);
        QVERIFY(controller.bindPage(QStringLiteral("p"), real, url(QStringLiteral("first"))));
        QTRY_VERIFY(controllerOf(host.get(), QStringLiteral("first")) == real);
        // The same item, not a rebuilt one.
        QVERIFY(item);
        QCOMPARE(itemNamed(host.get(), QStringLiteral("first")), item.data());
    }

    void rebindsACachedPageThatIsNotCurrent()
    {
        ApplicationController controller;
        auto* const placeholder = new SimplePage(QStringLiteral("p"), &controller);
        controller.registerPage(placeholder, QString(), QStringLiteral("P"), url(QStringLiteral("first")));
        controller.registerPage(new SimplePage(QStringLiteral("q"), &controller), QString(), QStringLiteral("Q"),
                                url(QStringLiteral("second")));
        controller.setCurrentPageId(QStringLiteral("p"));
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(controllerOf(host.get(), QStringLiteral("first")) == placeholder);
        // p stays built, hidden, once q is shown.
        controller.setCurrentPageId(QStringLiteral("q"));
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("second")));

        // The same QML: the cached item gets the new controller.
        auto* const real = new SimplePage(QStringLiteral("p"), &controller);
        QVERIFY(controller.bindPage(QStringLiteral("p"), real, url(QStringLiteral("first"))));
        QTRY_VERIFY(controllerOf(host.get(), QStringLiteral("first")) == real);

        // New QML: the cached page is rebuilt from it, with its context.
        auto* const next = new SimplePage(QStringLiteral("p"), &controller);
        QVERIFY(controller.bindPage(QStringLiteral("p"), next, url(QStringLiteral("required2"))));
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("required2")));
        QCOMPARE(seenAtCompletion(host.get(), QStringLiteral("required2"), "controllerAtCompletion"), next);
        QVERIFY(!itemNamed(host.get(), QStringLiteral("first")));
        QCOMPARE(controller.currentPageId(), QStringLiteral("q"));
    }

    void aPageThatPassesContextHasItFromTheStart()
    {
        ApplicationController controller;
        auto* const page = new SimplePage(QStringLiteral("p"), &controller);
        controller.registerPage(page, QString(), QStringLiteral("P"), url(QStringLiteral("required")));
        controller.registry()->setPassesContext(QStringLiteral("p"), true);
        controller.setCurrentPageId(QStringLiteral("p"));
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("required")));
        QCOMPARE(seenAtCompletion(host.get(), QStringLiteral("required"), "controllerAtCompletion"), page);
        QCOMPARE(seenAtCompletion(host.get(), QStringLiteral("required"), "settingsAppAtCompletion"), &controller);
    }

    void aPageThatDoesNotPassContextIsAssignedItsController()
    {
        // Created plain: setting settingsApp, which it does not declare, at
        // creation would warn.
        ApplicationController controller;
        auto* const page = new SimplePage(QStringLiteral("p"), &controller);
        controller.registerPage(page, QString(), QStringLiteral("P"), url(QStringLiteral("plain")));
        controller.setCurrentPageId(QStringLiteral("p"));
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(controllerOf(host.get(), QStringLiteral("plain")) == page);
    }

    void aPageThatPassesContextIsNotBuiltAgainForAnotherPagesBind()
    {
        ApplicationController controller;
        auto* const page = new SimplePage(QStringLiteral("p"), &controller);
        controller.registerPage(page, QString(), QStringLiteral("P"), url(QStringLiteral("required")));
        controller.registry()->setPassesContext(QStringLiteral("p"), true);
        controller.registerPage(new SimplePage(QStringLiteral("q"), &controller), QString(), QStringLiteral("Q"),
                                url(QStringLiteral("first")));
        controller.setCurrentPageId(QStringLiteral("p"));
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("required")));
        const QPointer<QQuickItem> built = itemNamed(host.get(), QStringLiteral("required"));

        // Binding q makes every cached page read its registry entry again.
        QVERIFY(controller.bindPage(QStringLiteral("q"), new SimplePage(QStringLiteral("q"), &controller),
                                    url(QStringLiteral("second"))));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY2(built, "a page whose QML did not change was built again");
        QCOMPARE(itemNamed(host.get(), QStringLiteral("required")), built.data());
    }

    void aPageWhoseQmlGoesAndComesBackIsBuiltAgain()
    {
        ApplicationController controller;
        auto* const page = new SimplePage(QStringLiteral("p"), &controller);
        controller.registerPage(page, QString(), QStringLiteral("P"), url(QStringLiteral("required")));
        controller.registry()->setPassesContext(QStringLiteral("p"), true);
        controller.setCurrentPageId(QStringLiteral("p"));
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("required")));
        QQuickItem* const prompt = itemNamed(host.get(), QStringLiteral("page-host-select-page"));
        QQuickItem* const unavailable = itemNamed(host.get(), QStringLiteral("page-host-page-unavailable"));
        QVERIFY(prompt);
        QVERIFY(unavailable);
        QVERIFY(!prompt->isVisible());

        // A page that passes context bound to no QML: nothing to create, and
        // nothing has gone wrong, so the page area asks for a page.
        QVERIFY(controller.bindPage(QStringLiteral("p"), page, QUrl()));
        QTRY_VERIFY(!itemNamed(host.get(), QStringLiteral("required")));
        QVERIFY(prompt->isVisible());
        QVERIFY(!unavailable->isVisible());

        // The same QML back: built again, with its context.
        QVERIFY(controller.bindPage(QStringLiteral("p"), page, url(QStringLiteral("required"))));
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("required")));
        QCOMPARE(seenAtCompletion(host.get(), QStringLiteral("required"), "controllerAtCompletion"), page);
        QVERIFY(!prompt->isVisible());
    }

    void saysThatAPageIsLoadingUntilItIsBuilt()
    {
        ApplicationController controller;
        auto* const page = new SimplePage(QStringLiteral("p"), &controller);
        controller.registerPage(page, QString(), QStringLiteral("P"), url(QStringLiteral("first")));
        controller.setCurrentPageId(QStringLiteral("p"));
        m_incubator.setHeld(true);
        const auto release = qScopeGuard([this] {
            m_incubator.setHeld(false);
        });
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QQuickItem* const indicator = loadingIndicator(host.get());
        QVERIFY(indicator);

        // Still incubating once the indicator's grace delay has passed, so
        // the indicator shows.
        QTRY_VERIFY(indicator->property("loading").toBool());
        QTRY_VERIFY(indicator->isVisible());
        QVERIFY(!itemNamed(host.get(), QStringLiteral("first")));

        m_incubator.setHeld(false);
        QTRY_VERIFY(controllerOf(host.get(), QStringLiteral("first")) == page);
        QVERIFY(!indicator->property("loading").toBool());
        QVERIFY(!indicator->isVisible());
    }

    void saysWhenTheCurrentPagesQmlDoesNotLoad()
    {
        // The engine reports the broken file once, when the page's Loader
        // compiles it.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("broken\\.qml:\\d+")));
        ApplicationController controller;
        controller.registerPage(new SimplePage(QStringLiteral("p"), &controller), QString(), QStringLiteral("P"),
                                url(QStringLiteral("broken")));
        controller.registerPage(new SimplePage(QStringLiteral("q"), &controller), QString(), QStringLiteral("Q"),
                                url(QStringLiteral("first")));
        controller.setCurrentPageId(QStringLiteral("p"));
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QQuickItem* const unavailable = itemNamed(host.get(), QStringLiteral("page-host-page-unavailable"));
        QQuickItem* const prompt = itemNamed(host.get(), QStringLiteral("page-host-select-page"));
        QVERIFY(unavailable);
        QVERIFY(prompt);
        QTRY_VERIFY(unavailable->isVisible());
        // It is a page, so the page area does not ask for one.
        QVERIFY(!prompt->isVisible());

        // A good page shown instead takes the message away.
        controller.setCurrentPageId(QStringLiteral("q"));
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("first")));
        QVERIFY(!unavailable->isVisible());

        // And it is back with the broken page, which is not loaded again.
        controller.setCurrentPageId(QStringLiteral("p"));
        QVERIFY(unavailable->isVisible());
    }

    void aPageThatPassesContextWaitsForAController()
    {
        ApplicationController controller;
        auto doomed = std::make_unique<SimplePage>(QStringLiteral("p"));
        controller.registerPage(doomed.get(), QString(), QStringLiteral("P"), url(QStringLiteral("required")));
        controller.registry()->setPassesContext(QStringLiteral("p"), true);
        controller.registerPage(new SimplePage(QStringLiteral("q"), &controller), QString(), QStringLiteral("Q"),
                                url(QStringLiteral("first")));
        controller.setCurrentPageId(QStringLiteral("p"));
        // Its controller is gone before the page is first shown, so the
        // registry has none for it.
        doomed.reset();
        QVERIFY(!controller.registry()->controller(QStringLiteral("p")));

        // Said once per refusal, however often the page's entry is read
        // again in the meantime.
        const QRegularExpression refused(QStringLiteral("PageHost: page p passes context but has no controller"));
        QTest::ignoreMessage(QtWarningMsg, refused);
        QTest::failOnWarning(refused);
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QQuickItem* const unavailable = itemNamed(host.get(), QStringLiteral("page-host-page-unavailable"));
        QVERIFY(unavailable);
        QTRY_VERIFY(unavailable->isVisible());
        QVERIFY(!itemNamed(host.get(), QStringLiteral("required")));
        // Binding q makes every cached page read its registry entry again.
        QVERIFY(controller.bindPage(QStringLiteral("q"), new SimplePage(QStringLiteral("q"), &controller),
                                    url(QStringLiteral("second"))));
        QVERIFY(unavailable->isVisible());
        QVERIFY(!itemNamed(host.get(), QStringLiteral("required")));

        // A bind that brings a live controller creates the page with it.
        auto* const live = new SimplePage(QStringLiteral("p"), &controller);
        QVERIFY(controller.bindPage(QStringLiteral("p"), live, url(QStringLiteral("required"))));
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("required")));
        QCOMPARE(seenAtCompletion(host.get(), QStringLiteral("required"), "controllerAtCompletion"), live);
        QVERIFY(!unavailable->isVisible());
    }

    void aDeepLinkWaitsForAPageThatCanRevealIt()
    {
        ApplicationController controller;
        auto* const placeholder = new SimplePage(QStringLiteral("p"), &controller);
        controller.registerPage(placeholder, QString(), QStringLiteral("P"), url(QStringLiteral("first")));
        controller.setCurrentPageId(QStringLiteral("p"));
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(controllerOf(host.get(), QStringLiteral("first")) == placeholder);

        // The page on screen has no revealAnchor(), so it leaves the anchor
        // for the page that replaces it in place.
        controller.setPendingAnchor(QStringLiteral("p"), QStringLiteral("the-anchor"));
        auto* const real = new SimplePage(QStringLiteral("p"), &controller);
        QVERIFY(controller.bindPage(QStringLiteral("p"), real, url(QStringLiteral("reveal"))));
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("reveal")));
        QTRY_COMPARE(itemNamed(host.get(), QStringLiteral("reveal"))->property("revealed").toString(),
                     QStringLiteral("the-anchor"));
        // Taken by the page that revealed it.
        QVERIFY(controller.takePendingAnchor(QStringLiteral("p")).isEmpty());
    }

    void aHiddenPageKeepsItsSizeUntilItIsShown()
    {
        ApplicationController controller;
        controller.registerPage(new SimplePage(QStringLiteral("p"), &controller), QString(), QStringLiteral("P"),
                                url(QStringLiteral("first")));
        controller.registerPage(new SimplePage(QStringLiteral("q"), &controller), QString(), QStringLiteral("Q"),
                                url(QStringLiteral("second")));
        controller.setCurrentPageId(QStringLiteral("p"));
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("first")));
        QQuickItem* const hidden = itemNamed(host.get(), QStringLiteral("first"));
        QTRY_VERIFY(hidden->width() > 0 && hidden->height() > 0);
        controller.setCurrentPageId(QStringLiteral("q"));
        QTRY_VERIFY(itemNamed(host.get(), QStringLiteral("second")));
        QQuickItem* const shown = itemNamed(host.get(), QStringLiteral("second"));
        QTRY_COMPARE(shown->size(), hidden->size());
        const QSizeF before = hidden->size();

        // The shown page follows the window. The hidden one is left alone.
        host->setSize(host->size() + QSizeF(200, 100));
        QTRY_VERIFY(shown->width() > before.width() && shown->height() > before.height());
        QCOMPARE(hidden->size(), before);

        // Shown again, it follows the page area once more.
        controller.setCurrentPageId(QStringLiteral("p"));
        QTRY_COMPARE(hidden->size(), shown->size());
    }

    void theModulePageShowsItsModulesState_data()
    {
        QTest::addColumn<bool>("failed");
        QTest::addColumn<bool>("disabledAfterCrash");
        QTest::addColumn<QString>("reason");
        QTest::addColumn<bool>("canRetry");
        QTest::addColumn<QString>("heading");
        QTest::addColumn<QString>("icon");
        QTest::addColumn<QString>("action");
        QTest::newRow("not loaded yet") << false << false << QString() << true
                                        << QStringLiteral("Mod is not loaded yet") << QStringLiteral("plugins")
                                        << QStringLiteral("Load");
        QTest::newRow("failed") << true << false << QStringLiteral("its library did not load") << true
                                << QStringLiteral("Mod could not be loaded") << QStringLiteral("dialog-error")
                                << QStringLiteral("Try Again");
        QTest::newRow("disabled after a crash")
            << false << true << QString() << true << QStringLiteral("Mod is turned off")
            << QStringLiteral("dialog-warning") << QStringLiteral("Try Again");
        // A reason the controller still holds from before is not shown on a
        // page that did not fail.
        QTest::newRow("reason while not failed")
            << false << false << QStringLiteral("an earlier failure") << false
            << QStringLiteral("Mod is not loaded yet") << QStringLiteral("plugins") << QStringLiteral("Load");
        QTest::newRow("reason while turned off")
            << false << true << QStringLiteral("an earlier failure") << false << QStringLiteral("Mod is turned off")
            << QStringLiteral("dialog-warning") << QStringLiteral("Try Again");
    }

    void theModulePageShowsItsModulesState()
    {
        QFETCH(bool, failed);
        QFETCH(bool, disabledAfterCrash);
        QFETCH(QString, reason);
        QFETCH(bool, canRetry);
        QFETCH(QString, heading);
        QFETCH(QString, icon);
        QFETCH(QString, action);
        ApplicationController controller;
        PlaceholderController* const placeholder = hostModulePage(controller);
        placeholder->setState(failed, disabledAfterCrash, reason, canRetry);
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(pageControlledBy(host.get(), placeholder));
        QQuickItem* const page = pageControlledBy(host.get(), placeholder);
        // Created as every module page is, with the app as well.
        QCOMPARE(page->property("settingsApp").value<QObject*>(), &controller);

        QVERIFY2(showsText(page, heading), qPrintable(heading));
        // The host's reason shows for a failure only.
        if (!reason.isEmpty()) {
            QCOMPARE(showsText(page, reason), failed);
        }
        QQuickItem* const message = placeholderMessage(page);
        QVERIFY(message);
        // Only a module turned off after a crash says why it is off.
        QCOMPARE(message->property("explanation").toString().isEmpty(), !disabledAfterCrash);
        QObject* const iconGroup = message->property("icon").value<QObject*>();
        QVERIFY(iconGroup);
        QCOMPARE(iconGroup->property("name").toString(), icon);

        QObject* const helpfulAction = message->property("helpfulAction").value<QObject*>();
        QVERIFY(helpfulAction);
        QCOMPARE(helpfulAction->property("text").toString(), action);
        QCOMPARE(helpfulAction->property("enabled").toBool(), canRetry);
        QVERIFY(QMetaObject::invokeMethod(helpfulAction, "trigger"));
        QCOMPARE(placeholder->retries(), canRetry ? 1 : 0);
    }

    void theModulePageFollowsItsModulesState()
    {
        ApplicationController controller;
        PlaceholderController* const placeholder = hostModulePage(controller);
        placeholder->setState(false, false, QString(), true);
        const std::unique_ptr<QQuickItem> host = createHost(controller);
        QVERIFY(host);
        QTRY_VERIFY(pageControlledBy(host.get(), placeholder));
        QQuickItem* const page = pageControlledBy(host.get(), placeholder);
        QVERIFY(showsText(page, QStringLiteral("Mod is not loaded yet")));

        // A load that fails while the page is on screen.
        placeholder->setState(true, false, QStringLiteral("its library did not load"), false);
        QVERIFY(showsText(page, QStringLiteral("Mod could not be loaded")));
        QVERIFY(!showsText(page, QStringLiteral("Mod is not loaded yet")));
        QVERIFY(showsText(page, QStringLiteral("its library did not load")));
        QQuickItem* const message = placeholderMessage(page);
        QVERIFY(message);
        QObject* const helpfulAction = message->property("helpfulAction").value<QObject*>();
        QVERIFY(helpfulAction);
        QCOMPARE(helpfulAction->property("text").toString(), QStringLiteral("Try Again"));
        QVERIFY(!helpfulAction->property("enabled").toBool());
    }

private:
    void writeQml(const QString& name, const QByteArray& body)
    {
        QFile file(m_dir.filePath(name + QStringLiteral(".qml")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(body);
    }

    QUrl url(const QString& name) const
    {
        return QUrl::fromLocalFile(m_dir.filePath(name + QStringLiteral(".qml")));
    }

    // Registers a module page as FacetHost registers one whose module is not
    // loaded, and makes it current. Returns its placeholder controller, which
    // @p controller owns.
    static PlaceholderController* hostModulePage(ApplicationController& controller)
    {
        const QString id = QStringLiteral("mod:page");
        auto* const placeholder = new PlaceholderController(id, &controller);
        controller.registerPage(placeholder, QString(), QStringLiteral("Mod"), FacetHost::placeholderPageSource());
        controller.registry()->setPassesContext(id, true);
        controller.setCurrentPageId(id);
        return placeholder;
    }

    // A PageHost over @p controller, or null with the component's errors
    // logged.
    std::unique_ptr<QQuickItem> createHost(ApplicationController& controller)
    {
        QQmlComponent component(&m_engine);
        component.setData("import org.phosphor.control\nPageHost { width: 800; height: 600 }\n", QUrl());
        std::unique_ptr<QObject> created(component.createWithInitialProperties(
            {{QStringLiteral("controller"), QVariant::fromValue<QObject*>(&controller)}}));
        if (!created) {
            qWarning("%s", qPrintable(component.errorString()));
            return nullptr;
        }
        auto* const host = qobject_cast<QQuickItem*>(created.get());
        if (!host) {
            return nullptr;
        }
        created.release();
        return std::unique_ptr<QQuickItem>(host);
    }

    QTemporaryDir m_dir;
    // Declared before m_engine, which is told it has a controller, so it
    // outlives the engine.
    PhosphorControlTest::TestIncubator m_incubator;
    // Declared after m_dir, so it goes before the files it loaded.
    QQmlEngine m_engine;
};

QTEST_MAIN(TestPageHost)
#include "test_page_host.moc"
