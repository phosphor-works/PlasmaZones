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
    /// Search terms for the page. FacetHost does not feed them to a
    /// SearchController itself; a host that wants unloaded pages found by
    /// them passes them to SearchController::setPageKeywords.
    QStringList keywords;
    /// The page's simple/advanced tier.
    PageRegistry::PageVisibility visibility = PageRegistry::PageVisibility::Always;
    /// Draw a divider after the page's sidebar row.
    bool dividerAfter = false;
    /// The page's other-mode counterpart: a local page id or
    /// `<module>:<page>`. Empty for none. See PageRegistry::Entry.
    QString counterpart;
    /// The page's section anchors, used by search and deep links. Every anchor
    /// is also a place other modules can add a section: `before:<anchor>` and
    /// `after:<anchor>`.
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
    /// Local to the module. Facet knows it as `<module>:<id>`.
    QString id;
    /// The target page, `<module>:<page>`.
    QString target;
    /// `top`, `bottom`, `before:<anchor>`, `after:<anchor>`, or the id of one
    /// of the target page's extension points.
    QString slot;
    /// The version of the slot the extension was built for.
    int targetVersion = 1;
    QString name;
    QStringList keywords;
    int order = 0;
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
///                       "Name": "KWin snap assist", "Keywords": ["assist"], "Order": 10}]
///     }
///
/// Every `Name`, `Description` and `Keywords`, at module, category, page and
/// extension level, may carry `[<locale>]` variants (`Name[de]`, `Name[pt_BR]`). The
/// module writes them into its metadata, usually from its translations when
/// it is built. parse() picks the best one for the locale, following the
/// locale's UI languages, and skips a variant that is empty or not of the
/// key's type.
///
/// `Visibility` is `Always` (the default), `AdvancedOnly` or `SimpleOnly`.
/// A module must declare at least one page. Keys parse() does not know are
/// reported as warnings, not errors, so metadata written for a newer Facet
/// still loads.
struct PHOSPHORCONTROL_EXPORT FacetModuleMetadata
{
    QString id;
    int apiVersion = 0;
    QString name;
    QString description;
    QString icon;
    /// The category the module's node goes under. Empty means `apps`.
    QString category;
    /// Another module's id to nest this module's node under, or empty.
    QString parent;
    int order = 0;
    QStringList keywords;
    QString translationDomain;
    QList<FacetCategoryMetadata> categories;
    /// Pre-order: each page is followed by its local descendants, and
    /// siblings keep their declaration order, which is the order the sidebar
    /// shows them in.
    QList<FacetPageMetadata> pages;
    QList<FacetExtensionPointMetadata> extensionPoints;
    QList<FacetExtensionMetadata> extensions;

    /// Parse @p json for @p locale. Every problem is appended to @p errors,
    /// and the metadata is usable only when none was added. Unknown keys are
    /// appended to @p warnings when it is given.
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
    [[nodiscard]] int slotVersion(const QString& pageId, const QString& slot) const;
};

} // namespace PhosphorControl
