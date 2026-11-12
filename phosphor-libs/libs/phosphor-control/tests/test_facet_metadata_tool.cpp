// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// phosphor-facet-metadata, run as the build runs it.

#include <PhosphorControl/FacetModuleMetadata.h>

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <optional>
#include <utility>

// The tool's path, from tests/CMakeLists.txt.
#ifndef PHOSPHOR_FACET_METADATA_TOOL
#error "PHOSPHOR_FACET_METADATA_TOOL must name the phosphor-facet-metadata executable"
#endif

using PhosphorControl::FacetModuleMetadata;

namespace {

const char* const ValidMetadata = R"({ "Id": "m", "Name": "Hello", "Pages": [ { "Id": "p", "Name": "P" } ] })";

// A catalog in @p language translating each source of @p messages.
QByteArray catalog(const QString& language, const QList<std::pair<QString, QString>>& messages)
{
    QString text = QStringLiteral(
                       "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<TS version=\"2.1\" language=\"%1\">"
                       "<context><name>FacetModule</name>\n")
                       .arg(language);
    for (const auto& [source, translation] : messages) {
        text += QStringLiteral("<message><source>%1</source><translation>%2</translation></message>\n")
                    .arg(source.toHtmlEscaped(), translation.toHtmlEscaped());
    }
    text += QStringLiteral("</context></TS>\n");
    return text.toUtf8();
}

QStringList keysStartingWith(const QJsonObject& json, const QString& prefix)
{
    QStringList keys;
    for (const QString& key : json.keys()) {
        if (key.startsWith(prefix)) {
            keys.append(key);
        }
    }
    return keys;
}

} // namespace

