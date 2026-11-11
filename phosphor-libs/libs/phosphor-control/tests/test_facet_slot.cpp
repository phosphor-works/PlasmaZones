// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// FacetSlot and FacetExtensionFrame against a stubbed extension provider, so
// the QML is exercised without loading any module.

#include <PhosphorControl/ApplicationController.h>
#include <PhosphorControl/PageController.h>

#include <QFile>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <memory>

using namespace PhosphorControl;

namespace {

class SimplePage : public PageController
{
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

QVariantMap entry(const QString& id)
{
    return {{QStringLiteral("id"), id},
            {QStringLiteral("moduleId"), QStringLiteral("extender")},
            {QStringLiteral("moduleName"), QStringLiteral("Extender")},
            {QStringLiteral("name"), id},
            {QStringLiteral("modulePageId"), QStringLiteral("extender:own")}};
}

// The visual tree: a Repeater's delegates and a Loader's item are visual
// children of their parent item, which is what this walks.
QList<QQuickItem*> descendants(QQuickItem* item)
{
    QList<QQuickItem*> all;
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

// The item a frame's Loader created.
QQuickItem* loadedSection(QQuickItem* frame)
{
    QQuickItem* const loader = findItem(frame, QStringLiteral("facet-extension-section"));
    return loader ? loader->property("item").value<QQuickItem*>() : nullptr;
}

bool shown(QQuickItem* frame, const QString& part)
{
    QQuickItem* const item = findItem(frame, part);
    return item && item->isVisible();
}

} // namespace

// Counts the sections built, whatever domain each got, so a section built and
// thrown away during setup shows up.
class BuildLog : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int builds MEMBER builds NOTIFY buildsChanged)
public:
    int builds = 0;
Q_SIGNALS:
    void buildsChanged();
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
        m_slot.reset();
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
        // PageHost hands a page its settingsApp only once the page is built,
        // so a FacetSlot bound to page.settingsApp starts without one.
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
        QCOMPARE(loadedSection(frame)->property("seenAtCompletion").value<QObject*>(), &m_domain);
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
        QCOMPARE(loadedSection(frame)->property("seenAtCompletion").value<QObject*>(), &m_domain);

