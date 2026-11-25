// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/engineruntimenamespace.h"

#include <PhosphorEngineRuntime/IIdleSource.h>

#include <memory>

namespace PhosphorServiceIdle {
class IdleService;
}

namespace PlasmaZones {

/// IIdleSource for plasmazonesd: the compositor's ext-idle-notify-v1 through
/// PhosphorServiceIdle, as a one-stage ladder at the runtime's timeout.
class WaylandIdleSource : public IIdleSource
{
    Q_OBJECT

public:
    /// A source, or null when the compositor has no ext-idle-notify-v1.
    static std::unique_ptr<IIdleSource> create();

    ~WaylandIdleSource() override;

    void setIdleTimeout(int timeoutMs) override;
    bool isArmed() const override;
    bool isIdle() const override;

private:
    explicit WaylandIdleSource(std::unique_ptr<PhosphorServiceIdle::IdleService> service);

    std::unique_ptr<PhosphorServiceIdle::IdleService> m_service;
};

} // namespace PlasmaZones
