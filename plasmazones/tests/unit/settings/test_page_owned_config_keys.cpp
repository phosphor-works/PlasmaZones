// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_page_owned_config_keys.cpp
 * @brief Ownership invariants for SettingsController::pageOwnedConfigKeys.
 *
 * The manifest maps each settings leaf page to the (group, key) pairs it
 * edits, and it drives the breadcrumb kebab's per-page Reset and Discard plus
 * the per-page dirty indicator. It is maintained by hand, and both ways of
 * getting it wrong are silent:
 *
 *   - a key MISSING from every page's list drops out of per-page Reset and
 *     never marks its page dirty, so an edit to it looks unsaved-free and
 *     survives a Reset the user believes cleared the page, and
 *   - a key listed by TWO pages lets one page's Discard revert an edit made on
 *     the other (the shared-tree failure this repo has already shipped once).
 *
 * The manifest's own header states both invariants and notes that a guard has
 * to link the whole SettingsController topology TU. This file is that guard.
 * It is scoped to what can be checked cheaply and exactly: the one-owner rule
 * across the WHOLE manifest, and complete coverage of a swept subset of
 * schema groups. everySweptSchemaKeyIsOwned lists the swept groups and why
 * each one joined; every Scrolling and Editor.* group the schema declares is
 * swept too, found by prefix rather than listed.
 *
 * The dynamic-workspaces pages exist only in a USE_KDE_FRAMEWORKS build. The
 * Workspaces.* schema rows stay declared in every build, so an OFF build
 * checks the opposite: no page owns them and no exemption names them.
 */

#include <QSet>
#include <QString>
#include <QStringList>
#include <QTest>

#include <PhosphorConfig/Schema.h>

#include "config/configdefaults.h"
#include "config/settings.h"
#include "config/settingsschema.h"
#include "settings/controller/settingscontroller.h"

using namespace PlasmaZones;

namespace {

QString qualify(const QString& group, const QString& key)
{
    return group + QLatin1Char('/') + key;
}

/// Schema keys, qualified as "group/key", that no page manifest owns ON
/// PURPOSE. Every other key in a swept group has to have exactly one owner.
const QSet<QString>& deliberatelyUnowned()
{
    static const QSet<QString> kSet{
        // The scrolling master switch is NOT here any more: it is owned by
        // scrolling-columns (its pending sidebar flip has to participate in
        // value-based dirtiness or it is silently lost on exit), and the
        // page-Reset hazard that used to justify unowning it is closed by
        // resetExemptModeEnableKeys() instead — pinned by the exemption slot
        // below.
        // The two global preset lists are NOT here any more: the Columns page
        // carries their editors again, because they are the vocabulary the
        // cycling shortcuts walk on a screen with no template — reachable
        // now that a screen can be set to no template explicitly. An editor
        // means an owner, so they moved into that page's manifest.
        // The default template is NOT here any more either: a template is
        // made default from its card's context menu on Scrolling → Templates,
        // and the library pages are never dirty (isLibraryPage), so the
        // context menu attributes the write to scrolling-columns — the owner
        // of the rest of the Scrolling group — through an external-edit
        // envelope, and the key moved into that page's manifest with it.
        // Tiling.Behavior/LockedScreens is runtime state, not a preference.
        // It records which screens the user has locked, is written only
        // through the D-Bus setter (settingsadaptor_registry.cpp's
        // "lockedScreens") and through setContextLocked, and no settings page
        // renders it. Giving it a page owner would be actively wrong: a Reset
        // on that page would silently unlock every screen the user had
        // locked. Surfaced when the release-grace work brought
        // Tiling.Behavior under this sweep for the first time.
        qualify(ConfigDefaults::tilingBehaviorGroup(), ConfigDefaults::lockedScreensKey()),
    };
    return kSet;
}

/// Every (group, key) any page manifest lists, qualified.
QSet<QString> ownedKeys()
{
    QSet<QString> owned;
    const auto& manifest = SettingsController::pageOwnedConfigKeys();
    for (auto it = manifest.constBegin(); it != manifest.constEnd(); ++it) {
        for (const Settings::ConfigKey& pair : it.value()) {
            owned.insert(qualify(pair.first, pair.second));
        }
    }
    return owned;
}

/// The schema groups an ON build's workspaces pages own outright.
QStringList workspacesSchemaGroups()
{
    return {ConfigDefaults::workspacesBehaviorGroup(), ConfigDefaults::workspacesNamedGroup(),
            ConfigDefaults::workspacesSlotsGroup()};
}

/// Whether the (@p group, @p key) pair is one a workspaces page edits: a
/// key in a Workspaces.* group, or a workspace chord leaf of Shortcuts.Global.
bool isWorkspaceKey(const QString& group, const QString& key)
{
    if (group.startsWith(ConfigDefaults::workspacesGroup() + QLatin1Char('.'))) {
        return true;
    }
    return group == ConfigDefaults::shortcutsGlobalGroup() && key.startsWith(QLatin1String("Workspace"));
}

} // namespace

