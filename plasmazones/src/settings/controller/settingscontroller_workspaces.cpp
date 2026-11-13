// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// SettingsController: the dynamic-workspaces surface the Workspaces pages and
// the rule editor bind to. The feature gate (compiled in, enabled, running),
// the cap banner, the constants QML cannot read on its own, the named
// workspace list and its shared name rule, and the chord helpers the in-page
// recorders use. Split from the page-key TU, which is about page classes and
// shared-domain key lists rather than any one feature; same class, separate
// TU.

#include "settingscontroller.h"

#include "common/kwinperoutputdesktops.h"
#include "config/configdefaults.h"
#include "config/settingsschema.h"

#include <PhosphorRules/ActionParams.h>
#include <PhosphorShortcuts/GlobalShortcutBlock.h>
#include <PhosphorWorkspaces/WorkspaceReconciler.h>

#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QKeySequence>
#include <QLatin1String>
#include <QMetaProperty>
#include <QStandardPaths>
#include <QUuid>

namespace PlasmaZones {

// A name the Named Workspaces page accepts is one a RouteToWorkspace rule must
// be able to carry, or the rule editor's workspace picker would offer a name
// the rule validator then refuses. The rule bound is the wider one by design,
// so the app pins the ordering here, where the picker and the page meet.
static_assert(PhosphorRules::MaxWorkspaceNameLength >= ConfigDefaults::WorkspaceNameMaxLength,
              "a declared workspace name must fit the RouteToWorkspace rule bound");
// The rule's id field carries an entry id as canonicalWorkspaceEntryId spells
// it: a braced QUuid string.
static_assert(PhosphorRules::MaxWorkspaceIdLength >= 38,
              "a braced QUuid string must fit the RouteToWorkspace id bound");

namespace {

/// The kwinrc this session's user edits. KWin's per-output toggle in System
/// Settings writes here, which is what the watcher below follows.
QString userKWinConfigPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kwinrc");
}

/// The Settings Q_PROPERTYs that hold a chord the daemon grabs globally: every
/// QString property whose name ends in "Shortcut". The editor's own shortcuts
/// are app-local, not global grabs, and carry the "editor" prefix.
bool isGlobalChordProperty(const QMetaProperty& prop)
{
    if (prop.metaType().id() != QMetaType::QString) {
        return false;
    }
    const QLatin1String name(prop.name());
    return name.endsWith(QLatin1String("Shortcut")) && !name.startsWith(QLatin1String("editor"));
}

/// The display label for the workspace chord a Settings property names, in
/// the daemon's own wording (shortcutmanager_table.cpp and the slot loops in
/// shortcutmanager.cpp), so a conflict message names the action the way the
/// cheatsheet and System Settings do. Empty for a property outside the
/// workspace family.
QString workspaceChordLabel(const QString& property)
{
    if (property == QLatin1String("workspaceFocusUpShortcut")) {
        return PhosphorI18n::tr("Focus Workspace Above");
    }
    if (property == QLatin1String("workspaceFocusDownShortcut")) {
        return PhosphorI18n::tr("Focus Workspace Below");
    }
    if (property == QLatin1String("workspaceMoveWindowUpShortcut")) {
        return PhosphorI18n::tr("Move Window to Workspace Above");
    }
    if (property == QLatin1String("workspaceMoveWindowDownShortcut")) {
        return PhosphorI18n::tr("Move Window to Workspace Below");
    }
    if (property == QLatin1String("workspaceMoveColumnUpShortcut")) {
        return PhosphorI18n::tr("Move Column to Workspace Above");
    }
    if (property == QLatin1String("workspaceMoveColumnDownShortcut")) {
        return PhosphorI18n::tr("Move Column to Workspace Below");
    }
    if (property == QLatin1String("workspaceReorderUpShortcut")) {
        return PhosphorI18n::tr("Move Workspace Up");
    }
    if (property == QLatin1String("workspaceReorderDownShortcut")) {
        return PhosphorI18n::tr("Move Workspace Down");
    }
    if (property == QLatin1String("workspaceMoveToMonitorLeftShortcut")) {
        return PhosphorI18n::tr("Move Workspace to Monitor on the Left");
    }
    if (property == QLatin1String("workspaceMoveToMonitorRightShortcut")) {
        return PhosphorI18n::tr("Move Workspace to Monitor on the Right");
    }
    for (int slot = 1; slot <= ConfigDefaults::WorkspaceSlotCount; ++slot) {
        if (property == QStringLiteral("workspaceFocusSlot%1Shortcut").arg(slot)) {
            return PhosphorI18n::tr("Focus Workspace %1", "single workspace slot number").arg(slot);
        }
        if (property == QStringLiteral("workspaceMoveSlot%1Shortcut").arg(slot)) {
            return PhosphorI18n::tr("Move Window to Workspace Slot %1", "single workspace slot number").arg(slot);
        }
    }
    return {};
}

/// The key a named entry is addressed by: its persistent id, or its name when
/// no producer has stamped one yet (the same key ShortcutIds::workspaceNamed*
/// uses for the entry's chords).
QString namedEntryKey(const QVariantMap& entry)
{
    const QString id = canonicalWorkspaceEntryId(entry.value(ConfigDefaults::namedEntryIdField()));
    return id.isEmpty() ? entry.value(ConfigDefaults::namedEntryNameField()).toString().trimmed() : id;
}

QString workspaceNameVerdictToken(WorkspaceNameVerdict verdict)
{
    switch (verdict) {
    case WorkspaceNameVerdict::Accepted:
        return QStringLiteral("accepted");
    case WorkspaceNameVerdict::Empty:
        return QStringLiteral("empty");
    case WorkspaceNameVerdict::TooLong:
        return QStringLiteral("tooLong");
    case WorkspaceNameVerdict::ForbiddenCharacter:
        return QStringLiteral("forbiddenCharacter");
    }
    return QStringLiteral("empty");
}

} // namespace

