// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorControl/FacetModuleMetadata.h"

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace PhosphorControl {

namespace {

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

// What a localizable key holds: a string (Name, Description) or an array of
// strings (Keywords).
enum class LocalizedKind : quint8 {
    String,
    Array,
};

// The value of `key` for the locale: the first variant that holds a usable
// value, then `key` itself. A variant that is empty or not of the key's type
// is skipped, so an untranslated `"Name[de]": ""` falls back to `Name`
// instead of hiding it, and a `"Name[de]": [...]` cannot replace a string. A
// list counts as empty when it holds no non-empty string.
QJsonValue localized(const QJsonObject& json, QLatin1String key, LocalizedKind kind, const QStringList& variants)
{
    const auto usable = [kind](const QJsonValue& value) {
        if (kind == LocalizedKind::String) {
            return value.isString() && !value.toString().isEmpty();
        }
        return !stringList(value).isEmpty();
    };
    for (const QString& variant : variants) {
        const auto it = json.constFind(key + QLatin1Char('[') + variant + QLatin1Char(']'));
        if (it != json.cend() && usable(*it)) {
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

// Whether the base value of a localizable key, when present, is of its kind.
// A wrong one is a problem, which reports the key itself rather than leaving
// it to read as missing or empty.
bool baseKindOk(const QJsonObject& json, QLatin1String key, LocalizedKind kind, const QString& where,
                QStringList* problems)
{
    const auto it = json.constFind(key);
    if (it == json.cend()) {
        return true;
    }
    if (kind == LocalizedKind::String && !it->isString()) {
        problems->append(QStringLiteral("%1\"%2\" must be a string").arg(where, key));
        return false;
    }
    if (kind == LocalizedKind::Array && !it->isArray()) {
        problems->append(QStringLiteral("%1\"%2\" must be a list").arg(where, key));
        return false;
    }
    return true;
}

// Keys parse() does not know, for the warnings list. A `<known>[<locale>]`
// variant of a localizable key counts as known.
QStringList unknownKeys(const QJsonObject& json, const QList<QLatin1String>& known,
                        const QList<QLatin1String>& localizable)
{
    const auto listed = [](const QList<QLatin1String>& keys, QStringView key) {
        return std::any_of(keys.cbegin(), keys.cend(), [key](QLatin1String candidate) {
            return key == candidate;
        });
    };
    QStringList unknown;
    for (auto it = json.cbegin(); it != json.cend(); ++it) {
        const QString key = it.key();
        bool isKnown = listed(known, key);
        if (!isKnown && key.endsWith(QLatin1Char(']'))) {
            const qsizetype bracket = key.indexOf(QLatin1Char('['));
            isKnown = bracket > 0 && listed(localizable, QStringView(key).left(bracket));
        }
        if (!isKnown) {
            unknown.append(key);
        }
    }
    return unknown;
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

FacetModuleMetadata FacetModuleMetadata::parse(const QJsonObject& json, const QLocale& locale, QStringList* errors,
                                               QStringList* warnings)
{
    QStringList problems;
    QStringList notes;
    FacetModuleMetadata metadata;
    const QStringList variants = localeVariants(locale);
    const QString top;

    metadata.id = stringValue(json, QLatin1String("Id"), top, &problems);
    if (!moduleIdPattern().match(metadata.id).hasMatch()) {
        problems.append(QStringLiteral("\"Id\" must match [A-Za-z0-9][A-Za-z0-9._-]*"));
    }
    // PluginCatalog refuses a plugin without an integer ApiVersion for the
    // seam before the host sees it; parse() only reads it.
    metadata.apiVersion = integerValue(json, QLatin1String("ApiVersion"), 0, top, &problems);
    const bool nameKindOk = baseKindOk(json, QLatin1String("Name"), LocalizedKind::String, top, &problems);
    metadata.name = localized(json, QLatin1String("Name"), LocalizedKind::String, variants).toString();
    if (metadata.name.isEmpty() && nameKindOk) {
        problems.append(QStringLiteral("\"Name\" is missing"));
    }
    baseKindOk(json, QLatin1String("Description"), LocalizedKind::String, top, &problems);
    metadata.description = localized(json, QLatin1String("Description"), LocalizedKind::String, variants).toString();
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
    baseKindOk(json, QLatin1String("Keywords"), LocalizedKind::Array, top, &problems);
    metadata.keywords = stringList(localized(json, QLatin1String("Keywords"), LocalizedKind::Array, variants));
    metadata.translationDomain = stringValue(json, QLatin1String("TranslationDomain"), top, &problems);
    for (const QString& key : unknownKeys(
             json,
             {QLatin1String("Id"), QLatin1String("ApiVersion"), QLatin1String("Name"), QLatin1String("Description"),
              QLatin1String("Icon"), QLatin1String("Category"), QLatin1String("Parent"), QLatin1String("Order"),
              QLatin1String("Keywords"), QLatin1String("TranslationDomain"), QLatin1String("Categories"),
              QLatin1String("Pages"), QLatin1String("ExtensionPoints"), QLatin1String("Extensions")},
             {QLatin1String("Name"), QLatin1String("Description"), QLatin1String("Keywords")})) {
        notes.append(QStringLiteral("unknown key \"%1\"").arg(key));
    }

    QSet<QString> categoryIds;
    const QJsonArray categories = arrayValue(json, QLatin1String("Categories"), top, &problems);
    for (const QJsonValue& value : categories) {
        const QJsonObject object = value.toObject();
        FacetCategoryMetadata category;
        category.id = object.value(QLatin1String("Id")).toString();
        const QString where = QStringLiteral("category %1: ").arg(category.id);
        const bool categoryNameKindOk =
            baseKindOk(object, QLatin1String("Name"), LocalizedKind::String, where, &problems);
        category.name = localized(object, QLatin1String("Name"), LocalizedKind::String, variants).toString();
        category.icon = stringValue(object, QLatin1String("Icon"), where, &problems);
        category.order = integerValue(object, QLatin1String("Order"), 0, where, &problems);
        for (const QString& key : unknownKeys(
                 object, {QLatin1String("Id"), QLatin1String("Name"), QLatin1String("Icon"), QLatin1String("Order")},
                 {QLatin1String("Name")})) {
            notes.append(QStringLiteral("%1unknown key \"%2\"").arg(where, key));
        }
        if (!categoryIdPattern().match(category.id).hasMatch()) {
            problems.append(QStringLiteral("category id %1 must match [a-z0-9][a-z0-9-]*").arg(category.id));
            continue;
        }
        if (category.name.isEmpty()) {
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

    QList<FacetPageMetadata> declared;
    QSet<QString> pageIds;
    const QJsonValue pagesValue = json.value(QLatin1String("Pages"));
    const QJsonArray pages = pagesValue.toArray();
    if (!pagesValue.isArray() || pages.isEmpty()) {
        // A module's own page is where Facet says why it did not load and
        // offers to load it again, which a module that only adds sections to
        // other modules' pages needs as much as any other.
        problems.append(QStringLiteral("\"Pages\" must list at least one page"));
    }
    for (const QJsonValue& value : pages) {
        const QJsonObject object = value.toObject();
        FacetPageMetadata page;
        page.id = object.value(QLatin1String("Id")).toString();
        const QString where = QStringLiteral("page %1: ").arg(page.id);
        const bool pageNameKindOk = baseKindOk(object, QLatin1String("Name"), LocalizedKind::String, where, &problems);
        page.name = localized(object, QLatin1String("Name"), LocalizedKind::String, variants).toString();
        page.icon = stringValue(object, QLatin1String("Icon"), where, &problems);
        page.parent = stringValue(object, QLatin1String("Parent"), where, &problems);
        page.category = stringValue(object, QLatin1String("Category"), where, &problems);
        baseKindOk(object, QLatin1String("Keywords"), LocalizedKind::Array, where, &problems);
        page.keywords = stringList(localized(object, QLatin1String("Keywords"), LocalizedKind::Array, variants));
        page.counterpart = stringValue(object, QLatin1String("Counterpart"), where, &problems);
        const QJsonArray anchors = arrayValue(object, QLatin1String("Anchors"), where, &problems);
        for (const QJsonValue& anchor : anchors) {
            if (!anchor.isString() || anchor.toString().isEmpty()) {
                problems.append(QStringLiteral("%1every \"Anchors\" entry must be a non-empty string").arg(where));
                break;
            }
            page.anchors.append(anchor.toString());
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
        for (const QString& key :
             unknownKeys(object,
                         {QLatin1String("Id"), QLatin1String("Name"), QLatin1String("Icon"), QLatin1String("Parent"),
                          QLatin1String("Category"), QLatin1String("Keywords"), QLatin1String("Visibility"),
                          QLatin1String("DividerAfter"), QLatin1String("Counterpart"), QLatin1String("Anchors")},
                         {QLatin1String("Name"), QLatin1String("Keywords")})) {
            notes.append(QStringLiteral("%1unknown key \"%2\"").arg(where, key));
        }
        if (!pageIdPattern().match(page.id).hasMatch()) {
            problems.append(QStringLiteral("page id %1 must be one or more [A-Za-z0-9][A-Za-z0-9._-]* segments joined "
                                           "by /")
                                .arg(page.id));
            continue;
        }
        if (page.name.isEmpty()) {
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
    for (const FacetPageMetadata& page : std::as_const(declared)) {
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
    QHash<QString, const FacetPageMetadata*> byId;
    for (const FacetPageMetadata& page : std::as_const(declared)) {
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
    for (const FacetPageMetadata& page : std::as_const(declared)) {
        if (!placed.contains(page.id)) {
            problems.append(QStringLiteral("page %1 is in or under a loop of local parents").arg(page.id));
        }
    }

    QSet<QString> pointKeys;
    const QJsonArray points = arrayValue(json, QLatin1String("ExtensionPoints"), top, &problems);
    for (const QJsonValue& value : points) {
        const QJsonObject object = value.toObject();
        FacetExtensionPointMetadata point;
        point.page = object.value(QLatin1String("Page")).toString();
        point.id = object.value(QLatin1String("Id")).toString();
        const QString where = QStringLiteral("extension point %1 on page %2: ").arg(point.id, point.page);
        point.version = integerValue(object, QLatin1String("Version"), 1, where, &problems);
        for (const QString& key :
             unknownKeys(object, {QLatin1String("Page"), QLatin1String("Id"), QLatin1String("Version")}, {})) {
            notes.append(QStringLiteral("%1unknown key \"%2\"").arg(where, key));
        }
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
    const QJsonArray extensions = arrayValue(json, QLatin1String("Extensions"), top, &problems);
    for (const QJsonValue& value : extensions) {
        const QJsonObject object = value.toObject();
        FacetExtensionMetadata extension;
        extension.id = object.value(QLatin1String("Id")).toString();
        const QString where = QStringLiteral("extension %1: ").arg(extension.id);
        // A key of the wrong type is reported once, by the reader below,
        // rather than again by the checks that would read its empty value.
        const qsizetype problemsBefore = problems.size();
        extension.target = stringValue(object, QLatin1String("Target"), where, &problems);
        extension.slot = stringValue(object, QLatin1String("Slot"), where, &problems);
        extension.targetVersion = integerValue(object, QLatin1String("TargetVersion"), 1, where, &problems);
        baseKindOk(object, QLatin1String("Name"), LocalizedKind::String, where, &problems);
        baseKindOk(object, QLatin1String("Keywords"), LocalizedKind::Array, where, &problems);
        extension.name = localized(object, QLatin1String("Name"), LocalizedKind::String, variants).toString();
        extension.keywords = stringList(localized(object, QLatin1String("Keywords"), LocalizedKind::Array, variants));
        extension.order = integerValue(object, QLatin1String("Order"), 0, where, &problems);
        for (const QString& key : unknownKeys(object,
                                              {QLatin1String("Id"), QLatin1String("Target"), QLatin1String("Slot"),
                                               QLatin1String("TargetVersion"), QLatin1String("Name"),
                                               QLatin1String("Keywords"), QLatin1String("Order")},
                                              {QLatin1String("Name"), QLatin1String("Keywords")})) {
            notes.append(QStringLiteral("%1unknown key \"%2\"").arg(where, key));
        }
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
        // the slot is FacetHost's call, which refuses that one extension with
        // a reason, so a slot form a newer Facet adds costs the module the
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
        if (extension.name.isEmpty()) {
            problems.append(QStringLiteral("extension %1 has no \"Name\"").arg(extension.id));
            continue;
        }
        extensionIds.insert(extension.id);
        metadata.extensions.append(extension);
    }

    if (errors) {
        errors->append(problems);
    }
    if (warnings) {
        warnings->append(notes);
    }
    return metadata;
}

const FacetExtensionMetadata* FacetModuleMetadata::extension(const QString& id) const
{
    for (const FacetExtensionMetadata& candidate : extensions) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

int FacetModuleMetadata::slotVersion(const QString& pageId, const QString& slot) const
{
    const FacetPageMetadata* const target = page(pageId);
    if (!target) {
        return 0;
    }
    int anchorsVersion = 1;
    for (const FacetExtensionPointMetadata& point : extensionPoints) {
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
    for (const FacetExtensionPointMetadata& point : extensionPoints) {
        if (point.page == pageId && point.id == slot && point.id != QLatin1String("anchors")) {
            return point.version;
        }
    }
    return 0;
}

const FacetPageMetadata* FacetModuleMetadata::page(const QString& id) const
{
    for (const FacetPageMetadata& candidate : pages) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace PhosphorControl