class TestPageOwnedConfigKeys : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /// No (group, key) pair may appear in two pages' lists. A shared pair
    /// means one page's Discard silently reverts the other page's edit.
    void everyKeyHasAtMostOneOwner()
    {
        const auto& manifest = SettingsController::pageOwnedConfigKeys();
        QHash<QString, QString> owner;
        QStringList duplicates;
        for (auto it = manifest.constBegin(); it != manifest.constEnd(); ++it) {
            for (const Settings::ConfigKey& pair : it.value()) {
                const QString qualified = qualify(pair.first, pair.second);
                const auto existing = owner.constFind(qualified);
                if (existing != owner.constEnd()) {
                    duplicates.append(qualified + QLatin1String(": ") + existing.value() + QLatin1String(" and ")
                                      + it.key());
                } else {
                    owner.insert(qualified, it.key());
                }
            }
        }
        duplicates.sort();
        QVERIFY2(duplicates.isEmpty(), qPrintable(duplicates.join(QLatin1String("; "))));
    }

    /// Every pair any page claims must be a key the schema actually declares.
    /// A pair naming a key that no longer exists is dead weight in Reset and
    /// never reports dirty, which is the "missing key" failure wearing a
    /// disguise: the page looks covered while the setting is not.
    ///
    /// Per-screen groups are excluded, not by allowlist but structurally: they
    /// are dynamic (one group per monitor, named from the screen id) and so
    /// have no static schema declaration to match against.
    void everyOwnedKeyIsSchemaDeclared()
    {
        const PhosphorConfig::Schema schema = buildSettingsSchema();
        const auto& manifest = SettingsController::pageOwnedConfigKeys();
        QStringList undeclared;
        for (auto it = manifest.constBegin(); it != manifest.constEnd(); ++it) {
            for (const Settings::ConfigKey& pair : it.value()) {
                const auto group = schema.groups.constFind(pair.first);
                if (group == schema.groups.constEnd()) {
                    undeclared.append(it.key() + QLatin1String(" -> ") + qualify(pair.first, pair.second)
                                      + QLatin1String(" (no such group)"));
                    continue;
                }
                bool found = false;
                for (const PhosphorConfig::KeyDef& def : *group) {
                    if (def.key == pair.second) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    undeclared.append(it.key() + QLatin1String(" -> ") + qualify(pair.first, pair.second));
                }
            }
        }
        undeclared.sort();
        QVERIFY2(undeclared.isEmpty(), qPrintable(undeclared.join(QLatin1String("; "))));
    }

    /// The other direction, over the swept groups (listed in the body, plus
    /// every Editor.* group): every key the schema declares under them must be
    /// owned by exactly one page, except the entries listed in
    /// deliberatelyUnowned().
    ///
    /// The scrolling pages also SHOW two settings they deliberately do not
    /// own — Tiling.Gaps/SmartGaps, forwarded from the shared gaps group and
    /// owned by tiling-behavior, and the per-monitor width and height
    /// overrides, which live in the per-screen scrolling store rather than in
    /// flat config keys. Neither is a Scrolling* schema key, so neither is in
    /// scope here; they are called out so a future reader does not "fix" their
    /// absence by adding them to a scrolling page's list and breaking the
    /// one-owner rule.
    void everySweptSchemaKeyIsOwned()
    {
        const PhosphorConfig::Schema schema = buildSettingsSchema();
        const QSet<QString> owned = ownedKeys();

        QStringList unowned;
        int checked = 0;
        // Every Scrolling schema group, found by prefix below. The two
        // indicator groups were once outside a hand-listed sweep, so a key
        // added to either could drop out of per-page Reset with nothing
        // failing, which is the exact class of regression this file exists
        // to catch. The DropIndicator group was added while that gap was
        // open, and the Wheel.Focus and Wheel.View groups were missed the
        // same way, hence the prefix. Rendering rides along for the same
        // reason: its Gpu key shipped without a manifest owner (no dirty
        // mark, per-page Reset and Discard silently skipped it) precisely
        // because this sweep did not cover the group.
        // Decorations.Performance joined when BlurScaleMultiplier landed:
        // every key in it is owned by the window-appearance page today, and
        // sweeping the group is what kept SuppressWhileFullscreen from
        // shipping ownerless the way Rendering.Gpu did.
        // Snapping.Behavior, Snapping.Behavior.ZoneSpan and Tiling.Behavior
        // joined with the trigger release graces: those arms were the only
        // ones outside this sweep, so deleting any of their ownership entries
        // would have dropped the key out of per-page dirty tracking, Reset
        // and Discard with nothing failing.
        // The Editor.* groups joined with Editor.Background (issue #1161): a
        // key missing from the editor page's manifest leaves the page clean
        // after an edit, so Save never appears and the change never reaches
        // the daemon or the open editor. They are found by prefix, so an
        // Editor group added later is swept without editing this list.
        QStringList sweptGroups = {
            ConfigDefaults::renderingGroup(),        ConfigDefaults::decorationsPerformanceGroup(),
            ConfigDefaults::snappingBehaviorGroup(), ConfigDefaults::snappingBehaviorZoneSpanGroup(),
            ConfigDefaults::tilingBehaviorGroup(),
        };
#ifdef USE_KDE_FRAMEWORKS
        // The Workspaces schema groups joined with dynamic per-monitor
        // workspaces. Workspaces.Behavior holds the gate scalars,
        // Workspaces.Named the named-entry list and Workspaces.Slots the
        // target pair of every quick slot, all owned by the workspaces
        // pages. Sweeping the groups keeps another slot, or another behaviour
        // scalar, from shipping ownerless the way Rendering.Gpu did. The
        // workspace chords are Shortcuts.Global keys, outside this sweep;
        // workspacesShortcutsPageOwnsEveryWorkspaceChord covers them. In a
        // build without the feature no page exists to own these groups, and
        // workspaceKeysHaveNoOwnerWithoutTheFeature checks that instead.
        sweptGroups.append(workspacesSchemaGroups());
#endif
        const QString scrollingGroup = ConfigDefaults::scrollingGroup();
        const QString scrollingPrefix = scrollingGroup + QLatin1Char('.');
        const QString editorPrefix = ConfigDefaults::editorGroup() + QLatin1Char('.');
        for (auto it = schema.groups.constBegin(); it != schema.groups.constEnd(); ++it) {
            if (it.key() == scrollingGroup || it.key().startsWith(scrollingPrefix)
                || it.key().startsWith(editorPrefix)) {
                sweptGroups.append(it.key());
            }
        }
        // The prefix sweeps must find the groups declared today, so a renamed
        // prefix cannot quietly sweep nothing. The wheel groups are named
        // because a hand-listed sweep once missed them.
        for (const QString& prefixedGroup :
             {scrollingGroup, ConfigDefaults::scrollingBehaviorGroup(), ConfigDefaults::scrollingTabIndicatorGroup(),
              ConfigDefaults::scrollingDropIndicatorGroup(), ConfigDefaults::scrollingZoneSelectorGroup(),
              ConfigDefaults::scrollingDragScrollGroup(), ConfigDefaults::scrollingWheelFocusGroup(),
              ConfigDefaults::scrollingWheelViewGroup(), ConfigDefaults::editorShortcutsGroup(),
              ConfigDefaults::editorSnappingGroup(), ConfigDefaults::editorFillOnDropGroup(),
              ConfigDefaults::editorBackgroundGroup()}) {
            QVERIFY2(sweptGroups.contains(prefixedGroup), qPrintable(prefixedGroup));
        }
        for (const QString& group : std::as_const(sweptGroups)) {
            const auto it = schema.groups.constFind(group);
            if (it == schema.groups.constEnd()) {
                // Collect-then-assert, like the rest of this file: an abort
                // here would hide every group after the first missing one.
                unowned.append(QStringLiteral("[missing schema group] ") + group);
                continue;
            }
            for (const PhosphorConfig::KeyDef& def : *it) {
                const QString qualified = qualify(group, def.key);
                if (deliberatelyUnowned().contains(qualified)) {
                    continue;
                }
                ++checked;
                if (!owned.contains(qualified)) {
                    unowned.append(qualified);
                }
            }
        }
        unowned.sort();
        QVERIFY2(unowned.isEmpty(), qPrintable(unowned.join(QLatin1String("; "))));
        // The sweep must actually have swept something. Without this the whole
        // slot passes vacuously if the group accessors are ever renamed and
        // the schema stops declaring these groups under these names.
        QVERIFY(checked > 0);
    }

    /// The allowlist itself is a schema key, not a stale string. An entry that
    /// no longer names a declared key would silently excuse nothing while
    /// reading as a live exemption.
    void deliberateExclusionsAreRealKeys()
    {
        const PhosphorConfig::Schema schema = buildSettingsSchema();
        QSet<QString> declared;
        for (auto git = schema.groups.constBegin(); git != schema.groups.constEnd(); ++git) {
            for (const PhosphorConfig::KeyDef& def : git.value()) {
                declared.insert(qualify(git.key(), def.key));
            }
        }
        QStringList stale;
        for (const QString& qualified : deliberatelyUnowned()) {
            if (!declared.contains(qualified)) {
                stale.append(qualified);
            }
        }
        stale.sort();
        QVERIFY2(stale.isEmpty(), qPrintable(stale.join(QLatin1String("; "))));
    }

    /// The reset-exemption contract for the feature enable master switches:
    /// every exempt key must be manifest-owned (a dangling exemption means the
    /// dirty-tracking fix regressed back to unowned), and every switch must
    /// be exempt (an enable key missing here would let a page Reset turn its
    /// feature off). Workspaces is the outlier: its enable lives under the
    /// Behavior group rather than a mode root group, and only a build with
    /// the feature has a page to own it.
    void resetExemptModeEnableKeysAreOwnedAndComplete()
    {
        const QSet<QString> owned = ownedKeys();

        const auto& exempt = SettingsController::resetExemptModeEnableKeys();
#ifdef USE_KDE_FRAMEWORKS
        QCOMPARE(exempt.size(), 4);
#else
        QCOMPARE(exempt.size(), 3);
#endif
        QSet<QString> exemptQualified;
        // Collect-then-assert, matching the rest of this file: a single
        // dangling exemption must not hide the state of the others.
        QStringList dangling;
        for (const auto& gk : exempt) {
            const QString qualified = qualify(gk.first, gk.second);
            exemptQualified.insert(qualified);
            if (!owned.contains(qualified)) {
                dangling.append(qualified);
            }
        }
        dangling.sort();
        QVERIFY2(dangling.isEmpty(), qPrintable(dangling.join(QLatin1String("; "))));
        QVERIFY(exemptQualified.contains(qualify(ConfigDefaults::snappingGroup(), ConfigDefaults::enabledKey())));
        QVERIFY(exemptQualified.contains(qualify(ConfigDefaults::tilingGroup(), ConfigDefaults::enabledKey())));
        QVERIFY(exemptQualified.contains(qualify(ConfigDefaults::scrollingGroup(), ConfigDefaults::enabledKey())));
#ifdef USE_KDE_FRAMEWORKS
        QVERIFY(
            exemptQualified.contains(qualify(ConfigDefaults::workspacesBehaviorGroup(), ConfigDefaults::enabledKey())));
#endif
    }

    /// The consent exemption: the ManageKWinPerOutput latch is written by the
    /// Workspaces Behavior page's consent prompt, not a row, so a Reset of
    /// that page must not withdraw it. It must still be owned, so a pending
    /// grant takes part in dirty, Save and Discard. A build without the
    /// feature has no page to own it and exempts nothing.
    void resetExemptConsentKeysAreOwnedAndComplete()
    {
        const QSet<QString> owned = ownedKeys();
        const auto& exempt = SettingsController::resetExemptConsentKeys();
#ifdef USE_KDE_FRAMEWORKS
        QCOMPARE(exempt.size(), 1);
        const QString consent =
            qualify(ConfigDefaults::workspacesBehaviorGroup(), ConfigDefaults::manageKWinPerOutputKey());
        QCOMPARE(qualify(exempt.first().first, exempt.first().second), consent);
        QVERIFY2(owned.contains(consent), qPrintable(consent));
        // Owned by the page whose Reset it is exempt from.
        const Settings::ConfigKeyList behaviorKeys =
            SettingsController::pageOwnedConfigKeys().value(QStringLiteral("workspaces-behavior"));
        QVERIFY(behaviorKeys.contains(
            Settings::ConfigKey{ConfigDefaults::workspacesBehaviorGroup(), ConfigDefaults::manageKWinPerOutputKey()}));
#else
        Q_UNUSED(owned)
        QVERIFY(exempt.isEmpty());
#endif
    }

