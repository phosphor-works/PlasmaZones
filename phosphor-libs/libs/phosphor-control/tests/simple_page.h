// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

// A page controller with nothing to stage, for the QML tests that need a page
// in the registry and do not care what it holds. tests/CMakeLists.txt lists
// this header among the sources of each test that includes it, which is how
// AUTOMOC sees its Q_OBJECT.

#include <PhosphorControl/PageController.h>

#include <QString>

namespace PhosphorControlTest {

class SimplePage : public PhosphorControl::PageController
{
    Q_OBJECT
public:
    using PhosphorControl::PageController::PageController;
    bool isDirty() const override
    {
        return false;
    }
    void apply() override
    {
        Q_EMIT applyResult(true, QString());
    }
    void discard() override
    {
        Q_EMIT discardResult(true, QString());
    }
};

} // namespace PhosphorControlTest
