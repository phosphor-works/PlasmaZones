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
///                  "Counterpart": ""}]
///     }
///
/// Every `Name`, `Description` and `Keywords`, at module, category and page
/// level, may carry `[<locale>]` variants (`Name[de]`, `Name[pt_BR]`). The
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

    /// Parse @p json for @p locale. Every problem is appended to @p errors,
    /// and the metadata is usable only when none was added. Unknown keys are
    /// appended to @p warnings when it is given.
    static FacetModuleMetadata parse(const QJsonObject& json, const QLocale& locale, QStringList* errors,
                                     QStringList* warnings = nullptr);

    /// The page with local id @p id, or nullptr.
    [[nodiscard]] const FacetPageMetadata* page(const QString& id) const;
};

} // namespace PhosphorControl
