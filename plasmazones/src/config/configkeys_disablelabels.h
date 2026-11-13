// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The persisted Rule::name labels for the per-mode disable rules. Not config
// keys, which is why they are free functions rather than ConfigKeys members.
// Include this header directly where a disable-rule name is built.

#include <PhosphorRules/ContextRuleBridge.h>
#include <PhosphorZones/AssignmentEntry.h>

#include <QString>

namespace PlasmaZones {

// ─── Disable-rule label helpers ─────────────────────────────────────────────
// Shared between the live Settings disable-list writer
// (Settings::writeDisableEntries) and the v3→v4 migration's disable-rule
// builders (configmigration_v4finalize.cpp). Both call sites must produce the
// same `Rule::name` string for a given (mode, screen, desktop, activity) tuple
// so that resaving an existing disable list (e.g. after a UI edit) doesn't
// fork into two slightly different labels for what is otherwise the same
// rule. The tuple half of the name is ContextRuleBridge::contextRuleName,
// the label every context rule carries, which is also what
// ContextRuleBridge's desktop remap matches on when it renames a moved
// disable rule.
//
// These are NOT translated. `Rule::name` is the persisted identity
// surface in rules.json; running the app under different locales must
// not change its on-disk text. The rule editor surfaces the name verbatim,
// matching the historic behaviour.
inline QString autotileDisableRulePrefix()
{
    return QStringLiteral("Autotile off · ");
}

inline QString snappingDisableRulePrefix()
{
    return QStringLiteral("Snapping off · ");
}

inline QString scrollingDisableRulePrefix()
{
    return QStringLiteral("Scrolling off · ");
}

/// Persistent label-prefix for the Rule::name field of a per-mode
/// disable rule. Exhaustive switch with no default, so -Wswitch flags a
/// future `Mode` enum value added in `AssignmentEntry.h` without an entry
/// here at compile time. The trailing Q_UNREACHABLE only documents that every
/// enumerator returned above; it is not a runtime check.
inline QString disableRulePrefixFor(PhosphorZones::AssignmentEntry::Mode mode)
{
    switch (mode) {
    case PhosphorZones::AssignmentEntry::Snapping:
        return snappingDisableRulePrefix();
    case PhosphorZones::AssignmentEntry::Autotile:
        return autotileDisableRulePrefix();
    case PhosphorZones::AssignmentEntry::Scrolling:
        return scrollingDisableRulePrefix();
    }
    Q_UNREACHABLE();
    return QString();
}

/// The full persisted name of a per-mode disable rule for the
/// (screen, desktop, activity) tuple the rule matches: the mode prefix, then
/// the context label. Pass @p desktop 0 and an empty @p activity for the axes
/// the rule does not pin, exactly as the rule's own tuple does.
inline QString disableRuleName(PhosphorZones::AssignmentEntry::Mode mode, const QString& screenId, int desktop,
                               const QString& activity)
{
    return disableRulePrefixFor(mode) + PhosphorRules::ContextRuleBridge::contextRuleName(screenId, desktop, activity);
}

} // namespace PlasmaZones
