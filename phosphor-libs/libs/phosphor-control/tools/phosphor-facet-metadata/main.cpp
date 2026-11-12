// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Build-time generator for a Facet module's metadata, run by
// phosphor_add_facet_module_metadata() (PhosphorFacetModule.cmake):
//
//   phosphor-facet-metadata strings  <metadata.json> <out.cpp>
//       The translatable strings as QT_TRANSLATE_NOOP calls in context
//       "FacetModule", for lupdate. Not compiled.
//   phosphor-facet-metadata localize <metadata.json> <out.json> [<file.ts>...]
//       The metadata with `Key[<lang>]` added for every string a catalog
//       translates, which is what the module embeds.
//   phosphor-facet-metadata anchors  <metadata.json> <out.json>
//       Each page's anchors, the version of its `anchors` extension point and
//       the version of each of its named extension points, for
//       FacetTestHost's check against the previous release.
//
// Every command first validates the metadata with FacetModuleMetadata::parse
// and fails the build on an error, unless --allow-invalid is given (the test
// fixtures that are invalid on purpose). parse()'s warnings, such as an
// unknown key, never fail it: metadata written for a newer Facet must still
// build. localize prints them, and the other two commands do not, so a build
// shows each once.
//
// localize reads only finished translations in context "FacetModule". A
// translation lrelease would ship anyway (an unfinished one, unless lrelease
// runs with -nounfinished) is not in the metadata, so a module that wants its
// pages and its sidebar entry to agree passes -nounfinished to lrelease.
//
// A catalog's `language` is written into the keys with '-' turned into '_',
// the form parse() matches. lupdate names a catalog for its region (de_DE)
// however general its translation is, and a user in de_AT matches `Name[de]`
// but never `Name[de_DE]`. So a regional catalog's strings go under the bare
// language, unless another catalog claims that language too, the catalog's
// script is not the language's usual one (zh_TW is Traditional Chinese), or
// QLocale reads the language part as another language (no_NO is nb). A
// `Key[<lang>]` the metadata already holds was written by hand and is kept.

#include <PhosphorControl/FacetModuleMetadata.h>

#include "../../src/facetmetadatakeys_p.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <optional>
#include <span>
#include <vector>

using PhosphorControl::FacetModuleMetadata;
namespace Keys = PhosphorControl::FacetMetadataKeys;

namespace {

// The lupdate context of every metadata string.
constexpr QLatin1String TRANSLATION_CONTEXT("FacetModule");

void fail(const QString& message)
{
    std::fprintf(stderr, "phosphor-facet-metadata: %s\n", qUtf8Printable(message));
}

void warn(const QString& message)
{
    std::fprintf(stderr, "phosphor-facet-metadata: warning: %s\n", qUtf8Printable(message));
}

bool readJson(const QString& path, QJsonObject* out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("cannot read %1: %2").arg(path, file.errorString()));
        return false;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError) {
        fail(QStringLiteral("%1 is not valid JSON: %2 at offset %3")
                 .arg(path, error.errorString(), QString::number(error.offset)));
        return false;
    }
    if (!document.isObject()) {
        fail(QStringLiteral("%1 does not hold a JSON object").arg(path));
        return false;
    }
    *out = document.object();
    return true;
}

// A file that already holds @p bytes is left alone, timestamp and all. The
// build reruns the tool whenever the tool is rebuilt, and an unchanged output
// must not make it recompile and relink the module that embeds it.
bool write(const QString& path, const QByteArray& bytes)
{
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.size() == bytes.size() && existing.readAll() == bytes) {
        return true;
    }
    existing.close();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        fail(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
        return false;
    }
    return true;
}

// One translatable field: where it is (for translator comments), its key, and
// whether it holds a list.
struct Field
{
    QString where;
    QLatin1String key;
    bool list = false;
};

// Visit every translatable field the key table names, as (object, field).
// The visitor may modify the object.
void forEachField(QJsonObject& root, const std::function<void(QJsonObject&, const Field&)>& visit)
{
    const auto visitKeys = [&visit](QJsonObject& object, std::span<const Keys::Key> keys, const QString& where) {
        for (const Keys::Key& key : keys) {
            if (key.localizable != Keys::Localizable::No) {
                visit(object, {where, key.name, key.localizable == Keys::Localizable::List});
            }
        }
    };
    visitKeys(root, Keys::Module, QStringLiteral("module"));
    for (const Keys::ObjectList& list : Keys::ObjectLists) {
        QJsonArray array = root.value(list.key).toArray();
        for (qsizetype i = 0; i < array.size(); ++i) {
            // Invalid metadata, which only --allow-invalid lets this far. It
            // is passed through as it is rather than turned into {}.
            if (!array.at(i).isObject()) {
                continue;
            }
            QJsonObject object = array.at(i).toObject();
            const QString id = object.value(QLatin1String("Id"))
                                   .toString(object.value(QLatin1String("Page")).toString() + QLatin1Char('#')
                                             + object.value(QLatin1String("Anchor")).toString());
            visitKeys(object, list.keys, QString(list.label) + QStringLiteral(" \"") + id + QLatin1Char('"'));
            array.replace(i, object);
        }
        if (!array.isEmpty()) {
            root.insert(list.key, array);
        }
    }
}

