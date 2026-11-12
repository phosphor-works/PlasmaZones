// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorControl/FacetModuleMetadata.h>

#include <QDebug>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QScopeGuard>
#include <QTest>
#include <QThread>

#include <memory>

using namespace PhosphorControl;

namespace {

// Parse @p text as JSON, failing the calling test on a syntax error, so a
// typo in a row cannot turn its JSON into {} and pass on some other error.
bool json(const char* text, QJsonObject* out)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray(text), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        qWarning() << "test JSON does not parse:" << error.errorString() << "at" << error.offset;
        return false;
    }
    *out = document.object();
    return true;
}

} // namespace

class TestFacetModuleMetadata : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesAModule()
    {
        QJsonObject object;
        QVERIFY(json(R"({
            "Id": "placement", "ApiVersion": 1, "Name": "Window Placement", "Description": "Snap, tile, scroll",
            "Icon": "org.phosphor_works.PlasmaZones", "Category": "window-management", "Order": 10,
            "Keywords": ["snap", "tile"], "TranslationDomain": "facet-module-placement",
            "Categories": [ { "Id": "accessibility", "Name": "Accessibility", "Icon": "a11y", "Order": 70 } ],
            "Pages": [ { "Id": "snapping", "Name": "Snapping", "Icon": "snap", "Keywords": ["drag"],
                         "Visibility": "AdvancedOnly", "DividerAfter": true, "Counterpart": "other:page",
                         "Anchors": ["A.b_c-d", "9"] },
                       { "Id": "pinned", "Name": "Pinned", "Category": "appearance" } ] })",
                     &object));
        QStringList errors;
        QStringList warnings;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, QLocale::c(), &errors, &warnings);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.id, QStringLiteral("placement"));
        QCOMPARE(metadata.apiVersion, 1);
        QCOMPARE(metadata.name, QStringLiteral("Window Placement"));
        QCOMPARE(metadata.description, QStringLiteral("Snap, tile, scroll"));
        QCOMPARE(metadata.icon, QStringLiteral("org.phosphor_works.PlasmaZones"));
        QCOMPARE(metadata.category, QStringLiteral("window-management"));
        QCOMPARE(metadata.order, 10);
        QCOMPARE(metadata.keywords, (QStringList{QStringLiteral("snap"), QStringLiteral("tile")}));
        QCOMPARE(metadata.translationDomain, QStringLiteral("facet-module-placement"));
        QCOMPARE(metadata.categories.size(), 1);
        QCOMPARE(metadata.categories.first().id, QStringLiteral("accessibility"));
        QCOMPARE(metadata.categories.first().name, QStringLiteral("Accessibility"));
        QCOMPARE(metadata.categories.first().icon, QStringLiteral("a11y"));
        QCOMPARE(metadata.categories.first().order, 70);
        QCOMPARE(metadata.pages.size(), 2);
        const FacetPageMetadata* const snapping = metadata.page(QStringLiteral("snapping"));
        QVERIFY(snapping);
        QCOMPARE(snapping->icon, QStringLiteral("snap"));
        QCOMPARE(snapping->keywords, QStringList{QStringLiteral("drag")});
        QCOMPARE(snapping->visibility, PageRegistry::PageVisibility::AdvancedOnly);
        QVERIFY(snapping->dividerAfter);
        QCOMPARE(snapping->counterpart, QStringLiteral("other:page"));
        QCOMPARE(snapping->anchors, (QStringList{QStringLiteral("A.b_c-d"), QStringLiteral("9")}));
        const FacetPageMetadata* const pinned = metadata.page(QStringLiteral("pinned"));
        QVERIFY(pinned);
        QCOMPARE(pinned->category, QStringLiteral("appearance"));
        QVERIFY(!metadata.page(QStringLiteral("nope")));
    }

    void keepsTabsAndNewlinesInText()
    {
        QJsonObject object;
        QVERIFY(json(R"({ "Id": "m", "Name": "M", "Description": "one\ttwo\nthree",
                          "Pages": [ { "Id": "p", "Name": "P" } ] })",
                     &object));
        QStringList errors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.description, QStringLiteral("one\ttwo\nthree"));
    }

    void picksTheBestTranslation_data()
    {
        QTest::addColumn<QString>("locale");
        QTest::addColumn<QString>("expected");
        QTest::newRow("full locale") << QStringLiteral("pt_BR") << QStringLiteral("Brasil");
        QTest::newRow("language only") << QStringLiteral("pt_PT") << QStringLiteral("Portugal");
        QTest::newRow("untranslated") << QStringLiteral("fr_FR") << QStringLiteral("English");
        // An empty translation is no translation: it must not hide the
        // English name, which would make the whole module invalid in that
        // locale only.
        QTest::newRow("empty translation") << QStringLiteral("de_DE") << QStringLiteral("English");
        // So is one of the wrong type, which would otherwise read as no name
        // and refuse the module in that locale only.
        QTest::newRow("translation not a string") << QStringLiteral("es_ES") << QStringLiteral("English");
        QTest::newRow("translation is a list") << QStringLiteral("it_IT") << QStringLiteral("English");
    }

    void picksTheBestTranslation()
    {
        QFETCH(QString, locale);
        QFETCH(QString, expected);
        QJsonObject object;
        QVERIFY(json(R"({ "Id": "m", "Name": "English", "Name[pt]": "Portugal", "Name[pt_BR]": "Brasil",
                          "Name[de]": "", "Name[es]": 7, "Name[it]": ["Italia"],
                          "Pages": [ { "Id": "p", "Name": "P" } ] })",
                     &object));
        QStringList errors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, QLocale(locale), &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.name, expected);
    }

    void fallsBackToTheLanguageForEveryLocalizedKey()
    {
        // A regional locale with no regional translation gets the
        // language's, for every localizable key at every level.
        QJsonObject object;
        QVERIFY(json(R"({ "Id": "m", "Name": "English", "Name[fr]": "Français",
                          "Description": "D", "Description[fr]": "Dé", "Keywords": ["k"], "Keywords[fr]": ["clé"],
                          "Categories": [ { "Id": "c", "Name": "C", "Name[fr]": "Cé" } ],
                          "Pages": [ { "Id": "p", "Name": "P", "Name[fr]": "Page", "Keywords[fr]": ["mot"],
                                       "Anchors": ["a"] } ],
                          "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "Name": "E",
                                            "Name[fr]": "Extension", "Keywords": ["x"], "Keywords[fr]": ["ix"] } ],
                          "Search": [ { "Page": "p", "Anchor": "a", "Title": "T", "Title[fr]": "Titre",
                                        "Keywords[fr]": ["terme"] } ] })",
                     &object));
        const QLocale locale(QLocale::French, QLocale::Switzerland);
        QStringList errors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, locale, &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.name, QStringLiteral("Français"));
        QCOMPARE(metadata.description, QStringLiteral("Dé"));
        QCOMPARE(metadata.keywords, QStringList{QStringLiteral("clé")});
        QCOMPARE(metadata.categories.first().name, QStringLiteral("Cé"));
        QCOMPARE(metadata.pages.first().name, QStringLiteral("Page"));
        QCOMPARE(metadata.pages.first().keywords, QStringList{QStringLiteral("mot")});
        QCOMPARE(metadata.extensions.first().name, QStringLiteral("Extension"));
        QCOMPARE(metadata.extensions.first().keywords, QStringList{QStringLiteral("ix")});
        QCOMPARE(metadata.search.first().title, QStringLiteral("Titre"));
        QCOMPARE(metadata.search.first().keywords, QStringList{QStringLiteral("terme")});
    }

    void keepsTheBaseKeywordsWhenATranslationIsUnusable_data()
    {
        QTest::addColumn<QString>("locale");
        QTest::newRow("translation not a list") << QStringLiteral("de_DE");
        QTest::newRow("translation lists only empty strings") << QStringLiteral("fr_FR");
    }

    void keepsTheBaseKeywordsWhenATranslationIsUnusable()
    {
        QFETCH(QString, locale);
        QJsonObject object;
        QVERIFY(json(R"({ "Id": "m", "Name": "M", "Keywords": ["base"], "Keywords[de]": "einzeln",
                          "Keywords[fr]": [""], "Pages": [ { "Id": "p", "Name": "P" } ] })",
                     &object));
        QStringList errors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, QLocale(locale), &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.keywords, QStringList{QStringLiteral("base")});
    }

    void requiresTheBaseValueInEveryLocale()
    {
        // Only translated, so it is missing in the build's C locale. It must
        // be missing in German too, or the module would load in one language
        // and be refused in another.
        QJsonObject object;
        QVERIFY(json(R"({ "Id": "m", "Name[de]": "M",
                          "Categories": [ { "Id": "c", "Name[de]": "C" } ],
                          "Pages": [ { "Id": "p", "Name": "", "Name[de]": "P", "Anchors": ["a"] },
                                     { "Id": "q", "Name": "Q", "Anchors": ["a"] } ],
                          "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "Name[de]": "E" } ],
                          "Search": [ { "Page": "q", "Anchor": "a", "Title[de]": "T" } ] })",
                     &object));
        const QStringList expected{
            QStringLiteral("\"Name\" is missing"),
            QStringLiteral("category c has no \"Name\""),
            QStringLiteral("page p has no \"Name\""),
            QStringLiteral("extension e has no \"Name\""),
            QStringLiteral("search entry q#a has no \"Title\""),
        };
        for (const QLocale& locale : {QLocale::c(), QLocale(QLocale::German, QLocale::Germany)}) {
            QStringList errors;
            FacetModuleMetadata::parse(object, locale, &errors);
            QCOMPARE(errors, expected);
        }
    }

    void ordersParentsFirstAndKeepsSiblingOrder()
    {
        QJsonObject object;
        QVERIFY(json(R"({
            "Id": "m", "Name": "M", "Pages": [
                { "Id": "a/b/c", "Name": "C", "Parent": "a/b" },
                { "Id": "a/b", "Name": "B", "Parent": "a" },
                { "Id": "z", "Name": "Z" },
                { "Id": "a", "Name": "A" } ] })",
                     &object));
        QStringList errors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QStringList order;
        for (const FacetPageMetadata& page : metadata.pages) {
            order.append(page.id);
        }
        // z was declared before a, so it stays first among the top-level
        // pages; a's subtree follows a.
        QCOMPARE(
            order,
            (QStringList{QStringLiteral("z"), QStringLiteral("a"), QStringLiteral("a/b"), QStringLiteral("a/b/c")}));
    }

    void placesALongChainWithoutRecursion()
    {
        QString text = QStringLiteral(R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p0", "Name": "P" })");
        for (int i = 1; i < 5000; ++i) {
            text += QStringLiteral(R"(, { "Id": "p%1", "Name": "P", "Parent": "p%2" })").arg(i).arg(i - 1);
        }
        text += QStringLiteral(" ] }");
        QJsonObject object;
        QVERIFY(json(text.toUtf8().constData(), &object));
        // On a thread with a small stack, so a walk that recursed once per
        // level would overflow it and crash the test instead of passing on
        // the main thread's megabytes.
        // The thread works on its own copies, which the shared pointers keep
        // alive should the wait time out and the test return while it runs.
        struct Result
        {
            QStringList errors;
            FacetModuleMetadata metadata;
        };
        const auto result = std::make_shared<Result>();
        std::unique_ptr<QThread> thread(QThread::create([result, object] {
            result->metadata = FacetModuleMetadata::parse(object, QLocale::c(), &result->errors);
        }));
        thread->setStackSize(256 * 1024);
        thread->start();
        // Destroying a QThread that still runs aborts the process, so one
        // that overran the wait is stopped first and the test fails instead.
        auto stop = qScopeGuard([&thread] {
            if (thread->isRunning()) {
                thread->terminate();
                thread->wait();
            }
        });
        QVERIFY(thread->wait(QDeadlineTimer(60000)));
        stop.dismiss();
        const QStringList& errors = result->errors;
        const FacetModuleMetadata& metadata = result->metadata;
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.pages.size(), 5000);
        QCOMPARE(metadata.pages.last().id, QStringLiteral("p4999"));
    }

    void acceptsACrossModuleParent()
    {
        QJsonObject object;
        QVERIFY(json(R"({ "Id": "m", "Name": "M",
            "Pages": [ { "Id": "p", "Name": "P", "Parent": "placement:snapping/behavior" } ] })",
                     &object));
        QStringList errors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.pages.size(), 1);
        QCOMPARE(metadata.pages.first().parent, QStringLiteral("placement:snapping/behavior"));
    }

    void warnsAboutKeysItCannotUse_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<QStringList>("expected");
        QTest::addColumn<int>("entries");
        const auto row = [](const char* name, const char* text, const QStringList& expected, int entries = 0) {
            QTest::newRow(name) << QByteArray(text) << expected << entries;
        };
        row("unknown keys at every level", R"({ "Id": "m", "Name": "M", "Name[de]": "M", "Plugins": [],
            "Categories": [ { "Id": "c", "Name": "C", "Colour": "red" } ],
            "Pages": [ { "Id": "p", "Name": "P", "Parnet": "q", "Anchors": ["a"] } ],
            "ExtensionPoints": [ { "Page": "p", "Id": "panel", "Versoin": 2 } ],
            "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "Name": "E", "Slto": "x" } ],
            "Search": [ { "Page": "p", "Anchor": "a", "Title": "T", "Titel": "U" } ] })",
            {QStringLiteral("unknown key \"Plugins\""), QStringLiteral("category c: unknown key \"Colour\""),
             QStringLiteral("page p: unknown key \"Parnet\""),
             QStringLiteral("extension point panel on page p: unknown key \"Versoin\""),
             QStringLiteral("extension e: unknown key \"Slto\""),
             QStringLiteral("search entry p#a: unknown key \"Titel\"")},
            1);
        // Only the localizable keys take a [<locale>].
        row("translation of a key that has none", R"({ "Id": "m", "Name": "M", "Icon[de]": "i",
            "Pages": [ { "Id": "p", "Name": "P", "Icon[de]": "i" } ],
            "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "Name": "E", "Target[de]": "o:q" } ] })",
            {QStringLiteral("unknown key \"Icon[de]\""), QStringLiteral("page p: unknown key \"Icon[de]\""),
             QStringLiteral("extension e: unknown key \"Target[de]\"")});
        // parse() matches `pt_BR`, never `pt-BR`, and never an empty locale.
        row("translation for a locale never picked",
            R"({ "Id": "m", "Name": "M", "Name[pt-BR]": "B", "Name[]": "E",
                 "Pages": [ { "Id": "p", "Name": "P", "Keywords[de-AT]": ["k"] } ] })",
            {QStringLiteral("\"Name[]\" is never picked: a locale is written with letters, digits and '_', as in "
                            "Name[pt_BR]"),
             QStringLiteral("\"Name[pt-BR]\" is never picked: a locale is written with letters, digits and '_', as "
                            "in Name[pt_BR]"),
             QStringLiteral("page p: \"Keywords[de-AT]\" is never picked: a locale is written with letters, digits "
                            "and '_', as in Keywords[pt_BR]")});
        // A kind a newer Facet added costs the entry, not the module.
        row("search entry of an unknown kind",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p", "Anchor": "a", "Kind": "Thing", "Title": "T" } ] })",
            {QStringLiteral("search entry p#a has \"Kind\" Thing, which this Facet does not know, so it is left out")});
    }

    void warnsAboutKeysItCannotUse()
    {
        QFETCH(QByteArray, text);
        QFETCH(QStringList, expected);
        QFETCH(int, entries);
        QJsonObject object;
        QVERIFY(json(text.constData(), &object));
        QStringList errors;
        QStringList warnings;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, QLocale::c(), &errors, &warnings);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(warnings, expected);
        QCOMPARE(metadata.search.size(), entries);
    }

    void parsesExtensionPointsAndExtensions()
    {
        QJsonObject object;
        QVERIFY(json(R"({
            "Id": "m", "Name": "M",
            "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["speed", "size"] } ],
            "ExtensionPoints": [ { "Page": "p", "Id": "panel", "Version": 3 } ],
            "Extensions": [ { "Id": "e", "Target": "other:page", "Slot": "after:x", "TargetVersion": 2,
                              "Name": "E", "Name[de]": "Ee", "Keywords": ["k"], "Order": 5 } ] })",
                     &object));
        QStringList errors;
        QStringList warnings;
        const FacetModuleMetadata metadata =
            FacetModuleMetadata::parse(object, QLocale(QStringLiteral("de_DE")), &errors, &warnings);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
        const FacetPageMetadata* const page = metadata.page(QStringLiteral("p"));
        QVERIFY(page);
        QCOMPARE(page->anchors, (QStringList{QStringLiteral("speed"), QStringLiteral("size")}));
        QCOMPARE(metadata.extensionPoints.size(), 1);
        QCOMPARE(metadata.extensionPoints.first().page, QStringLiteral("p"));
        QCOMPARE(metadata.extensionPoints.first().id, QStringLiteral("panel"));
        QCOMPARE(metadata.extensionPoints.first().version, 3);
        const FacetExtensionMetadata* const extension = metadata.extension(QStringLiteral("e"));
        QVERIFY(extension);
        QCOMPARE(extension->target, QStringLiteral("other:page"));
        QCOMPARE(extension->slot, QStringLiteral("after:x"));
        QCOMPARE(extension->targetVersion, 2);
        QCOMPARE(extension->name, QStringLiteral("Ee"));
        QCOMPARE(extension->keywords, QStringList{QStringLiteral("k")});
        QCOMPARE(extension->order, 5);
        QVERIFY(!metadata.extension(QStringLiteral("nope")));
    }

    void acceptsSlotsItDoesNotKnow()
    {
        // Whether a page offers a slot is the host's call, per extension. A
        // slot form a newer Facet adds must not cost the module its pages.
        QJsonObject object;
        QVERIFY(json(R"({
            "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
            "Extensions": [ { "Id": "a", "Target": "o:p", "Slot": "replace:x", "Name": "A" },
                            { "Id": "b", "Target": "o:p", "Slot": "after:", "Name": "B" },
                            { "Id": "c", "Target": "o:p", "Slot": "anchors", "Name": "C" } ] })",
                     &object));
        QStringList errors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.extensions.size(), 3);
    }

    void aPointIdIsPerPage()
    {
        QJsonObject object;
        QVERIFY(json(R"({
            "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" }, { "Id": "q", "Name": "Q" } ],
            "ExtensionPoints": [ { "Page": "p", "Id": "panel", "Version": 2 },
                                 { "Page": "q", "Id": "panel", "Version": 3 } ] })",
                     &object));
        QStringList errors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.extensionPoints.size(), 2);
        QCOMPARE(metadata.slotVersion(QStringLiteral("p"), QStringLiteral("panel")), 2);
        QCOMPARE(metadata.slotVersion(QStringLiteral("q"), QStringLiteral("panel")), 3);
    }

    void reportsANameOfTheWrongTypeAsSuch()
    {
        // The key is there, so "missing" would mislead.
        QJsonObject object;
        QVERIFY(json(R"({ "Id": "m", "Name": 5, "Pages": [ { "Id": "p", "Name": ["P"] } ] })", &object));
        QStringList errors;
        FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        QCOMPARE(errors,
                 (QStringList{QStringLiteral("\"Name\" must be a string"),
                              QStringLiteral("page p: \"Name\" must be a string")}));
    }

    void reportsAKeyOfTheWrongTypeOnce_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<QString>("expected");
        QTest::newRow("extension") << QByteArray(R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                          "Extensions": [ { "Id": "e", "Target": 3, "Slot": "top", "Name": "E" } ] })")
                                   << QStringLiteral("extension e: \"Target\" must be a string");
        // A search entry's Title or Anchor of the wrong type is reported
        // once too, and not also as "has no Title" or "names anchor": the
        // rows of reportsEveryProblem pin that, as they count the problems.
    }

    void reportsAKeyOfTheWrongTypeOnce()
    {
        QFETCH(QByteArray, text);
        QFETCH(QString, expected);
        QJsonObject object;
        QVERIFY(json(text.constData(), &object));
        QStringList errors;
        FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        QCOMPARE(errors, QStringList{expected});
    }

    void offersSlotsAtTheirVersion_data()
    {
        QTest::addColumn<QString>("page");
        QTest::addColumn<QString>("slot");
        QTest::addColumn<int>("version");
        QTest::newRow("top") << QStringLiteral("p") << QStringLiteral("top") << 4;
        QTest::newRow("bottom") << QStringLiteral("p") << QStringLiteral("bottom") << 4;
        QTest::newRow("before an anchor") << QStringLiteral("p") << QStringLiteral("before:speed") << 4;
        QTest::newRow("after an anchor") << QStringLiteral("p") << QStringLiteral("after:speed") << 4;
        QTest::newRow("unknown anchor") << QStringLiteral("p") << QStringLiteral("after:size") << 0;
        QTest::newRow("named point") << QStringLiteral("p") << QStringLiteral("panel") << 2;
        QTest::newRow("anchors is not a slot") << QStringLiteral("p") << QStringLiteral("anchors") << 0;
        QTest::newRow("unknown point") << QStringLiteral("p") << QStringLiteral("other") << 0;
        QTest::newRow("default anchors version") << QStringLiteral("q") << QStringLiteral("top") << 1;
        QTest::newRow("point on another page") << QStringLiteral("q") << QStringLiteral("panel") << 0;
        QTest::newRow("unknown page") << QStringLiteral("z") << QStringLiteral("top") << 0;
    }

    void offersSlotsAtTheirVersion()
    {
        QFETCH(QString, page);
        QFETCH(QString, slot);
        QFETCH(int, version);
        QJsonObject object;
        QVERIFY(json(R"({
            "Id": "m", "Name": "M",
            "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["speed"] }, { "Id": "q", "Name": "Q" } ],
            "ExtensionPoints": [ { "Page": "p", "Id": "anchors", "Version": 4 },
                                 { "Page": "p", "Id": "panel", "Version": 2 } ] })",
                     &object));
        QStringList errors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.slotVersion(page, slot), version);
    }

    void parsesSearchEntries()
    {
        QJsonObject object;
        QVERIFY(json(R"({
            "Id": "m", "Name": "M", "SearchProviders": true,
            "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["speed", "size"] },
                       { "Id": "q", "Name": "Q", "Anchors": ["speed"] } ],
            "Search": [ { "Page": "p", "Anchor": "speed", "Title": "Speed", "Title[de]": "Tempo",
                          "Keywords": ["fast"], "Keywords[de]": ["schnell"], "AdvancedOnly": true },
                        { "Page": "p", "Anchor": "size", "Kind": "Section", "Title": "Size" },
                        { "Page": "q", "Anchor": "speed", "Kind": "Setting", "Title": "Speed" } ] })",
                     &object));
        QStringList errors;
        QStringList warnings;
        const FacetModuleMetadata metadata =
            FacetModuleMetadata::parse(object, QLocale(QStringLiteral("de_DE")), &errors, &warnings);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
        QVERIFY(metadata.searchProviders);
        QCOMPARE(metadata.search.size(), 3);
        const FacetSearchEntryMetadata& speed = metadata.search.at(0);
        QCOMPARE(speed.page, QStringLiteral("p"));
        QCOMPARE(speed.anchor, QStringLiteral("speed"));
        QCOMPARE(speed.title, QStringLiteral("Tempo"));
        QCOMPARE(speed.keywords, QStringList{QStringLiteral("schnell")});
        QVERIFY(speed.advancedOnly);
        QVERIFY(!speed.section);
        const FacetSearchEntryMetadata& size = metadata.search.at(1);
        QCOMPARE(size.anchor, QStringLiteral("size"));
        QVERIFY(size.section);
        QVERIFY(!size.advancedOnly);
        // The same title at another place is another entry, and an explicit
        // Setting is what no Kind means.
        const FacetSearchEntryMetadata& other = metadata.search.at(2);
        QCOMPARE(other.page, QStringLiteral("q"));
        QVERIFY(!other.section);

        QVERIFY(json(R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ] })", &object));
        errors.clear();
        QVERIFY(!FacetModuleMetadata::parse(object, QLocale::c(), &errors).searchProviders);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    }

    void reportsEveryProblem_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<int>("count");
        const auto row = [](const char* name, const char* text, const QString& expected, int count = 1) {
            QTest::newRow(name) << QByteArray(text) << expected << count;
        };
        // Every row has one fault and a valid page list otherwise, so the
        // expected message is the only one it can match, and the count says
        // the fault is not reported again under another name.
        row("no id", R"({ "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ] })", QStringLiteral("\"Id\" must"));
        row("bad id", R"({ "Id": "a:b", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Id\" must"));
        row("id with a trailing newline", R"({ "Id": "m\n", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Id\" must"));
        row("no name", R"({ "Id": "m", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Name\" is missing"));
        row("api version not a whole number",
            R"({ "Id": "m", "ApiVersion": "1", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"ApiVersion\" must be a whole number"));
        row("order not a whole number",
            R"({ "Id": "m", "Name": "M", "Order": 1.5, "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Order\" must be a whole number"));
        row("bad category",
            R"({ "Id": "m", "Name": "M", "Category": "Bad Cat", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Category\" Bad Cat"));
        row("category with a trailing newline",
            R"({ "Id": "m", "Name": "M", "Category": "input\n", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Category\" input"));
        row("parent not a module id",
            R"({ "Id": "m", "Name": "M", "Parent": "a b", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("is not a module id"));
        row("own parent", R"({ "Id": "m", "Name": "M", "Parent": "m", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("names the module itself"));
        row("no pages", R"({ "Id": "m", "Name": "M" })", QStringLiteral("at least one page"));
        row("empty pages", R"({ "Id": "m", "Name": "M", "Pages": [] })", QStringLiteral("at least one page"));
        row("pages not a list", R"({ "Id": "m", "Name": "M", "Pages": { "Id": "p" } })",
            QStringLiteral("at least one page"));
        row("page with colon", R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a:b", "Name": "X" } ] })",
            QStringLiteral("page id a:b"));
        row("page with an empty segment", R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a//b", "Name": "X" } ] })",
            QStringLiteral("page id a//b"));
        row("page with a dot segment", R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a/../b", "Name": "X" } ] })",
            QStringLiteral("page id a/../b"));
        row("page with a trailing slash", R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a/", "Name": "X" } ] })",
            QStringLiteral("page id a/"));
        row("page with a trailing newline", R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a\n", "Name": "X" } ] })",
            QStringLiteral("page id a"));
        row("page without name", R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a" } ] })",
            QStringLiteral("page a has no \"Name\""));
        row("duplicate page",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" }, { "Id": "a", "Name": "B" } ] })",
            QStringLiteral("declared twice"));
        row("unknown local parent",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "Parent": "x" } ] })",
            QStringLiteral("does not declare"));
        row("malformed cross parent",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "Parent": ":x" } ] })",
            QStringLiteral("<module>:<page>"));
        row("cross parent with a trailing newline",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "Parent": "o:x\n" } ] })",
            QStringLiteral("<module>:<page>"));
        row("cross parent naming its own module",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" }, { "Id": "b", "Name": "B", "Parent": "m:a" } ] })",
            QStringLiteral("names its own module"));
        row("parent and category",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "Parent": "o:x", "Category": "input" } ] })",
            QStringLiteral("both \"Parent\" and \"Category\""));
        row("bad page category",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "Category": "Bad" } ] })",
            QStringLiteral("\"Category\" Bad"));
        row("bad visibility",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "Visibility": "Sometimes" } ] })",
            QStringLiteral("\"Visibility\" must be"));
        row("divider not a bool",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "DividerAfter": 1 } ] })",
            QStringLiteral("\"DividerAfter\" must be"));
        row("counterpart not declared",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "Counterpart": "x" } ] })",
            QStringLiteral("not another page of this module"));
        row("counterpart is itself",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "Counterpart": "a" } ] })",
            QStringLiteral("not another page of this module"));
        row("icon not a string", R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "Icon": 3 } ] })",
            QStringLiteral("\"Icon\" must be a string"));
        row("cycle", R"({ "Id": "m", "Name": "M", "Pages": [
                   { "Id": "a", "Name": "A", "Parent": "b" }, { "Id": "b", "Name": "B", "Parent": "a" } ] })",
            QStringLiteral("in or under a loop of local parents"), 2);
        row("category without name",
            R"({ "Id": "m", "Name": "M", "Categories": [ { "Id": "c" } ], "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("category c has no \"Name\""));
        row("bad category id",
            R"({ "Id": "m", "Name": "M", "Categories": [ { "Id": "C!", "Name": "C" } ], "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("category id C!"));
        row("category defined twice",
            R"({ "Id": "m", "Name": "M", "Categories": [ { "Id": "c", "Name": "C" }, { "Id": "c", "Name": "D" } ],
                 "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("category c is defined twice"));
        row("point on an undeclared page",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "ExtensionPoints": [ { "Page": "x", "Id": "e" } ] })",
            QStringLiteral("extension point e is on page x, which the module does not declare"));
        row("point named top",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" } ],
                 "ExtensionPoints": [ { "Page": "a", "Id": "top" } ] })",
            QStringLiteral("extension point id top must match"));
        row("point named bottom",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" } ],
                 "ExtensionPoints": [ { "Page": "a", "Id": "bottom" } ] })",
            QStringLiteral("extension point id bottom must match"));
        row("point without id",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" } ],
                 "ExtensionPoints": [ { "Page": "a" } ] })",
            QStringLiteral("extension point id  must match"));
        row("point without page",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" } ],
                 "ExtensionPoints": [ { "Id": "e" } ] })",
            QStringLiteral("extension point e is on page , which the module does not declare"));
        row("points not a list",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" } ],
                 "ExtensionPoints": { "Page": "a", "Id": "e" } })",
            QStringLiteral("\"ExtensionPoints\" must be a list"));
        row("page keywords not a list",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Keywords": "k" } ] })",
            QStringLiteral("page p: \"Keywords\" must be a list"));
        row("extension name not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "Name": ["E"] } ] })",
            QStringLiteral("extension e: \"Name\" must be a string"));
        row("categories not a list",
            R"({ "Id": "m", "Name": "M", "Categories": { "Id": "c", "Name": "C" },
                 "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Categories\" must be a list"));
        row("anchors not a list",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "Anchors": "speed" } ] })",
            QStringLiteral("page a: \"Anchors\" must be a list"));
        row("anchor not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "Anchors": ["speed", 3] } ] })",
            QStringLiteral("page a: every \"Anchors\" entry must be a non-empty string"));
        row("point version zero",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" } ],
                 "ExtensionPoints": [ { "Page": "a", "Id": "p", "Version": 0 } ] })",
            QStringLiteral("extension point p has version 0, which is below 1"));
        row("point version not a whole number",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" } ],
                 "ExtensionPoints": [ { "Page": "a", "Id": "p", "Version": "2" } ] })",
            QStringLiteral("\"Version\" must be a whole number"));
        row("point declared twice",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" } ],
                 "ExtensionPoints": [ { "Page": "a", "Id": "p" }, { "Page": "a", "Id": "p" } ] })",
            QStringLiteral("extension point p is declared twice"));
        row("extension without id",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Target": "o:p", "Slot": "top", "Name": "E" } ] })",
            QStringLiteral("extension id  must match"));
        row("extension declared twice",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "Name": "E" },
                                 { "Id": "e", "Target": "o:p", "Slot": "top", "Name": "E" } ] })",
            QStringLiteral("extension e is declared twice"));
        row("extension with a local target",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "p", "Slot": "top", "Name": "E" } ] })",
            QStringLiteral("which is not <module>:<page>"));
        row("extension target with a trailing newline",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p\n", "Slot": "top", "Name": "E" } ] })",
            QStringLiteral("which is not <module>:<page>"));
        row("extension on its own module",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "m:p", "Slot": "top", "Name": "E" } ] })",
            QStringLiteral("targets its own module"));
        row("extensions not a list",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": { "Id": "e", "Target": "o:p", "Slot": "top", "Name": "E" } })",
            QStringLiteral("\"Extensions\" must be a list"));
        row("extension without slot",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p", "Name": "E" } ] })",
            QStringLiteral("extension e has no \"Slot\""));
        row("extension slot not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": 3, "Name": "E" } ] })",
            QStringLiteral("extension e: \"Slot\" must be a string"));
        row("extension target version zero",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "TargetVersion": 0, "Name": "E" } ] })",
            QStringLiteral("TargetVersion 0"));
        row("extension target version negative",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "TargetVersion": -1, "Name": "E" } ] })",
            QStringLiteral("TargetVersion -1"));
        row("extension target version out of range",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "TargetVersion": 3000000000,
                                   "Name": "E" } ] })",
            QStringLiteral("extension e: \"TargetVersion\" must be a whole number"));
        row("extension target version not a number",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "TargetVersion": "2", "Name": "E" } ] })",
            QStringLiteral("extension e: \"TargetVersion\" must be a whole number"));
        row("extension order not a whole number",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "Order": 1.5, "Name": "E" } ] })",
            QStringLiteral("extension e: \"Order\" must be a whole number"));
        row("extension without name",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top" } ] })",
            QStringLiteral("extension e has no \"Name\""));
        row("anchor with a colon",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a:b"] } ] })",
            QStringLiteral("page p: anchor a:b must match [A-Za-z0-9][A-Za-z0-9._-]*"));
        row("anchor listed twice",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a", "a"] } ] })",
            QStringLiteral("page p: anchor a is listed twice"));
        row("search not a list",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": { "Page": "p", "Anchor": "a", "Title": "T" } })",
            QStringLiteral("\"Search\" must be a list"));
        row("search entry on an undeclared page",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Search": [ { "Page": "x", "Anchor": "a", "Title": "T" } ] })",
            QStringLiteral("search entry x#a is on page x"));
        row("search entry at an unlisted anchor",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p", "Anchor": "b", "Title": "T" } ] })",
            QStringLiteral("names anchor b"));
        row("search entry without a title",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p", "Anchor": "a" } ] })",
            QStringLiteral("p#a has no \"Title\""));
        row("search entry title not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p", "Anchor": "a", "Title": 3 } ] })",
            QStringLiteral("search entry p#a: \"Title\" must be a string"));
        row("search entry advanced flag not a boolean",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p", "Anchor": "a", "Title": "T", "AdvancedOnly": "yes" } ] })",
            QStringLiteral("\"AdvancedOnly\" must be true or false"));
        row("search providers not a boolean",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ], "SearchProviders": 1 })",
            QStringLiteral("\"SearchProviders\" must be true or false"));
        row("anchor starting with a dot",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": [".a"] } ] })",
            QStringLiteral("page p: anchor .a must match"));
        row("anchor starting with a dash",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["-a"] } ] })",
            QStringLiteral("page p: anchor -a must match"));
        row("anchor with a slash",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a/b"] } ] })",
            QStringLiteral("page p: anchor a/b must match"));
        row("anchor with a space",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a b"] } ] })",
            QStringLiteral("page p: anchor a b must match"));
        row("anchor with a trailing newline",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a\n"] } ] })",
            QStringLiteral("page p: anchor a\n must match"));
        row("empty anchor", R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": [""] } ] })",
            QStringLiteral("page p: every \"Anchors\" entry must be a non-empty string"));
        // Reported once, and the anchors after it are still read, so the
        // search entry naming one adds no problem of its own.
        row("anchor entries not strings",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": [1, "", "a"] } ],
                 "Search": [ { "Page": "p", "Anchor": "a", "Title": "T" } ] })",
            QStringLiteral("page p: every \"Anchors\" entry must be a non-empty string"));
        // The page stays, so what names the bad anchor adds nothing.
        row("search entry at a bad anchor",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a:b"] } ],
                 "Search": [ { "Page": "p", "Anchor": "a:b", "Title": "T" } ] })",
            QStringLiteral("page p: anchor a:b must match"));
        row("description not a string",
            R"({ "Id": "m", "Name": "M", "Description": 1, "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Description\" must be a string"));
        row("module icon not a string",
            R"({ "Id": "m", "Name": "M", "Icon": 1, "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Icon\" must be a string"));
        row("module category not a string",
            R"({ "Id": "m", "Name": "M", "Category": 1, "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Category\" must be a string"));
        row("module parent not a string",
            R"({ "Id": "m", "Name": "M", "Parent": 1, "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Parent\" must be a string"));
        row("module keywords not a list",
            R"({ "Id": "m", "Name": "M", "Keywords": "k", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Keywords\" must be a list"));
        row("translation domain not a string",
            R"({ "Id": "m", "Name": "M", "TranslationDomain": 1, "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"TranslationDomain\" must be a string"));
        row("module id not a string", R"({ "Id": 5, "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Id\" must be a string"));
        row("module keyword not a string",
            R"({ "Id": "m", "Name": "M", "Keywords": ["k", 3], "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("every \"Keywords\" entry must be a non-empty string"));
        row("empty page keyword",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Keywords": [""] } ] })",
            QStringLiteral("page p: every \"Keywords\" entry must be a non-empty string"));
        row("name with a carriage return", R"({ "Id": "m", "Name": "a\rb", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Name\" holds a character it may not: a control character other than tab and "
                           "newline, U+FFFE, U+FFFF or half of a surrogate pair"));
        row("keyword with a control character",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Keywords": ["a\u0001"] } ] })",
            QStringLiteral("page p: \"Keywords\" holds a character it may not"));
        row("title with a control character",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p", "Anchor": "a", "Title": "T\u001f" } ] })",
            QStringLiteral("search entry p#a: \"Title\" holds a character it may not"));
        // lupdate writes these into the catalog as they are, which XML
        // cannot hold.
        row("name with U+FFFE", R"({ "Id": "m", "Name": "a\ufffeb", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Name\" holds a character it may not"));
        row("keyword with U+FFFF",
            R"({ "Id": "m", "Name": "M", "Keywords": ["a\uffff"], "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Keywords\" holds a character it may not"));
        // A lone surrogate is U+FFFD by the time lupdate reads it.
        row("name with half a surrogate pair",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "a\ud83d" } ] })",
            QStringLiteral("page p: \"Name\" holds a character it may not"));
        row("name with the other half of a surrogate pair",
            R"({ "Id": "m", "Name": "\ude00a", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Name\" holds a character it may not"));
        row("halves of a surrogate pair the wrong way round",
            R"({ "Id": "m", "Name": "\ude00\ud83d", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Name\" holds a character it may not"));
        // A whole pair is one character.
        row("name with a character outside the BMP",
            R"({ "Id": "m", "Name": "a\ud83d\ude00", "Pages": [ { "Id": "p", "Name": "P" } ] })", QString(), 0);
        row("category id not a string",
            R"({ "Id": "m", "Name": "M", "Categories": [ { "Id": 1, "Name": "C" } ], "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("category: \"Id\" must be a string"));
        row("category icon not a string",
            R"({ "Id": "m", "Name": "M", "Categories": [ { "Id": "c", "Name": "C", "Icon": 1 } ],
                 "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("category c: \"Icon\" must be a string"));
        row("category order not a whole number",
            R"({ "Id": "m", "Name": "M", "Categories": [ { "Id": "c", "Name": "C", "Order": "1" } ],
                 "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("category c: \"Order\" must be a whole number"));
        row("category name with a control character",
            R"({ "Id": "m", "Name": "M", "Categories": [ { "Id": "c", "Name": "C\r" } ],
                 "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("category c: \"Name\" holds a character it may not"));
        row("page id not a string", R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": 5, "Name": "P" } ] })",
            QStringLiteral("page: \"Id\" must be a string"));
        row("page parent not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Parent": 1 } ] })",
            QStringLiteral("page p: \"Parent\" must be a string"));
        row("page category not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Category": 1 } ] })",
            QStringLiteral("page p: \"Category\" must be a string"));
        row("page counterpart not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Counterpart": 1 } ] })",
            QStringLiteral("page p: \"Counterpart\" must be a string"));
        row("visibility not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Visibility": 1 } ] })",
            QStringLiteral("page p: \"Visibility\" must be"));
        row("counterpart naming its own module",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" }, { "Id": "b", "Name": "B", "Counterpart": "m:a" } ] })",
            QStringLiteral("page b has counterpart m:a, which is neither a page of this module nor"));
        row("malformed cross-module counterpart",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A", "Counterpart": "o:" } ] })",
            QStringLiteral("page a has counterpart o:, which is neither a page of this module nor"));
        row("point page not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" } ],
                 "ExtensionPoints": [ { "Page": 1, "Id": "e" } ] })",
            QStringLiteral("extension point: \"Page\" must be a string"));
        row("point id not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" } ],
                 "ExtensionPoints": [ { "Page": "a", "Id": 1 } ] })",
            QStringLiteral("extension point: \"Id\" must be a string"));
        row("point id with a capital",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "a", "Name": "A" } ],
                 "ExtensionPoints": [ { "Page": "a", "Id": "Panel" } ] })",
            QStringLiteral("extension point id Panel must match"));
        row("extension id not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": 1, "Target": "o:p", "Slot": "top", "Name": "E" } ] })",
            QStringLiteral("extension: \"Id\" must be a string"));
        row("extension keywords not a list",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "Name": "E", "Keywords": "k" } ] })",
            QStringLiteral("extension e: \"Keywords\" must be a list"));
        row("extension keyword not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ],
                 "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "Name": "E", "Keywords": [1] } ] })",
            QStringLiteral("extension e: every \"Keywords\" entry must be a non-empty string"));
        row("page not an object", R"({ "Id": "m", "Name": "M", "Pages": [ 5, { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Pages\"[0] must be an object"));
        row("category not an object",
            R"({ "Id": "m", "Name": "M", "Categories": [ "c" ], "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Categories\"[0] must be an object"));
        row("point not an object",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ], "ExtensionPoints": [ [] ] })",
            QStringLiteral("\"ExtensionPoints\"[0] must be an object"));
        row("extension not an object",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ], "Extensions": [ null ] })",
            QStringLiteral("\"Extensions\"[0] must be an object"));
        row("search entry not an object",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P" } ], "Search": [ "p#a" ] })",
            QStringLiteral("\"Search\"[0] must be an object"));
        row("search entry page not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": 1, "Anchor": "a", "Title": "T" } ] })",
            QStringLiteral("search entry #a: \"Page\" must be a string"));
        row("search entry anchor not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p", "Anchor": 1, "Title": "T" } ] })",
            QStringLiteral("search entry p#: \"Anchor\" must be a string"));
        row("search entry kind not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p", "Anchor": "a", "Kind": 1, "Title": "T" } ] })",
            QStringLiteral("search entry p#a: \"Kind\" must be a string"));
        row("search entry keywords not a list",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p", "Anchor": "a", "Title": "T", "Keywords": "k" } ] })",
            QStringLiteral("search entry p#a: \"Keywords\" must be a list"));
        row("search entry keyword not a string",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p", "Anchor": "a", "Title": "T", "Keywords": [1] } ] })",
            QStringLiteral("search entry p#a: every \"Keywords\" entry must be a non-empty string"));
        row("search entry without a page",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Anchor": "a", "Title": "T" } ] })",
            QStringLiteral("search entry #a has no \"Page\""));
        row("search entry without an anchor",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p", "Title": "T" } ] })",
            QStringLiteral("search entry p# has no \"Anchor\""));
        row("search entry declared twice",
            R"({ "Id": "m", "Name": "M", "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p", "Anchor": "a", "Title": "T" },
                             { "Page": "p", "Anchor": "a", "Title": "T", "Title[de]": "U" } ] })",
            QStringLiteral("search entry p#a titled T is declared twice"));
        row("advanced search entry on a simple page",
            R"({ "Id": "m", "Name": "M",
                 "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["a"], "Visibility": "SimpleOnly" } ],
                 "Search": [ { "Page": "p", "Anchor": "a", "Title": "T", "AdvancedOnly": true } ] })",
            QStringLiteral("search entry p#a is AdvancedOnly, but page p shows only in simple mode"));
        row("advanced search entry under a simple page",
            R"({ "Id": "m", "Name": "M",
                 "Pages": [ { "Id": "p", "Name": "P", "Visibility": "SimpleOnly" },
                            { "Id": "p/q", "Name": "Q", "Parent": "p", "Anchors": ["a"] } ],
                 "Search": [ { "Page": "p/q", "Anchor": "a", "Title": "T", "AdvancedOnly": true } ] })",
            QStringLiteral("search entry p/q#a is AdvancedOnly, but page p shows only in simple mode"));
        // The page is declared, so the entry on it is not told it is not:
        // the loop is the one fault, reported once per page in it.
        row("search entry on a page in a loop",
            R"({ "Id": "m", "Name": "M", "Pages": [
                   { "Id": "a", "Name": "A", "Parent": "b", "Anchors": ["x"] }, { "Id": "b", "Name": "B", "Parent": "a" } ],
                 "Search": [ { "Page": "a", "Anchor": "x", "Title": "T" } ] })",
            QStringLiteral("in or under a loop of local parents"), 2);
        // A localizable key is required on its base value: one with only
        // a translation would be refused in every other locale.
        row("name only translated", R"({ "Id": "m", "Name[de]": "M", "Pages": [ { "Id": "p", "Name": "P" } ] })",
            QStringLiteral("\"Name\" is missing"));
    }

    void reportsEveryProblem()
    {
        QFETCH(QByteArray, text);
        QFETCH(QString, expected);
        QFETCH(int, count);
        QJsonObject object;
        QVERIFY(json(text.constData(), &object));
        QStringList errors;
        FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        QVERIFY2(errors.join(QLatin1Char('\n')).contains(expected), qPrintable(errors.join(QLatin1Char('\n'))));
        QVERIFY2(errors.size() == count, qPrintable(errors.join(QLatin1Char('\n'))));
    }

    void reportsEveryProblemAtOnce()
    {
        // Problems accumulate: one parse names all of them.
        QJsonObject object;
        QVERIFY(json(R"({ "Id": "a:b", "Category": "Bad Cat",
                          "Pages": [ { "Id": "x", "Name": "X", "Parent": "y" } ] })",
                     &object));
        QStringList errors;
        FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        const QString all = errors.join(QLatin1Char('\n'));
        QCOMPARE(errors.size(), 4);
        QVERIFY2(all.contains(QStringLiteral("\"Id\" must")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("\"Name\" is missing")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("\"Category\" Bad Cat")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("does not declare")), qPrintable(all));
    }
};

QTEST_GUILESS_MAIN(TestFacetModuleMetadata)
#include "test_facet_module_metadata.moc"
