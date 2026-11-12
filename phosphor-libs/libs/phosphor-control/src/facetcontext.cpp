// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorControl/FacetContext.h"

#include "PhosphorControl/FacetHost.h"
#include "PhosphorControl/PageController.h"
#include "PhosphorControl/StagingDomain.h"

#include <PhosphorConfig/IBackend.h>

#include <QDir>
#include <QFileInfo>
#include <QThread>

namespace PhosphorControl {

FacetContext::FacetContext(FacetHost* host, const FacetModuleMetadata& metadata)
    : m_host(host)
    , m_metadata(metadata)
{
}

QString FacetContext::moduleId() const
{
    return m_metadata.id;
}

const FacetModuleMetadata& FacetContext::metadata() const
{
    return m_metadata;
}

QString FacetContext::pageId(const QString& localPageId) const
{
    return FacetHost::pageId(m_metadata.id, localPageId);
}

QString FacetContext::registeredAs(const StagingDomain* domain) const
{
    for (const PageRegistration& page : m_pages) {
        if (page.controller.data() == domain) {
            return QStringLiteral("page %1's controller").arg(page.localId);
        }
    }
    for (const ExtensionRegistration& extension : m_extensions) {
        if (extension.domain.data() == domain) {
            return QStringLiteral("extension %1's domain").arg(extension.localId);
        }
    }
    for (const QPointer<StagingDomain>& headless : m_domains) {
        if (headless.data() == domain) {
            return QStringLiteral("a headless domain");
        }
    }
    return QString();
}

bool FacetContext::registerPage(const QString& localPageId, PageController* controller, const QUrl& qmlSource)
{
    if (!m_metadata.page(localPageId)) {
        m_errors.append(QStringLiteral("registered page %1, which the metadata does not declare").arg(localPageId));
        return false;
    }
    if (m_registeredPages.contains(localPageId)) {
        m_errors.append(QStringLiteral("registered page %1 twice").arg(localPageId));
        return false;
    }
    if (!controller) {
        m_errors.append(QStringLiteral("registered page %1 with no controller").arg(localPageId));
        return false;
    }
    const QString scoped = pageId(localPageId);
    if (controller->id() != scoped) {
        m_errors.append(
            QStringLiteral("page %1's controller has id %2, expected %3").arg(localPageId, controller->id(), scoped));
        return false;
    }
    if (controller->thread() != QThread::currentThread()) {
        m_errors.append(QStringLiteral("page %1's controller lives in another thread").arg(localPageId));
        return false;
    }
    if (qmlSource.isEmpty() || !qmlSource.isValid()) {
        // A page with no QML would read as a category to the registry: no
        // row, no search result and an empty page body.
        m_errors.append(QStringLiteral("page %1 has no valid QML source").arg(localPageId));
        return false;
    }
    if (qmlSource.isRelative()) {
        // The page host would resolve it against its own location, not the
        // module's, and load nothing.
        m_errors.append(
            QStringLiteral("page %1's QML source %2 is not an absolute URL").arg(localPageId, qmlSource.toString()));
        return false;
    }
    if (const QString taken = registeredAs(controller); !taken.isEmpty()) {
        m_errors.append(QStringLiteral("page %1's controller is already registered as %2").arg(localPageId, taken));
        return false;
    }
    m_pages.append(PageRegistration{localPageId, controller, qmlSource});
    m_registeredPages.insert(localPageId);
    return true;
}

bool FacetContext::registerDomain(StagingDomain* domain)
{
    if (!domain) {
        m_errors.append(QStringLiteral("registered a null domain"));
        return false;
    }
    if (const QString taken = registeredAs(domain); !taken.isEmpty()) {
        m_errors.append(QStringLiteral("registered a domain that is already registered as %1").arg(taken));
        return false;
    }
    if (domain->thread() != QThread::currentThread()) {
        m_errors.append(QStringLiteral("registered a domain that lives in another thread"));
        return false;
    }
    m_domains.append(domain);
    return true;
}

bool FacetContext::registerExtension(const QString& localExtensionId, StagingDomain* domain, const QUrl& qmlSource)
{
    if (!m_metadata.extension(localExtensionId)) {
        m_errors.append(
            QStringLiteral("registered extension %1, which the metadata does not declare").arg(localExtensionId));
        return false;
    }
    if (m_registeredExtensions.contains(localExtensionId)) {
        m_errors.append(QStringLiteral("registered extension %1 twice").arg(localExtensionId));
        return false;
    }
    if (!domain) {
        m_errors.append(QStringLiteral("registered extension %1 with no domain").arg(localExtensionId));
        return false;
    }
    if (const QString taken = registeredAs(domain); !taken.isEmpty()) {
        m_errors.append(
            QStringLiteral("extension %1's domain is already registered as %2").arg(localExtensionId, taken));
        return false;
    }
    if (domain->thread() != QThread::currentThread()) {
        m_errors.append(QStringLiteral("extension %1's domain lives in another thread").arg(localExtensionId));
        return false;
    }
    if (qmlSource.isEmpty() || !qmlSource.isValid()) {
        m_errors.append(QStringLiteral("extension %1 has no valid QML source").arg(localExtensionId));
        return false;
    }
    if (qmlSource.isRelative()) {
        // FacetExtensionFrame's Loader would resolve it against
        // org.phosphor.control's own QML directory, not the module's, and
        // load the wrong file or none.
        m_errors.append(QStringLiteral("extension %1's QML source %2 is not an absolute URL")
                            .arg(localExtensionId, qmlSource.toString()));
        return false;
    }
    // In m_domains only for the host's checks after registerInto. The host
    // registers and attaches it once the section is shown.
    m_domains.append(domain);
    m_extensions.append(ExtensionRegistration{localExtensionId, domain, qmlSource});
    m_registeredExtensions.insert(localExtensionId);
    return true;
}

PhosphorConfig::IBackend* FacetContext::configBackend(const QString& path, const ConfigBackendFactory& create)
{
    if (!QDir::isAbsolutePath(path)) {
        m_errors.append(QStringLiteral("opened config file %1, which is not an absolute path").arg(path));
        return nullptr;
    }
    // QDir::isAbsolutePath also passes a Qt resource path, which no backend
    // can write, and a path that ends in a separator, `.` or `..`, which
    // names a directory.
    if (path.startsWith(QLatin1Char(':'))) {
        m_errors.append(QStringLiteral("opened config file %1, which is a Qt resource path").arg(path));
        return nullptr;
    }
    const QString fileName = QFileInfo(path).fileName();
    if (fileName.isEmpty() || fileName == QLatin1String(".") || fileName == QLatin1String("..")) {
        m_errors.append(QStringLiteral("opened config file %1, which has no file name").arg(path));
        return nullptr;
    }
    PhosphorConfig::IBackend* const backend = m_host->sharedConfigBackend(path, create);
    if (!backend) {
        m_errors.append(QStringLiteral("could not open config file %1").arg(path));
    }
    return backend;
}

} // namespace PhosphorControl
