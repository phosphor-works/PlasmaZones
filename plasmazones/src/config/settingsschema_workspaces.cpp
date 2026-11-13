// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The workspaces half of the settings schema: the Workspaces.Behavior gate
// scalars, the Workspaces.Named declaration list and its canonicalizing
// validator, the Workspaces.Slots target family, and the shared
// workspace-name predicate every writer of a name checks against. Split out
// of settingsschema.cpp for file-size, along with the workspace rows of the
// Shortcuts.Global list. Both schema entry points (appendWorkspacesSchema and
// appendWorkspacesShortcutKeys) are declared alongside every other
// appendXxxSchema in settingsschema.h.
//
// Every row here is declared in every build, including USE_KDE_FRAMEWORKS=OFF
// builds where the feature itself is compiled out. Settings::purgeStaleKeys
// deletes whatever the schema does not declare, so guarding a row on the
// build flavour would wipe a user's workspace configuration the first time an
// OFF build saved, even though an ON build on the same machine reads it.

#include "settingsschema.h"

#include "settingsschema_p.h"

#include "configdefaults.h"

#include <PhosphorRules/ActionParams.h>

#include <QSet>
#include <QUuid>

namespace PlasmaZones {

namespace {

/// A bidirectional formatting control: these reorder the text around them,
/// so a name carrying one renders differently from the characters it holds
/// (the "Trojan Source" spoof), in the settings list and in KDE's shortcut
/// settings alike.
constexpr bool isBidiControl(char32_t cp)
{
    return cp == 0x061C || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E)
        || (cp >= 0x2066 && cp <= 0x2069);
}

/// Whether one code point may appear in a workspace name. See
/// checkWorkspaceName for the policy.
bool isAllowedNameCodePoint(char32_t cp)
{
    if (QChar::category(cp) == QChar::Other_Control) {
        return false;
    }
    if (QChar::isNonCharacter(cp)) {
        return false;
    }
    // LINE and PARAGRAPH SEPARATOR break a line in every renderer and in the
    // kglobalshortcutsrc INI value the name is written into.
    if (cp == 0x2028 || cp == 0x2029) {
        return false;
    }
    return !isBidiControl(cp);
}

/// Canonicalize the named-workspace declaration list, the same
/// drop-malformed contract canonicalTriggerList applies to trigger lists. No
/// D-Bus setter reaches this key, and config.json is hand-editable, so the
/// validator is the one boundary every path through the store crosses.
///
/// Per entry, in order:
///  - not a map: dropped.
///  - name refused by checkWorkspaceName: dropped.
///  - canonical name repeating an earlier kept entry: dropped (first wins),
///    because two entries claiming one name make which slice a name-keyed
///    lookup reaches depend on list order.
///  - non-empty id repeating an earlier kept entry: dropped (first wins), for
///    the same reason on the id-keyed lookups.
///  - past WorkspaceNamedEntryMaxCount kept entries: dropped.
///
/// The validator runs on every read as well as every write, and a profile
/// compare runs it on both sides, so it must be a pure function of its
/// input: it keeps an id or drops it, and never mints one. An id-less entry
/// survives with an empty id; producers stamp one (see namedEntryIdField).
///
/// Surviving entries are rewritten to exactly the declared fields, so an
/// unknown field a future (or hand-edited) config carries does not persist
/// through a save cycle.
QVariant canonicalNamedEntries(const QVariant& v)
{
    using CD = ConfigDefaults;
    const QVariantList raw = v.toList();
    QVariantList out;
    out.reserve(qMin(static_cast<int>(raw.size()), CD::WorkspaceNamedEntryMaxCount));
    QSet<QString> seenNames;
    QSet<QString> seenIds;
    for (const QVariant& entry : raw) {
        if (out.size() >= CD::WorkspaceNamedEntryMaxCount) {
            break;
        }
        if (entry.typeId() != QMetaType::QVariantMap) {
            continue;
        }
        const QVariantMap src = entry.toMap();
        const WorkspaceNameCheck check = checkWorkspaceName(src.value(CD::namedEntryNameField()).toString());
        if (check.verdict != WorkspaceNameVerdict::Accepted || seenNames.contains(check.name)) {
            continue;
        }
        const QString id = canonicalWorkspaceEntryId(src.value(CD::namedEntryIdField()));
        if (!id.isEmpty() && seenIds.contains(id)) {
            continue;
        }
        seenNames.insert(check.name);
        if (!id.isEmpty()) {
            seenIds.insert(id);
        }
        QVariantMap canon;
        canon[CD::namedEntryIdField()] = id;
        canon[CD::namedEntryNameField()] = check.name;
        // Trimmed, and an implausibly long id is read as unpinned: the same
        // grossly-malformed-payload bound the RouteToScreen rule target uses.
        const QString output = src.value(CD::namedEntryOutputField()).toString().trimmed();
        canon[CD::namedEntryOutputField()] = output.size() > PhosphorRules::MaxScreenIdLength ? QString() : output;
        // Position is the preferred slice index; -1 means "before the trailing
        // empty". A non-numeric or negative-beyond--1 value falls back to that
        // sentinel rather than persisting an index no reconciler can honour.
        bool posOk = false;
        const int position = src.value(CD::namedEntryPositionField(), -1).toInt(&posOk);
        canon[CD::namedEntryPositionField()] = (posOk && position >= 0) ? position : -1;
        canon[CD::namedEntryFocusShortcutField()] = src.value(CD::namedEntryFocusShortcutField()).toString();
        canon[CD::namedEntryMoveShortcutField()] = src.value(CD::namedEntryMoveShortcutField()).toString();
        out.append(canon);
    }
    return out;
}

/// A slot target's stored-name half: the canonical name, or empty when the
/// stored text is not a name any declaration could carry.
QVariant canonicalSlotTargetName(const QVariant& v)
{
    const WorkspaceNameCheck check = checkWorkspaceName(v.toString());
    return check.verdict == WorkspaceNameVerdict::Accepted ? check.name : QString();
}

/// A slot target's id half. A plain function rather than the exported
/// canonicalWorkspaceEntryId itself, because KeyDef::validator wants a
/// QVariant result.
QVariant canonicalSlotTargetId(const QVariant& v)
{
    return canonicalWorkspaceEntryId(v);
}

} // namespace

