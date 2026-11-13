// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include <cstddef>

namespace PlasmaZones {

class Settings;
class ShortcutManager;

/// The static (settings-driven, non-indexed) shortcut registration table.
/// Defined in shortcutmanager_table.cpp; walked by ShortcutManager's
/// buildEntries() and staticShortcutIds(). The indexed slot families are NOT
/// here: their getters are array-indexed, so ShortcutManager::buildEntries
/// registers them in its own loops (see there for the list).
namespace ShortcutTable {

struct StaticEntry
{
    const char* id;
    QString (*defGetter)();
    QString (Settings::*curGetter)() const;
    const char* label;
    /// Capture-less so it decays to a function pointer; reaches the manager
    /// only through this argument.
    void (*fire)(ShortcutManager*);
    /// Part of the dynamic-workspaces family: holds a grab only while the
    /// feature runs (ShortcutManager::setWorkspaceFamilyActive). The default
    /// member initializer keeps the many ungated rows' aggregate initializers
    /// free of -Wmissing-field-initializers.
    bool workspaceGated = false;
};

/// Pointer to the first row; `staticEntryCount()` rows follow contiguously.
const StaticEntry* staticEntries();
std::size_t staticEntryCount();

} // namespace ShortcutTable
} // namespace PlasmaZones