class TestFacetMetadataTool : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        QVERIFY2(m_dir.emplace().isValid(), qPrintable(m_dir->errorString()));
    }

    void refusesInvalidMetadataUnlessAllowed()
    {
        QString source;
        QVERIFY(write(QStringLiteral("bad.json"), R"({ "Id": "m", "Pages": [ { "Id": "p" } ] })", &source));
        const QString out = m_dir->filePath(QStringLiteral("out.json"));
        QString errors;
        QString outcome = run(1, {QStringLiteral("localize"), source, out}, &errors);
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        QVERIFY2(errors.contains(QStringLiteral("\"Name\" is missing")), qPrintable(errors));
        QVERIFY2(errors.contains(QStringLiteral("page p has no \"Name\"")), qPrintable(errors));
        QVERIFY(!QFile::exists(out));
        outcome = run(0, {QStringLiteral("--allow-invalid"), QStringLiteral("localize"), source, out});
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        QVERIFY(QFile::exists(out));
    }

    void writesEscapedStringsForLupdate()
    {
        QString source;
        QVERIFY(write(QStringLiteral("m.json"),
                      "{ \"Id\": \"m\", \"Name\": \"Say \\\"hi\\\" \\\\ now\", \"Description\": \"one\\ttwo\\nthree\","
                      " \"Keywords\": [\"k\", \"k\", \"Gr\xc3\xbc\xc3\x9f\x65\"],"
                      " \"Pages\": [ { \"Id\": \"p\", \"Name\": \"P\" } ] }",
                      &source));
        const QString out = m_dir->filePath(QStringLiteral("strings.cpp"));
        const QString outcome = run(0, {QStringLiteral("strings"), source, out});
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        const QString text = read(out);
        QVERIFY2(text.contains(QStringLiteral("//: Facet module metadata: name of module\n"
                                              "QT_TRANSLATE_NOOP(\"FacetModule\", \"Say \\\"hi\\\" \\\\ now\")\n")),
                 qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("QT_TRANSLATE_NOOP(\"FacetModule\", \"one\\ttwo\\nthree\")")),
                 qPrintable(text));
        // lupdate reads the file as UTF-8, so other text is written as it is.
        QVERIFY2(text.contains(QStringLiteral("QT_TRANSLATE_NOOP(\"FacetModule\", \"Grüße\")")), qPrintable(text));
        // Each string once, however often it appears.
        QCOMPARE(text.count(QStringLiteral("\"k\"")), 1);
    }

    void keepsInvalidTextFromBreakingTheStringsFile()
    {
        // parse() refuses a control character in a translatable string, so
        // only --allow-invalid lets one this far. It must still not break a
        // literal or end a comment early, and neither may a newline in an id
        // or in the metadata's own path.
        QString source;
        QVERIFY(write(QStringLiteral("m\nx.json"),
                      R"({ "Id": "m", "Name": "a\rb\u0001c\u007f",
                           "Pages": [ { "Id": "p\nq", "Name": "Page" } ] })",
                      &source));
        const QString out = m_dir->filePath(QStringLiteral("strings.cpp"));
        const QString outcome = run(0, {QStringLiteral("--allow-invalid"), QStringLiteral("strings"), source, out});
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        const QString text = read(out);
        QVERIFY2(text.contains(QStringLiteral("QT_TRANSLATE_NOOP(\"FacetModule\", \"a\\rb\\001c\\177\")")),
                 qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("//: Facet module metadata: name of page \"p q\"\n")), qPrintable(text));
        const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            QVERIFY2(line.startsWith(QLatin1String("//")) || line.startsWith(QLatin1String("QT_TRANSLATE_NOOP(")),
                     qPrintable(line));
        }
        QVERIFY2(lines.first().endsWith(QLatin1String("Not compiled: lupdate reads it.")), qPrintable(text));
    }

    void namesEveryUseOfAStringInItsComment()
    {
        QString source;
        QVERIFY(write(QStringLiteral("m.json"),
                      R"({ "Id": "m", "Name": "General", "Keywords": ["General"],
                           "Pages": [ { "Id": "p", "Name": "General" } ] })",
                      &source));
        const QString out = m_dir->filePath(QStringLiteral("strings.cpp"));
        const QString outcome = run(0, {QStringLiteral("strings"), source, out});
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        const QString text = read(out);
        QCOMPARE(text.count(QStringLiteral("QT_TRANSLATE_NOOP(")), 1);
        QVERIFY2(text.contains(QStringLiteral("//: Facet module metadata: name of module; a search keyword of module; "
                                              "name of page \"p\"\n")),
                 qPrintable(text));
    }

    void translatesEveryLocalizableField()
    {
        // Every localizable key of every object, through both commands and
        // back through parse(), so the tool and the parser cannot disagree
        // on which keys are localizable.
        QString source;
        QVERIFY(write(QStringLiteral("m.json"), R"({ "Id": "m", "Name": "Module", "Description": "About",
            "Keywords": ["module-word"],
            "Categories": [ { "Id": "c", "Name": "Category" } ],
            "Pages": [ { "Id": "p", "Name": "Page", "Keywords": ["page-word"], "Anchors": ["a"] } ],
            "Extensions": [ { "Id": "e", "Target": "o:p", "Slot": "top", "Name": "Section",
                              "Keywords": ["section-word"] } ],
            "Search": [ { "Page": "p", "Anchor": "a", "Title": "Setting", "Keywords": ["setting-word"] } ] })",
                      &source));
        const QStringList sources{
            QStringLiteral("Module"),       QStringLiteral("module-word"), QStringLiteral("Category"),
            QStringLiteral("Page"),         QStringLiteral("page-word"),   QStringLiteral("Section"),
            QStringLiteral("section-word"), QStringLiteral("Setting"),     QStringLiteral("setting-word")};
        QList<std::pair<QString, QString>> messages;
        for (const QString& text : sources) {
            messages.append({text, QStringLiteral("fr-") + text});
        }
        QString ts;
        QVERIFY(write(QStringLiteral("m_fr.ts"), catalog(QStringLiteral("fr"), messages), &ts));

        const QString strings = m_dir->filePath(QStringLiteral("strings.cpp"));
        QString outcome = run(0, {QStringLiteral("strings"), source, strings});
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        const QString text = read(strings);
        for (const QString& string : sources + QStringList{QStringLiteral("About")}) {
            QVERIFY2(text.contains(QStringLiteral("\"FacetModule\", \"%1\")").arg(string)), qPrintable(string));
        }

        const QString out = m_dir->filePath(QStringLiteral("out.json"));
        QString errors;
        outcome = run(0, {QStringLiteral("localize"), source, out, ts}, &errors);
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        QVERIFY2(errors.isEmpty(), qPrintable(errors));
        const QJsonObject json = QJsonDocument::fromJson(read(out).toUtf8()).object();
        // The description has no translation, so it gets no key.
        QVERIFY(!json.contains(QStringLiteral("Description[fr]")));

        QStringList parseErrors;
        QStringList warnings;
        const FacetModuleMetadata metadata =
            FacetModuleMetadata::parse(json, QLocale(QStringLiteral("fr_FR")), &parseErrors, &warnings);
        QVERIFY2(parseErrors.isEmpty(), qPrintable(parseErrors.join(QLatin1Char('\n'))));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.name, QStringLiteral("fr-Module"));
        QCOMPARE(metadata.description, QStringLiteral("About"));
        QCOMPARE(metadata.keywords, QStringList{QStringLiteral("fr-module-word")});
        QCOMPARE(metadata.categories.first().name, QStringLiteral("fr-Category"));
        QCOMPARE(metadata.pages.first().name, QStringLiteral("fr-Page"));
        QCOMPARE(metadata.pages.first().keywords, QStringList{QStringLiteral("fr-page-word")});
        QCOMPARE(metadata.extensions.first().name, QStringLiteral("fr-Section"));
        QCOMPARE(metadata.extensions.first().keywords, QStringList{QStringLiteral("fr-section-word")});
        QCOMPARE(metadata.search.first().title, QStringLiteral("fr-Setting"));
        QCOMPARE(metadata.search.first().keywords, QStringList{QStringLiteral("fr-setting-word")});
    }

    void localizesFromCatalogs()
    {
        QString source;
        QVERIFY(write(QStringLiteral("m.json"), R"({ "Id": "m", "Name": "Hello", "Keywords": ["one", "two"],
                                                     "Pages": [ { "Id": "p", "Name": "P" } ] })",
                      &source));
        QString ts;
        QVERIFY(write(QStringLiteral("m_fr.ts"),
                      catalog(QStringLiteral("fr"),
                              {{QStringLiteral("Hello"), QStringLiteral("Bonjour")},
                               {QStringLiteral("two"), QStringLiteral("deux")}}),
                      &ts));
        const QString out = m_dir->filePath(QStringLiteral("out.json"));
        const QString outcome = run(0, {QStringLiteral("localize"), source, out, ts});
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        const QJsonObject json = QJsonDocument::fromJson(read(out).toUtf8()).object();
        QCOMPARE(json.value(QStringLiteral("Name[fr]")).toString(), QStringLiteral("Bonjour"));
        // An untranslated keyword keeps its source text: a search term in
        // the source language still finds the page.
        QCOMPARE(json.value(QStringLiteral("Keywords[fr]")).toVariant().toStringList(),
                 (QStringList{QStringLiteral("one"), QStringLiteral("deux")}));
        // The page name has no translation, so it gets no key.
        const QJsonObject page = json.value(QStringLiteral("Pages")).toArray().first().toObject();
        QVERIFY(!page.contains(QStringLiteral("Name[fr]")));
    }

    void keepsHandWrittenTranslations()
    {
        QString source;
        QVERIFY(write(QStringLiteral("m.json"),
                      R"({ "Id": "m", "Name": "Hello", "Name[fr]": "Salut", "Keywords": ["one"],
                           "Keywords[fr]": ["un"], "Pages": [ { "Id": "p", "Name": "P" } ] })",
                      &source));
        QString ts;
        QVERIFY(write(QStringLiteral("m_fr.ts"),
                      catalog(QStringLiteral("fr"),
                              {{QStringLiteral("Hello"), QStringLiteral("Bonjour")},
                               {QStringLiteral("one"), QStringLiteral("une")},
                               {QStringLiteral("P"), QStringLiteral("Pé")}}),
                      &ts));
        const QString out = m_dir->filePath(QStringLiteral("out.json"));
        const QString outcome = run(0, {QStringLiteral("localize"), source, out, ts});
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        const QJsonObject json = QJsonDocument::fromJson(read(out).toUtf8()).object();
        QCOMPARE(json.value(QStringLiteral("Name[fr]")).toString(), QStringLiteral("Salut"));
        QCOMPARE(json.value(QStringLiteral("Keywords[fr]")).toVariant().toStringList(),
                 QStringList{QStringLiteral("un")});
        // What was not written by hand is still translated.
        const QJsonObject page = json.value(QStringLiteral("Pages")).toArray().first().toObject();
        QCOMPARE(page.value(QStringLiteral("Name[fr]")).toString(), QStringLiteral("Pé"));
    }

    void usesOnlyFinishedTranslationsInItsContext_data()
    {
        QTest::addColumn<QByteArray>("message");
        QTest::addColumn<bool>("used");
        QTest::newRow("finished") << QByteArray(
            "<context><name>FacetModule</name><message><source>Hello</source>"
            "<translation>Bonjour</translation></message></context>")
                                  << true;
        QTest::newRow("another context") << QByteArray(
            "<context><name>Other</name><message><source>Hello</source>"
            "<translation>Bonjour</translation></message></context>")
                                         << false;
        QTest::newRow("unfinished") << QByteArray(
            "<context><name>FacetModule</name><message><source>Hello</source>"
            "<translation type=\"unfinished\">Bonjour</translation></message>"
            "</context>") << false;
        QTest::newRow("vanished") << QByteArray(
            "<context><name>FacetModule</name><message><source>Hello</source>"
            "<translation type=\"vanished\">Bonjour</translation></message>"
            "</context>") << false;
        QTest::newRow("obsolete") << QByteArray(
            "<context><name>FacetModule</name><message><source>Hello</source>"
            "<translation type=\"obsolete\">Bonjour</translation></message>"
            "</context>") << false;
        QTest::newRow("disambiguated") << QByteArray(
            "<context><name>FacetModule</name><message><source>Hello"
            "</source><comment>greeting</comment><translation>Bonjour"
            "</translation></message></context>")
                                       << false;
        QTest::newRow("plural") << QByteArray(
            "<context><name>FacetModule</name><message numerus=\"yes\"><source>"
            "Hello</source><translation><numerusform>Bonjour</numerusform>"
            "</translation></message></context>")
                                << false;
        QTest::newRow("length variants") << QByteArray(
            "<context><name>FacetModule</name><message><source>Hello</source><translation "
            "variants=\"yes\"><lengthvariant>Bonjour</lengthvariant><lengthvariant>Bjr"
            "</lengthvariant></translation></message></context>")
                                         << false;
    }

    void usesOnlyFinishedTranslationsInItsContext()
    {
        QFETCH(QByteArray, message);
        QFETCH(bool, used);
        QString source;
        QVERIFY(write(QStringLiteral("m.json"), ValidMetadata, &source));
        QString ts;
        QVERIFY(write(QStringLiteral("m_fr.ts"), "<TS version=\"2.1\" language=\"fr\">" + message + "</TS>", &ts));
        const QString out = m_dir->filePath(QStringLiteral("out.json"));
        const QString outcome = run(0, {QStringLiteral("localize"), source, out, ts});
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        const QJsonObject json = QJsonDocument::fromJson(read(out).toUtf8()).object();
        QCOMPARE(json.value(QStringLiteral("Name[fr]")).toString(), used ? QStringLiteral("Bonjour") : QString());
    }

    void readsMessagesItDoesNotUse_data()
    {
        QTest::addColumn<QByteArray>("other");
        // A plural, length variants or a control character anywhere in the
        // catalog: the module's own messages may hold any of them.
        QTest::newRow("plural") << QByteArray(
            "<message numerus=\"yes\"><source>%n file(s)</source><translation>"
            "<numerusform>%n fichier</numerusform><numerusform>%n fichiers"
            "</numerusform></translation></message>");
        QTest::newRow("length variants") << QByteArray(
            "<message><source>Long</source><translation variants=\"yes\">"
            "<lengthvariant>Longue</lengthvariant><lengthvariant>L"
            "</lengthvariant></translation></message>");
        QTest::newRow("control character in the source")
            << QByteArray("<message><source>a<byte value=\"x1\"/>b</source><translation>c</translation></message>");
        QTest::newRow("control character in the translation")
            << QByteArray("<message><source>ab</source><translation>c<byte value=\"x1\"/>d</translation></message>");
    }

    void readsMessagesItDoesNotUse()
    {
        QFETCH(QByteArray, other);
        QString source;
        QVERIFY(write(QStringLiteral("m.json"), ValidMetadata, &source));
        const QByteArray hello("<message><source>Hello</source><translation>Bonjour</translation></message>");
        // In another context and in FacetModule's own.
        QString ts;
        QVERIFY(write(QStringLiteral("m_fr.ts"),
                      "<TS version=\"2.1\" language=\"fr\"><context><name>Other</name>" + other
                          + "</context><context><name>FacetModule</name>" + other + hello + "</context></TS>",
                      &ts));
        const QString out = m_dir->filePath(QStringLiteral("out.json"));
        QString errors;
        const QString outcome = run(0, {QStringLiteral("localize"), source, out, ts}, &errors);
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        QVERIFY2(errors.isEmpty(), qPrintable(errors));
        const QJsonObject json = QJsonDocument::fromJson(read(out).toUtf8()).object();
        QCOMPARE(json.value(QStringLiteral("Name[fr]")).toString(), QStringLiteral("Bonjour"));
    }

    void decodesControlCharactersInMessages()
    {
        QString source;
        QVERIFY(write(QStringLiteral("m.json"),
                      R"({ "Id": "m", "Name": "Tab\there", "Pages": [ { "Id": "p", "Name": "P" } ] })", &source));
        QString ts;
        QVERIFY(write(QStringLiteral("m_fr.ts"),
                      "<TS version=\"2.1\" language=\"fr\"><context><name>FacetModule</name><message><source>Tab"
                      "<byte value=\"x9\"/>here</source><translation>Onglet<byte value=\"9\"/>ici</translation>"
                      "</message></context></TS>",
                      &ts));
        const QString out = m_dir->filePath(QStringLiteral("out.json"));
        const QString outcome = run(0, {QStringLiteral("localize"), source, out, ts});
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        const QJsonObject json = QJsonDocument::fromJson(read(out).toUtf8()).object();
        QCOMPARE(json.value(QStringLiteral("Name[fr]")).toString(), QStringLiteral("Onglet\tici"));
    }

    void keysEachCatalogByTheLanguageUsersMatch_data()
    {
        QTest::addColumn<QStringList>("languages");
        QTest::addColumn<QStringList>("keys");
        QTest::addColumn<QString>("userLocale");
        QTest::addColumn<QString>("name");
        // Translations are the catalog's language, so the name says which
        // one a user got.
        QTest::newRow("regional catalog serves the language")
            << QStringList{QStringLiteral("de_DE")} << QStringList{QStringLiteral("Name[de]")}
            << QStringLiteral("de_AT") << QStringLiteral("de_DE");
        QTest::newRow("dash") << QStringList{QStringLiteral("pt-BR")} << QStringList{QStringLiteral("Name[pt]")}
                              << QStringLiteral("pt_BR") << QStringLiteral("pt-BR");
        QTest::newRow("two regions of one language")
            << QStringList{QStringLiteral("pt-BR"), QStringLiteral("pt_PT")}
            << QStringList{QStringLiteral("Name[pt_BR]"), QStringLiteral("Name[pt_PT]")} << QStringLiteral("pt_PT")
            << QStringLiteral("pt_PT");
        QTest::newRow("language and one region")
            << QStringList{QStringLiteral("de_AT"), QStringLiteral("de")}
            << QStringList{QStringLiteral("Name[de]"), QStringLiteral("Name[de_AT]")} << QStringLiteral("de_CH")
            << QStringLiteral("de");
        // Traditional Chinese is not for every zh user.
        QTest::newRow("script other than the language's usual one")
            << QStringList{QStringLiteral("zh_TW")} << QStringList{QStringLiteral("Name[zh_TW]")}
            << QStringLiteral("zh_CN") << QStringLiteral("Hello");
        QTest::newRow("explicit script") << QStringList{QStringLiteral("sr_Latn_RS")}
                                         << QStringList{QStringLiteral("Name[sr_Latn_RS]")}
                                         << QStringLiteral("sr_Latn_RS") << QStringLiteral("sr_Latn_RS");
        // QLocale reads `no` as Norwegian Bokmål, nb, so `no` is no bare
        // language a locale matches.
        QTest::newRow("language QLocale reads as another")
            << QStringList{QStringLiteral("no_NO")} << QStringList{QStringLiteral("Name[no_NO]")}
            << QStringLiteral("nb_NO") << QStringLiteral("Hello");
    }

    void keysEachCatalogByTheLanguageUsersMatch()
    {
        QFETCH(QStringList, languages);
        QFETCH(QStringList, keys);
        QFETCH(QString, userLocale);
        QFETCH(QString, name);
        QString source;
        QVERIFY(write(QStringLiteral("m.json"), ValidMetadata, &source));
        QStringList arguments{QStringLiteral("localize"), source, m_dir->filePath(QStringLiteral("out.json"))};
        for (const QString& language : std::as_const(languages)) {
            QString ts;
            QVERIFY(write(QStringLiteral("m_%1.ts").arg(language),
                          catalog(language, {{QStringLiteral("Hello"), language}}), &ts));
            arguments.append(ts);
        }
        QString errors;
        const QString outcome = run(0, arguments, &errors);
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        QVERIFY2(errors.isEmpty(), qPrintable(errors));
        const QJsonObject json = QJsonDocument::fromJson(read(arguments.at(2)).toUtf8()).object();
        QCOMPARE(keysStartingWith(json, QStringLiteral("Name[")), keys);
        QStringList parseErrors;
        const FacetModuleMetadata metadata = FacetModuleMetadata::parse(json, QLocale(userLocale), &parseErrors);
        QVERIFY2(parseErrors.isEmpty(), qPrintable(parseErrors.join(QLatin1Char('\n'))));
        QCOMPARE(metadata.name, name);
    }

    void refusesTwoCatalogsForOneLanguage()
    {
        QString source;
        QVERIFY(write(QStringLiteral("m.json"), ValidMetadata, &source));
        QString dash;
        QString underscore;
        QVERIFY(write(QStringLiteral("a.ts"), catalog(QStringLiteral("pt-BR"), {}), &dash));
        QVERIFY(write(QStringLiteral("b.ts"), catalog(QStringLiteral("pt_BR"), {}), &underscore));
        QString errors;
        const QString out = m_dir->filePath(QStringLiteral("out.json"));
        const QString outcome = run(1, {QStringLiteral("localize"), source, out, underscore, dash}, &errors);
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        QVERIFY2(errors.contains(QStringLiteral("%1 and %2 are both catalogs for pt_BR").arg(dash, underscore)),
                 qPrintable(errors));
        QVERIFY(!QFile::exists(out));
    }

    void leavesOutACatalogNoLocaleMatches()
    {
        QString source;
        QVERIFY(write(QStringLiteral("m.json"), ValidMetadata, &source));
        QString ts;
        QVERIFY(write(QStringLiteral("m_sr.ts"),
                      catalog(QStringLiteral("sr@latin"), {{QStringLiteral("Hello"), QStringLiteral("Zdravo")}}), &ts));
        QString errors;
        const QString out = m_dir->filePath(QStringLiteral("out.json"));
        const QString outcome = run(0, {QStringLiteral("localize"), source, out, ts}, &errors);
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        QVERIFY2(
            errors.contains(QStringLiteral("warning: %1 is for language sr@latin, which no locale matches").arg(ts)),
            qPrintable(errors));
        const QJsonObject json = QJsonDocument::fromJson(read(out).toUtf8()).object();
        QCOMPARE(keysStartingWith(json, QStringLiteral("Name[")), QStringList());
    }

    void printsWarningsWithoutFailing()
    {
        // A key this Facet does not know may be one a newer Facet does, so
        // it is a warning, which localize prints once for the build and
        // which fails nothing.
        QString source;
        QVERIFY(write(QStringLiteral("m.json"),
                      R"({ "Id": "m", "Name": "M", "Colour": "red", "Pages": [ { "Id": "p", "Name": "P" } ] })",
                      &source));
        QString errors;
        QString outcome =
            run(0, {QStringLiteral("localize"), source, m_dir->filePath(QStringLiteral("o.json"))}, &errors);
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        QCOMPARE(errors, QStringLiteral("phosphor-facet-metadata: warning: %1: unknown key \"Colour\"\n").arg(source));
        for (const QString& command : {QStringLiteral("strings"), QStringLiteral("anchors")}) {
            outcome = run(0, {command, source, m_dir->filePath(QStringLiteral("o.txt"))}, &errors);
            QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
            QVERIFY2(errors.isEmpty(), qPrintable(errors));
        }
    }

    void refusesACatalogWithoutALanguage()
    {
        QString source;
        QVERIFY(write(QStringLiteral("m.json"), ValidMetadata, &source));
        QString ts;
        QVERIFY(write(QStringLiteral("x.ts"), R"(<TS version="2.1"></TS>)", &ts));
        QString errors;
        const QString outcome =
            run(1, {QStringLiteral("localize"), source, m_dir->filePath(QStringLiteral("o.json")), ts}, &errors);
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        QVERIFY2(errors.contains(QStringLiteral("no language attribute")), qPrintable(errors));
    }

    void writesTheAnchorList()
    {
        QString source;
        QVERIFY(write(QStringLiteral("m.json"), R"({ "Id": "m", "Name": "M",
            "Pages": [ { "Id": "p", "Name": "P", "Anchors": ["b", "a"] }, { "Id": "q", "Name": "Q" } ],
            "ExtensionPoints": [ { "Page": "p", "Id": "anchors", "Version": 3 },
                                 { "Page": "q", "Id": "panel", "Version": 5 } ] })",
                      &source));
        const QString out = m_dir->filePath(QStringLiteral("anchors.json"));
        const QString outcome = run(0, {QStringLiteral("anchors"), source, out});
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        const QJsonObject json = QJsonDocument::fromJson(read(out).toUtf8()).object();
        QCOMPARE(json.value(QStringLiteral("Module")).toString(), QStringLiteral("m"));
        const QJsonObject p = json.value(QStringLiteral("Pages")).toObject().value(QStringLiteral("p")).toObject();
        QCOMPARE(p.value(QStringLiteral("Version")).toInt(), 3);
        // Sorted, so reordering them in the metadata leaves the file as it is.
        QCOMPARE(p.value(QStringLiteral("Anchors")).toVariant().toStringList(),
                 (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
        // A named point has its own version, not the anchors one.
        const QJsonObject q = json.value(QStringLiteral("Pages")).toObject().value(QStringLiteral("q")).toObject();
        QCOMPARE(q.value(QStringLiteral("Version")).toInt(), 1);
    }

    void passesNonObjectsThroughUnderAllowInvalid()
    {
        QString source;
        QVERIFY(write(QStringLiteral("m.json"),
                      R"({ "Id": "m", "Name": "M", "Pages": [ 5, { "Id": "p", "Name": "P" } ] })", &source));
        const QString out = m_dir->filePath(QStringLiteral("out.json"));
        const QString outcome = run(0, {QStringLiteral("--allow-invalid"), QStringLiteral("localize"), source, out});
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        const QJsonArray pages =
            QJsonDocument::fromJson(read(out).toUtf8()).object().value(QStringLiteral("Pages")).toArray();
        QCOMPARE(pages.size(), 2);
        QCOMPARE(pages.at(0), QJsonValue(5));
    }

    void failsWithAReason_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<QString>("reason");
        const QString usage = QStringLiteral("unknown command or wrong arguments");
        QTest::newRow("no arguments") << QStringList() << 2 << usage;
        QTest::newRow("unknown command") << QStringList{QStringLiteral("frobnicate"), QStringLiteral("$good"),
                                                        QStringLiteral("$out")}
                                         << 2 << usage;
        QTest::newRow("strings with a catalog") << QStringList{QStringLiteral("strings"), QStringLiteral("$good"),
                                                               QStringLiteral("$out"), QStringLiteral("$ts")}
                                                << 2 << usage;
        // The command line is wrong before the metadata is missing.
        QTest::newRow("unknown command and missing metadata")
            << QStringList{QStringLiteral("frobnicate"), QStringLiteral("$missing"), QStringLiteral("$out")} << 2
            << usage;
        QTest::newRow("missing metadata")
            << QStringList{QStringLiteral("anchors"), QStringLiteral("$missing"), QStringLiteral("$out")} << 1
            << QStringLiteral("cannot read");
        QTest::newRow("malformed metadata")
            << QStringList{QStringLiteral("anchors"), QStringLiteral("$malformed"), QStringLiteral("$out")} << 1
            << QStringLiteral("is not valid JSON");
        QTest::newRow("metadata not an object")
            << QStringList{QStringLiteral("anchors"), QStringLiteral("$array"), QStringLiteral("$out")} << 1
            << QStringLiteral("does not hold a JSON object");
        QTest::newRow("missing catalog") << QStringList{QStringLiteral("localize"), QStringLiteral("$good"),
                                                        QStringLiteral("$out"), QStringLiteral("$missing")}
                                         << 1 << QStringLiteral("cannot read");
        QTest::newRow("malformed catalog") << QStringList{QStringLiteral("localize"), QStringLiteral("$good"),
                                                          QStringLiteral("$out"), QStringLiteral("$badts")}
                                           << 1 << QStringLiteral("bad.ts: ");
        QTest::newRow("unwritable output")
            << QStringList{QStringLiteral("anchors"), QStringLiteral("$good"), QStringLiteral("$nodir")} << 1
            << QStringLiteral("cannot write");
    }

    void failsWithAReason()
    {
        QFETCH(QStringList, arguments);
        QFETCH(int, exitCode);
        QFETCH(QString, reason);
        QHash<QString, QString> paths;
        QString path;
        QVERIFY(write(QStringLiteral("good.json"), ValidMetadata, &path));
        paths.insert(QStringLiteral("$good"), path);
        QVERIFY(write(QStringLiteral("malformed.json"), "{ \"Id\": ", &path));
        paths.insert(QStringLiteral("$malformed"), path);
        QVERIFY(write(QStringLiteral("array.json"), "[1]", &path));
        paths.insert(QStringLiteral("$array"), path);
        QVERIFY(write(QStringLiteral("ok.ts"), catalog(QStringLiteral("fr"), {}), &path));
        paths.insert(QStringLiteral("$ts"), path);
        QVERIFY(write(QStringLiteral("bad.ts"), "<TS language=\"fr\"><context>", &path));
        paths.insert(QStringLiteral("$badts"), path);
        paths.insert(QStringLiteral("$missing"), m_dir->filePath(QStringLiteral("missing")));
        paths.insert(QStringLiteral("$out"), m_dir->filePath(QStringLiteral("out")));
        paths.insert(QStringLiteral("$nodir"), m_dir->filePath(QStringLiteral("no/such/dir/out")));
        for (QString& argument : arguments) {
            argument = paths.value(argument, argument);
        }
        QString errors;
        const QString outcome = run(exitCode, arguments, &errors);
        QVERIFY2(outcome.isEmpty(), qPrintable(outcome));
        QVERIFY2(errors.contains(reason), qPrintable(errors));
        QVERIFY(!QFile::exists(paths.value(QStringLiteral("$out"))));
    }

