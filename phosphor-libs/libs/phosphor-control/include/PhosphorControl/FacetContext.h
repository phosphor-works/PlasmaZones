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

#include <functional>
#include <memory>

namespace PhosphorConfig {
class IBackend;
}

namespace PhosphorControl {

class FacetHost;
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
/// configBackend() is the one call that acts at once: the backend is shared
/// with the other modules writing that file, so it exists whether or not this
/// registration is kept.
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

    /// Makes the backend of a config file. @p path is the file to open, which
    /// is the file the module named but not always the string it passed:
    /// symbolic links are followed and `.` and `..` are resolved, so every
    /// spelling of one file arrives here as the same path. A host that
    /// redirects config files (FacetHost::setConfigRedirect, as FacetTestHost
    /// does) passes that path moved under its redirect directory.
    using ConfigBackendFactory = std::function<std::unique_ptr<PhosphorConfig::IBackend>(const QString& path)>;
    /// The config backend for the file at @p path, an absolute path, shared
    /// by every module that opens the same file, so one Apply writes each
    /// module's keys into one in-memory document and no module overwrites
    /// another's. Two paths name the same file when they lead to it through
    /// symbolic links, `.` or `..`, whether or not the file or its directory
    /// exists yet. The first module to open a file creates its backend with
    /// @p create; later ones get that backend and @p create is not called, so
    /// modules sharing a file must set its backend up alike. Each module
    /// wraps the backend in its own PhosphorConfig::Store or settings object.
    ///
    /// Sharing asks this of the modules:
    /// - Every Store on the backend must be given the same path resolver
    ///   object (PhosphorConfig::Schema::pathResolver is compared by pointer,
    ///   so an equal copy counts as another) and the same version stamp, key
    ///   and version. The first Store's stand, and what a later Store brings
    ///   instead is ignored, with a warning. A Store's migrations run on the
    ///   whole document, and write it to the file when they raise its version.
    /// - A PhosphorConfig::JsonBackend allows one live group (IGroup) at a
    ///   time, whichever module holds it. A second one asserts in a debug
    ///   build and is read-only in a release build, so its writes are lost.
    ///   Open a group, use it and let it go without running other modules'
    ///   code in between.
    /// - sync(), commit() and reparseConfiguration() act on the whole
    ///   document. A write also saves what the other modules have not saved
    ///   yet, and a reparse drops it.
    ///
    /// The backend lives as long as the ApplicationController, which owns it,
    /// so a domain can still be applied through it after the FacetHost is
    /// gone. It does not outlive the module's own objects, though. The
    /// controller destroys the backends first and the page controllers and
    /// domains it adopted from modules after them, and an object parented to
    /// the module's root lives until the process ends. So do not use the
    /// backend from a destructor, and do not keep one of its groups (IGroup)
    /// in a member: a PhosphorConfig::JsonBackend group reaches into its
    /// backend when it is destroyed.
    ///
    /// Returns null, and records a module error, when @p path is not
    /// absolute or @p create returns null. The same goes for a @p path that
    /// starts with `:` (a Qt resource, which cannot be written) or that names
    /// no file because it ends in a separator, `.` or `..`.
    PhosphorConfig::IBackend* configBackend(const QString& path, const ConfigBackendFactory& create);

private:
    friend class FacetHost;
    FacetContext(FacetHost* host, const FacetModuleMetadata& metadata);

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

    FacetHost* m_host;
    const FacetModuleMetadata& m_metadata;
    QList<PageRegistration> m_pages;
    QList<ExtensionRegistration> m_extensions;
    QSet<QString> m_registeredPages;
    QSet<QString> m_registeredExtensions;
    QList<QPointer<StagingDomain>> m_domains;
    QStringList m_errors;
};

} // namespace PhosphorControl
