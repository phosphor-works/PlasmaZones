// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorControl/FacetModuleMetadata.h>

#include <QJsonDocument>
#include <QJsonParseError>
#include <QTest>

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
                         "Visibility": "AdvancedOnly", "DividerAfter": true, "Counterpart": "other:page" },
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
        QCOMPARE(metadata.page(QStringLiteral("pinned"))->category, QStringLiteral("appearance"));
        QVERIFY(!metadata.page(QStringLiteral("nope")));
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
        // So is one of the wrong type.
        QTest::newRow("translation not a string") << QStringLiteral("es_ES") << QStringLiteral("English");
    }

    void picksTheBestTranslation()
    {
        QFETCH(QString, locale);
        QFETCH(QString, expected);
        QJsonObject object;
        QVERIFY(json(R"({ "Id": "m", "Name": "English", "Name[pt]": "Portugal", "Name[pt_BR]": "Brasil",
                          "Name[de]": "", "Name[es]": 7, "Pages": [ { "Id": "p", "Name": "P" } ] })",
                     &object));
        QStringList errors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, QLocale(locale), &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.name, expected);
    }

    void fallsBackToTheLanguageForEveryLocalizedKey()
    {
        // A regional locale with no regional translation gets the
        // language's, at module and page level alike.
        QJsonObject object;
        QVERIFY(json(R"({ "Id": "m", "Name": "English", "Name[fr]": "Français",
                          "Pages": [ { "Id": "p", "Name": "P", "Name[fr]": "Page", "Keywords[fr]": ["clé"] } ] })",
                     &object));
        const QLocale locale(QLocale::French, QLocale::Switzerland);
        QStringList errors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, locale, &errors);
        QVERIFY(errors.isEmpty());
        QCOMPARE(metadata.name, QStringLiteral("Français"));
        QCOMPARE(metadata.pages.first().name, QStringLiteral("Page"));
        QCOMPARE(metadata.pages.first().keywords, QStringList{QStringLiteral("clé")});
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
        QStringList errors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        QVERIFY(errors.isEmpty());
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
        FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    }

    void warnsAboutUnknownKeys()
    {
        QJsonObject object;
        QVERIFY(json(R"({ "Id": "m", "Name": "M", "Name[de]": "M", "Extensions": [],
            "Categories": [ { "Id": "c", "Name": "C", "Colour": "red" } ],
            "Pages": [ { "Id": "p", "Name": "P", "Parnet": "q" } ] })",
                     &object));
        QStringList errors;
        QStringList warnings;
        FacetModuleMetadata::parse(object, QLocale::c(), &errors, &warnings);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
        const QString all = warnings.join(QLatin1Char('\n'));
        QCOMPARE(warnings.size(), 3);
        QVERIFY2(all.contains(QStringLiteral("\"Extensions\"")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("\"Colour\"")), qPrintable(all));
        QVERIFY2(all.contains(QStringLiteral("\"Parnet\"")), qPrintable(all));
    }

    void reportsEveryProblem_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<QString>("expected");
        const auto row = [](const char* name, const char* text, const QString& expected) {
            QTest::newRow(name) << QByteArray(text) << expected;
        };
        // Every row has one fault and a valid page list otherwise, so the
        // expected message is the only one it can match.
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
            QStringLiteral("in or under a loop of local parents"));
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
    }

    void reportsEveryProblem()
    {
        QFETCH(QByteArray, text);
        QFETCH(QString, expected);
        QJsonObject object;
        QVERIFY(json(text.constData(), &object));
        QStringList errors;
        FacetModuleMetadata::parse(object, QLocale::c(), &errors);
        QVERIFY2(errors.join(QLatin1Char('\n')).contains(expected), qPrintable(errors.join(QLatin1Char('\n'))));
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
