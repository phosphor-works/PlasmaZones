// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/settings.h"
#include "config/settings/settings_detail.h"
#include "config/configdefaults.h"
#include "core/platform/logging.h"

#include <QUuid>

#include <array>
#include <iterator>

namespace PlasmaZones {

// ── Workspaces (PhosphorConfig::Store-backed) ───────────────────────────────
// Dynamic per-monitor workspaces: the gate scalars, the named-workspace
// declarations, the verb chords, and the indexed slot families (focus chord,
// move chord, and the target as an entry id plus the name it had). Defaults
// live in configdefaults_workspaces.h, key accessors in
// configkeys_workspaces.h. The schema rows live in
// settingsschema_workspaces.cpp: appendWorkspacesSchema for the Workspaces.*
// groups, and appendWorkspacesShortcutKeys for the Shortcuts.Global chord
// leaves (which appendShortcutsSchema calls).

P_STORE_GET(bool, workspacesEnabled, workspacesBehaviorGroup, enabledKey, bool)
P_STORE_SET_BOOL(setWorkspacesEnabled, workspacesBehaviorGroup, enabledKey, workspacesEnabledChanged)

P_STORE_GET(bool, workspacesManageKWinPerOutput, workspacesBehaviorGroup, manageKWinPerOutputKey, bool)
P_STORE_SET_BOOL(setWorkspacesManageKWinPerOutput, workspacesBehaviorGroup, manageKWinPerOutputKey,
                 workspacesManageKWinPerOutputChanged)

P_STORE_GET(bool, workspacesSnapBackOsdHint, workspacesBehaviorGroup, snapBackOsdHintKey, bool)
P_STORE_SET_BOOL(setWorkspacesSnapBackOsdHint, workspacesBehaviorGroup, snapBackOsdHintKey,
                 workspacesSnapBackOsdHintChanged)

P_STORE_GET(bool, workspacesRebindKWinShortcuts, workspacesBehaviorGroup, rebindKWinDesktopShortcutsKey, bool)
P_STORE_SET_BOOL(setWorkspacesRebindKWinShortcuts, workspacesBehaviorGroup, rebindKWinDesktopShortcutsKey,
                 workspacesRebindKWinShortcutsChanged)

// Named-workspace declarations: whole-replace QVariantList composite, same
// write shape as the trigger lists (read-back compare so a semantically
// identical list swallows no signal).
QVariantList Settings::workspacesNamedEntries() const
{
    return m_store->readVariant(ConfigDefaults::workspacesNamedGroup(), ConfigDefaults::entriesKey()).toList();
}

void Settings::setWorkspacesNamedEntries(const QVariantList& entries)
{
    // Same stale-read guard every sibling composite setter takes: the
    // read-back compare below decides whether anything changed, so it must
    // not compare against a backend copy another process has already moved
    // past. See refreshCleanBackendFromDisk's definition for the full
    // cross-process coherence rationale.
    refreshCleanBackendFromDisk();
    const QVariantList before =
        m_store->readVariant(ConfigDefaults::workspacesNamedGroup(), ConfigDefaults::entriesKey()).toList();
    m_store->write(ConfigDefaults::workspacesNamedGroup(), ConfigDefaults::entriesKey(), entries);
    const QVariantList after =
        m_store->readVariant(ConfigDefaults::workspacesNamedGroup(), ConfigDefaults::entriesKey()).toList();
    if (before == after) {
        return;
    }
    Q_EMIT workspacesNamedEntriesChanged();
    Q_EMIT settingsChanged();
}

// ── Verb shortcuts (Shortcuts.Global leaves, like every other chord) ────────

P_STORE_GET(QString, workspaceFocusUpShortcut, shortcutsGlobalGroup, workspaceFocusUpKey, QString)
P_STORE_SET_STRING(setWorkspaceFocusUpShortcut, shortcutsGlobalGroup, workspaceFocusUpKey,
                   workspaceFocusUpShortcutChanged)
P_STORE_GET(QString, workspaceFocusDownShortcut, shortcutsGlobalGroup, workspaceFocusDownKey, QString)
P_STORE_SET_STRING(setWorkspaceFocusDownShortcut, shortcutsGlobalGroup, workspaceFocusDownKey,
                   workspaceFocusDownShortcutChanged)
P_STORE_GET(QString, workspaceMoveWindowUpShortcut, shortcutsGlobalGroup, workspaceMoveWindowUpKey, QString)
P_STORE_SET_STRING(setWorkspaceMoveWindowUpShortcut, shortcutsGlobalGroup, workspaceMoveWindowUpKey,
                   workspaceMoveWindowUpShortcutChanged)
P_STORE_GET(QString, workspaceMoveWindowDownShortcut, shortcutsGlobalGroup, workspaceMoveWindowDownKey, QString)
P_STORE_SET_STRING(setWorkspaceMoveWindowDownShortcut, shortcutsGlobalGroup, workspaceMoveWindowDownKey,
                   workspaceMoveWindowDownShortcutChanged)
P_STORE_GET(QString, workspaceMoveColumnUpShortcut, shortcutsGlobalGroup, workspaceMoveColumnUpKey, QString)
P_STORE_SET_STRING(setWorkspaceMoveColumnUpShortcut, shortcutsGlobalGroup, workspaceMoveColumnUpKey,
                   workspaceMoveColumnUpShortcutChanged)
P_STORE_GET(QString, workspaceMoveColumnDownShortcut, shortcutsGlobalGroup, workspaceMoveColumnDownKey, QString)
P_STORE_SET_STRING(setWorkspaceMoveColumnDownShortcut, shortcutsGlobalGroup, workspaceMoveColumnDownKey,
                   workspaceMoveColumnDownShortcutChanged)
P_STORE_GET(QString, workspaceReorderUpShortcut, shortcutsGlobalGroup, workspaceReorderUpKey, QString)
P_STORE_SET_STRING(setWorkspaceReorderUpShortcut, shortcutsGlobalGroup, workspaceReorderUpKey,
                   workspaceReorderUpShortcutChanged)
P_STORE_GET(QString, workspaceReorderDownShortcut, shortcutsGlobalGroup, workspaceReorderDownKey, QString)
P_STORE_SET_STRING(setWorkspaceReorderDownShortcut, shortcutsGlobalGroup, workspaceReorderDownKey,
                   workspaceReorderDownShortcutChanged)
P_STORE_GET(QString, workspaceMoveToMonitorLeftShortcut, shortcutsGlobalGroup, workspaceMoveToMonitorLeftKey, QString)
P_STORE_SET_STRING(setWorkspaceMoveToMonitorLeftShortcut, shortcutsGlobalGroup, workspaceMoveToMonitorLeftKey,
                   workspaceMoveToMonitorLeftShortcutChanged)
P_STORE_GET(QString, workspaceMoveToMonitorRightShortcut, shortcutsGlobalGroup, workspaceMoveToMonitorRightKey, QString)
P_STORE_SET_STRING(setWorkspaceMoveToMonitorRightShortcut, shortcutsGlobalGroup, workspaceMoveToMonitorRightKey,
                   workspaceMoveToMonitorRightShortcutChanged)

// ── Indexed workspace slots ─────────────────────────────────────────────────
// A focus chord, a move chord and a target (TargetIdN plus TargetN) per slot,
// addressed 0-based. The families are one shape, so they share the read/write
// primitives below, the writeTriggerList precedent: the bounds contract and
// the changed-check live in one place. Each slot announces on its own NOTIFY (the per-slot
// Q_PROPERTYs in settings.h), which is also what reload, profile staging and
// per-page reset/discard fire through the metaobject walk.

namespace {
/// One of the ConfigDefaults key builders (1-based slot → key string).
using SlotKeyFn = QString (*)(int);
using SlotSignalFn = void (Settings::*)();

bool slotIndexInRange(int index)
{
    return index >= 0 && index < ConfigDefaults::WorkspaceSlotCount;
}

QString readSlotValue(PhosphorConfig::Store* store, const QString& group, SlotKeyFn keyFn, int index)
{
    // Out-of-range reads answer empty rather than qFatal-ing through the key
    // builder: the index can arrive from QML, where a stale model row can
    // outlive the page that produced it.
    if (!slotIndexInRange(index)) {
        return {};
    }
    return store->read<QString>(group, keyFn(index + 1));
}

/// Write one slot key and report whether the stored value moved; the caller
/// emits on that. Compared after the write rather than before, because the
/// target keys carry schema validators (canonical id, canonical name), so what
/// lands in the store can differ from what was passed in.
bool writeSlotValue(PhosphorConfig::Store* store, const QString& group, SlotKeyFn keyFn, int index,
                    const QString& value)
{
    if (!slotIndexInRange(index)) {
        return false;
    }
    const QString key = keyFn(index + 1);
    const QString before = store->read<QString>(group, key);
    store->write(group, key, value);
    return store->read<QString>(group, key) != before;
}

// The per-slot NOTIFYs, indexed 0-based. Sized from their initializers and
// pinned to the slot count, so raising the count without adding the matching
// signals fails the build instead of indexing past the end.
constexpr auto FocusSlotSignals = std::to_array<SlotSignalFn>({
    &Settings::workspaceFocusSlot1ShortcutChanged,
    &Settings::workspaceFocusSlot2ShortcutChanged,
    &Settings::workspaceFocusSlot3ShortcutChanged,
    &Settings::workspaceFocusSlot4ShortcutChanged,
    &Settings::workspaceFocusSlot5ShortcutChanged,
    &Settings::workspaceFocusSlot6ShortcutChanged,
    &Settings::workspaceFocusSlot7ShortcutChanged,
    &Settings::workspaceFocusSlot8ShortcutChanged,
    &Settings::workspaceFocusSlot9ShortcutChanged,
});
constexpr auto MoveSlotSignals = std::to_array<SlotSignalFn>({
    &Settings::workspaceMoveSlot1ShortcutChanged,
    &Settings::workspaceMoveSlot2ShortcutChanged,
    &Settings::workspaceMoveSlot3ShortcutChanged,
    &Settings::workspaceMoveSlot4ShortcutChanged,
    &Settings::workspaceMoveSlot5ShortcutChanged,
    &Settings::workspaceMoveSlot6ShortcutChanged,
    &Settings::workspaceMoveSlot7ShortcutChanged,
    &Settings::workspaceMoveSlot8ShortcutChanged,
    &Settings::workspaceMoveSlot9ShortcutChanged,
});
constexpr auto TargetSignals = std::to_array<SlotSignalFn>({
    &Settings::workspaceSlot1TargetChanged,
    &Settings::workspaceSlot2TargetChanged,
    &Settings::workspaceSlot3TargetChanged,
    &Settings::workspaceSlot4TargetChanged,
    &Settings::workspaceSlot5TargetChanged,
    &Settings::workspaceSlot6TargetChanged,
    &Settings::workspaceSlot7TargetChanged,
    &Settings::workspaceSlot8TargetChanged,
    &Settings::workspaceSlot9TargetChanged,
});
static_assert(std::size(FocusSlotSignals) == ConfigDefaults::WorkspaceSlotCount,
              "add a NOTIFY (and a Q_PROPERTY) for each new workspace focus slot");
static_assert(std::size(MoveSlotSignals) == ConfigDefaults::WorkspaceSlotCount,
              "add a NOTIFY (and a Q_PROPERTY) for each new workspace move slot");
static_assert(std::size(TargetSignals) == ConfigDefaults::WorkspaceSlotCount,
              "add a NOTIFY (and a Q_PROPERTY) for each new workspace slot target");
} // namespace

QString Settings::workspaceFocusSlotShortcut(int index) const
{
    return readSlotValue(m_store.get(), ConfigDefaults::shortcutsGlobalGroup(), &ConfigDefaults::workspaceFocusSlotKey,
                         index);
}

void Settings::setWorkspaceFocusSlotShortcut(int index, const QString& shortcut)
{
    if (!writeSlotValue(m_store.get(), ConfigDefaults::shortcutsGlobalGroup(), &ConfigDefaults::workspaceFocusSlotKey,
                        index, shortcut)) {
        return;
    }
    Q_EMIT(this->*FocusSlotSignals[index])();
    Q_EMIT settingsChanged();
}

QString Settings::workspaceMoveSlotShortcut(int index) const
{
    return readSlotValue(m_store.get(), ConfigDefaults::shortcutsGlobalGroup(), &ConfigDefaults::workspaceMoveSlotKey,
                         index);
}

void Settings::setWorkspaceMoveSlotShortcut(int index, const QString& shortcut)
{
    if (!writeSlotValue(m_store.get(), ConfigDefaults::shortcutsGlobalGroup(), &ConfigDefaults::workspaceMoveSlotKey,
                        index, shortcut)) {
        return;
    }
    Q_EMIT(this->*MoveSlotSignals[index])();
    Q_EMIT settingsChanged();
}

QString Settings::workspaceSlotTargetId(int index) const
{
    return readSlotValue(m_store.get(), ConfigDefaults::workspacesSlotsGroup(),
                         &ConfigDefaults::workspaceSlotTargetIdKey, index);
}

QString Settings::workspaceSlotTargetName(int index) const
{
    return readSlotValue(m_store.get(), ConfigDefaults::workspacesSlotsGroup(), &ConfigDefaults::workspaceSlotTargetKey,
                         index);
}

QVariantMap Settings::workspaceSlotTarget(int index) const
{
    return {
        {ConfigDefaults::namedEntryIdField(), workspaceSlotTargetId(index)},
        {ConfigDefaults::namedEntryNameField(), workspaceSlotTargetName(index)},
    };
}

void Settings::setWorkspaceSlotTarget(int index, const QString& entryId)
{
    if (!slotIndexInRange(index)) {
        return;
    }
    // The name is taken from the declaration the id names, never from the
    // caller, so the stored pair cannot disagree with itself. Re-assigning the
    // same id also refreshes a name the declaration has since been renamed
    // from, which is the name the daemon's fallback would otherwise try.
    //
    // Resolved against the declarations as the store holds them now, unsaved
    // ones included: a workspace added on the Named page and assigned here
    // before Save is a legitimate order of edits.
    QString id;
    QString name;
    if (!entryId.trimmed().isEmpty()) {
        const QUuid wanted = QUuid::fromString(entryId.trimmed());
        if (!wanted.isNull()) {
            const QVariantList entries = workspacesNamedEntries();
            for (const QVariant& entry : entries) {
                const QVariantMap declaration = entry.toMap();
                if (QUuid::fromString(declaration.value(ConfigDefaults::namedEntryIdField()).toString()) == wanted) {
                    id = wanted.toString();
                    name = declaration.value(ConfigDefaults::namedEntryNameField()).toString();
                    break;
                }
            }
        }
        if (id.isEmpty()) {
            qCWarning(lcConfig) << "setWorkspaceSlotTarget: no named workspace carries the id" << entryId
                                << "so quick slot" << index + 1 << "was left unchanged";
            return;
        }
    }
    // Both halves are written before either is compared, and the emit decision
    // takes both: a slot whose id stayed put but whose stored name moved is a
    // change the daemon's name fallback can observe.
    const bool idMoved = writeSlotValue(m_store.get(), ConfigDefaults::workspacesSlotsGroup(),
                                        &ConfigDefaults::workspaceSlotTargetIdKey, index, id);
    const bool nameMoved = writeSlotValue(m_store.get(), ConfigDefaults::workspacesSlotsGroup(),
                                          &ConfigDefaults::workspaceSlotTargetKey, index, name);
    if (!idMoved && !nameMoved) {
        return;
    }
    Q_EMIT(this->*TargetSignals[index])();
    Q_EMIT settingsChanged();
}

// The per-slot property accessors: thin forwards to the indexed forms above.
#define P_WORKSPACE_SLOT(N)                                                                                            \
    QString Settings::workspaceFocusSlot##N##Shortcut() const                                                          \
    {                                                                                                                  \
        return workspaceFocusSlotShortcut(N - 1);                                                                      \
    }                                                                                                                  \
    void Settings::setWorkspaceFocusSlot##N##Shortcut(const QString& shortcut)                                         \
    {                                                                                                                  \
        setWorkspaceFocusSlotShortcut(N - 1, shortcut);                                                                \
    }                                                                                                                  \
    QString Settings::workspaceMoveSlot##N##Shortcut() const                                                           \
    {                                                                                                                  \
        return workspaceMoveSlotShortcut(N - 1);                                                                       \
    }                                                                                                                  \
    void Settings::setWorkspaceMoveSlot##N##Shortcut(const QString& shortcut)                                          \
    {                                                                                                                  \
        setWorkspaceMoveSlotShortcut(N - 1, shortcut);                                                                 \
    }                                                                                                                  \
    QVariantMap Settings::workspaceSlot##N##Target() const                                                             \
    {                                                                                                                  \
        return workspaceSlotTarget(N - 1);                                                                             \
    }                                                                                                                  \
    void Settings::setWorkspaceSlot##N##Target(const QVariantMap& target)                                              \
    {                                                                                                                  \
        setWorkspaceSlotTarget(N - 1, target.value(ConfigDefaults::namedEntryIdField()).toString());                   \
    }

P_WORKSPACE_SLOT(1)
P_WORKSPACE_SLOT(2)
P_WORKSPACE_SLOT(3)
P_WORKSPACE_SLOT(4)
P_WORKSPACE_SLOT(5)
P_WORKSPACE_SLOT(6)
P_WORKSPACE_SLOT(7)
P_WORKSPACE_SLOT(8)
P_WORKSPACE_SLOT(9)
#undef P_WORKSPACE_SLOT

} // namespace PlasmaZones
