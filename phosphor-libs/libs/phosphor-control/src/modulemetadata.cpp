// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorControl/ModuleMetadata.h"

#include "modulemetadatakeys_p.h"

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <span>

namespace PhosphorControl {

namespace {

using ModuleMetadataKeys::Localizable;
using KeyTable = std::span<const ModuleMetadataKeys::Key>;

// The same rule PluginCatalog applies to the "Id" it vets, so a module id
// that reached the host always passes here too. Anchored with
// anchoredPattern(), not ^...$: in PCRE `$` also matches before a final
// newline, which would let "general\n" through as an id.
const QRegularExpression& moduleIdPattern()
{
    static const QRegularExpression pattern(
        QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9][A-Za-z0-9._-]*")));
    return pattern;
}

// A page's anchor: the module id rule today, kept apart from it because the
// two name different things and either may change on its own.
const QRegularExpression& anchorPattern()
{
    static const QRegularExpression pattern(
        QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9][A-Za-z0-9._-]*")));
    return pattern;
}

// Page ids nest with '/', one or more segments that each start with a letter
// or digit, so `a//b`, `a/../b` and a trailing '/' are all refused. Never
// ':', which separates a module id from a page id in a cross-module parent.
const QRegularExpression& pageIdPattern()
{
    static const QRegularExpression pattern(QRegularExpression::anchoredPattern(
        QStringLiteral("[A-Za-z0-9][A-Za-z0-9._-]*(/[A-Za-z0-9][A-Za-z0-9._-]*)*")));
    return pattern;
}

const QRegularExpression& categoryIdPattern()
{
    static const QRegularExpression pattern(QRegularExpression::anchoredPattern(QStringLiteral("[a-z0-9][a-z0-9-]*")));
    return pattern;
}

// The `<locale>` of a `Key[<locale>]` variant that localeVariants() can
// produce. `Name[pt-BR]` or `Name[]` is never picked, whatever the locale.
const QRegularExpression& localeSuffixPattern()
{
    static const QRegularExpression pattern(
        QRegularExpression::anchoredPattern(QString(ModuleMetadataKeys::LocalePattern)));
    return pattern;
}

// The locale variants to try for a key, best first: the locale's UI
// languages, which Qt already lists from the most specific form to the bare
// language (pt-Latn-BR, pt-BR, pt-Latn, pt), with '-' turned into the '_'
// the metadata keys use.
QStringList localeVariants(const QLocale& locale)
{
    QStringList variants;
    for (QString language : locale.uiLanguages()) {
        language.replace(QLatin1Char('-'), QLatin1Char('_'));
        if (!language.isEmpty() && !variants.contains(language)) {
            variants.append(language);
        }
    }
    if (variants.isEmpty()) {
        variants.append(locale.name());
    }
    return variants;
}

QStringList stringList(const QJsonValue& value)
{
    QStringList list;
    const QJsonArray array = value.toArray();
    for (const QJsonValue& item : array) {
        if (item.isString() && !item.toString().isEmpty()) {
            list.append(item.toString());
        }
    }
    return list;
}

// The value of `key` for the locale: the first variant that holds a usable
// value, then `key` itself. A variant that is empty or not of the key's type
// is skipped, so an untranslated `"Name[de]": ""` falls back to `Name`
// instead of hiding it, and a `"Name[de]": [...]` cannot replace a string. A
// list counts as empty when it holds no non-empty string.
QJsonValue localized(const QJsonObject& json, QLatin1String key, Localizable kind, const QStringList& variants)
{
    for (const QString& variant : variants) {
        const auto it = json.constFind(key + QLatin1Char('[') + variant + QLatin1Char(']'));
        if (it != json.cend() && ModuleMetadataKeys::isUsable(*it, kind)) {
            return *it;
        }
    }
    return json.value(key);
}

// An optional integer key: absent is @p fallback, anything other than a
// whole number in int range is a problem.
int integerValue(const QJsonObject& json, QLatin1String key, int fallback, const QString& where, QStringList* problems)
{
    const auto it = json.constFind(key);
    if (it == json.cend()) {
        return fallback;
    }
    const double value = it->toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!it->isDouble() || std::trunc(value) != value || value < std::numeric_limits<int>::min()
        || value > std::numeric_limits<int>::max()) {
        problems->append(QStringLiteral("%1\"%2\" must be a whole number").arg(where, key));
        return fallback;
    }
    return static_cast<int>(value);
}

// An optional string key: absent is empty, anything other than a string is a
// problem.
QString stringValue(const QJsonObject& json, QLatin1String key, const QString& where, QStringList* problems)
{
    const auto it = json.constFind(key);
    if (it == json.cend()) {
        return QString();
    }
    if (!it->isString()) {
        problems->append(QStringLiteral("%1\"%2\" must be a string").arg(where, key));
        return QString();
    }
    return it->toString();
}

// An optional array key: absent is empty, anything other than an array is a
// problem, so a malformed list is reported against this module rather than
// surfacing later as an unexplained refusal of whatever relied on it.
QJsonArray arrayValue(const QJsonObject& json, QLatin1String key, const QString& where, QStringList* problems)
{
    const auto it = json.constFind(key);
    if (it == json.cend()) {
        return QJsonArray();
    }
    if (!it->isArray()) {
        problems->append(QStringLiteral("%1\"%2\" must be a list").arg(where, key));
        return QJsonArray();
    }
    return it->toArray();
}

// The objects of the top-level list @p array, read from @p key. An element
// that is not an object is a problem, named by its position, rather than an
// empty object whose missing id the checks after would report instead.
QList<QJsonObject> objectsIn(const QJsonArray& array, QLatin1String key, QStringList* problems)
{
    QList<QJsonObject> objects;
    for (qsizetype i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject()) {
            problems->append(QStringLiteral("\"%1\"[%2] must be an object").arg(key, QString::number(i)));
            continue;
        }
        objects.append(array.at(i).toObject());
    }
    return objects;
}