#ifdef USE_KDE_FRAMEWORKS
    /// The workspace chords are Shortcuts.Global leaves, so the group sweep
    /// above does not reach them. Every one of them, and the target pair of
    /// every quick slot, must be owned by workspaces-shortcuts, the page whose
    /// in-page recorders edit them: owned by another page, that page's Reset
    /// would clear a chord the user set here, and unowned, an edit here
    /// would never light Save. Driven from the schema, so a chord added to
    /// appendWorkspacesShortcutKeys without a manifest entry fails here.
    void workspacesShortcutsPageOwnsEveryWorkspaceChord()
    {
        const PhosphorConfig::Schema schema = buildSettingsSchema();
        const Settings::ConfigKeyList pageKeys =
            SettingsController::pageOwnedConfigKeys().value(QStringLiteral("workspaces-shortcuts"));
        QSet<QString> pageOwned;
        for (const Settings::ConfigKey& pair : pageKeys) {
            pageOwned.insert(qualify(pair.first, pair.second));
        }

        QStringList expected;
        const auto globals = schema.groups.constFind(ConfigDefaults::shortcutsGlobalGroup());
        QVERIFY(globals != schema.groups.constEnd());
        for (const PhosphorConfig::KeyDef& def : *globals) {
            if (isWorkspaceKey(ConfigDefaults::shortcutsGlobalGroup(), def.key)) {
                expected.append(qualify(ConfigDefaults::shortcutsGlobalGroup(), def.key));
            }
        }
        for (int slot = 1; slot <= ConfigDefaults::WorkspaceSlotCount; ++slot) {
            expected.append(
                qualify(ConfigDefaults::workspacesSlotsGroup(), ConfigDefaults::workspaceSlotTargetIdKey(slot)));
            expected.append(
                qualify(ConfigDefaults::workspacesSlotsGroup(), ConfigDefaults::workspaceSlotTargetKey(slot)));
        }
        // The schema filter must have found the verb chords and both slot
        // families, or a renamed key prefix would make this pass on nothing.
        for (const QString& key :
             {ConfigDefaults::workspaceFocusUpKey(), ConfigDefaults::workspaceMoveToMonitorRightKey(),
              ConfigDefaults::workspaceFocusSlotKey(ConfigDefaults::WorkspaceSlotCount),
              ConfigDefaults::workspaceMoveSlotKey(1)}) {
            QVERIFY2(expected.contains(qualify(ConfigDefaults::shortcutsGlobalGroup(), key)), qPrintable(key));
        }

        QStringList missing;
        for (const QString& qualified : std::as_const(expected)) {
            if (!pageOwned.contains(qualified)) {
                missing.append(qualified);
            }
        }
        missing.sort();
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QLatin1String("; "))));
    }