// ── Feature gate ─────────────────────────────────────────────────────────────

bool SettingsController::workspacesAvailable() const
{
#ifdef USE_KDE_FRAMEWORKS
    return true;
#else
    return false;
#endif
}

bool SettingsController::kwinPerOutputDesktopsEnabled() const
{
#ifdef USE_KDE_FRAMEWORKS
    // One shared reader with the daemon's gate. The decoding rationale, and
    // why the two must not drift apart again, lives on the helper.
    return kwinPerOutputVirtualDesktopsEnabled();
#else
    return false;
#endif
}

bool SettingsController::workspacesActive() const
{
    return m_workspacesActive;
}

void SettingsController::refreshWorkspacesActive()
{
#ifdef USE_KDE_FRAMEWORKS
    // Ordered so the kwinrc read is the last resort: off, or consent already
    // recorded, needs no file read.
    const bool active = m_settings.workspacesEnabled()
        && (m_settings.workspacesManageKWinPerOutput() || kwinPerOutputVirtualDesktopsEnabled());
#else
    const bool active = false;
#endif
    if (active == m_workspacesActive) {
        return;
    }
    m_workspacesActive = active;
    Q_EMIT workspacesActiveChanged();
}

void SettingsController::wireWorkspaces()
{
    // The named list feeds namedWorkspaces, the rule summaries and the slot
    // pickers. Forwarded rather than re-derived so every consumer sees one
    // notification per change to the list.
    connect(&m_settings, &Settings::workspacesNamedEntriesChanged, this, &SettingsController::namedWorkspacesChanged);
#ifdef USE_KDE_FRAMEWORKS
    connect(&m_settings, &Settings::workspacesEnabledChanged, this, &SettingsController::refreshWorkspacesActive);
    connect(&m_settings, &Settings::workspacesManageKWinPerOutputChanged, this,
            &SettingsController::refreshWorkspacesActive);
    // The other input is KWin's own per-output flag, which the user can flip
    // in System Settings while this app is open, and which the daemon writes
    // once consent is recorded. KConfig replaces kwinrc by renaming a new file
    // over it, which drops an inotify watch on the old file, so every change
    // re-arms the watch. The directory is watched only while kwinrc does not
    // exist, to see it appear.
    m_kwinConfigWatcher = new QFileSystemWatcher(this);
    const auto armWatch = [this] {
        const QString file = userKWinConfigPath();
        const QString dir = QFileInfo(file).absolutePath();
        if (QFileInfo::exists(file)) {
            if (!m_kwinConfigWatcher->files().contains(file)) {
                m_kwinConfigWatcher->addPath(file);
            }
            if (m_kwinConfigWatcher->directories().contains(dir)) {
                m_kwinConfigWatcher->removePath(dir);
            }
        } else if (!m_kwinConfigWatcher->directories().contains(dir)) {
            m_kwinConfigWatcher->addPath(dir);
        }
    };
    const auto onTouched = [this, armWatch] {
        armWatch();
        refreshWorkspacesActive();
    };
    connect(m_kwinConfigWatcher, &QFileSystemWatcher::fileChanged, this, onTouched);
    connect(m_kwinConfigWatcher, &QFileSystemWatcher::directoryChanged, this, onTouched);
    armWatch();
#endif
    refreshWorkspacesActive();
}