// Whether the base value of a localizable key, when present, is of its kind.
// A wrong one is a problem, which reports the key itself rather than leaving
// it to read as missing or empty.
bool baseKindOk(const QJsonObject& json, QLatin1String key, Localizable kind, const QString& where,
                QStringList* problems)
{
    const auto it = json.constFind(key);
    if (it == json.cend()) {
        return true;
    }
    if (kind == Localizable::String && !it->isString()) {
        problems->append(QStringLiteral("%1\"%2\" must be a string").arg(where, key));
        return false;
    }
    if (kind == Localizable::List && !it->isArray()) {
        problems->append(QStringLiteral("%1\"%2\" must be a list").arg(where, key));
        return false;
    }
    return true;
}

// Whether @p text holds a character a source string must not: a control
// character other than tab and newline, U+FFFE or U+FFFF, or half of a
// surrogate pair.
bool hasUnfitCharacter(QStringView text)
{
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c.unicode() < 0x20 && c != QLatin1Char('\t') && c != QLatin1Char('\n')) {
            return true;
        }
        if (c.unicode() == 0xfffe || c.unicode() == 0xffff) {
            return true;
        }
        if (c.isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            ++i;
        } else if (c.isSurrogate()) {
            return true;
        }
    }
    return false;
}

// The base values of the localizable keys in @p keys, which are the source
// strings lupdate extracts. An unfit character (hasUnfitCharacter()) is a
// problem. A control character is never text a user should see in a name,
// title or keyword, and lupdate can only carry one into a catalog as a
// `<byte>` element, which translation tools show as a code. lupdate writes
// U+FFFE and U+FFFF into the catalog as they are, which XML cannot hold, so
// lrelease and the tool refuse the whole catalog. A lone surrogate becomes
// U+FFFD in the strings file lupdate reads, so its message is never the
// metadata's and is never translated. A list's entries must be non-empty
// strings, as an `Anchors` list's must, rather than being dropped without a
// word. The `[<locale>]` variants are not checked: they never reach lupdate,
// and one that is not usable is skipped (localized()).
void checkLocalizableText(const QJsonObject& json, KeyTable keys, const QString& where, QStringList* problems)
{
    const auto unfitProblem = [&](QLatin1String key) {
        problems->append(QStringLiteral("%1\"%2\" holds a character it may not: a control character other than "
                                        "tab and newline, U+FFFE, U+FFFF or half of a surrogate pair")
                             .arg(where, key));
    };
    for (const ModuleMetadataKeys::Key& key : keys) {
        const QJsonValue value = json.value(key.name);
        if (key.localizable == Localizable::String && value.isString()) {
            if (hasUnfitCharacter(value.toString())) {
                unfitProblem(key.name);
            }
        } else if (key.localizable == Localizable::List && value.isArray()) {
            const QJsonArray items = value.toArray();
            for (const QJsonValue& item : items) {
                if (!item.isString() || item.toString().isEmpty()) {
                    problems->append(
                        QStringLiteral("%1every \"%2\" entry must be a non-empty string").arg(where, key.name));
                    break;
                }
                if (hasUnfitCharacter(item.toString())) {
                    unfitProblem(key.name);
                    break;
                }
            }
        }
    }
}