WorkspaceNameCheck checkWorkspaceName(const QString& raw)
{
    WorkspaceNameCheck result;
    const QString name = raw.trimmed();
    if (name.isEmpty()) {
        result.verdict = WorkspaceNameVerdict::Empty;
        return result;
    }
    if (name.size() > ConfigDefaults::WorkspaceNameMaxLength) {
        result.verdict = WorkspaceNameVerdict::TooLong;
        return result;
    }
    // Walk CODE POINTS, not UTF-16 units: a per-unit test sees each half of a
    // non-BMP character (an emoji) as an unprintable surrogate and refuses a
    // perfectly good name. A surrogate WITHOUT its partner is refused, since
    // it cannot be encoded into the UTF-8 the config and kglobalshortcutsrc
    // are written in.
    const qsizetype size = name.size();
    for (qsizetype i = 0; i < size; ++i) {
        const QChar unit = name.at(i);
        char32_t cp = unit.unicode();
        if (unit.isHighSurrogate()) {
            if (i + 1 >= size || !name.at(i + 1).isLowSurrogate()) {
                result.verdict = WorkspaceNameVerdict::ForbiddenCharacter;
                return result;
            }
            cp = QChar::surrogateToUcs4(unit, name.at(i + 1));
            ++i;
        } else if (unit.isLowSurrogate()) {
            result.verdict = WorkspaceNameVerdict::ForbiddenCharacter;
            return result;
        }
        if (!isAllowedNameCodePoint(cp)) {
            result.verdict = WorkspaceNameVerdict::ForbiddenCharacter;
            return result;
        }
    }
    result.verdict = WorkspaceNameVerdict::Accepted;
    result.name = name;
    return result;
}

QString canonicalWorkspaceEntryId(const QVariant& raw)
{
    // Only a string can carry an id: QVariant::toString() would otherwise
    // render a number or a bool as text, which never parses as a UUID anyway,
    // but a map or list must not be stringified at all.
    if (raw.typeId() != QMetaType::QString) {
        return QString();
    }
    const QUuid id = QUuid::fromString(raw.toString().trimmed());
    return id.isNull() ? QString() : id.toString();
}

void appendWorkspacesSchema(PhosphorConfig::Schema& schema)
{
    using CD = ConfigDefaults;
    // Dynamic per-monitor workspaces: the master gate, the consent latch for
    // KWin's per-output key, and the behaviour toggles.
    schema.groups[CD::workspacesBehaviorGroup()] = {
        {CD::enabledKey(), CD::workspacesEnabled(), QMetaType::Bool,
         QStringLiteral("Gives every monitor its own list of workspaces instead of one global set shared by all "
                        "screens. Requires KWin's per-output virtual desktops.")},
        {CD::manageKWinPerOutputKey(), CD::workspacesManageKWinPerOutput(), QMetaType::Bool,
         QStringLiteral("Records that the user consented to PlasmaZones writing KWin's PerOutputVirtualDesktops key. "
                        "The key is never written without this, and never reverted when the feature is disabled.")},
        {CD::snapBackOsdHintKey(), CD::workspacesSnapBackOsdHint(), QMetaType::Bool,
         QStringLiteral("Shows an OSD hint when a monitor switched to another monitor's workspace, for example "
                        "through the Pager or the Overview, snaps back to its own. Also shows one when a window "
                        "lands on a workspace that is being removed and is moved to the current one.")},
        {CD::rebindKWinDesktopShortcutsKey(), CD::workspacesRebindKWinShortcuts(), QMetaType::Bool,
         QStringLiteral("Takes over KWin's stock Switch One Desktop and Walk Through Desktops shortcuts while the "
                        "feature is on, since they walk the whole shared desktop pool rather than one monitor's "
                        "list. Restored on disable.")},
    };
    schema.groups[CD::workspacesNamedGroup()] = {
        {CD::entriesKey(), CD::workspacesNamedEntries(), QMetaType::QVariantList,
         QStringLiteral("The named workspaces, each of which persists while empty and may be pinned to a monitor."),
         canonicalNamedEntries},
    };
    // Quick-slot targets: the named workspace each move slot sends the
    // active window to, stored as the entry's id plus the name it had when
    // assigned. Both empty = unassigned (the slot's chord does nothing).
    auto& slotKeys = schema.groups[CD::workspacesSlotsGroup()];
    slotKeys.reserve(2 * CD::WorkspaceSlotCount);
    for (int slot = 1; slot <= CD::WorkspaceSlotCount; ++slot) {
        slotKeys.append({CD::workspaceSlotTargetIdKey(slot), CD::workspaceSlotTargetId(), QMetaType::QString,
                         QStringLiteral("The named workspace quick slot %1 targets. Empty leaves the slot "
                                        "unassigned.")
                             .arg(slot),
                         canonicalSlotTargetId});
        slotKeys.append({CD::workspaceSlotTargetKey(slot), CD::workspaceSlotTarget(), QMetaType::QString,
                         QStringLiteral("The name quick slot %1's workspace had when it was assigned. Used when "
                                        "that workspace no longer exists under its original identity.")
                             .arg(slot),
                         canonicalSlotTargetName});
    }
}

