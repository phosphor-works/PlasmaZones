// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <PhosphorControl/PageController.h>
#include <QObject>

namespace PlasmaZones {

/// Thin PhosphorControl::PageController wrapper that lets SettingsController
/// register virtual / parent pages with the framework's PageRegistry without
/// providing a concrete controller.
///
/// The adapter is the framework-facing identity (id + lifecycle hooks) of a
/// page that has no controller of its own. A page that does have one (the
/// General and Editor pages, for example) registers that controller
/// directly instead, and PageHost hands it to the page as `controller`.
///
/// Per-page apply/discard semantics are deliberately no-ops here — dirty
/// tracking is centralised in SettingsController and orchestrated through
/// the single SettingsStagingDomain registered alongside these adapters.
class PageAdapter : public PhosphorControl::PageController
{
    Q_OBJECT

public:
    explicit PageAdapter(QString id, QObject* parent = nullptr);
    ~PageAdapter() override;

    bool isDirty() const override;
    void apply() override;
    void discard() override;
};

} // namespace PlasmaZones