// @p text for a `//` comment line: a control character, which could end the
// comment early (a newline) or make lupdate drop the line, becomes a space.
QString commentText(const QString& text)
{
    QString out = text;
    for (QChar& c : out) {
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) {
            c = QLatin1Char(' ');
        }
    }
    return out;
}

// @p text as a C string literal. A control character other than tab, newline
// and carriage return is a three-digit octal escape, which, unlike \x, cannot
// swallow a digit that follows it.
QString cString(const QString& text)
{
    QString escaped;
    escaped.reserve(text.size() + 2);
    escaped += QLatin1Char('"');
    for (const QChar c : text) {
        switch (c.unicode()) {
        case '\\':
            escaped += QLatin1String("\\\\");
            break;
        case '"':
            escaped += QLatin1String("\\\"");
            break;
        case '\n':
            escaped += QLatin1String("\\n");
            break;
        case '\t':
            escaped += QLatin1String("\\t");
            break;
        case '\r':
            escaped += QLatin1String("\\r");
            break;
        default:
            if (c.unicode() < 0x20 || c.unicode() == 0x7f) {
                escaped += QStringLiteral("\\%1").arg(uint(c.unicode()), 3, 8, QLatin1Char('0'));
            } else {
                escaped += c;
            }
        }
    }
    escaped += QLatin1Char('"');
    return escaped;
}

bool strings(QJsonObject root, const QString& source, const QString& out)
{
    // Each string once, with every place it is used in its one comment, so a
    // translator sees all of them whichever came first.
    QStringList order;
    QHash<QString, QStringList> usages;
    forEachField(root, [&](QJsonObject& object, const Field& field) {
        const QString kind = field.list ? QStringLiteral("a search keyword") : QString(field.key).toLower();
        const QString usage = QStringLiteral("%1 of %2").arg(kind, field.where);
        QStringList values;
        if (field.list) {
            for (const QJsonValue& item : object.value(field.key).toArray()) {
                values.append(item.toString());
            }
        } else {
            values.append(object.value(field.key).toString());
        }
        for (const QString& value : std::as_const(values)) {
            if (value.isEmpty()) {
                continue;
            }
            auto it = usages.find(value);
            if (it == usages.end()) {
                order.append(value);
                it = usages.insert(value, QStringList());
            }
            if (!it->contains(usage)) {
                it->append(usage);
            }
        }
    });
    QString text = QStringLiteral(
                       "// Generated by phosphor-facet-metadata from %1. Not compiled: lupdate reads "
                       "it.\n// clang-format off\n")
                       .arg(commentText(source));
    for (const QString& value : std::as_const(order)) {
        text += QStringLiteral("//: Facet module metadata: %1\nQT_TRANSLATE_NOOP(%2, %3)\n")
                    .arg(commentText(usages.value(value).join(QStringLiteral("; "))),
                         cString(QString(TRANSLATION_CONTEXT)), cString(value));
    }
    return write(out, text.toUtf8());
}

// One catalog: its language as the keys spell it, and source -> translation
// for the finished translations in context FacetModule.
struct Catalog
{
    QString path;
    QString language;
    QHash<QString, QString> messages;
};

// The text of the element the reader is on, up to its end tag. A
// `<byte value="xNN"/>` stands for character NN, which is how lupdate writes
// a control character. Any other child, a plural's <numerusform> or a
// <lengthvariant>, makes the text unusable and sets @p plain false. Never a
// reader error: a message the metadata does not use must not fail the build,
// whatever it holds.
QString elementText(QXmlStreamReader& xml, bool* plain)
{
    QString text;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isCharacters()) {
            text += xml.text();
        } else if (xml.isEndElement()) {
            break;
        } else if (xml.isStartElement()) {
            if (xml.name() == QLatin1String("byte")) {
                const QStringView value = xml.attributes().value(QLatin1String("value"));
                bool ok = false;
                const uint code =
                    value.startsWith(QLatin1Char('x')) ? value.sliced(1).toUInt(&ok, 16) : value.toUInt(&ok, 10);
                if (ok && code < 0x110000) {
                    text += QStringView(QChar::fromUcs4(char32_t(code)));
                } else {
                    *plain = false;
                }
            } else {
                *plain = false;
            }
            xml.skipCurrentElement();
        }
    }
    return text;
}