/// The Shortcuts.Global rows for the dynamic-workspaces verbs and quick
/// slots. Declared in settingsschema.h and called from appendShortcutsSchema
/// while it builds that group's list.
void appendWorkspacesShortcutKeys(QVector<PhosphorConfig::KeyDef>& globals)
{
    using CD = ConfigDefaults;
    using SchemaRows::addShortcut;
    addShortcut(globals, CD::workspaceFocusUpKey(), CD::workspaceFocusUpShortcut(),
                QStringLiteral("Switches this monitor to the workspace above its current one."));
    addShortcut(globals, CD::workspaceFocusDownKey(), CD::workspaceFocusDownShortcut(),
                QStringLiteral("Switches this monitor to the workspace below its current one."));
    addShortcut(globals, CD::workspaceMoveWindowUpKey(), CD::workspaceMoveWindowUpShortcut(),
                QStringLiteral("Moves the focused window to the workspace above this monitor's current one."));
    addShortcut(globals, CD::workspaceMoveWindowDownKey(), CD::workspaceMoveWindowDownShortcut(),
                QStringLiteral("Moves the focused window to the workspace below this monitor's current one."));
    addShortcut(globals, CD::workspaceMoveColumnUpKey(), CD::workspaceMoveColumnUpShortcut(),
                QStringLiteral("Moves the focused window's whole scrolling column to the workspace above."));
    addShortcut(globals, CD::workspaceMoveColumnDownKey(), CD::workspaceMoveColumnDownShortcut(),
                QStringLiteral("Moves the focused window's whole scrolling column to the workspace below."));
    addShortcut(globals, CD::workspaceReorderUpKey(), CD::workspaceReorderUpShortcut(),
                QStringLiteral("Moves the current workspace one place earlier in this monitor's list."));
    addShortcut(globals, CD::workspaceReorderDownKey(), CD::workspaceReorderDownShortcut(),
                QStringLiteral("Moves the current workspace one place later in this monitor's list."));
    addShortcut(globals, CD::workspaceMoveToMonitorLeftKey(), CD::workspaceMoveToMonitorLeftShortcut(),
                QStringLiteral("Hands the current workspace to the monitor to the left."));
    addShortcut(globals, CD::workspaceMoveToMonitorRightKey(), CD::workspaceMoveToMonitorRightShortcut(),
                QStringLiteral("Hands the current workspace to the monitor to the right."));
    // Workspace quick-shortcut slots. Both families ship unbound, for the
    // reasons on workspaceMoveSlotShortcut and workspaceFocusSlotShortcut in
    // configdefaults_workspaces.h. The user assigns the chords, and the move
    // slots' targets, on the settings app's workspace shortcuts page.
    for (int slot = 1; slot <= CD::WorkspaceSlotCount; ++slot) {
        addShortcut(globals, CD::workspaceMoveSlotKey(slot), CD::workspaceMoveSlotShortcut(),
                    QStringLiteral("Moves the focused window to the named workspace assigned to quick slot %1. "
                                   "Unbound by default.")
                        .arg(slot));
        // Positional, unlike the move slot above: this switches the acting
        // monitor to the Nth workspace of its own list. It does not read the
        // Workspaces.Slots target assignments.
        addShortcut(
            globals, CD::workspaceFocusSlotKey(slot), CD::workspaceFocusSlotShortcut(),
            QStringLiteral("Switches this monitor to workspace %1 of its own list. Unbound by default.").arg(slot));
    }
}

} // namespace PlasmaZones
