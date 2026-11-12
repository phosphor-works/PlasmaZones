// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The chrome's page QML against a live ApplicationController. PageHost: the
// page it shows, and the controller it hands that page, follow the registry
// when a page is bound to a new controller or QML, whether the page is on
// screen or only cached. FacetModulePage: it builds without a warning in
// each state its placeholder controller reports, and shows that state.

#include <PhosphorControl/ApplicationController.h>
#include <PhosphorControl/PageController.h>

#include <QCoreApplication>
#include <QFile>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <utility>

using namespace PhosphorControl;

namespace {

class SimplePage : public PageController
{
    Q_OBJECT
public:
    using PageController::PageController;
    bool isDirty() const override
    {
        return false;
    }
    void apply() override
    {
        Q_EMIT applyResult(true, QString());
    }
    void discard() override
    {
        Q_EMIT discardResult(true, QString());
    }
};

// What FacetModulePage reads from the placeholder controller PageHost hands
// it, in one fixed state.
class PlaceholderController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString moduleName READ moduleName CONSTANT)
    Q_PROPERTY(bool failed READ failed CONSTANT)
    Q_PROPERTY(bool disabledAfterCrash READ disabledAfterCrash CONSTANT)
    Q_PROPERTY(QString reason READ reason CONSTANT)
    Q_PROPERTY(bool canRetry READ canRetry CONSTANT)
public:
    PlaceholderController(bool failed, bool disabledAfterCrash, QString reason, bool canRetry)
        : m_failed(failed)
        , m_disabledAfterCrash(disabledAfterCrash)
        , m_reason(std::move(reason))
        , m_canRetry(canRetry)
    {
    }
    QString moduleName() const
    {
        return QStringLiteral("Mod");
    }
    bool failed() const
    {
        return m_failed;
    }
    bool disabledAfterCrash() const
    {
        return m_disabledAfterCrash;
    }
    QString reason() const
    {
        return m_reason;
    }
    bool canRetry() const
    {
        return m_canRetry;
    }
    Q_INVOKABLE void retry()
    {
    }

private:
    bool m_failed;
    bool m_disabledAfterCrash;
    QString m_reason;
    bool m_canRetry;
};

// Loader items are visual children, which QObject::findChild does not see.
// The first item under @p root, depth first, that @p matches accepts.
template<typename Predicate>
QQuickItem* findItem(QQuickItem* root, const Predicate& matches)
{
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
        for (const char* name : {"first", "second", "third"}) {
            QFile page(m_dir.filePath(QString::fromLatin1(name) + QStringLiteral(".qml")));
            QVERIFY(page.open(QIODevice::WriteOnly));
            page.write(QByteArray("import QtQuick\nItem { objectName: \"") + name
                       + "\"; property QtObject controller: null }\n");
        }
    }

    void init()
    {
        // Every warning QML reports, from a file or from the components
        // these tests build from data, fails the test that caused it.
        QTest::failOnWarning(QRegularExpression(QStringLiteral("(\\.qml|<Unknown File>):\\d+")));
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
        // the real page into it, and the new item gets the real controller.
        auto* const real = new SimplePage(QStringLiteral("p"), &controller);
        QVERIFY(controller.bindPage(QStringLiteral("p"), real, url(QStringLiteral("second"))));
        QTRY_VERIFY(controllerOf(host.get(), QStringLiteral("second")) == real);
        QVERIFY(!itemNamed(host.get(), QStringLiteral("first")));
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

        // New QML: the cached page is rebuilt from it, with the controller.
        auto* const next = new SimplePage(QStringLiteral("p"), &controller);
        QVERIFY(controller.bindPage(QStringLiteral("p"), next, url(QStringLiteral("third"))));
        QTRY_VERIFY(controllerOf(host.get(), QStringLiteral("third")) == next);
        QVERIFY(!itemNamed(host.get(), QStringLiteral("first")));
        QCOMPARE(controller.currentPageId(), QStringLiteral("q"));
    }

    void theModulePageBuildsWithoutWarnings_data()
    {
        // Null: no controller, which PageHost never leaves a page with, but
        // which the page must survive while it is being built.
        QTest::addColumn<bool>("hasController");
        QTest::addColumn<bool>("failed");
        QTest::addColumn<bool>("disabledAfterCrash");
        QTest::addColumn<QString>("reason");
        QTest::addColumn<QString>("heading");
        QTest::newRow("no controller") << false << false << false << QString() << QStringLiteral(" is not loaded yet");
        QTest::newRow("not loaded yet") << true << false << false << QString()
                                        << QStringLiteral("Mod is not loaded yet");
        QTest::newRow("failed") << true << true << false << QStringLiteral("its library did not load")
                                << QStringLiteral("Mod could not be loaded");
        QTest::newRow("disabled after a crash")
            << true << false << true << QString() << QStringLiteral("Mod is turned off");
        // A reason the controller still holds from before is not shown on a
        // page that did not fail.
        QTest::newRow("reason while not failed") << true << false << false << QStringLiteral("an earlier failure")
                                                 << QStringLiteral("Mod is not loaded yet");
        QTest::newRow("reason while turned off")
            << true << false << true << QStringLiteral("an earlier failure") << QStringLiteral("Mod is turned off");
    }

    void theModulePageBuildsWithoutWarnings()
    {
        QFETCH(bool, hasController);
        QFETCH(bool, failed);
        QFETCH(bool, disabledAfterCrash);
        QFETCH(QString, reason);
        QFETCH(QString, heading);
        PlaceholderController placeholder(failed, disabledAfterCrash, reason, true);
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import org.phosphor.control\nFacetModulePage { width: 400; height: 300 }\n", QUrl());
        QVariantMap properties;
        if (hasController) {
            properties.insert(QStringLiteral("controller"), QVariant::fromValue<QObject*>(&placeholder));
        }
        std::unique_ptr<QObject> created(component.createWithInitialProperties(properties));
        QVERIFY2(created, qPrintable(component.errorString()));
        auto* const page = qobject_cast<QQuickItem*>(created.get());
        QVERIFY(page);
        QCoreApplication::processEvents();
        QVERIFY2(showsText(page, heading), qPrintable(heading));
        // The host's reason shows for a failure only.
        if (!reason.isEmpty()) {
            QCOMPARE(showsText(page, reason), failed);
        }
    }

private:
    QUrl url(const QString& name) const
    {
        return QUrl::fromLocalFile(m_dir.filePath(name + QStringLiteral(".qml")));
    }

    // A PageHost over @p controller, or null with the component's errors
    // logged.
    std::unique_ptr<QQuickItem> createHost(ApplicationController& controller)
    {
        QQmlComponent component(&m_engine);
        component.setData("import org.phosphor.control\nPageHost {}\n", QUrl());
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
    // Declared after m_dir, so it goes before the files it loaded.
    QQmlEngine m_engine;
};

QTEST_MAIN(TestPageHost)
#include "test_page_host.moc"