// Keys @p keys does not list, appended to @p notes. A `<key>[<locale>]`
// variant of a localizable key counts as known, unless its locale is not one
// parse() can ever pick, which gets a note of its own.
void noteUnknownKeys(const QJsonObject& json, KeyTable keys, const QString& where, QStringList* notes)
{
    for (auto it = json.cbegin(); it != json.cend(); ++it) {
        const QString key = it.key();
        const bool known = std::any_of(keys.begin(), keys.end(), [&key](const ModuleMetadataKeys::Key& candidate) {
            return key == candidate.name;
        });
        if (known) {
            continue;
        }
        const qsizetype bracket = key.indexOf(QLatin1Char('['));
        if (bracket > 0 && key.endsWith(QLatin1Char(']'))) {
            const QStringView base = QStringView(key).left(bracket);
            const bool localizable =
                std::any_of(keys.begin(), keys.end(), [base](const ModuleMetadataKeys::Key& candidate) {
                    return candidate.localizable != Localizable::No && base == candidate.name;
                });
            if (localizable) {
                const QStringView locale = QStringView(key).sliced(bracket + 1, key.size() - bracket - 2);
                if (!localeSuffixPattern().matchView(locale).hasMatch()) {
                    notes->append(QStringLiteral("%1\"%2\" is never picked: a locale is written with letters, digits "
                                                 "and '_', as in %3[pt_BR]")
                                      .arg(where, key, base));
                }
                continue;
            }
        }
        notes->append(QStringLiteral("%1unknown key \"%2\"").arg(where, key));
    }
}

struct CrossModuleRef
{
    QString module;
    QString page;
};

// `<module>:<page>`, or nullopt when @p ref is not of that form.
std::optional<CrossModuleRef> crossModuleRef(const QString& ref)
{
    const qsizetype colon = ref.indexOf(QLatin1Char(':'));
    if (colon <= 0) {
        return std::nullopt;
    }
    CrossModuleRef out{ref.left(colon), ref.mid(colon + 1)};
    if (!moduleIdPattern().match(out.module).hasMatch() || !pageIdPattern().match(out.page).hasMatch()) {
        return std::nullopt;
    }
    return out;
}

std::optional<PageRegistry::PageVisibility> visibilityFromString(const QString& text)
{
    if (text == QLatin1String("Always")) {
        return PageRegistry::PageVisibility::Always;
    }
    if (text == QLatin1String("AdvancedOnly")) {
        return PageRegistry::PageVisibility::AdvancedOnly;
    }
    if (text == QLatin1String("SimpleOnly")) {
        return PageRegistry::PageVisibility::SimpleOnly;
    }
    return std::nullopt;
}

} // namespace

