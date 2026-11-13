// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// FacetSlot and FacetExtensionFrame against a stubbed extension provider, so
// the QML is exercised without loading any module.

#include "simple_page.h"

#include <PhosphorControl/ApplicationController.h>
#include <PhosphorControl/FacetSlotModel.h>

#include <QAbstractItemModelTester>
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>
#include <utility>

using namespace PhosphorControl;
using PhosphorControlTest::SimplePage;

namespace {

// Kirigami.MessageType's values, which an InlineMessage's type holds.
constexpr int MessageWarning = 2;
constexpr int MessageError = 3;

QVariantMap entry(const QString& id)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("moduleId"), QStringLiteral("extender")},
            {QStringLiteral("moduleName"), QStringLiteral("Extender")},
            {QStringLiteral("name"), id},
            {QStringLiteral("modulePageId"), QStringLiteral("extender:own")}};
}

// The visual tree: a Repeater's delegates and a Loader's item are visual
// children of their parent item, which is what this walks. Empty for a null
// @p item, so a lookup under an item that is gone fails its check instead of
// crashing the test.
QList<QQuickItem*> descendants(QQuickItem* item)
{
    QList<QQuickItem*> all;
    if (!item) {
        return all;
    }
    const QList<QQuickItem*> children = item->childItems();
    for (QQuickItem* child : children) {
        all.append(child);
        all.append(descendants(child));
    }
    return all;
}

QQuickItem* findItem(QQuickItem* root, const QString& objectName)
{
    const QList<QQuickItem*> all = descendants(root);
    for (QQuickItem* item : all) {
        if (item->objectName() == objectName) {
            return item;
        }
    }
    return nullptr;
}

// The item a frame's Loader created, or null.
QQuickItem* loadedSection(QQuickItem* frame)
{
    QQuickItem* const loader = findItem(frame, QStringLiteral("facet-extension-section"));
    return loader ? loader->property("item").value<QQuickItem*>() : nullptr;
}

// The domain the section a frame built from Required.qml or Required2.qml
// held when it was completed, or null when the frame has no section.
QObject* seenAtCompletion(QQuickItem* frame)
{
    QQuickItem* const section = loadedSection(frame);
    return section ? section->property("seenAtCompletion").value<QObject*>() : nullptr;
}

// The extension ids @p model lists, in order.
QStringList extensionIds(const FacetSlotModel* model)
{
    QStringList ids;
    for (int row = 0; row < model->rowCount(); ++row) {
        ids.append(model->data(model->index(row), FacetSlotModel::ExtensionIdRole).toString());
    }
    return ids;
}

bool shown(QQuickItem* frame, const QString& part)
{
    QQuickItem* const item = findItem(frame, part);
    return item && item->isVisible();
}

} // namespace

// Counts the sections built from Required.qml and Required2.qml, which count
// themselves in Component.onCompleted, whatever domain each got, so a section
// built and thrown away during setup shows up. Other section files do not
// count.
class BuildLog : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int builds MEMBER builds NOTIFY buildsChanged)
public:
    int builds = 0;
Q_SIGNALS:
    void buildsChanged();
};

// Stands behind a page's reveal registry: the anchors registered with it, and
// the ones taken out again.
class AnchorLog : public QObject
{
    Q_OBJECT
public:
    QHash<QString, QPointer<QObject>> registered;
    QStringList unregistered;

    Q_INVOKABLE void add(const QString& anchorId, QObject* item)
    {
        registered.insert(anchorId, item);
    }
    Q_INVOKABLE void remove(const QString& anchorId, QObject* item)
    {
        if (registered.value(anchorId) == item) {
            registered.remove(anchorId);
            unregistered.append(anchorId);
        }
    }
};

