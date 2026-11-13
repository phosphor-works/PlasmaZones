// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// PlasmaZones settings schema: declarative description of every key that
// Settings persists, for use with PhosphorConfig::Store.
//
// Assembled from one appendXxx() call per config domain; each adds that
// domain's groups. A key Settings persists that no appendXxx() declares is
// deleted by Settings::purgeStaleKeys on the next save.

#include "plasmazones_export.h"

#include <PhosphorConfig/Schema.h>

#include <QString>
#include <QVariant>

namespace PlasmaZones {

/// Returns the full PZ settings schema. Called once during Settings
/// construction and handed to Store; a new domain adds its appendXxx() call
/// to buildSettingsSchema.
PLASMAZONES_EXPORT PhosphorConfig::Schema buildSettingsSchema();

/// Shared immortal schema for READ-ONLY consumers (choice lookups, value
/// resolution). Built once on first use. Settings must NOT use this — it
/// hands its Schema to a Store, which owns a copy per instance.
PLASMAZONES_EXPORT const PhosphorConfig::Schema& cachedSettingsSchema();

// ─── Workspace names and ids ────────────────────────────────────────────────

/// Why checkWorkspaceName refused a name, or Accepted.
enum class WorkspaceNameVerdict {
    Accepted,
    Empty,
    TooLong,
    ForbiddenCharacter,
};

/// The result of checkWorkspaceName: the verdict, and the canonical name when
/// the verdict is Accepted (empty otherwise).
struct WorkspaceNameCheck
{
    WorkspaceNameVerdict verdict = WorkspaceNameVerdict::Empty;
    QString name;
};

/// The one workspace-name rule. Every writer of a named-workspace name (the
/// Workspaces.Named validator, the settings page's Add and rename forms, the
/// daemon adopting a name the user typed into KWin) checks against it, so
/// none of them can accept a name another would drop.
///
/// Takes the RAW text and trims it with QString::trimmed(); the trimmed text
/// is the canonical name. Refused when nothing is left (Empty), when it is
/// longer than ConfigDefaults::WorkspaceNameMaxLength UTF-16 units (TooLong),
/// or when any code point is a control character (Cc), an unpaired
/// surrogate, a Unicode noncharacter, LINE or PARAGRAPH SEPARATOR, or a
/// bidirectional formatting control (ForbiddenCharacter). Every other code
/// point is accepted, including emoji and other non-BMP characters, joiners,
/// variation selectors and code points unassigned in Qt's Unicode tables.
///
/// The name is not just display text: the daemon hands it to KGlobalAccel as
/// an action's description, which lands in kglobalshortcutsrc. Uniqueness is
/// a list property, so it is the caller's to check against the canonical
/// names of the other entries.
PLASMAZONES_EXPORT WorkspaceNameCheck checkWorkspaceName(const QString& raw);

/// Canonicalize a named-workspace entry id (or a slot target's id): a string
/// that parses as a UUID becomes QUuid::toString() (braced, lowercase), and
/// anything else becomes empty, meaning "no id". Pure, so it never mints.
PLASMAZONES_EXPORT QString canonicalWorkspaceEntryId(const QVariant& raw);

// ─── Group helpers ──────────────────────────────────────────────────────────
// Each helper appends one domain's groups to the schema.

/// Canonicalize a trigger list (cap size, coerce entries to
/// {modifier:int, mouseButton:int} maps). Defined in settingsschema.cpp;
/// shared with the per-domain schema TUs because trigger-list keys now span
/// domains (Tiling.Behavior and Scrolling.Behavior).
QVariant canonicalTriggerList(const QVariant& v);

/// canonicalTriggerList plus a drop of the AlwaysActive sentinel, for the
/// wheel chord lists that are read with exact matching. See the definition
/// for why the sentinel cannot travel that path.
QVariant canonicalWheelTriggerList(const QVariant& v);

/// Canonicalize a theme-fallback colour: keep EMPTY (the "follow the colour
/// scheme" sentinel) and any string QColor can parse, and drop anything else
/// back to empty. The D-Bus boundary already refuses an unparseable colour,
/// but the DISK path does not go through it — a hand-edited config reached
/// QML unchecked and Qt paints an invalid QColor as black. Shared by every
/// theme-fallback colour key: the zone colours, the Windows border/tint
/// colours, the tab indicator's three, and the drop indicator's two.
QVariant canonicalThemeFallbackColor(const QVariant& v);

/// Bound the animation-shader assignment tree at the persistence boundary:
/// string lengths, parameter-map size and value shapes, override count. The
/// twin of sanitizeOverlayShaderTree, sharing its bounds; see
/// settingsschema_shadertrees.cpp for why the motion keys beside it cannot get
/// the same treatment.
QVariant sanitizeShaderProfileTree(const QVariant& v);

/// The decoration-tree twin of the above. Its parameter map is nested one
/// level deeper ({packId -> {paramId -> value}}), so only the inner level is
/// bounded scalar-only.
QVariant sanitizeDecorationProfileTree(const QVariant& v);

/// The MOTION tree's read-side bound, which simply runs the same canonicaliser its
/// setter does (`canonicalMotionProfileTree`, settings/profiletrees.cpp).
///
/// Registered because the setter cannot be the only boundary: configmigration_v8 writes
/// this key directly into the JSON document, outside the store, and config.json is
/// hand-editable, so neither path ever passed through a setter. Sharing one function is
/// what keeps the "canonicalise and bound on one pass" property the setter relies on.
QVariant sanitizeMotionProfileTree(const QVariant& v);

/// The motion tree's persistence form, shared by the setter and the sanitizer above.
QVariantMap canonicalMotionProfileTree(const QVariantMap& tree);

void appendShadersSchema(PhosphorConfig::Schema& schema);
void appendOverlayShadersSchema(PhosphorConfig::Schema& schema);
void appendAppearanceSchema(PhosphorConfig::Schema& schema);
void appendOrderingSchema(PhosphorConfig::Schema& schema);
void appendAnimationsSchema(PhosphorConfig::Schema& schema);
void appendRenderingSchema(PhosphorConfig::Schema& schema);
void appendPerformanceSchema(PhosphorConfig::Schema& schema);
void appendZoneGeometrySchema(PhosphorConfig::Schema& schema);
void appendShortcutsSchema(PhosphorConfig::Schema& schema);
void appendEditorSchema(PhosphorConfig::Schema& schema);
void appendExclusionsSchema(PhosphorConfig::Schema& schema);
void appendDisplaySchema(PhosphorConfig::Schema& schema);
void appendZoneSelectorSchema(PhosphorConfig::Schema& schema);
void appendActivationSchema(PhosphorConfig::Schema& schema);
void appendBehaviorSchema(PhosphorConfig::Schema& schema);
// The tiling entry point lives in settingsschema_tiling.cpp (split out by
// domain, the scrolling shape).
void appendAutotilingSchema(PhosphorConfig::Schema& schema);
// The three scrolling entry points live in settingsschema_scrolling.cpp (split
// out by domain). appendScrollingShortcutsSchema is called from
// appendShortcutsSchema, not from buildSettingsSchema.
void appendScrollingSchema(PhosphorConfig::Schema& schema);
void appendScrollingZoneSelectorSchema(PhosphorConfig::Schema& schema);
void appendScrollingShortcutsSchema(PhosphorConfig::Schema& schema);
void appendWindowsSchema(PhosphorConfig::Schema& schema);
void appendWorkspacesSchema(PhosphorConfig::Schema& schema);
/// The dynamic-workspaces rows of the Shortcuts.Global key list. Lives with
/// the rest of the workspaces schema in settingsschema_workspaces.cpp;
/// appendShortcutsSchema calls it while building that group's list, so the
/// keys land in Shortcuts.Global alongside every other global verb.
void appendWorkspacesShortcutKeys(QVector<PhosphorConfig::KeyDef>& globals);
void appendGapsSchema(PhosphorConfig::Schema& schema);
void appendDecorationsSchema(PhosphorConfig::Schema& schema);

} // namespace PlasmaZones