std::optional<Catalog> readCatalog(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("cannot read %1: %2").arg(path, file.errorString()));
        return std::nullopt;
    }
    Catalog catalog;
    catalog.path = path;
    QXmlStreamReader xml(&file);
    QString context;
    QString source;
    QString translation;
    bool usable = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == QLatin1String("TS")) {
                catalog.language = xml.attributes().value(QLatin1String("language")).toString();
                catalog.language.replace(QLatin1Char('-'), QLatin1Char('_'));
            } else if (name == QLatin1String("context")) {
                context.clear();
            } else if (name == QLatin1String("name")) {
                bool plain = true;
                context = elementText(xml, &plain);
            } else if (name == QLatin1String("message")) {
                source.clear();
                translation.clear();
                // The metadata's strings are never plural.
                usable = xml.attributes().value(QLatin1String("numerus")) != QLatin1String("yes");
            } else if (name == QLatin1String("source")) {
                bool plain = true;
                source = elementText(xml, &plain);
                usable = usable && plain;
            } else if (name == QLatin1String("comment")) {
                // A disambiguation makes it another message than the
                // metadata's, which carry none.
                bool plain = true;
                usable = usable && elementText(xml, &plain).isEmpty();
            } else if (name == QLatin1String("translation")) {
                // Finished only: an unfinished, vanished or obsolete one has
                // a type.
                usable = usable && xml.attributes().value(QLatin1String("type")).isEmpty();
                bool plain = true;
                translation = elementText(xml, &plain);
                usable = usable && plain;
            }
        } else if (xml.isEndElement() && xml.name() == QLatin1String("message")) {
            if (usable && context == TRANSLATION_CONTEXT && !source.isEmpty() && !translation.isEmpty()) {
                catalog.messages.insert(source, translation);
            }
        }
    }
    if (xml.hasError()) {
        fail(QStringLiteral("%1: %2").arg(path, xml.errorString()));
        return std::nullopt;
    }
    if (catalog.language.isEmpty()) {
        fail(QStringLiteral("%1 has no language attribute on <TS>").arg(path));
        return std::nullopt;
    }
    return catalog;
}

// The bare language a regional catalog may serve, or empty: `de` for de_DE,
// nothing for zh_TW, whose script is not zh's usual one, or for no_NO, whose
// language QLocale reads as nb. Only the script and the language code come
// from QLocale, never its name(), which would turn `de` into de_DE.
QString bareLanguage(const QString& language)
{
    const qsizetype underscore = language.indexOf(QLatin1Char('_'));
    if (underscore <= 0) {
        return QString();
    }
    const QString bare = language.left(underscore);
    const QLocale locale(language);
    if (locale.language() == QLocale::C || locale.language() == QLocale::AnyLanguage
        || QLocale::languageToCode(locale.language()) != bare
        || locale.script() != QLocale(locale.language()).script()) {
        return QString();
    }
    return bare;
}

