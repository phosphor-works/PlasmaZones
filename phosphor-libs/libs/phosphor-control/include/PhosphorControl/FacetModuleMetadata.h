// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "PhosphorControl/PageRegistry.h"
#include "phosphorcontrol_export.h"

#include <QJsonObject>
#include <QList>
#include <QLocale>
#include <QString>
#include <QStringList>

namespace PhosphorControl {

/// A top-level sidebar category a module defines.
struct PHOSPHORCONTROL_EXPORT FacetCategoryMetadata
{
    QString id;
    QString name;
    QString icon;
    int order = 0;
};

/// One page of a module, as its metadata describes it.
struct PHOSPHORCONTROL_EXPORT FacetPageMetadata
{
    /// Local to the module: `snapping` or `snapping/behavior`.
    QString id;
    QString name;
    QString icon;
    /// Empty for a page directly under the module (or under `category`), a
    /// local page id for a page under one of the module's own pages, or
    /// `<module>:<page>` for a page under another module's page.
    QString parent;
    /// A category to put the page in directly, instead of under its module.
    /// Exclusive with `parent`.
    QString category;
    /// Search terms for the page. FacetHost passes them, with the module's
    /// own keywords, to its SearchController (FacetHost::setSearchController),
    /// so the page is found before its module loads.
    QStringList keywords;
    /// The page's simple/advanced tier.
    PageRegistry::PageVisibility visibility = PageRegistry::PageVisibility::Always;
    /// Draw a divider after the page's sidebar row.
    bool dividerAfter = false;
    /// The page's other-mode counterpart: a local page id or
    /// `<module>:<page>`. Empty for none. See PageRegistry::Entry.
    QString counterpart;
    /// The page's section anchors: the names its QML reveals for a deep link
    /// (`<page>#<anchor>`, ApplicationController::setPendingAnchor). A search
    /// entry lands on one, and every anchor is also a place other modules can
    /// add a section, `before:<anchor>` and `after:<anchor>`, which the page's
    /// QML places with a FacetSlot next to the anchored section.
    QStringList anchors;
};

/// A named place on one of the module's pages that other modules may add a
/// section to, beyond the implicit `top`, `bottom`, `before:<anchor>` and
/// `after:<anchor>` slots every page has. The reserved id `anchors` declares
/// the version of a page's implicit slots instead.
struct PHOSPHORCONTROL_EXPORT FacetExtensionPointMetadata
{
    /// The module's local page id.
    QString page;
    QString id;
    /// Bumped whenever the slot moves or its meaning changes, so extensions
    /// built for the old one are skipped rather than shown in the wrong place.
    int version = 1;
};

/// A section this module adds to another module's page.
struct PHOSPHORCONTROL_EXPORT FacetExtensionMetadata
{
    /// Local to the module. Facet knows it as `<module>:<id>`, the same form
    /// as a scoped page id, so an extension and a page of one module may share
    /// an id. The two never meet: extension ids name sections only
    /// (FacetHost::refusedExtensions, extensionsOn), never sidebar pages.
    QString id;
    /// The target page, `<module>:<page>`.
    QString target;
    /// `top`, `bottom`, `before:<anchor>`, `after:<anchor>`, or the id of one
    /// of the target page's extension points. parse() only requires one;
    /// FacetHost refuses the extension, with the reason, when the target page
    /// does not offer it.
    QString slot;
    /// The version of the slot the extension was built for.
    int targetVersion = 1;
    QString name;
    /// Search terms for the section, which FacetHost indexes while the
    /// extension is active.
    QStringList keywords;
    int order = 0;
};

/// A static search target on one of the module's pages: a setting or section
/// at one of the page's anchors. Facet indexes these from the metadata, so
/// search finds them before the module loads.
struct PHOSPHORCONTROL_EXPORT FacetSearchEntryMetadata
{
    /// The module's local page id.
    QString page;
    /// One of that page's anchors.
    QString anchor;
    /// `"Kind": "Section"`. `"Setting"`, the default, leaves it false. A
    /// kind this Facet does not know is a warning, and parse() leaves that
    /// entry out, so an entry written for a newer Facet costs only itself.
    bool section = false;
    QString title;
    QStringList keywords;
    /// The row shows only in advanced mode (SearchEntry::advancedOnly). An
    /// error on a page that, or any of whose local ancestors, shows only in
    /// simple mode, where search could never offer it.
    bool advancedOnly = false;
};

/// The metadata a Facet module embeds with Q_PLUGIN_METADATA:
///
///     {
///       "Id": "plasmazones", "ApiVersion": 1,
///       "Name": "KWin and Plasma", "Name[de]": "KWin und Plasma",
///       "Description": "...", "Icon": "org.phosphor_works.PlasmaZones",
///       "Category": "window-management", "Parent": "placement", "Order": 20,
///       "Keywords": ["kwin"], "TranslationDomain": "facet-module-plasmazones",
///       "Categories": [{"Id": "...", "Name": "...", "Icon": "...", "Order": 0}],
///       "Pages": [{"Id": "effect", "Name": "KWin Effect", "Icon": "...",
///                  "Parent": "", "Category": "", "Keywords": ["blur"],
///                  "Visibility": "AdvancedOnly", "DividerAfter": false,
///                  "Counterpart": "", "Anchors": ["blur"]}],
///       "ExtensionPoints": [{"Page": "effect", "Id": "outline", "Version": 1}],
///       "Extensions": [{"Id": "snap-assist", "Target": "placement:snapping/behavior",
///                       "Slot": "after:drag-modifier", "TargetVersion": 1,
///                       "Name": "KWin snap assist", "Keywords": ["assist"], "Order": 10}],
///       "Search": [{"Page": "effect", "Anchor": "blur", "Kind": "Setting", "Title": "Blur",
///                   "Keywords": ["frost"], "AdvancedOnly": false}],
///       "SearchProviders": false
///     }
///
/// The localizable keys may carry `[<locale>]` variants (`Name[de]`,
/// `Name[pt_BR]`): `Name`, `Description` and `Keywords` on the module, `Name`
/// on a category, `Name` and `Keywords` on a page and on an extension, and
/// `Title` and `Keywords` on a search entry. A locale is written with '_',
/// never '-': `Name[pt-BR]` is never picked, and parse() warns about it.
/// phosphor_add_facet_module_metadata() writes the variants into the module's
/// metadata from its translations when it is built, keyed by the language
/// and its script (`Name[de_Latn]`, `Name[zh_Hant]`) where only one catalog
/// claims them, so every locale of that language and script finds it, and by
/// the catalog's own language otherwise, in Qt's code for it (`nb`, never
/// `no`). It keeps a usable variant written by hand, which wins over the
/// catalog's at the catalog's key, at the language and script when the
/// catalog keeps its own longer key, and at the bare language when the
/// catalog's script is the language's usual one. A variant written by hand
/// uses Qt's code for the language too, since `Name[no]` is never picked.
/// parse() picks the best variant for the locale, following the locale's UI languages from the most specific
/// form to the bare language (`de_Latn_AT`, `de_AT`, `de_Latn`, `de`), and
/// skips one that is empty or not of the key's type. Whether a required key
/// (`Name`, a search entry's `Title`) is there is decided on its base value
/// alone, so metadata valid in one locale is valid in all of them. The base
/// values are what lupdate extracts: a string must hold no control character
/// other than tab and newline, no U+FFFE or U+FFFF and no half of a surrogate
/// pair, and every `Keywords` entry must be a non-empty string.
///
/// `Visibility` is `Always` (the default), `AdvancedOnly` or `SimpleOnly`.
/// A module must declare at least one page, even one that only adds sections
/// to other modules' pages: its own page is where Facet says why it did not
/// load and offers to load it again. Keys parse() does not know are reported
/// as warnings, not errors, so metadata written for a newer Facet still loads.
///
/// Anchors match `[A-Za-z0-9][A-Za-z0-9._-]*`, and a search entry's anchor
/// must be one its page lists. `SearchProviders` says the module registers
/// dynamic search providers, so Facet loads it the first time the user
/// searches.
struct PHOSPHORCONTROL_EXPORT FacetModuleMetadata
{
    QString id;
    int apiVersion = 0;
    QString name;
    QString description;
    QString icon;
    /// The category the module's node goes under when it is not nested under
    /// `parent`: when it names none, when that module is not available, or
    /// when nesting under it would close a cycle. Empty, or a category no
    /// module and not Facet defines, means `apps`.
    QString category;
    /// Another module's id to nest this module's node under, or empty. When
    /// that module is not available the node goes under `category` instead.
    QString parent;
    int order = 0;
    QStringList keywords;
    /// Read and kept for the module's own use. Facet does not load a catalog
    /// by it: the metadata's strings come translated from the module's
    /// TS_FILES (phosphor_add_facet_module_metadata()), and the module
    /// installs a translator for its QML itself.
    QString translationDomain;
    QList<FacetCategoryMetadata> categories;
    /// Pre-order: each page is followed by its local descendants, and
    /// siblings keep their declaration order, which is the order the sidebar
    /// shows them in.
    QList<FacetPageMetadata> pages;
    QList<FacetExtensionPointMetadata> extensionPoints;
    QList<FacetExtensionMetadata> extensions;
    QList<FacetSearchEntryMetadata> search;
    bool searchProviders = false;