#else
    /// Without the feature the workspaces pages do not exist, while the
    /// Workspaces.* schema rows stay declared so an OFF build never purges a
    /// configuration an ON build on the same machine reads. No page may own
    /// any of those keys, nor a workspace chord: an owner would put a Reset
    /// button over keys the build cannot show.
    void workspaceKeysHaveNoOwnerWithoutTheFeature()
    {
        const auto& manifest = SettingsController::pageOwnedConfigKeys();
        for (const QString& page : {QStringLiteral("workspaces-behavior"), QStringLiteral("workspaces-named"),
                                    QStringLiteral("workspaces-shortcuts")}) {
            QVERIFY2(!manifest.contains(page), qPrintable(page));
        }
        QStringList owned;
        for (auto it = manifest.constBegin(); it != manifest.constEnd(); ++it) {
            for (const Settings::ConfigKey& pair : it.value()) {
                if (isWorkspaceKey(pair.first, pair.second)) {
                    owned.append(it.key() + QLatin1String(" -> ") + qualify(pair.first, pair.second));
                }
            }
        }
        owned.sort();
        QVERIFY2(owned.isEmpty(), qPrintable(owned.join(QLatin1String("; "))));
        // The rows themselves are still declared.
        const PhosphorConfig::Schema schema = buildSettingsSchema();
        for (const QString& group : workspacesSchemaGroups()) {
            QVERIFY2(schema.groups.contains(group), qPrintable(group));
        }
    }