ModuleMetadata ModuleMetadata::parse(const QJsonObject& json, const QLocale& locale, QStringList* errors,
                                     QStringList* warnings)
{
    QStringList problems;
    QStringList notes;
    ModuleMetadata metadata;
    const QStringList variants = localeVariants(locale);
    const QString top;

    // A localizable key is required on its base value, never on a variant:
    // metadata whose only name is `Name[de]` would otherwise load in German
    // and be refused in every other language, the build's C locale included.
    const auto hasBase = [](const QJsonObject& object, QLatin1String key) {
        return !object.value(key).toString().isEmpty();
    };

    const qsizetype idProblemsBefore = problems.size();
    metadata.id = stringValue(json, QLatin1String("Id"), top, &problems);
    if (problems.size() == idProblemsBefore && !moduleIdPattern().match(metadata.id).hasMatch()) {
        problems.append(QStringLiteral("\"Id\" must match [A-Za-z0-9][A-Za-z0-9._-]*"));
    }
    // PluginCatalog refuses a plugin without an integer ApiVersion for the
    // seam before the host sees it; parse() only reads it.
    metadata.apiVersion = integerValue(json, QLatin1String("ApiVersion"), 0, top, &problems);
    const bool nameKindOk = baseKindOk(json, QLatin1String("Name"), Localizable::String, top, &problems);
    metadata.name = localized(json, QLatin1String("Name"), Localizable::String, variants).toString();
    if (!hasBase(json, QLatin1String("Name")) && nameKindOk) {
        problems.append(QStringLiteral("\"Name\" is missing"));
    }
    baseKindOk(json, QLatin1String("Description"), Localizable::String, top, &problems);
    metadata.description = localized(json, QLatin1String("Description"), Localizable::String, variants).toString();
    metadata.icon = stringValue(json, QLatin1String("Icon"), top, &problems);
    metadata.category = stringValue(json, QLatin1String("Category"), top, &problems);
    if (!metadata.category.isEmpty() && !categoryIdPattern().match(metadata.category).hasMatch()) {
        problems.append(QStringLiteral("\"Category\" %1 must match [a-z0-9][a-z0-9-]*").arg(metadata.category));
    }
    metadata.parent = stringValue(json, QLatin1String("Parent"), top, &problems);
    if (!metadata.parent.isEmpty()) {
        if (!moduleIdPattern().match(metadata.parent).hasMatch()) {
            problems.append(QStringLiteral("\"Parent\" %1 is not a module id").arg(metadata.parent));
        } else if (metadata.parent == metadata.id) {
            problems.append(QStringLiteral("\"Parent\" names the module itself"));
        }
    }
    metadata.order = integerValue(json, QLatin1String("Order"), 0, top, &problems);
    baseKindOk(json, QLatin1String("Keywords"), Localizable::List, top, &problems);
    metadata.keywords = stringList(localized(json, QLatin1String("Keywords"), Localizable::List, variants));
    metadata.translationDomain = stringValue(json, QLatin1String("TranslationDomain"), top, &problems);
    checkLocalizableText(json, ModuleMetadataKeys::Module, top, &problems);
    noteUnknownKeys(json, ModuleMetadataKeys::Module, top, &notes);

    QSet<QString> categoryIds;
    const QList<QJsonObject> categories = objectsIn(arrayValue(json, QLatin1String("Categories"), top, &problems),
                                                    QLatin1String("Categories"), &problems);
    for (const QJsonObject& object : categories) {
        ModuleCategoryMetadata category;
        const qsizetype problemsBefore = problems.size();
        category.id = stringValue(object, QLatin1String("Id"), QStringLiteral("category: "), &problems);
        if (problems.size() != problemsBefore) {
            continue;
        }
        const QString where = QStringLiteral("category %1: ").arg(category.id);
        const bool categoryNameKindOk =
            baseKindOk(object, QLatin1String("Name"), Localizable::String, where, &problems);
        category.name = localized(object, QLatin1String("Name"), Localizable::String, variants).toString();
        category.icon = stringValue(object, QLatin1String("Icon"), where, &problems);
        category.order = integerValue(object, QLatin1String("Order"), 0, where, &problems);
        checkLocalizableText(object, ModuleMetadataKeys::Category, where, &problems);
        noteUnknownKeys(object, ModuleMetadataKeys::Category, where, &notes);
        if (!categoryIdPattern().match(category.id).hasMatch()) {
            problems.append(QStringLiteral("category id %1 must match [a-z0-9][a-z0-9-]*").arg(category.id));
            continue;
        }
        if (!hasBase(object, QLatin1String("Name"))) {
            if (categoryNameKindOk) {
                problems.append(QStringLiteral("category %1 has no \"Name\"").arg(category.id));
            }
            continue;
        }
        if (categoryIds.contains(category.id)) {
            problems.append(QStringLiteral("category %1 is defined twice").arg(category.id));
            continue;
        }
        categoryIds.insert(category.id);
        metadata.categories.append(category);
    }

    QList<ModulePageMetadata> declared;
    QSet<QString> pageIds;
    const QJsonValue pagesValue = json.value(QLatin1String("Pages"));
    if (!pagesValue.isArray() || pagesValue.toArray().isEmpty()) {
        // A module's own page is where the host says why it did not load and
        // offers to load it again, which a module that only adds sections to
        // other modules' pages needs as much as any other.
        problems.append(QStringLiteral("\"Pages\" must list at least one page"));
    }
    const QList<QJsonObject> pages = objectsIn(pagesValue.toArray(), QLatin1String("Pages"), &problems);
    for (const QJsonObject& object : pages) {
        ModulePageMetadata page;
        const qsizetype idProblems = problems.size();
        page.id = stringValue(object, QLatin1String("Id"), QStringLiteral("page: "), &problems);
        if (problems.size() != idProblems) {
            continue;
        }
        const QString where = QStringLiteral("page %1: ").arg(page.id);
        const bool pageNameKindOk = baseKindOk(object, QLatin1String("Name"), Localizable::String, where, &problems);
        page.name = localized(object, QLatin1String("Name"), Localizable::String, variants).toString();
        page.icon = stringValue(object, QLatin1String("Icon"), where, &problems);
        page.parent = stringValue(object, QLatin1String("Parent"), where, &problems);
        page.category = stringValue(object, QLatin1String("Category"), where, &problems);
        baseKindOk(object, QLatin1String("Keywords"), Localizable::List, where, &problems);
        page.keywords = stringList(localized(object, QLatin1String("Keywords"), Localizable::List, variants));
        page.counterpart = stringValue(object, QLatin1String("Counterpart"), where, &problems);
        // A bad anchor is reported and the page kept, so the search entries
        // and children that name the page do not each add an error of their
        // own. The module is refused either way.
        // A bad entry is reported once and skipped, so the anchors after it
        // are still read.
        QSet<QString> anchorSet;
        bool reportedNonString = false;
        const QJsonArray anchors = arrayValue(object, QLatin1String("Anchors"), where, &problems);
        for (const QJsonValue& value : anchors) {
            if (!value.isString() || value.toString().isEmpty()) {
                if (!reportedNonString) {
                    problems.append(QStringLiteral("%1every \"Anchors\" entry must be a non-empty string").arg(where));
                    reportedNonString = true;
                }
                continue;
            }
            const QString anchor = value.toString();
            if (!anchorPattern().match(anchor).hasMatch()) {
                problems.append(QStringLiteral("%1anchor %2 must match [A-Za-z0-9][A-Za-z0-9._-]*").arg(where, anchor));
            } else if (anchorSet.contains(anchor)) {
                problems.append(QStringLiteral("%1anchor %2 is listed twice").arg(where, anchor));
                continue;
            }
            anchorSet.insert(anchor);
            page.anchors.append(anchor);
        }
        if (const auto visibility = object.constFind(QLatin1String("Visibility")); visibility != object.cend()) {
            const auto parsed = visibilityFromString(visibility->toString());
            if (!visibility->isString() || !parsed) {
                problems.append(
                    QStringLiteral("%1\"Visibility\" must be Always, AdvancedOnly or SimpleOnly").arg(where));
            } else {
                page.visibility = *parsed;
            }
        }
        if (const auto divider = object.constFind(QLatin1String("DividerAfter")); divider != object.cend()) {
            if (!divider->isBool()) {
                problems.append(QStringLiteral("%1\"DividerAfter\" must be true or false").arg(where));
            } else {
                page.dividerAfter = divider->toBool();
            }
        }
        checkLocalizableText(object, ModuleMetadataKeys::Page, where, &problems);
        noteUnknownKeys(object, ModuleMetadataKeys::Page, where, &notes);
        if (!pageIdPattern().match(page.id).hasMatch()) {
            problems.append(QStringLiteral("page id %1 must be one or more [A-Za-z0-9][A-Za-z0-9._-]* segments joined "
                                           "by /")
                                .arg(page.id));
            continue;
        }
        if (!hasBase(object, QLatin1String("Name"))) {
            if (pageNameKindOk) {
                problems.append(QStringLiteral("page %1 has no \"Name\"").arg(page.id));
            }
            continue;
        }
        if (!page.parent.isEmpty() && !page.category.isEmpty()) {
            problems.append(QStringLiteral("page %1 sets both \"Parent\" and \"Category\"").arg(page.id));
            continue;
        }
        if (!page.category.isEmpty() && !categoryIdPattern().match(page.category).hasMatch()) {
            problems.append(
                QStringLiteral("page %1: \"Category\" %2 must match [a-z0-9][a-z0-9-]*").arg(page.id, page.category));
            continue;
        }
        if (pageIds.contains(page.id)) {
            problems.append(QStringLiteral("page %1 is declared twice").arg(page.id));
            continue;
        }
        pageIds.insert(page.id);
        declared.append(page);
    }

    // Validate parents and counterparts, then order the pages: a pre-order
    // walk of the module's page forest, roots and siblings each in
    // declaration order. Sibling order is the only order a module has, so the
    // walk keeps it, and lifting a parent ahead of its children never moves
    // it past a sibling declared before it.
    QHash<QString, QStringList> localChildren;
    QStringList roots;
    for (const ModulePageMetadata& page : std::as_const(declared)) {
        if (!page.counterpart.isEmpty()) {
            if (page.counterpart.contains(QLatin1Char(':'))) {
                const auto ref = crossModuleRef(page.counterpart);
                if (!ref || ref->module == metadata.id) {
                    problems.append(QStringLiteral("page %1 has counterpart %2, which is neither a page of this module "
                                                   "nor <other module>:<page>")
                                        .arg(page.id, page.counterpart));
                }
            } else if (!pageIds.contains(page.counterpart) || page.counterpart == page.id) {
                problems.append(QStringLiteral("page %1 has counterpart %2, which is not another page of this module")
                                    .arg(page.id, page.counterpart));
            }
        }
        if (page.parent.isEmpty()) {
            roots.append(page.id);
            continue;
        }
        if (page.parent.contains(QLatin1Char(':'))) {
            const auto ref = crossModuleRef(page.parent);
            if (!ref) {
                problems.append(
                    QStringLiteral("page %1 has parent %2, which is not <module>:<page>").arg(page.id, page.parent));
            } else if (ref->module == metadata.id) {
                problems.append(QStringLiteral("page %1 has parent %2, which names its own module instead of using "
                                               "the local page id")
                                    .arg(page.id, page.parent));
            }
            roots.append(page.id);
            continue;
        }
        if (!pageIds.contains(page.parent)) {
            problems.append(
                QStringLiteral("page %1 has parent %2, which the module does not declare").arg(page.id, page.parent));
            roots.append(page.id);
            continue;
        }
        localChildren[page.parent].append(page.id);
    }
    // Every declared page, the ones in a loop of local parents included, so
    // what names one of those is not also told the page does not exist.
    QHash<QString, const ModulePageMetadata*> byId;
    for (const ModulePageMetadata& page : std::as_const(declared)) {
        byId.insert(page.id, &page);
    }
    // Iterative, so a long parent chain in a plugin's JSON cannot overflow
    // the stack.
    QStringList stack;
    for (auto it = roots.crbegin(); it != roots.crend(); ++it) {
        stack.append(*it);
    }
    QSet<QString> placed;
    while (!stack.isEmpty()) {
        const QString id = stack.takeLast();
        placed.insert(id);
        metadata.pages.append(*byId.value(id));
        const QStringList children = localChildren.value(id);
        for (auto it = children.crbegin(); it != children.crend(); ++it) {
            stack.append(*it);
        }
    }
    // A page the walk never reached is in, or under, a loop of local
    // parents.
    for (const ModulePageMetadata& page : std::as_const(declared)) {
        if (!placed.contains(page.id)) {
            problems.append(QStringLiteral("page %1 is in or under a loop of local parents").arg(page.id));
        }
    }

    QSet<QString> pointKeys;
    const QList<QJsonObject> points = objectsIn(arrayValue(json, QLatin1String("ExtensionPoints"), top, &problems),
                                                QLatin1String("ExtensionPoints"), &problems);
    for (const QJsonObject& object : points) {
        ModuleExtensionPointMetadata point;
        const qsizetype idProblems = problems.size();
        point.page = stringValue(object, QLatin1String("Page"), QStringLiteral("extension point: "), &problems);
        point.id = stringValue(object, QLatin1String("Id"), QStringLiteral("extension point: "), &problems);
        if (problems.size() != idProblems) {
            continue;
        }
        const QString where = QStringLiteral("extension point %1 on page %2: ").arg(point.id, point.page);
        point.version = integerValue(object, QLatin1String("Version"), 1, where, &problems);
        noteUnknownKeys(object, ModuleMetadataKeys::ExtensionPoint, where, &notes);
        if (!pageIds.contains(point.page)) {
            problems.append(QStringLiteral("extension point %1 is on page %2, which the module does not declare")
                                .arg(point.id, point.page));
            continue;
        }
        if (!categoryIdPattern().match(point.id).hasMatch() || point.id == QLatin1String("top")
            || point.id == QLatin1String("bottom")) {
            problems.append(QStringLiteral("extension point id %1 must match [a-z0-9][a-z0-9-]* and not be top or "
                                           "bottom")
                                .arg(point.id));
            continue;
        }
        if (point.version < 1) {
            problems.append(QStringLiteral("extension point %1 has version %2, which is below 1")
                                .arg(point.id, QString::number(point.version)));
            continue;
        }
        const QString key = point.page + QLatin1Char('|') + point.id;
        if (pointKeys.contains(key)) {
            problems.append(
                QStringLiteral("extension point %1 is declared twice on page %2").arg(point.id, point.page));
            continue;
        }
        pointKeys.insert(key);
        metadata.extensionPoints.append(point);
    }

    QSet<QString> extensionIds;
    const QList<QJsonObject> extensions = objectsIn(arrayValue(json, QLatin1String("Extensions"), top, &problems),
                                                    QLatin1String("Extensions"), &problems);
    for (const QJsonObject& object : extensions) {
        ModuleExtensionMetadata extension;
        // A key of the wrong type is reported once, by the reader below,
        // rather than again by the checks that would read its empty value.
        const qsizetype problemsBefore = problems.size();
        extension.id = stringValue(object, QLatin1String("Id"), QStringLiteral("extension: "), &problems);
        if (problems.size() != problemsBefore) {
            continue;
        }
        const QString where = QStringLiteral("extension %1: ").arg(extension.id);
        extension.target = stringValue(object, QLatin1String("Target"), where, &problems);
        extension.slot = stringValue(object, QLatin1String("Slot"), where, &problems);
        extension.targetVersion = integerValue(object, QLatin1String("TargetVersion"), 1, where, &problems);
        baseKindOk(object, QLatin1String("Name"), Localizable::String, where, &problems);
        baseKindOk(object, QLatin1String("Keywords"), Localizable::List, where, &problems);
        extension.name = localized(object, QLatin1String("Name"), Localizable::String, variants).toString();
        extension.keywords = stringList(localized(object, QLatin1String("Keywords"), Localizable::List, variants));
        extension.order = integerValue(object, QLatin1String("Order"), 0, where, &problems);
        checkLocalizableText(object, ModuleMetadataKeys::Extension, where, &problems);
        noteUnknownKeys(object, ModuleMetadataKeys::Extension, where, &notes);
        if (problems.size() != problemsBefore) {
            continue;
        }
        if (!moduleIdPattern().match(extension.id).hasMatch()) {
            problems.append(QStringLiteral("extension id %1 must match [A-Za-z0-9][A-Za-z0-9._-]*").arg(extension.id));
            continue;
        }
        if (extensionIds.contains(extension.id)) {
            problems.append(QStringLiteral("extension %1 is declared twice").arg(extension.id));
            continue;
        }
        const auto target = crossModuleRef(extension.target);
        if (!target) {
            problems.append(QStringLiteral("extension %1 targets %2, which is not <module>:<page>")
                                .arg(extension.id, extension.target));
            continue;
        }
        if (target->module == metadata.id) {
            problems.append(QStringLiteral("extension %1 targets its own module, which adds sections to its own "
                                           "pages directly")
                                .arg(extension.id));
            continue;
        }
        // Only its presence is checked here. Whether the target page offers
        // the slot is ModuleHost's call, which refuses that one extension with
        // a reason, so a slot form a newer PhosphorControl adds costs the module the
        // section and not its pages.
        if (extension.slot.isEmpty()) {
            problems.append(QStringLiteral("extension %1 has no \"Slot\"").arg(extension.id));
            continue;
        }
        if (extension.targetVersion < 1) {
            problems.append(QStringLiteral("extension %1 has TargetVersion %2, which is below 1")
                                .arg(extension.id, QString::number(extension.targetVersion)));
            continue;
        }
        if (!hasBase(object, QLatin1String("Name"))) {
            problems.append(QStringLiteral("extension %1 has no \"Name\"").arg(extension.id));
            continue;
        }
        extensionIds.insert(extension.id);
        metadata.extensions.append(extension);
    }

    // The page among @p pageId and its local parents that shows only in
    // simple mode, or empty. PageRegistry hides a page in a mode when any of
    // its parents is hidden in it.
    const auto simpleOnlyPage = [&byId](const QString& pageId) {
        QSet<QString> seen;
        for (const ModulePageMetadata* page = byId.value(pageId); page && !seen.contains(page->id);
             page = byId.value(page->parent)) {
            seen.insert(page->id);
            if (page->visibility == PageRegistry::PageVisibility::SimpleOnly) {
                return page->id;
            }
        }
        return QString();
    };
    QSet<QString> searchKeys;
    const QList<QJsonObject> search =
        objectsIn(arrayValue(json, QLatin1String("Search"), top, &problems), QLatin1String("Search"), &problems);
    for (const QJsonObject& object : search) {
        ModuleSearchEntryMetadata entry;
        const QString where =
            QStringLiteral("search entry %1#%2: ")
                .arg(object.value(QLatin1String("Page")).toString(), object.value(QLatin1String("Anchor")).toString());
        // A key of the wrong type is reported once, by the reader below,
        // rather than again by the checks that would read its empty value.
        const qsizetype problemsBefore = problems.size();
        entry.page = stringValue(object, QLatin1String("Page"), where, &problems);
        entry.anchor = stringValue(object, QLatin1String("Anchor"), where, &problems);
        const QString kind = object.contains(QLatin1String("Kind"))
            ? stringValue(object, QLatin1String("Kind"), where, &problems)
            : QStringLiteral("Setting");
        baseKindOk(object, QLatin1String("Title"), Localizable::String, where, &problems);
        baseKindOk(object, QLatin1String("Keywords"), Localizable::List, where, &problems);
        entry.title = localized(object, QLatin1String("Title"), Localizable::String, variants).toString();
        entry.keywords = stringList(localized(object, QLatin1String("Keywords"), Localizable::List, variants));
        if (const auto advanced = object.constFind(QLatin1String("AdvancedOnly")); advanced != object.cend()) {
            if (!advanced->isBool()) {
                problems.append(QStringLiteral("%1\"AdvancedOnly\" must be true or false").arg(where));
            } else {
                entry.advancedOnly = advanced->toBool();
            }
        }
        checkLocalizableText(object, ModuleMetadataKeys::SearchEntry, where, &problems);
        noteUnknownKeys(object, ModuleMetadataKeys::SearchEntry, where, &notes);
        if (problems.size() != problemsBefore) {
            continue;
        }
        const QString at = entry.page + QLatin1Char('#') + entry.anchor;
        // A kind a newer PhosphorControl added. What that version asks of one is unknown
        // here, so the entry is left out with a warning rather than checked
        // against this PhosphorControl's rules and costing the module its pages.
        if (kind != QLatin1String("Setting") && kind != QLatin1String("Section")) {
            notes.append(
                QStringLiteral("search entry %1 has \"Kind\" %2, which this PhosphorControl does not know, so it is "
                               "left out")
                    .arg(at, kind));
            continue;
        }
        entry.section = kind == QLatin1String("Section");
        if (entry.page.isEmpty()) {
            problems.append(QStringLiteral("search entry %1 has no \"Page\"").arg(at));
            continue;
        }
        if (entry.anchor.isEmpty()) {
            problems.append(QStringLiteral("search entry %1 has no \"Anchor\"").arg(at));
            continue;
        }
        const ModulePageMetadata* const target = byId.value(entry.page);
        if (!target) {
            problems.append(
                QStringLiteral("search entry %1 is on page %2, which the module does not declare").arg(at, entry.page));
            continue;
        }
        if (!target->anchors.contains(entry.anchor)) {
            problems.append(
                QStringLiteral("search entry %1 names anchor %2, which page %3 does not list in \"Anchors\"")
                    .arg(at, entry.anchor, entry.page));
            continue;
        }
        const QString baseTitle = object.value(QLatin1String("Title")).toString();
        if (baseTitle.isEmpty()) {
            problems.append(QStringLiteral("search entry %1 has no \"Title\"").arg(at));
            continue;
        }
        if (entry.advancedOnly) {
            if (const QString simpleOnly = simpleOnlyPage(entry.page); !simpleOnly.isEmpty()) {
                problems.append(QStringLiteral("search entry %1 is AdvancedOnly, but page %2 shows only in simple "
                                               "mode, so search never offers it")
                                    .arg(at, simpleOnly));
                continue;
            }
        }
        // The unit separator cannot occur in a page id or an anchor, so two
        // different entries never share a key.
        const QString searchKey = at + QLatin1Char('\x1f') + baseTitle;
        if (searchKeys.contains(searchKey)) {
            problems.append(QStringLiteral("search entry %1 titled %2 is declared twice").arg(at, baseTitle));
            continue;
        }
        searchKeys.insert(searchKey);
        metadata.search.append(entry);
    }
    if (const auto providers = json.constFind(QLatin1String("SearchProviders")); providers != json.cend()) {
        if (!providers->isBool()) {
            problems.append(QStringLiteral("\"SearchProviders\" must be true or false"));
        } else {
            metadata.searchProviders = providers->toBool();
        }
    }

    if (errors) {
        errors->append(problems);
    }
    if (warnings) {
        warnings->append(notes);
    }
    return metadata;
}

