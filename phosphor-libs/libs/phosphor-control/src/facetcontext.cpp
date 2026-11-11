// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorControl/FacetContext.h"

#include "PhosphorControl/FacetHost.h"
#include "PhosphorControl/PageController.h"
#include "PhosphorControl/StagingDomain.h"

#include <QThread>

#include <algorithm>

namespace PhosphorControl {

FacetContext::FacetContext(const FacetModuleMetadata& metadata)
    : m_metadata(metadata)
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
    const bool controllerTaken = std::any_of(m_pages.cbegin(), m_pages.cend(), [controller](const auto& page) {
        return page.controller.data() == controller;
    });
    if (controllerTaken || m_domains.contains(QPointer<StagingDomain>(controller))) {
        m_errors.append(QStringLiteral("page %1's controller is already registered").arg(localPageId));
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
    if (m_domains.contains(QPointer<StagingDomain>(domain))) {
        m_errors.append(QStringLiteral("registered a domain twice"));
        return false;
    }
    const bool isPage = std::any_of(m_pages.cbegin(), m_pages.cend(), [domain](const auto& page) {
        return page.controller.data() == domain;
    });
    if (isPage) {
        m_errors.append(QStringLiteral("registered a page controller as a domain"));
        return false;
    }
    if (domain->thread() != QThread::currentThread()) {
        m_errors.append(QStringLiteral("registered a domain that lives in another thread"));
        return false;
    }
    m_domains.append(domain);
    return true;
}

} // namespace PhosphorControl
