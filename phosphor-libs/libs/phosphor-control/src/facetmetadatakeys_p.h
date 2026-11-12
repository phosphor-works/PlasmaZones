// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// The keys of a Facet module's metadata, and which of them are localizable.
//
// FacetModuleMetadata::parse() reads this table to tell a known key from an
// unknown one, and the phosphor-facet-metadata tool reads it to find the
// strings it hands to lupdate and localizes from the module's catalogs. One
// table, so a key the parser learns is a key the tool translates. Private:
// the tool is built from this source tree and nothing else includes it.

#include <QJsonArray>
#include <QJsonValue>
#include <QLatin1String>
#include <QtTypes>

#include <algorithm>
#include <span>

namespace PhosphorControl::FacetMetadataKeys {

/// The `<locale>` of a `Key[<locale>]` variant that parse() can pick:
/// letters and digits in '_'-joined parts, the form of the locale variants
/// it tries (`pt_BR`, `zh_Hant_TW`, `de`). The tool writes no other.
inline constexpr QLatin1String LocalePattern("[A-Za-z0-9]+(_[A-Za-z0-9]+)*");

/// What a key holds, as far as localization goes.
enum class Localizable : quint8 {
    No,
    /// A string, with `Key[<locale>]` variants.
    String,
    /// A list of strings, with `Key[<locale>]` variants.
    List,
};

/// Whether @p value can stand for a localizable key of kind @p kind: a
/// non-empty string, or a list holding at least one non-empty string. A
/// `Key[<locale>]` variant that is not is skipped by parse(), and the tool
/// does not count one as a translation written by hand.
inline bool isUsable(const QJsonValue& value, Localizable kind)
{
    if (kind == Localizable::String) {
        return value.isString() && !value.toString().isEmpty();
    }
    if (kind == Localizable::List && value.isArray()) {
        const QJsonArray items = value.toArray();
        return std::any_of(items.begin(), items.end(), [](const QJsonValue& item) {
            return item.isString() && !item.toString().isEmpty();
        });
    }
    return false;
}

struct Key
{
    QLatin1String name;
    Localizable localizable = Localizable::No;
};

/// The keys of the metadata's top-level object, the module itself.
inline constexpr Key Module[] = {
    {QLatin1String("Id")},
    {QLatin1String("ApiVersion")},
    {QLatin1String("Name"), Localizable::String},
    {QLatin1String("Description"), Localizable::String},
    {QLatin1String("Icon")},
    {QLatin1String("Category")},
    {QLatin1String("Parent")},
    {QLatin1String("Order")},
    {QLatin1String("Keywords"), Localizable::List},
    {QLatin1String("TranslationDomain")},
    {QLatin1String("Categories")},
    {QLatin1String("Pages")},
    {QLatin1String("ExtensionPoints")},
    {QLatin1String("Extensions")},
    {QLatin1String("Search")},
    {QLatin1String("SearchProviders")},
};

inline constexpr Key Category[] = {
    {QLatin1String("Id")},
    {QLatin1String("Name"), Localizable::String},
    {QLatin1String("Icon")},
    {QLatin1String("Order")},
};

inline constexpr Key Page[] = {
    {QLatin1String("Id")},          {QLatin1String("Name"), Localizable::String},
    {QLatin1String("Icon")},        {QLatin1String("Parent")},
    {QLatin1String("Category")},    {QLatin1String("Keywords"), Localizable::List},
    {QLatin1String("Visibility")},  {QLatin1String("DividerAfter")},
    {QLatin1String("Counterpart")}, {QLatin1String("Anchors")},
};

inline constexpr Key ExtensionPoint[] = {
    {QLatin1String("Page")},
    {QLatin1String("Id")},
    {QLatin1String("Version")},
};

inline constexpr Key Extension[] = {
    {QLatin1String("Id")},
    {QLatin1String("Target")},
    {QLatin1String("Slot")},
    {QLatin1String("TargetVersion")},
    {QLatin1String("Name"), Localizable::String},
    {QLatin1String("Keywords"), Localizable::List},
    {QLatin1String("Order")},
};

inline constexpr Key SearchEntry[] = {
    {QLatin1String("Page")},
    {QLatin1String("Anchor")},
    {QLatin1String("Kind")},
    {QLatin1String("Title"), Localizable::String},
    {QLatin1String("Keywords"), Localizable::List},
    {QLatin1String("AdvancedOnly")},
};

/// A top-level key holding a list of objects, with the keys of those objects
/// and the word the tool's translator comments call one of them.
struct ObjectList
{
    QLatin1String key;
    QLatin1String label;
    std::span<const Key> keys;
};

inline constexpr ObjectList ObjectLists[] = {
    {QLatin1String("Categories"), QLatin1String("category"), Category},
    {QLatin1String("Pages"), QLatin1String("page"), Page},
    {QLatin1String("ExtensionPoints"), QLatin1String("extension point"), ExtensionPoint},
    {QLatin1String("Extensions"), QLatin1String("extension"), Extension},
    {QLatin1String("Search"), QLatin1String("search entry"), SearchEntry},
};

} // namespace PhosphorControl::FacetMetadataKeys