        // Same section and QML, another domain.
        QVariantMap moved = m_listed.first().toMap();
        moved.insert(QStringLiteral("domain"), QVariant::fromValue<QObject*>(&m_otherDomain));
        m_listed = {moved};
        m_controller->notifyExtensionsChanged(QStringLiteral("good:general"));
        QTRY_COMPARE(loadedSection(frame)->property("seenAtCompletion").value<QObject*>(), &m_otherDomain);
    }

    void aSectionThatNavigatesWhileBuiltLeavesAConsistentSlot()
    {
        // As PageHost's slots do, this one follows the current page. The
        // first section moves it while the slot is still adding the rest.
        m_controller->setCurrentPageId(QStringLiteral("good:general"));
        m_engine = std::make_unique<QQmlEngine>();
        m_engine->rootContext()->setContextProperty(QStringLiteral("buildLog"), &m_buildLog);
        m_engine->rootContext()->setContextProperty(QStringLiteral("appController"), m_controller.get());
        QQmlComponent component(m_engine.get());
        component.setData(
            "import org.phosphor.control\n"
            "FacetSlot { pageId: settingsApp ? settingsApp.currentPageId : \"\" }\n",
            QUrl());
        m_slot.reset(component.createWithInitialProperties(
            {{QStringLiteral("settingsApp"), QVariant::fromValue<QObject*>(m_controller.get())},
             {QStringLiteral("slot"), QStringLiteral("top")}}));
        QVERIFY2(m_slot, qPrintable(component.errorString()));
        auto* const slot = qobject_cast<QQuickItem*>(m_slot.get());
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
        // The frame of a section whose module failed, then loads: three of
        // its properties change one at a time.
        QFETCH(QStringList, order);
        m_engine = std::make_unique<QQmlEngine>();
        m_engine->rootContext()->setContextProperty(QStringLiteral("buildLog"), &m_buildLog);
        QQmlComponent component(m_engine.get());
        component.setData("import org.phosphor.control\nFacetExtensionFrame {}\n", QUrl());
        m_slot.reset(component.createWithInitialProperties({
            {QStringLiteral("extensionId"), QStringLiteral("extender:order")},
            {QStringLiteral("moduleName"), QStringLiteral("Extender")},
            {QStringLiteral("modulePageId"), QStringLiteral("extender:own")},
            {QStringLiteral("qmlSource"), QUrl()},
            {QStringLiteral("domain"), QVariant::fromValue<QObject*>(nullptr)},
            {QStringLiteral("failed"), true},
            {QStringLiteral("moduleFailed"), true},
            {QStringLiteral("disabledAfterCrash"), false},
            {QStringLiteral("error"), QStringLiteral("it did not load")},
            {QStringLiteral("settingsApp"), QVariant::fromValue<QObject*>(m_controller.get())},
        }));
        QVERIFY2(m_slot, qPrintable(component.errorString()));
        auto* const frame = qobject_cast<QQuickItem*>(m_slot.get());
        frame->setParentItem(&m_page);
        QVERIFY(!loadedSection(frame));

        const QVariantMap next{{QStringLiteral("failed"), false},
                               {QStringLiteral("qmlSource"), QUrl::fromLocalFile(qml(QStringLiteral("Required.qml")))},
                               {QStringLiteral("domain"), QVariant::fromValue<QObject*>(&m_domain)}};
        for (const QString& property : std::as_const(order)) {
            QVERIFY(frame->setProperty(qPrintable(property), next.value(property)));
        }
        frame->setProperty("moduleFailed", false);
        frame->setProperty("error", QString());
        QTRY_VERIFY(loadedSection(frame));
        QCoreApplication::processEvents();
        QCOMPARE(loadedSection(frame)->property("seenAtCompletion").value<QObject*>(), &m_domain);
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
        m_engine = std::make_unique<QQmlEngine>();
        m_engine->rootContext()->setContextProperty(QStringLiteral("buildLog"), &m_buildLog);
        QQmlComponent component(m_engine.get());
        component.setData("import org.phosphor.control\nFacetExtensionFrame {}\n", QUrl());
        m_slot.reset(component.createWithInitialProperties({
            {QStringLiteral("extensionId"), QStringLiteral("extender:moves")},
            {QStringLiteral("moduleName"), QStringLiteral("Extender")},
            {QStringLiteral("modulePageId"), QStringLiteral("extender:own")},
            {QStringLiteral("qmlSource"), QUrl::fromLocalFile(qml(QStringLiteral("Required.qml")))},
            {QStringLiteral("domain"), QVariant::fromValue<QObject*>(&m_domain)},
            {QStringLiteral("failed"), false},
            {QStringLiteral("moduleFailed"), false},
            {QStringLiteral("disabledAfterCrash"), false},
            {QStringLiteral("error"), QString()},
            {QStringLiteral("settingsApp"), QVariant::fromValue<QObject*>(m_controller.get())},
        }));
        QVERIFY2(m_slot, qPrintable(component.errorString()));
        auto* const frame = qobject_cast<QQuickItem*>(m_slot.get());
        frame->setParentItem(&m_page);
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
        QTRY_COMPARE(loadedSection(frame)->property("seenAtCompletion").value<QObject*>(), &m_otherDomain);
        QCoreApplication::processEvents();
        QCOMPARE(m_buildLog.builds, 2);
    }

    void aSectionWhoseDomainIsGoneIsNotBuilt()
    {
        // A provider that has not re-listed it yet: shown as gone, and no
        // section is built without its domain.
        m_listed = {section(QStringLiteral("extender:orphan"), QStringLiteral("Required.qml"))};
        auto domain = std::make_unique<QObject>();
        m_listed[0] = [&] {
            QVariantMap map = m_listed.first().toMap();
            map.insert(QStringLiteral("domain"), QVariant::fromValue<QObject*>(domain.get()));
            return map;
        }();
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        QQuickItem* const frame = findItem(slot, QStringLiteral("facet-extension:extender:orphan"));
        QVERIFY(frame);
        QVERIFY(loadedSection(frame));
        QCOMPARE(m_buildLog.builds, 1);

        domain.reset();
        QTRY_VERIFY(shown(frame, QStringLiteral("facet-extension-error")));
        QVERIFY(!shown(frame, QStringLiteral("facet-extension-section")));
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
        QVERIFY(!card->property("text").toString().contains(QStringLiteral("did not load")));
        QObject* const open = frame->findChild<QObject*>(QStringLiteral("facet-extension-open-module"));
        QVERIFY(open);
        QVERIFY(!open->property("visible").toBool());
    }

    void aSectionWhoseQmlFailsShowsTheCard()
    {
        m_listed = {section(QStringLiteral("extender:bad-qml"), QStringLiteral("Broken.qml"))};
        QQuickItem* const slot = createSlot(m_controller.get());
        QVERIFY(slot);
        QQuickItem* const frame = findItem(slot, QStringLiteral("facet-extension:extender:bad-qml"));
        QVERIFY(frame);
        QVERIFY(!loadedSection(frame));
        QVERIFY(shown(frame, QStringLiteral("facet-extension-error")));
        // Nothing to report from the host: the module loaded.
        QVERIFY(!shown(frame, QStringLiteral("facet-extension-reason")));
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
        QObject* const open = frame->findChild<QObject*>(QStringLiteral("facet-extension-open-module"));
        QVERIFY(open);
        QVERIFY(open->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(open, "trigger"));
        QCOMPARE(m_controller->currentPageId(), QStringLiteral("extender:own"));
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
        QVariantMap map = entry(id);
        map.insert(QStringLiteral("qmlSource"), QUrl::fromLocalFile(qml(file)));
        map.insert(QStringLiteral("domain"), QVariant::fromValue<QObject*>(&m_domain));
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

    QQuickItem* createSlot(ApplicationController* settingsApp)
    {
        m_engine = std::make_unique<QQmlEngine>();
        m_engine->rootContext()->setContextProperty(QStringLiteral("buildLog"), &m_buildLog);
        QQmlComponent component(m_engine.get());
        component.setData("import org.phosphor.control\nFacetSlot {}\n", QUrl());
        m_slot.reset(component.createWithInitialProperties(
            {{QStringLiteral("settingsApp"), QVariant::fromValue<QObject*>(settingsApp)},
             {QStringLiteral("pageId"), QStringLiteral("good:general")},
             {QStringLiteral("slot"), QStringLiteral("top")}}));
        if (!m_slot) {
            qWarning().noquote() << component.errorString();
            return nullptr;
        }
        // In a parent, as on a page: Qt works out a parentless item's
        // visibility only until it is first hidden.
        auto* const slot = qobject_cast<QQuickItem*>(m_slot.get());
        if (slot) {
            slot->setParentItem(&m_page);
        }
        return slot;
    }

    QTemporaryDir m_dir;
    QObject m_domain;
    QObject m_otherDomain;
    BuildLog m_buildLog;
    QVariantList m_listed;
    QQuickItem m_page;
    std::unique_ptr<ApplicationController> m_controller;
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<QObject> m_slot;
};

QTEST_MAIN(TestFacetSlot)
#include "test_facet_slot.moc"