const ModuleExtensionMetadata* ModuleMetadata::extension(const QString& id) const
{
    for (const ModuleExtensionMetadata& candidate : extensions) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

int ModuleMetadata::slotVersion(const QString& pageId, const QString& slot) const
{
    const ModulePageMetadata* const target = page(pageId);
    if (!target) {
        return 0;
    }
    int anchorsVersion = 1;
    for (const ModuleExtensionPointMetadata& point : extensionPoints) {
        if (point.page == pageId && point.id == QLatin1String("anchors")) {
            anchorsVersion = point.version;
        }
    }
    if (slot == QLatin1String("top") || slot == QLatin1String("bottom")) {
        return anchorsVersion;
    }
    if (slot.startsWith(QLatin1String("before:")) || slot.startsWith(QLatin1String("after:"))) {
        return target->anchors.contains(slot.section(QLatin1Char(':'), 1)) ? anchorsVersion : 0;
    }
    for (const ModuleExtensionPointMetadata& point : extensionPoints) {
        if (point.page == pageId && point.id == slot && point.id != QLatin1String("anchors")) {
            return point.version;
        }
    }
    return 0;
}

const ModulePageMetadata* ModuleMetadata::page(const QString& id) const
{
    for (const ModulePageMetadata& candidate : pages) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace PhosphorControl
