// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "PhosphorControl/FacetModuleMetadata.h"
#include "phosphorcontrol_export.h"

#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtCore/qtclasshelpermacros.h>

namespace PhosphorControl {

class PageController;
class StagingDomain;

/// What Facet hands a module while it loads (IFacetModule::registerInto).
///
/// Page ids are scoped: a module declares `snapping/behavior` in its metadata,
/// and Facet knows the page as `placement:snapping/behavior`, so two modules
/// can never collide. A module builds each PageController with
/// pageId(localId) and registers it under its local id.
///
/// The context only records. Once registerInto returns, Facet checks the
/// whole registration and binds it all, or, when anything was refused or a
/// declared page or extension is missing, keeps none of it: no page is bound
/// and no domain joins Apply. Ownership follows the same split. On success a controller or
/// domain with no QObject parent is adopted by the ApplicationController. On
/// failure nothing is adopted, so parent what you create to the module's root
/// object.
///
/// The context exists only for the duration of registerInto. Do not keep a
/// reference to it.
class PHOSPHORCONTROL_EXPORT FacetContext
{
public:
    Q_DISABLE_COPY_MOVE(FacetContext)

    [[nodiscard]] QString moduleId() const;
    [[nodiscard]] const FacetModuleMetadata& metadata() const;

    /// The scoped id of the module's page @p localPageId.
    [[nodiscard]] QString pageId(const QString& localPageId) const;

    /// Register the controller and QML page for the metadata page
    /// @p localPageId. Refused, and recorded as a module error, for a page
    /// the metadata does not declare, a page already registered, a null
    /// controller, a controller whose id() is not pageId(localPageId), a
    /// controller that lives in another thread, a @p qmlSource that is
    /// empty, invalid or relative (use the module's `qrc:` URL), or a
    /// controller already registered (for another page, as a headless
    /// domain or as an extension's domain).
    bool registerPage(const QString& localPageId, PageController* controller, const QUrl& qmlSource);

    /// Register a staging domain that has no page of its own, so it joins
    /// Apply, Discard and the dirty flag. Refused, and recorded as a module
    /// error, for a null domain, one already registered (as a headless domain,
    /// one of the module's page controllers, which join through registerPage,
    /// or an extension's domain), or one that lives in another thread.
    bool registerDomain(StagingDomain* domain);

    /// Register the section for the extension @p localExtensionId the
    /// metadata declares: its staging domain and the QML the page's FacetSlot
    /// shows. Once the section is shown (its target page offers the slot, and
    /// the page's own module has loaded), the domain joins Apply, Discard and
    /// the dirty flag and counts toward that page, for good. Until then it
    /// stays out of all of them, since nothing shows it. The QML gets the
    /// domain as its `domain` property, which it must declare (QtObject,
    /// required or not), and the ApplicationController as `settingsApp` if it
    /// declares that. Every declared extension must be
    /// registered, whether or not its target is installed. Refused, and
    /// recorded as a module error, for an extension the metadata does not
    /// declare, one registered twice, a null domain, a domain already
    /// registered (as a page controller, a headless domain or another
    /// extension's), one that lives in another thread, or a @p qmlSource that
    /// is empty, invalid or relative.
    bool registerExtension(const QString& localExtensionId, StagingDomain* domain, const QUrl& qmlSource);

private:
    friend class FacetHost;
    explicit FacetContext(const FacetModuleMetadata& metadata);

    struct PageRegistration
    {
        QString localId;
        QPointer<PageController> controller;
        QUrl qmlSource;
    };

    struct ExtensionRegistration
    {
        QString localId;
        QPointer<StagingDomain> domain;
        QUrl qmlSource;
    };

    /// How @p domain is already registered, for the refusal message, or empty
    /// when it is not.
    [[nodiscard]] QString registeredAs(const StagingDomain* domain) const;

    const FacetModuleMetadata& m_metadata;
    QList<PageRegistration> m_pages;
    QList<ExtensionRegistration> m_extensions;
    QSet<QString> m_registeredPages;
    QSet<QString> m_registeredExtensions;
    QList<QPointer<StagingDomain>> m_domains;
    QStringList m_errors;
};

} // namespace PhosphorControl