class TestFacetSlot : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        writeQml(QStringLiteral("Section.qml"),
                 "import QtQuick\n"
                 "Item { property QtObject domain; property QtObject settingsApp; implicitHeight: 10 }\n");
        // Reads its domain while it is being built, which only works when the
        // frame hands the domain over as an initial property.
        writeQml(QStringLiteral("Required.qml"),
                 "import QtQuick\n"
                 "Item { required property QtObject domain; property QtObject seenAtCompletion;\n"
                 "       Component.onCompleted: { seenAtCompletion = domain; buildLog.builds += 1; }\n"
                 "       implicitHeight: 10 }\n");
        // Another file with Required.qml's content, for a section whose QML
        // moves.
        writeQml(QStringLiteral("Required2.qml"),
                 "import QtQuick\n"
                 "Item { required property QtObject domain; property QtObject seenAtCompletion;\n"
                 "       Component.onCompleted: { seenAtCompletion = domain; buildLog.builds += 1; }\n"
                 "       implicitHeight: 10 }\n");
        writeQml(QStringLiteral("Broken.qml"), "import QtQuick\nItem { this is not QML }\n");
        // Navigates while it is being built, as a section reacting to its
        // own state might.
        writeQml(QStringLiteral("Navigate.qml"),
                 "import QtQuick\n"
                 "Item { property QtObject domain; Component.onCompleted: appController.currentPageId = "
                 "\"extender:own\" }\n");
    }

    void init()
    {
        // Fails the test on every warning the QML engine reports against a
        // location, which its message text carries as `<url>:<line>`: a .qml
        // or .js file, or <Unknown File> for a component built from data. A
        // section created with a property it does not declare is reported
        // against FacetExtensionFrame.qml, at its setSource call: the
        // section's Loader is synchronous here, since nothing in this test
        // creates a frame inside an asynchronous incubation. Also on a
        // TypeError a script reports, and on the warning injectIfAssignable
        // logs for a property it could not set, neither of which need carry
        // a location. A test that expects a warning declares it with
        // QTest::ignoreMessage, which takes precedence.
        QTest::failOnWarning(
            QRegularExpression(QStringLiteral("(\\.(qml|js)|<Unknown File>):\\d+|TypeError|injectIfAssignable")));
        m_controller = std::make_unique<ApplicationController>();
        m_controller->registerPage(new SimplePage(QStringLiteral("extender:own"), m_controller.get()), QString(),
                                   QStringLiteral("Own"), QUrl::fromLocalFile(qml(QStringLiteral("Section.qml"))));
        m_controller->registerPage(new SimplePage(QStringLiteral("good:general"), m_controller.get()), QString(),
                                   QStringLiteral("General"), QUrl::fromLocalFile(qml(QStringLiteral("Section.qml"))));
        m_listed.clear();
        m_buildLog.builds = 0;
        QVERIFY(
            m_controller->setExtensionProvider(m_controller.get(), [this](const QString& pageId, const QString& slot) {
                return pageId == QLatin1String("good:general") && slot == QLatin1String("top") ? m_listed
                                                                                               : QVariantList();
            }));
    }

    void cleanup()
    {
        m_root.reset();
        m_engine.reset();
        m_controller.reset();
    }

    void showsEachSectionAndRefreshesForItsPage()
    {
        m_listed = {section(QStringLiteral("extender:ok"), QStringLiteral("Section.qml")),
                    failed(QStringLiteral("extender:broken"), QStringLiteral("it failed to load"))};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        QVERIFY(slot->isVisible());
        QCOMPARE(slot->property("count").toInt(), 2);

        QQuickItem* const okFrame = findItem(slot, QStringLiteral("facet-extension:extender:ok"));
        QVERIFY(okFrame);
        QQuickItem* const loaded = loadedSection(okFrame);
        QVERIFY2(loaded, "the section's QML was not loaded");
        QCOMPARE(loaded->property("domain").value<QObject*>(), &m_domain);
        QCOMPARE(loaded->property("settingsApp").value<QObject*>(), m_controller.get());
        QVERIFY(!shown(okFrame, QStringLiteral("facet-extension-error")));
        QVERIFY(!shown(okFrame, QStringLiteral("facet-extension-reason")));

        QQuickItem* const brokenFrame = findItem(slot, QStringLiteral("facet-extension:extender:broken"));
        QVERIFY(brokenFrame);
        QVERIFY(!loadedSection(brokenFrame));
        QVERIFY(shown(brokenFrame, QStringLiteral("facet-extension-error")));
        QVERIFY(shown(brokenFrame, QStringLiteral("facet-extension-reason")));
        QCOMPARE(findItem(brokenFrame, QStringLiteral("facet-extension-reason"))->property("text").toString(),
                 QStringLiteral("it failed to load"));

        // Another page changing does not touch this slot; its own page does.
        m_listed.clear();
        m_controller->notifyExtensionsChanged(QStringLiteral("other"));
        QCOMPARE(slot->property("count").toInt(), 2);
        m_controller->notifyExtensionsChanged(QStringLiteral("good:general"));
        QCOMPARE(slot->property("count").toInt(), 0);
        QVERIFY(!slot->isVisible());
    }

    void sectionsAppearWhenSettingsAppArrivesLate()
    {
        // A page that does not pass context gets its settingsApp from PageHost
        // only once it is built, so a FacetSlot bound to page.settingsApp
        // starts without one.
        m_listed = {section(QStringLiteral("extender:ok"), QStringLiteral("Section.qml"))};
        QQuickItem* const slot = createSlot(nullptr);
        QVERIFY(slot);
        QCOMPARE(slot->property("count").toInt(), 0);
        QVERIFY(!slot->isVisible());

        slot->setProperty("settingsApp", QVariant::fromValue<QObject*>(m_controller.get()));
        QCOMPARE(slot->property("count").toInt(), 1);
        QVERIFY(slot->isVisible());
        QVERIFY(loadedSection(findItem(slot, QStringLiteral("facet-extension:extender:ok"))));
    }

    void aSectionThatStaysKeepsItsFrame()
    {
        m_listed = {section(QStringLiteral("extender:a"), QStringLiteral("Section.qml"))};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        const QPointer<QQuickItem> frame = findItem(slot, QStringLiteral("facet-extension:extender:a"));
        QVERIFY(frame);
        const QPointer<QQuickItem> loaded = loadedSection(frame);
        QVERIFY(loaded);

        // Another section arrives ahead of it, as a module loading while the
        // page is open does.
        m_listed.prepend(section(QStringLiteral("extender:b"), QStringLiteral("Section.qml")));
        m_controller->notifyExtensionsChanged(QStringLiteral("good:general"));
        QCOMPARE(slot->property("count").toInt(), 2);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY2(frame, "the frame of a section that stayed was rebuilt");
        QVERIFY2(loaded, "the QML of a section that stayed was reloaded");
        QCOMPARE(findItem(slot, QStringLiteral("facet-extension:extender:a")), frame.data());
    }

    void aSectionCanRequireItsDomain()
    {
        m_listed = {section(QStringLiteral("extender:required"), QStringLiteral("Required.qml"))};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        QQuickItem* const frame = findItem(slot, QStringLiteral("facet-extension:extender:required"));
        QVERIFY(frame);
        QQuickItem* const loaded = loadedSection(frame);
        QVERIFY2(loaded, "a section declaring a required domain did not load");
        QCOMPARE(loaded->property("seenAtCompletion").value<QObject*>(), &m_domain);
        QVERIFY(!shown(frame, QStringLiteral("facet-extension-error")));
        // Built once, not once per property the frame received.
        QCoreApplication::processEvents();
        QCOMPARE(m_buildLog.builds, 1);
    }

    void aSectionWhoseModuleLoadsLaterGetsItsDomain()
    {
        // A retry while the page is open: the row stays, and its properties
        // change one at a time.
        m_listed = {failed(QStringLiteral("extender:flip"), QStringLiteral("it did not load"))};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        const QPointer<QQuickItem> frame = findItem(slot, QStringLiteral("facet-extension:extender:flip"));
        QVERIFY(frame);
        QVERIFY(shown(frame, QStringLiteral("facet-extension-error")));

        m_listed = {section(QStringLiteral("extender:flip"), QStringLiteral("Required.qml"))};
        m_controller->notifyExtensionsChanged(QStringLiteral("good:general"));
        QTRY_VERIFY(loadedSection(frame));
        QVERIFY(frame);
        QCOMPARE(seenAtCompletion(frame), &m_domain);
        QVERIFY(!shown(frame, QStringLiteral("facet-extension-error")));
        QCoreApplication::processEvents();
        QCOMPARE(m_buildLog.builds, 1);
    }

    void aNewDomainReachesTheSection()
    {
        m_listed = {section(QStringLiteral("extender:moved"), QStringLiteral("Required.qml"))};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        const QPointer<QQuickItem> frame = findItem(slot, QStringLiteral("facet-extension:extender:moved"));
        QVERIFY(frame);
        QCOMPARE(seenAtCompletion(frame), &m_domain);

        // Same section and QML, another domain.
        QVariantMap moved = m_listed.first().toMap();
        moved.insert(QStringLiteral("domain"), QVariant::fromValue<QObject*>(&m_otherDomain));
        m_listed = {moved};
        m_controller->notifyExtensionsChanged(QStringLiteral("good:general"));
        QTRY_COMPARE(seenAtCompletion(frame), &m_otherDomain);
    }

    void aSectionThatNavigatesWhileBuiltLeavesAConsistentSlot()
    {
        // As PageHost's slots do, this one follows the current page. The
        // first section moves it while the slot is still adding the rest.
        m_controller->setCurrentPageId(QStringLiteral("good:general"));
        QQmlEngine* const engine = makeEngine();
        engine->rootContext()->setContextProperty(QStringLiteral("appController"), m_controller.get());
        QQmlComponent component(engine);
        component.setData(
            "import org.phosphor.control\n"
            "FacetSlot { pageId: settingsApp ? settingsApp.currentPageId : \"\" }\n",
            QUrl());
        m_root.reset(component.createWithInitialProperties(
            {{QStringLiteral("settingsApp"), QVariant::fromValue<QObject*>(m_controller.get())},
             {QStringLiteral("slot"), QStringLiteral("top")}}));
        QVERIFY2(m_root, qPrintable(component.errorString()));
        auto* const slot = qobject_cast<QQuickItem*>(m_root.get());
        QVERIFY(slot);
        slot->setParentItem(&m_page);
        // Arriving in a slot already on screen, as when a module loads.
        m_listed = {section(QStringLiteral("extender:navigate"), QStringLiteral("Navigate.qml")),
                    section(QStringLiteral("extender:after"), QStringLiteral("Section.qml"))};
        m_controller->notifyExtensionsChanged(QStringLiteral("good:general"));
        QCOMPARE(m_controller->currentPageId(), QStringLiteral("extender:own"));
        // It shows what the page it ended on lists, which is nothing.
        QCOMPARE(slot->property("count").toInt(), 0);
        QVERIFY(!findItem(slot, QStringLiteral("facet-extension:extender:after")));
    }

    void aFrameLoadsOnceWhateverOrderItsPropertiesChangeIn_data()
    {
        QTest::addColumn<QStringList>("order");
        // A delegate model sets changed roles in an order that varies from
        // run to run, so each order is driven here directly.
        const QStringList properties{QStringLiteral("failed"), QStringLiteral("qmlSource"), QStringLiteral("domain")};
        QStringList order = properties;
        std::sort(order.begin(), order.end());
        do {
            QTest::newRow(qPrintable(order.join(QLatin1Char(',')))) << order;
        } while (std::next_permutation(order.begin(), order.end()));
    }

    void aFrameLoadsOnceWhateverOrderItsPropertiesChangeIn()
    {
        // The frame of a section whose module failed, then loads: the
        // properties of its row change one at a time.
        QFETCH(QStringList, order);
        QQuickItem* const frame = createFrame({
            {QStringLiteral("extensionId"), QStringLiteral("extender:order")},
            {QStringLiteral("qmlSource"), QUrl()},
            {QStringLiteral("domain"), QVariant::fromValue<QObject*>(nullptr)},
            {QStringLiteral("failed"), true},
            {QStringLiteral("moduleFailed"), true},
            {QStringLiteral("error"), QStringLiteral("it did not load")},
        });
        QVERIFY(frame);
        QVERIFY(!loadedSection(frame));

        const QVariantMap next{{QStringLiteral("failed"), false},
                               {QStringLiteral("qmlSource"), QUrl::fromLocalFile(qml(QStringLiteral("Required.qml")))},
                               {QStringLiteral("domain"), QVariant::fromValue<QObject*>(&m_domain)}};
        for (const QString& property : std::as_const(order)) {
            QVERIFY(frame->setProperty(qPrintable(property), next.value(property)));
        }
        QVERIFY(frame->setProperty("moduleFailed", false));
        QVERIFY(frame->setProperty("error", QString()));
        // Nothing is built while the row is still changing: the load waits
        // until control is back in the event loop, when the whole row has
        // landed, whichever property changed last.
        QCOMPARE(m_buildLog.builds, 0);
        QVERIFY(!loadedSection(frame));
        QTRY_VERIFY(loadedSection(frame));
        QCoreApplication::processEvents();
        QCOMPARE(seenAtCompletion(frame), &m_domain);
        QCOMPARE(m_buildLog.builds, 1);
    }

    void aShownSectionRebuildsOnceForANewSourceAndDomain_data()
    {
        QTest::addColumn<bool>("domainFirst");
        QTest::newRow("domain first") << true;
        QTest::newRow("source first") << false;
    }

    void aShownSectionRebuildsOnceForANewSourceAndDomain()
    {
        QFETCH(bool, domainFirst);
        QQuickItem* const frame = createFrame({
            {QStringLiteral("extensionId"), QStringLiteral("extender:moves")},
            {QStringLiteral("qmlSource"), QUrl::fromLocalFile(qml(QStringLiteral("Required.qml")))},
            {QStringLiteral("domain"), QVariant::fromValue<QObject*>(&m_domain)},
            {QStringLiteral("failed"), false},
            {QStringLiteral("moduleFailed"), false},
            {QStringLiteral("error"), QString()},
        });
        QVERIFY(frame);
        QCOMPARE(m_buildLog.builds, 1);

        const QUrl source = QUrl::fromLocalFile(qml(QStringLiteral("Required2.qml")));
        const QVariant domain = QVariant::fromValue<QObject*>(&m_otherDomain);
        if (domainFirst) {
            frame->setProperty("domain", domain);
            frame->setProperty("qmlSource", source);
        } else {
            frame->setProperty("qmlSource", source);
            frame->setProperty("domain", domain);
        }
        QTRY_COMPARE(seenAtCompletion(frame), &m_otherDomain);
        QCoreApplication::processEvents();
        QCOMPARE(m_buildLog.builds, 2);
    }

    void aSectionWhoseDomainIsGoneIsNotBuilt()
    {
        // A provider that has not re-listed it yet: shown as gone, and no
        // section is built without its domain.
        auto domain = std::make_unique<QObject>();
        m_listed = {section(QStringLiteral("extender:orphan"), QStringLiteral("Required.qml"), domain.get())};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        QQuickItem* const frame = findItem(slot, QStringLiteral("facet-extension:extender:orphan"));
        QVERIFY(frame);
        QVERIFY(loadedSection(frame));
        QCOMPARE(m_buildLog.builds, 1);

        domain.reset();
        QTRY_VERIFY(shown(frame, QStringLiteral("facet-extension-error")));
        QVERIFY(!shown(frame, QStringLiteral("facet-extension-section")));
        QCOMPARE(findItem(frame, QStringLiteral("facet-extension-error"))->property("text").toString(),
                 QStringLiteral("This section could not be shown."));
        QCoreApplication::processEvents();
        QCOMPARE(m_buildLog.builds, 1);
    }

    void aSectionThatFailsOnItsOwnDoesNotBlameItsModule()
    {
        // The module loaded, and only this section cannot be shown.
        QVariantMap broken = entry(QStringLiteral("extender:gone"));
        broken.insert(QStringLiteral("error"), QStringLiteral("the section's staging domain was deleted"));
        m_listed = {broken};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        QQuickItem* const frame = findItem(slot, QStringLiteral("facet-extension:extender:gone"));
        QVERIFY(frame);
        QQuickItem* const card = findItem(frame, QStringLiteral("facet-extension-error"));
        QVERIFY(card && card->isVisible());
        QCOMPARE(card->property("text").toString(), QStringLiteral("This section could not be shown."));
        QCOMPARE(card->property("type").toInt(), MessageError);
        QObject* const open = frame->findChild<QObject*>(QStringLiteral("facet-extension-open-module"));
        QVERIFY(open);
        QVERIFY(!open->property("visible").toBool());
    }

    void aSectionWhoseQmlFailsShowsTheCard()
    {
        // The engine reports the broken file once, when the frame loads it.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Broken\\.qml:\\d+")));
        m_listed = {section(QStringLiteral("extender:bad-qml"), QStringLiteral("Broken.qml"))};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        QQuickItem* const frame = findItem(slot, QStringLiteral("facet-extension:extender:bad-qml"));
        QVERIFY(frame);
        QVERIFY(!loadedSection(frame));
        QVERIFY(shown(frame, QStringLiteral("facet-extension-error")));
        QQuickItem* const card = findItem(frame, QStringLiteral("facet-extension-error"));
        QCOMPARE(card->property("text").toString(),
                 QStringLiteral("This section could not be shown, because its QML did not load."));
        QCOMPARE(card->property("type").toInt(), MessageError);
        // Nothing to report from the host: the module loaded.
        QVERIFY(!shown(frame, QStringLiteral("facet-extension-reason")));
    }

    void aModuleThatDidNotLoadIsNamedOnTheCard()
    {
        m_listed = {failed(QStringLiteral("extender:broken"), QStringLiteral("it failed to load"))};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        QQuickItem* const frame = findItem(slot, QStringLiteral("facet-extension:extender:broken"));
        QVERIFY(frame);
        QQuickItem* const card = findItem(frame, QStringLiteral("facet-extension-error"));
        QVERIFY(card && card->isVisible());
        QCOMPARE(card->property("text").toString(),
                 QStringLiteral("This section could not be shown, because the module it comes from did not load."));
        QCOMPARE(card->property("type").toInt(), MessageError);
    }

    void aDisabledModuleCardOpensItsPage()
    {
        QVariantMap disabled = failed(QStringLiteral("extender:off"), QStringLiteral("it was loading when it crashed"));
        disabled.insert(QStringLiteral("disabledAfterCrash"), true);
        m_listed = {disabled};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        QQuickItem* const frame = findItem(slot, QStringLiteral("facet-extension:extender:off"));
        QVERIFY(frame);
        QVERIFY(shown(frame, QStringLiteral("facet-extension-error")));
        // The translated card says the module was turned off after a crash,
        // as a warning rather than an error. The host's English reason would
        // only repeat it, as on the module's own page.
        QQuickItem* const card = findItem(frame, QStringLiteral("facet-extension-error"));
        QCOMPARE(card->property("text").toString(),
                 QStringLiteral("This section is turned off, because Facet closed unexpectedly the last time it "
                                "loaded the module it comes from."));
        QCOMPARE(card->property("type").toInt(), MessageWarning);
        QVERIFY(!shown(frame, QStringLiteral("facet-extension-reason")));
        QObject* const open = frame->findChild<QObject*>(QStringLiteral("facet-extension-open-module"));
        QVERIFY(open);
        QVERIFY(open->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(open, "trigger"));
        QCOMPARE(m_controller->currentPageId(), QStringLiteral("extender:own"));
    }

    void aSectionIsASearchAnchorOnItsPage()
    {
        // A page's reveal registry, as plasmazones' SettingsFlickable has
        // one, with a slot inside the page.
        m_listed = {section(QStringLiteral("extender:ok"), QStringLiteral("Section.qml")),
                    failed(QStringLiteral("extender:broken"), QStringLiteral("it failed to load"))};
        AnchorLog log;
        // The page's frames unregister their anchors from the log as they go,
        // so they go before it, on a failed check too, rather than in
        // cleanup() once the log is gone.
        const auto pageGoesFirst = qScopeGuard([this] {
            m_root.reset();
            m_engine.reset();
        });
        QQmlEngine* const engine = makeEngine();
        engine->rootContext()->setContextProperty(QStringLiteral("anchorLog"), &log);
        engine->rootContext()->setContextProperty(QStringLiteral("appController"), m_controller.get());
        QQmlComponent component(engine);
        component.setData(
            "import QtQuick\n"
            "import org.phosphor.control\n"
            "Item {\n"
            "    function registerSearchAnchor(anchorId, item) { anchorLog.add(anchorId, item) }\n"
            "    function unregisterSearchAnchor(anchorId, item) { anchorLog.remove(anchorId, item) }\n"
            "    Item {\n"
            "        FacetSlot { objectName: \"slot\"; settingsApp: appController;\n"
            "                    pageId: \"good:general\"; slot: \"top\" }\n"
            "    }\n"
            "}\n",
            QUrl());
        m_root.reset(component.create());
        QVERIFY2(m_root, qPrintable(component.errorString()));
        auto* const page = qobject_cast<QQuickItem*>(m_root.get());
        QVERIFY(page);
        page->setParentItem(&m_page);
        QQuickItem* const slot = findItem(page, QStringLiteral("slot"));
        QVERIFY(slot);
        QCOMPARE(slot->property("count").toInt(), 2);

        // Each frame under its section's anchor, a card standing in for a
        // section that cannot be shown too.
        QQuickItem* const okFrame = findItem(slot, QStringLiteral("facet-extension:extender:ok"));
        QQuickItem* const brokenFrame = findItem(slot, QStringLiteral("facet-extension:extender:broken"));
        QVERIFY(okFrame);
        QVERIFY(brokenFrame);
        QTRY_COMPARE(log.registered.size(), 2);
        QCOMPARE(log.registered.value(QStringLiteral("extender:ok")).data(), okFrame);
        QCOMPARE(log.registered.value(QStringLiteral("extender:broken")).data(), brokenFrame);

        // A section that goes takes its anchor with it, and only its own.
        m_listed = {section(QStringLiteral("extender:ok"), QStringLiteral("Section.qml"))};
        m_controller->notifyExtensionsChanged(QStringLiteral("good:general"));
        QCOMPARE(slot->property("count").toInt(), 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_COMPARE(log.unregistered, QStringList{QStringLiteral("extender:broken")});
        QCOMPARE(log.registered.keys(), QStringList{QStringLiteral("extender:ok")});

        // The whole page going takes the rest.
        m_root.reset();
        QCOMPARE(log.unregistered, (QStringList{QStringLiteral("extender:broken"), QStringLiteral("extender:ok")}));
        QVERIFY(log.registered.isEmpty());
    }

    void changingThePageOrSlotRereadsTheList()
    {
        m_listed = {section(QStringLiteral("extender:ok"), QStringLiteral("Section.qml"))};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        QCOMPARE(slot->property("count").toInt(), 1);
        slot->setProperty("slot", QStringLiteral("bottom"));
        QCOMPARE(slot->property("count").toInt(), 0);
        slot->setProperty("slot", QStringLiteral("top"));
        QCOMPARE(slot->property("count").toInt(), 1);
        slot->setProperty("pageId", QStringLiteral("other:page"));
        QCOMPARE(slot->property("count").toInt(), 0);
    }

    void aSectionThatMovesKeepsItsFrame()
    {
        m_listed = {section(QStringLiteral("extender:a"), QStringLiteral("Section.qml")),
                    section(QStringLiteral("extender:b"), QStringLiteral("Section.qml"))};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        auto* const model = slot->findChild<FacetSlotModel*>();
        QVERIFY(model);
        QAbstractItemModelTester tester(model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        const QPointer<QQuickItem> a = findItem(slot, QStringLiteral("facet-extension:extender:a"));
        const QPointer<QQuickItem> b = findItem(slot, QStringLiteral("facet-extension:extender:b"));
        QVERIFY(a);
        QVERIFY(b);

        m_listed = {m_listed.at(1), m_listed.at(0)};
        m_controller->notifyExtensionsChanged(QStringLiteral("good:general"));
        QCOMPARE(extensionIds(model), (QStringList{QStringLiteral("extender:b"), QStringLiteral("extender:a")}));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY2(a, "the frame of a section that moved was rebuilt");
        QVERIFY2(b, "the frame of a section that moved was rebuilt");
    }

    void anIdListedTwiceThenOnceLeavesOneRow()
    {
        m_listed = {section(QStringLiteral("extender:a"), QStringLiteral("Section.qml")),
                    section(QStringLiteral("extender:a"), QStringLiteral("Section.qml"))};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        auto* const model = slot->findChild<FacetSlotModel*>();
        QVERIFY(model);
        QAbstractItemModelTester tester(model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QCOMPARE(slot->property("count").toInt(), 2);

        m_listed = {m_listed.first()};
        m_controller->notifyExtensionsChanged(QStringLiteral("good:general"));
        QCOMPARE(slot->property("count").toInt(), 1);
        QCOMPARE(extensionIds(model), QStringList{QStringLiteral("extender:a")});
    }

    void aSlotListsWhatItsSettingsAppLists()
    {
        m_listed = {section(QStringLiteral("extender:ok"), QStringLiteral("Section.qml"))};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        QCOMPARE(slot->property("count").toInt(), 1);

        // Another app, which has no provider and so lists nothing.
        ApplicationController other;
        QVERIFY(slot->setProperty("settingsApp", QVariant::fromValue<QObject*>(&other)));
        QCOMPARE(slot->property("count").toInt(), 0);
        QVERIFY(!slot->isVisible());
        QVERIFY(slot->setProperty("settingsApp", QVariant::fromValue<QObject*>(m_controller.get())));
        QCOMPARE(slot->property("count").toInt(), 1);
        QVERIFY(slot->isVisible());
        // No app at all. The property is typed ApplicationController, so the
        // null goes in as that type: a null QObject* does not convert to it.
        QVERIFY(slot->setProperty("settingsApp", QVariant::fromValue<ApplicationController*>(nullptr)));
        QCOMPARE(slot->property("count").toInt(), 0);
        QVERIFY(!slot->isVisible());
    }

    void aSlotWhoseSettingsAppIsDestroyedEmpties()
    {
        m_listed = {section(QStringLiteral("extender:ok"), QStringLiteral("Section.qml"))};
        auto other = std::make_unique<ApplicationController>();
        QVERIFY(other->setExtensionProvider(other.get(), [this](const QString& pageId, const QString& slot) {
            return pageId == QLatin1String("good:general") && slot == QLatin1String("top") ? m_listed : QVariantList();
        }));
        QQuickItem* const slot = createSlot(other.get());
        QVERIFY(slot);
        QCOMPARE(slot->property("count").toInt(), 1);

        other.reset();
        QCOMPARE(slot->property("count").toInt(), 0);
        QVERIFY(!slot->isVisible());
    }

private:
    void writeQml(const QString& name, const QByteArray& body)
    {
        QFile file(m_dir.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(body);
    }

    QString qml(const QString& name) const
    {
        return m_dir.filePath(name);
    }

    QVariantMap section(const QString& id, const QString& file)
    {
        return section(id, file, &m_domain);
    }

    // A section of a loaded module, built from @p file with @p domain.
    QVariantMap section(const QString& id, const QString& file, QObject* domain)
    {
        QVariantMap map = entry(id);
        map.insert(QStringLiteral("qmlSource"), QUrl::fromLocalFile(qml(file)));
        map.insert(QStringLiteral("domain"), QVariant::fromValue<QObject*>(domain));
        return map;
    }

    // A section whose module did not load.
    static QVariantMap failed(const QString& id, const QString& reason)
    {
        QVariantMap map = entry(id);
        map.insert(QStringLiteral("error"), reason);
        map.insert(QStringLiteral("moduleFailed"), true);
        return map;
    }

    // A new engine for the next root, replacing the last one, with buildLog
    // in its context for the section files that count their builds.
    QQmlEngine* makeEngine()
    {
        m_root.reset();
        m_engine = std::make_unique<QQmlEngine>();
        m_engine->rootContext()->setContextProperty(QStringLiteral("buildLog"), &m_buildLog);
        return m_engine.get();
    }

    // Puts @p root on the test's page, as a slot or frame is on a real one:
    // Qt works out a parentless item's visibility only until it is first
    // hidden. Null when @p root is not an item.
    QQuickItem* placeOnPage(QObject* root)
    {
        auto* const item = qobject_cast<QQuickItem*>(root);
        if (item) {
            item->setParentItem(&m_page);
        }
        return item;
    }

    QQuickItem* createSlot(ApplicationController* settingsApp)
    {
        QQmlComponent component(makeEngine());
        component.setData("import org.phosphor.control\nFacetSlot {}\n", QUrl());
        m_root.reset(component.createWithInitialProperties(
            {{QStringLiteral("settingsApp"), QVariant::fromValue<QObject*>(settingsApp)},
             {QStringLiteral("pageId"), QStringLiteral("good:general")},
             {QStringLiteral("slot"), QStringLiteral("top")}}));
        if (!m_root) {
            qWarning().noquote() << component.errorString();
            return nullptr;
        }
        return placeOnPage(m_root.get());
    }

    // A FacetExtensionFrame of a section of "Extender" whose module page is
    // extender:own, created with @p properties over a loaded section's
    // defaults, as FacetSlot creates one from a row.
    QQuickItem* createFrame(const QVariantMap& properties)
    {
        QVariantMap all{
            {QStringLiteral("moduleName"), QStringLiteral("Extender")},
            {QStringLiteral("modulePageId"), QStringLiteral("extender:own")},
            {QStringLiteral("disabledAfterCrash"), false},
            {QStringLiteral("settingsApp"), QVariant::fromValue<QObject*>(m_controller.get())},
        };
        all.insert(properties);
        QQmlComponent component(makeEngine());
        component.setData("import org.phosphor.control\nFacetExtensionFrame {}\n", QUrl());
        m_root.reset(component.createWithInitialProperties(all));
        if (!m_root) {
            qWarning().noquote() << component.errorString();
            return nullptr;
        }
        return placeOnPage(m_root.get());
    }

    QTemporaryDir m_dir;
    QObject m_domain;
    QObject m_otherDomain;
    BuildLog m_buildLog;
    QVariantList m_listed;
    QQuickItem m_page;
    std::unique_ptr<ApplicationController> m_controller;
    std::unique_ptr<QQmlEngine> m_engine;
    // The slot, frame or page a test created, on m_page.
    std::unique_ptr<QObject> m_root;
};

QTEST_MAIN(TestFacetSlot)
#include "test_facet_slot.moc"