bool localize(QJsonObject root, const QStringList& tsFiles, const QString& out)
{
    std::vector<Catalog> catalogs;
    for (const QString& path : tsFiles) {
        std::optional<Catalog> catalog = readCatalog(path);
        if (!catalog) {
            return false;
        }
        catalogs.push_back(std::move(*catalog));
    }
    // By language, then path, so neither the outcome nor the messages depend
    // on the order the build lists the files in.
    std::sort(catalogs.begin(), catalogs.end(), [](const Catalog& a, const Catalog& b) {
        return a.language != b.language ? a.language < b.language : a.path < b.path;
    });
    for (std::size_t i = 1; i < catalogs.size(); ++i) {
        if (catalogs[i].language == catalogs[i - 1].language) {
            fail(QStringLiteral("%1 and %2 are both catalogs for %3, and which one the metadata should use is "
                                "unclear")
                     .arg(catalogs[i - 1].path, catalogs[i].path, catalogs[i].language));
            return false;
        }
    }
    const QRegularExpression localePattern(QRegularExpression::anchoredPattern(QString(Keys::LocalePattern)));
    std::erase_if(catalogs, [&localePattern](const Catalog& catalog) {
        if (localePattern.match(catalog.language).hasMatch()) {
            return false;
        }
        warn(QStringLiteral("%1 is for language %2, which no locale matches, so the metadata leaves it out")
                 .arg(catalog.path, catalog.language));
        return true;
    });
    QHash<QString, int> claims;
    for (const Catalog& catalog : catalogs) {
        const QString bare = bareLanguage(catalog.language);
        ++claims[bare.isEmpty() ? catalog.language : bare];
    }
    struct Keyed
    {
        const Catalog* catalog;
        QString key;
    };
    std::vector<Keyed> keyed;
    for (const Catalog& catalog : catalogs) {
        const QString bare = bareLanguage(catalog.language);
        const bool ownsBare = !bare.isEmpty() && claims.value(bare) == 1;
        keyed.push_back({&catalog, ownsBare ? bare : catalog.language});
    }

    forEachField(root, [&](QJsonObject& object, const Field& field) {
        for (const Keyed& entry : keyed) {
            const QString localizedKey = field.key + QLatin1Char('[') + entry.key + QLatin1Char(']');
            // Written by hand, which wins over the catalog.
            if (object.contains(localizedKey)) {
                continue;
            }
            const QHash<QString, QString>& messages = entry.catalog->messages;
            if (field.list) {
                // Each keyword falls back to its source text on its own: a
                // keyword is a search term, and an untranslated one still
                // finds the page.
                QJsonArray translated;
                bool any = false;
                for (const QJsonValue& item : object.value(field.key).toArray()) {
                    const QString text = messages.value(item.toString());
                    any = any || !text.isEmpty();
                    translated.append(text.isEmpty() ? item : QJsonValue(text));
                }
                if (any) {
                    object.insert(localizedKey, translated);
                }
            } else {
                const QString text = messages.value(object.value(field.key).toString());
                if (!text.isEmpty()) {
                    object.insert(localizedKey, text);
                }
            }
        }
    });
    return write(out, QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool anchors(const FacetModuleMetadata& metadata, const QString& out)
{
    QJsonObject pages;
    for (const auto& page : metadata.pages) {
        QJsonObject entry;
        entry.insert(QLatin1String("Version"), metadata.slotVersion(page.id, QStringLiteral("top")));
        // Sorted, so reordering a page's anchors does not change the file.
        QStringList sorted = page.anchors;
        sorted.sort();
        entry.insert(QLatin1String("Anchors"), QJsonArray::fromStringList(sorted));
        // Keyed by id, which QJsonObject keeps sorted, so the file does not
        // depend on the order the metadata declares them in either.
        QJsonObject points;
        for (const auto& point : metadata.extensionPoints) {
            if (point.page == page.id && point.id != QLatin1String("anchors")) {
                points.insert(point.id, point.version);
            }
        }
        entry.insert(QLatin1String("ExtensionPoints"), points);
        pages.insert(page.id, entry);
    }
    QJsonObject root;
    root.insert(QLatin1String("Module"), metadata.id);
    root.insert(QLatin1String("Pages"), pages);
    return write(out, QJsonDocument(root).toJson(QJsonDocument::Indented));
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Generates a Facet module's metadata at build time."));
    parser.addHelpOption();
    const QCommandLineOption allowInvalid(QStringLiteral("allow-invalid"),
                                          QStringLiteral("Do not fail on invalid metadata (test fixtures)."));
    parser.addOption(allowInvalid);
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("strings, localize or anchors"));
    parser.addPositionalArgument(QStringLiteral("metadata"), QStringLiteral("The module's metadata.json"));
    parser.addPositionalArgument(QStringLiteral("output"), QStringLiteral("The file to write"));
    parser.addPositionalArgument(QStringLiteral("catalogs"), QStringLiteral("localize: the module's .ts files"),
                                 QStringLiteral("[file.ts...]"));
    parser.process(app);

    // The command line is checked before anything is read, so a wrong one is
    // reported as such and not as a problem with the metadata.
    const QStringList args = parser.positionalArguments();
    const QString command = args.value(0);
    const bool wellFormed = command == QLatin1String("localize")
        ? args.size() >= 3
        : (command == QLatin1String("strings") || command == QLatin1String("anchors")) && args.size() == 3;
    if (!wellFormed) {
        fail(QStringLiteral("unknown command or wrong arguments: %1\n\n%2")
                 .arg(args.join(QLatin1Char(' ')), parser.helpText()));
        return 2;
    }
    const QString& source = args.at(1);
    const QString& output = args.at(2);

    QJsonObject root;
    if (!readJson(source, &root)) {
        return 1;
    }
    QStringList errors;
    QStringList warnings;
    const FacetModuleMetadata metadata = FacetModuleMetadata::parse(root, QLocale::c(), &errors, &warnings);
    if (!errors.isEmpty() && !parser.isSet(allowInvalid)) {
        for (const QString& error : std::as_const(errors)) {
            fail(QStringLiteral("%1: %2").arg(source, error));
        }
        return 1;
    }

    bool ok = false;
    if (command == QLatin1String("strings")) {
        ok = strings(root, source, output);
    } else if (command == QLatin1String("localize")) {
        for (const QString& warning : std::as_const(warnings)) {
            warn(QStringLiteral("%1: %2").arg(source, warning));
        }
        ok = localize(root, args.mid(3), output);
    } else {
        ok = anchors(metadata, output);
    }
    return ok ? 0 : 1;
}