// ── Cap banner and constants ─────────────────────────────────────────────────

bool SettingsController::workspacesAtCap() const
{
    // Read off the daemon's own desktop count (getVirtualDesktopCount, kept
    // current through virtualDesktopCountChanged), so the settings app needs
    // no KWin listener of its own. A count that did not come from the daemon
    // is the display fallback of 1 and says nothing about the cap.
    return m_virtualDesktopCountFromDaemon
        && m_virtualDesktopCount >= PhosphorWorkspaces::WorkspaceReconciler::DefaultDesktopCap;
}

int SettingsController::workspacesDesktopCap() const
{
    return PhosphorWorkspaces::WorkspaceReconciler::DefaultDesktopCap;
}

int SettingsController::workspaceSlotCount() const
{
    return ConfigDefaults::WorkspaceSlotCount;
}

int SettingsController::workspaceNameMaxLength() const
{
    return ConfigDefaults::WorkspaceNameMaxLength;
}

int SettingsController::workspaceNamedEntryMaxCount() const
{
    return ConfigDefaults::WorkspaceNamedEntryMaxCount;
}

QVariantMap SettingsController::namedEntryFields() const
{
    return {
        {QStringLiteral("id"), ConfigDefaults::namedEntryIdField()},
        {QStringLiteral("name"), ConfigDefaults::namedEntryNameField()},
        {QStringLiteral("output"), ConfigDefaults::namedEntryOutputField()},
        {QStringLiteral("position"), ConfigDefaults::namedEntryPositionField()},
        {QStringLiteral("focusShortcut"), ConfigDefaults::namedEntryFocusShortcutField()},
        {QStringLiteral("moveShortcut"), ConfigDefaults::namedEntryMoveShortcutField()},
    };
}

// ── Named workspaces ─────────────────────────────────────────────────────────

QVariantList SettingsController::namedWorkspaces() const
{
    // The validator (canonicalNamedEntries) runs on every store read, so each
    // name here is already canonical and unique, and each id is canonical or
    // empty.
    QVariantList out;
    const QVariantList entries = m_settings.workspacesNamedEntries();
    out.reserve(entries.size());
    for (const QVariant& value : entries) {
        const QVariantMap entry = value.toMap();
        const QString name = entry.value(ConfigDefaults::namedEntryNameField()).toString();
        if (name.isEmpty()) {
            continue;
        }
        out.append(QVariantMap{
            {ConfigDefaults::namedEntryIdField(), entry.value(ConfigDefaults::namedEntryIdField()).toString()},
            {ConfigDefaults::namedEntryNameField(), name},
        });
    }
    return out;
}

QString SettingsController::namedWorkspaceName(const QString& entryId) const
{
    const QString wanted = canonicalWorkspaceEntryId(entryId);
    if (wanted.isEmpty()) {
        return {};
    }
    const QVariantList entries = m_settings.workspacesNamedEntries();
    for (const QVariant& value : entries) {
        const QVariantMap entry = value.toMap();
        if (canonicalWorkspaceEntryId(entry.value(ConfigDefaults::namedEntryIdField())) == wanted) {
            return entry.value(ConfigDefaults::namedEntryNameField()).toString();
        }
    }
    return {};
}

QVariantMap SettingsController::checkWorkspaceName(const QString& raw) const
{
    const WorkspaceNameCheck check = PlasmaZones::checkWorkspaceName(raw);
    return {
        {QStringLiteral("accepted"), check.verdict == WorkspaceNameVerdict::Accepted},
        {QStringLiteral("verdict"), workspaceNameVerdictToken(check.verdict)},
        {QStringLiteral("name"), check.name},
    };
}

