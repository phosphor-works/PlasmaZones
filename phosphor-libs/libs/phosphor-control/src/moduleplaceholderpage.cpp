// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhosphorControl/ModulePlaceholderPage.h"

namespace PhosphorControl {

ModulePlaceholderPage::ModulePlaceholderPage(const QString& id, QObject* parent)
    : PageController(id, parent)
{
}

ModulePlaceholderPage::~ModulePlaceholderPage() = default;

bool ModulePlaceholderPage::isDirty() const
{
    return false;
}

void ModulePlaceholderPage::apply()
{
    Q_EMIT applyResult(true, QString());
}

void ModulePlaceholderPage::discard()
{
    Q_EMIT discardResult(true, QString());
}

} // namespace PhosphorControl