private:
    // Writes @p text to @p name in the test's directory and sets @p path to
    // it, or returns false with a warning saying why.
    bool write(const QString& name, const QByteArray& text, QString* path)
    {
        *path = m_dir->filePath(name);
        QFile file(*path);
        if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size()) {
            qWarning() << "cannot write" << *path << file.errorString();
            return false;
        }
        return true;
    }

    static QString read(const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    }

    // Runs the tool with @p arguments. Empty when it exits with @p expected,
    // otherwise what it did instead, with its standard error. @p errors gets
    // its standard error either way.
    static QString run(int expected, const QStringList& arguments, QString* errors = nullptr)
    {
        QProcess process;
        process.start(QStringLiteral(PHOSPHOR_FACET_METADATA_TOOL), arguments);
        if (!process.waitForStarted(10000)) {
            return QStringLiteral("did not start: %1").arg(process.errorString());
        }
        if (!process.waitForFinished(30000)) {
            process.kill();
            process.waitForFinished(5000);
            return QStringLiteral("did not finish within 30 s");
        }
        const QString standardError = QString::fromUtf8(process.readAllStandardError());
        if (errors) {
            *errors = standardError;
        }
        if (process.exitStatus() != QProcess::NormalExit) {
            return QStringLiteral("crashed: %1\n%2").arg(process.errorString(), standardError);
        }
        if (process.exitCode() != expected) {
            return QStringLiteral("exited with %1, not %2:\n%3")
                .arg(QString::number(process.exitCode()), QString::number(expected), standardError);
        }
        return QString();
    }

    std::optional<QTemporaryDir> m_dir;
};

QTEST_GUILESS_MAIN(TestFacetMetadataTool)
#include "test_facet_metadata_tool.moc"