QString SettingsController::newWorkspaceEntryId() const
{
    return QUuid::createUuid().toString();
}

QVariantList SettingsController::withWorkspaceEntryIds(const QVariantList& entries) const
{
    // The validator keeps ids but never mints one, so the writer does. Only
    // an entry with no usable id gets a fresh one: an existing id is what
    // slot targets, rule targets and the daemon follow through a rename.
    QVariantList out;
    out.reserve(entries.size());
    for (const QVariant& value : entries) {
        QVariantMap entry = value.toMap();
        if (canonicalWorkspaceEntryId(entry.value(ConfigDefaults::namedEntryIdField())).isEmpty()) {
            entry.insert(ConfigDefaults::namedEntryIdField(), newWorkspaceEntryId());
        }
        out.append(entry);
    }
    return out;
}

// ── Chord recorders ──────────────────────────────────────────────────────────

QString SettingsController::canonicalShortcut(const QString& chord) const
{
    const QString trimmed = chord.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    const QKeySequence sequence = QKeySequence::fromString(trimmed, QKeySequence::PortableText);
    if (sequence.isEmpty()) {
        return {};
    }
    for (int i = 0; i < sequence.count(); ++i) {
        if (sequence[i].key() == Qt::Key_unknown) {
            return {};
        }
    }
    // PortableText in Qt's own modifier order (Meta, Ctrl, Alt, Shift), the
    // spelling the daemon registers and the config stores.
    return sequence.toString(QKeySequence::PortableText);
}

QVariantMap SettingsController::workspaceShortcutConflict(const QString& chord, const QString& selfKey) const
{
    QVariantMap none{{QStringLiteral("conflict"), false}};
    const QString wanted = canonicalShortcut(chord);
    if (wanted.isEmpty()) {
        return none;
    }
    const auto conflict = [](const QString& owner, const QString& label) {
        return QVariantMap{
            {QStringLiteral("conflict"), true},
            {QStringLiteral("owner"), owner},
            {QStringLiteral("label"), label},
        };
    };

    // Every chord the config holds for a global PlasmaZones action, pending
    // edits included: the store is what the daemon registers on the next
    // reload.
    const QMetaObject* mo = m_settings.metaObject();
    for (int i = 0; i < mo->propertyCount(); ++i) {
        const QMetaProperty prop = mo->property(i);
        if (!isGlobalChordProperty(prop)) {
            continue;
        }
        const QString property = QString::fromLatin1(prop.name());
        if (property == selfKey) {
            continue;
        }
        if (canonicalShortcut(prop.read(&m_settings).toString()) != wanted) {
            continue;
        }
        const QString label = workspaceChordLabel(property);
        return conflict(property, label.isEmpty() ? PhosphorI18n::tr("another PlasmaZones action") : label);
    }

    // The named workspaces' own chords, from the in-memory list (so another
    // row's unsaved chord counts too).
    const QVariantList entries = m_settings.workspacesNamedEntries();
    for (const QVariant& value : entries) {
        const QVariantMap entry = value.toMap();
        const QString name = entry.value(ConfigDefaults::namedEntryNameField()).toString();
        for (const QString& field :
             {ConfigDefaults::namedEntryFocusShortcutField(), ConfigDefaults::namedEntryMoveShortcutField()}) {
            const QString owner = namedEntryShortcutKey(entry, field);
            if (owner == selfKey || canonicalShortcut(entry.value(field).toString()) != wanted) {
                continue;
            }
            const QString label = field == ConfigDefaults::namedEntryFocusShortcutField()
                ? PhosphorI18n::tr("Focus Workspace \"%1\"", "named workspace shortcut").arg(name)
                : PhosphorI18n::tr("Move Window to Workspace \"%1\"", "named workspace shortcut").arg(name);
            return conflict(owner, label);
        }
    }
    return none;
}

QString SettingsController::namedEntryShortcutKey(const QVariantMap& entry, const QString& field) const
{
    return QStringLiteral("entry:%1:%2").arg(namedEntryKey(entry), field);
}

bool SettingsController::canBlockGlobalShortcuts() const
{
    return PhosphorShortcuts::canBlockGlobalShortcuts();
}

bool SettingsController::setGlobalShortcutsBlocked(bool blocked)
{
    return PhosphorShortcuts::setGlobalShortcutsBlocked(blocked);
}

} // namespace PlasmaZones