    /// Parse @p json for @p locale. Every problem is appended to @p errors,
    /// and the metadata is usable only when none was added. What leaves it
    /// usable is appended to @p warnings when it is given: an unknown key or
    /// a search entry of an unknown `Kind`, either of which a newer Facet may
    /// know, and a `[<locale>]` variant no locale ever picks.
    static FacetModuleMetadata parse(const QJsonObject& json, const QLocale& locale, QStringList* errors,
                                     QStringList* warnings = nullptr);

    /// The page with local id @p id, or nullptr.
    [[nodiscard]] const FacetPageMetadata* page(const QString& id) const;
    /// The extension with local id @p id, or nullptr.
    [[nodiscard]] const FacetExtensionMetadata* extension(const QString& id) const;

    /// The version slot @p slot of the local page @p pageId offers, or 0
    /// when the page has no such slot. `top` and `bottom` always exist,
    /// `before:<a>` and `after:<a>` exist for each of the page's anchors, and
    /// all of them carry the version of the page's `anchors` extension point
    /// (1 when it declares none). A named slot carries its own version.
    /// PageHost places `top` and `bottom` around every page. The page's own
    /// QML places the anchor and named slots, each with a FacetSlot.
    [[nodiscard]] int slotVersion(const QString& pageId, const QString& slot) const;
};

} // namespace PhosphorControl