#endif

    /// Same contract for the cross-page default selections: both keys must be
    /// manifest-owned (a dangling exemption means the dirty attribution
    /// regressed back to unowned), and both defaults set from the library
    /// pages' context menus must be exempt (an entry missing here would let a
    /// Reset on the owner page silently clear a selection the page never
    /// shows an editor row for).
    void resetExemptDefaultSelectionKeysAreOwnedAndComplete()
    {
        const QSet<QString> owned = ownedKeys();

        const auto& exempt = SettingsController::resetExemptDefaultSelectionKeys();
        QCOMPARE(exempt.size(), 2);
        QSet<QString> exemptQualified;
        QStringList dangling;
        for (const auto& gk : exempt) {
            const QString qualified = qualify(gk.first, gk.second);
            exemptQualified.insert(qualified);
            if (!owned.contains(qualified)) {
                dangling.append(qualified);
            }
        }
        dangling.sort();
        QVERIFY2(dangling.isEmpty(), qPrintable(dangling.join(QLatin1String("; "))));
        QVERIFY(exemptQualified.contains(
            qualify(ConfigDefaults::snappingBehaviorWindowHandlingGroup(), ConfigDefaults::defaultLayoutIdKey())));
        QVERIFY(
            exemptQualified.contains(qualify(ConfigDefaults::scrollingGroup(), ConfigDefaults::defaultTemplateKey())));
        // The tiling default is deliberately absent: the Algorithm page
        // carries a real picker for it, so its Reset is honest.
        QVERIFY(
            !exemptQualified.contains(qualify(ConfigDefaults::tilingAlgorithmGroup(), ConfigDefaults::defaultKey())));
    }
};

QTEST_MAIN(TestPageOwnedConfigKeys)
#include "test_page_owned_config_keys.moc"
