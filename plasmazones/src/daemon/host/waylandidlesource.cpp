// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "waylandidlesource.h"

#include <PhosphorServiceIdle/IdleService.h>

#include <QVariantMap>

namespace PlasmaZones {

std::unique_ptr<IIdleSource> WaylandIdleSource::create()
{
    // No QObject parent: the unique_ptr owns it.
    auto service = std::make_unique<PhosphorServiceIdle::IdleService>(nullptr);
    if (!service->isSupported()) {
        return nullptr;
    }
    return std::unique_ptr<IIdleSource>(new WaylandIdleSource(std::move(service)));
}

WaylandIdleSource::WaylandIdleSource(std::unique_ptr<PhosphorServiceIdle::IdleService> service)
    : m_service(std::move(service))
{
    connect(m_service.get(), &PhosphorServiceIdle::IdleService::idled, this, [this](int /*stage*/) {
        Q_EMIT idled();
    });
    connect(m_service.get(), &PhosphorServiceIdle::IdleService::resumed, this, &WaylandIdleSource::resumed);
}

WaylandIdleSource::~WaylandIdleSource() = default;

void WaylandIdleSource::setIdleTimeout(int timeoutMs)
{
    if (timeoutMs <= 0) {
        m_service->setStages({});
        return;
    }
    // One stage. The keys are PhosphorServiceIdle's contract, spelled through
    // its constants: a typo yields a zero-timeout stage the parser drops.
    m_service->setStages({QVariantMap{
        {PhosphorServiceIdle::StageKey::Name, QStringLiteral("decorations")},
        {PhosphorServiceIdle::StageKey::TimeoutMs, timeoutMs},
    }});
}

bool WaylandIdleSource::isArmed() const
{
    return m_service->isArmed();
}

bool WaylandIdleSource::isIdle() const
{
    return m_service->isIdle();
}

} // namespace PlasmaZones
