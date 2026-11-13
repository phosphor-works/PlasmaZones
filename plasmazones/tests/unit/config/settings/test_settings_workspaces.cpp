// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_settings_workspaces.cpp
 * @brief The dynamic-workspaces half of the settings schema and of Settings.
 *
 * Pins:
 *   - checkWorkspaceName, the one name rule every writer of a workspace name
 *     shares, including the code-point walk that accepts emoji and refuses
 *     controls, bidi overrides and unpaired surrogates,
 *   - the Workspaces.Named validator (canonicalNamedEntries): id
 *     canonicalisation without ever minting one, first-wins duplicate drops,
 *     the entry cap and the output bound,
 *   - the Workspaces.Slots target pair validators,
 *   - the trigger-list validators' drop of incomplete entries and the wheel
 *     list's filter running before its cap,
 *   - where the workspace rows sit in the schema,
 *   - Settings: the workspace setters' change gating, setWorkspaceSlotTarget's
 *     id-then-name contract, the per-slot Q_PROPERTY NOTIFYs on every bulk
 *     path, and the machine-scoped key predicate.
 */

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QUuid>

#include <memory>
#include <vector>

#include <PhosphorConfig/JsonBackend.h>
#include <PhosphorConfig/Schema.h>
#include <PhosphorRules/ActionParams.h>

#include "config/configdefaults.h"
#include "config/settings.h"
#include "config/settingsschema.h"
#include "core/types/enums.h"
#include "helpers/IsolatedConfigGuard.h"

using namespace PlasmaZones;
using PlasmaZones::TestHelpers::IsolatedConfigGuard;

Q_DECLARE_METATYPE(PlasmaZones::WorkspaceNameVerdict)

namespace {

/// Run one schema key's validator, failing loudly when the key is gone.
QVariant validate(const QString& group, const QString& key, const QVariant& value)
{
    const PhosphorConfig::KeyDef* def = cachedSettingsSchema().findKey(group, key);
    if (!def || !def->validator) {
        qFatal("no validator for %s/%s", qPrintable(group), qPrintable(key));
    }
    return def->validator(value);
}

QVariantList validateNamed(const QVariantList& entries)
{
    return validate(ConfigDefaults::workspacesNamedGroup(), ConfigDefaults::entriesKey(), entries).toList();
}

QVariantMap namedEntry(const QVariant& id, const QString& name, const QString& output = QString(),
                       const QVariant& position = -1)
{
    QVariantMap entry;
    if (id.isValid()) {
        entry[ConfigDefaults::namedEntryIdField()] = id;
    }
    entry[ConfigDefaults::namedEntryNameField()] = name;
    entry[ConfigDefaults::namedEntryOutputField()] = output;
    entry[ConfigDefaults::namedEntryPositionField()] = position;
    return entry;
}

QVariantMap trigger(int modifier, int mouseButton)
{
    return {{ConfigDefaults::triggerModifierField(), modifier},
            {ConfigDefaults::triggerMouseButtonField(), mouseButton}};
}

/// One spy per distinct NOTIFY signal of every Settings property whose name
/// starts with "workspace", so a bulk path can be checked for firing exactly
/// the signal it should and no other.
class WorkspaceNotifySpies
{
public:
    explicit WorkspaceNotifySpies(Settings* settings)
    {
        const QMetaObject* mo = settings->metaObject();
        for (int i = mo->propertyOffset(); i < mo->propertyCount(); ++i) {
            const QMetaProperty prop = mo->property(i);
            if (!QLatin1String(prop.name()).startsWith(QLatin1String("workspace")) || !prop.hasNotifySignal()) {
                continue;
            }
            const QMetaMethod notify = prop.notifySignal();
            const QByteArray signature = notify.methodSignature();
            if (m_index.contains(signature)) {
                continue;
            }
            m_index.insert(signature, static_cast<int>(m_spies.size()));
            m_spies.push_back(std::make_unique<QSignalSpy>(settings, notify));
            m_names.append(signature);
        }
    }

    int size() const
    {
        return static_cast<int>(m_spies.size());
    }

    /// Every signal that fired, as "signature xcount", for a failure message
    /// and for an exact compare.
    QStringList fired() const
    {
        QStringList out;
        for (std::size_t i = 0; i < m_spies.size(); ++i) {
            if (m_spies[i]->count() > 0) {
                out.append(QStringLiteral("%1 x%2")
                               .arg(QString::fromLatin1(m_names.at(static_cast<int>(i))))
                               .arg(m_spies[i]->count()));
            }
        }
        return out;
    }

    void clear()
    {
        for (const auto& spy : m_spies) {
            spy->clear();
        }
    }

private:
    std::vector<std::unique_ptr<QSignalSpy>> m_spies;
    QHash<QByteArray, int> m_index;
    QList<QByteArray> m_names;
};

/// Writes the named list to config.json the way another process does: its own
/// Settings over its own backend, saved.
void writeNamedFromAnotherProcess(const QVariantList& entries)
{
    Settings other;
    other.setWorkspacesNamedEntries(entries);
    QVERIFY(other.save());
}

/// (name, position) of every entry in list order: what a preserving reload's
/// per-entry merge decides, without the canonical fields it never touches.
QStringList namesAndPositions(const QVariantList& entries)
{
    QStringList out;
    for (const QVariant& value : entries) {
        const QVariantMap entry = value.toMap();
        out.append(QStringLiteral("%1@%2")
                       .arg(entry.value(ConfigDefaults::namedEntryNameField()).toString())
                       .arg(entry.value(ConfigDefaults::namedEntryPositionField()).toInt()));
    }
    return out;
}

/// The id of every entry in list order.
QStringList entryIds(const QVariantList& entries)
{
    QStringList out;
    for (const QVariant& value : entries) {
        out.append(value.toMap().value(ConfigDefaults::namedEntryIdField()).toString());
    }
    return out;
}

/// The one entry fired() reports when exactly @p signalName fired once.
QStringList firedOnce(const char* signalName)
{
    return {QStringLiteral("%1() x1").arg(QLatin1String(signalName))};
}

} // namespace

class TestSettingsWorkspaces : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    // =========================================================================
    // The shared name rule
    // =========================================================================

    void checkWorkspaceName_verdicts_data()
    {
        QTest::addColumn<QString>("raw");
        QTest::addColumn<PlasmaZones::WorkspaceNameVerdict>("verdict");
        QTest::addColumn<QString>("name");

        using V = WorkspaceNameVerdict;
        const QString maxName(ConfigDefaults::WorkspaceNameMaxLength, QLatin1Char('a'));
        QTest::newRow("trimmed") << QStringLiteral("  Work  ") << V::Accepted << QStringLiteral("Work");
        QTest::newRow("empty") << QString() << V::Empty << QString();
        QTest::newRow("blank") << QStringLiteral("   ") << V::Empty << QString();
        QTest::newRow("at cap") << maxName << V::Accepted << maxName;
        QTest::newRow("past cap") << QString(ConfigDefaults::WorkspaceNameMaxLength + 1, QLatin1Char('a')) << V::TooLong
                                  << QString();
        // A non-BMP character is a surrogate pair: a per-unit
        // printability test refuses this, the code-point walk accepts it.
        const QString rocket = QStringLiteral("Dev ") + QString::fromUcs4(U"\U0001F680", 1);
        QTest::newRow("emoji") << rocket << V::Accepted << rocket;
        const QString technologist = QString::fromUcs4(U"\U0001F469‍\U0001F4BB", 3);
        QTest::newRow("zwj sequence") << technologist << V::Accepted << technologist;
        const QString softHyphen = QStringLiteral("Re") + QChar(0x00AD) + QStringLiteral("search");
        QTest::newRow("soft hyphen") << softHyphen << V::Accepted << softHyphen;
        QTest::newRow("rlo override") << (QStringLiteral("a") + QChar(0x202E) + QStringLiteral("b"))
                                      << V::ForbiddenCharacter << QString();
        QTest::newRow("lrm mark") << (QStringLiteral("a") + QChar(0x200E) + QStringLiteral("b"))
                                  << V::ForbiddenCharacter << QString();
        QTest::newRow("arabic letter mark")
            << (QStringLiteral("a") + QChar(0x061C) + QStringLiteral("b")) << V::ForbiddenCharacter << QString();
        QTest::newRow("isolate") << (QStringLiteral("a") + QChar(0x2066) + QStringLiteral("b")) << V::ForbiddenCharacter
                                 << QString();
        QTest::newRow("line separator") << (QStringLiteral("a") + QChar(0x2028) + QStringLiteral("b"))
                                        << V::ForbiddenCharacter << QString();
        QTest::newRow("paragraph separator")
            << (QStringLiteral("a") + QChar(0x2029) + QStringLiteral("b")) << V::ForbiddenCharacter << QString();
        QTest::newRow("tab inside") << QStringLiteral("a\tb") << V::ForbiddenCharacter << QString();
        QTest::newRow("lone high surrogate")
            << (QStringLiteral("a") + QChar(0xD83D)) << V::ForbiddenCharacter << QString();
        QTest::newRow("lone low surrogate")
            << (QStringLiteral("a") + QChar(0xDE80) + QStringLiteral("b")) << V::ForbiddenCharacter << QString();
        QTest::newRow("noncharacter fdd0") << QString(QChar(0xFDD0)) << V::ForbiddenCharacter << QString();
        QTest::newRow("noncharacter fffe")
            << (QStringLiteral("a") + QChar(0xFFFE)) << V::ForbiddenCharacter << QString();
    }

    void checkWorkspaceName_verdicts()
    {
        QFETCH(QString, raw);
        QFETCH(PlasmaZones::WorkspaceNameVerdict, verdict);
        QFETCH(QString, name);
        const WorkspaceNameCheck check = checkWorkspaceName(raw);
        QCOMPARE(static_cast<int>(check.verdict), static_cast<int>(verdict));
        QCOMPARE(check.name, name);
    }

    void canonicalWorkspaceEntryId_keepsOnlyStringUuids()
    {
        const QUuid uuid = QUuid::createUuid();
        QCOMPARE(canonicalWorkspaceEntryId(uuid.toString()), uuid.toString());
        QCOMPARE(canonicalWorkspaceEntryId(uuid.toString(QUuid::WithoutBraces).toUpper()), uuid.toString());
        QCOMPARE(canonicalWorkspaceEntryId(QString(QStringLiteral("  ") + uuid.toString() + QStringLiteral(" "))),
                 uuid.toString());
        QCOMPARE(canonicalWorkspaceEntryId(QStringLiteral("garbage")), QString());
        QCOMPARE(canonicalWorkspaceEntryId(QString()), QString());
        QCOMPARE(canonicalWorkspaceEntryId(QVariant(42)), QString());
        QCOMPARE(canonicalWorkspaceEntryId(QVariant(QVariantMap{{QStringLiteral("id"), uuid.toString()}})), QString());
    }

    // =========================================================================
    // Workspaces.Named validator
    // =========================================================================

    void namedEntries_canonicaliseIdsWithoutMinting()
    {
        const QUuid kept = QUuid::createUuid();
        const QUuid loose = QUuid::createUuid();
        const QVariantList out = validateNamed({
            namedEntry(kept.toString(), QStringLiteral("Kept")),
            namedEntry(loose.toString(QUuid::WithoutBraces).toUpper(), QStringLiteral("Loose")),
            namedEntry(QStringLiteral("not-a-uuid"), QStringLiteral("Garbage")),
            namedEntry(QVariant(), QStringLiteral("Idless")),
            namedEntry(7, QStringLiteral("Numeric")),
        });
        QCOMPARE(out.size(), 5);
        QCOMPARE(out.at(0).toMap().value(ConfigDefaults::namedEntryIdField()).toString(), kept.toString());
        QCOMPARE(out.at(1).toMap().value(ConfigDefaults::namedEntryIdField()).toString(), loose.toString());
        QCOMPARE(out.at(2).toMap().value(ConfigDefaults::namedEntryIdField()).toString(), QString());
        QCOMPARE(out.at(3).toMap().value(ConfigDefaults::namedEntryIdField()).toString(), QString());
        QCOMPARE(out.at(4).toMap().value(ConfigDefaults::namedEntryIdField()).toString(), QString());

        // Every kept entry carries exactly the declared fields.
        const QStringList fields{ConfigDefaults::namedEntryIdField(),
                                 ConfigDefaults::namedEntryNameField(),
                                 ConfigDefaults::namedEntryOutputField(),
                                 ConfigDefaults::namedEntryPositionField(),
                                 ConfigDefaults::namedEntryFocusShortcutField(),
                                 ConfigDefaults::namedEntryMoveShortcutField()};
        for (const QVariant& entry : out) {
            QStringList keys = entry.toMap().keys();
            keys.sort();
            QStringList expected = fields;
            expected.sort();
            QCOMPARE(keys, expected);
        }

        // A second pass changes nothing, so an id-less entry stays id-less
        // however often the store reads it: the validator never mints.
        QCOMPARE(validateNamed(out), out);
    }

    void namedEntries_dropFirstWinsDuplicatesAndMalformedEntries()
    {
        const QString shared = QUuid::createUuid().toString();
        const QVariantList out = validateNamed({
            namedEntry(shared, QStringLiteral("Web")),
            // Same id: dropped, though its name is new.
            namedEntry(shared, QStringLiteral("Mail")),
            // Same canonical name as the first: dropped, though its id is new.
            namedEntry(QUuid::createUuid().toString(), QStringLiteral("  Web ")),
            // Two id-less entries are not duplicates of each other.
            namedEntry(QVariant(), QStringLiteral("Chat")),
            namedEntry(QVariant(), QStringLiteral("Music")),
            // Refused names and non-map entries are dropped.
            namedEntry(QVariant(), QStringLiteral("   ")),
            namedEntry(QVariant(), QStringLiteral("a") + QChar(0x202E)),
            QVariant(QStringLiteral("just a string")),
        });
        QStringList names;
        for (const QVariant& entry : out) {
            names.append(entry.toMap().value(ConfigDefaults::namedEntryNameField()).toString());
        }
        QCOMPARE(names, (QStringList{QStringLiteral("Web"), QStringLiteral("Chat"), QStringLiteral("Music")}));
        QCOMPARE(out.at(0).toMap().value(ConfigDefaults::namedEntryIdField()).toString(), shared);
    }

    void namedEntries_capOutputAndPosition()
    {
        QVariantList many;
        for (int i = 0; i < ConfigDefaults::WorkspaceNamedEntryMaxCount + 5; ++i) {
            many.append(namedEntry(QUuid::createUuid().toString(), QStringLiteral("Workspace %1").arg(i)));
        }
        const QVariantList capped = validateNamed(many);
        QCOMPARE(capped.size(), ConfigDefaults::WorkspaceNamedEntryMaxCount);
        // The cap keeps the FIRST entries.
        QCOMPARE(capped.last().toMap().value(ConfigDefaults::namedEntryNameField()).toString(),
                 QStringLiteral("Workspace %1").arg(ConfigDefaults::WorkspaceNamedEntryMaxCount - 1));

        const QVariantList out = validateNamed({
            namedEntry(QVariant(), QStringLiteral("Long output"),
                       QString(PhosphorRules::MaxScreenIdLength + 72, QLatin1Char('x'))),
            namedEntry(QVariant(), QStringLiteral("Padded output"), QStringLiteral("  DP-1 "), 3),
            namedEntry(QVariant(), QStringLiteral("Bad position"), QString(), QStringLiteral("abc")),
        });
        QCOMPARE(out.size(), 3);
        QCOMPARE(out.at(0).toMap().value(ConfigDefaults::namedEntryOutputField()).toString(), QString());
        QCOMPARE(out.at(1).toMap().value(ConfigDefaults::namedEntryOutputField()).toString(), QStringLiteral("DP-1"));
        QCOMPARE(out.at(1).toMap().value(ConfigDefaults::namedEntryPositionField()).toInt(), 3);
        QCOMPARE(out.at(2).toMap().value(ConfigDefaults::namedEntryPositionField()).toInt(), -1);
    }

    /// The same contract through Settings: what a writer hands over is
    /// canonicalised on the way in, and reading twice yields the same ids.
    void namedEntries_throughSettingsNeverMint()
    {
        IsolatedConfigGuard guard;
        Settings settings;
        const QUuid id = QUuid::createUuid();
        settings.setWorkspacesNamedEntries({
            namedEntry(id.toString(QUuid::WithoutBraces), QStringLiteral(" Web ")),
            namedEntry(QVariant(), QStringLiteral("Mail")),
        });
        const QVariantList first = settings.workspacesNamedEntries();
        QCOMPARE(first.size(), 2);
        QCOMPARE(first.at(0).toMap().value(ConfigDefaults::namedEntryIdField()).toString(), id.toString());
        QCOMPARE(first.at(0).toMap().value(ConfigDefaults::namedEntryNameField()).toString(), QStringLiteral("Web"));
        QCOMPARE(first.at(1).toMap().value(ConfigDefaults::namedEntryIdField()).toString(), QString());
        QCOMPARE(settings.workspacesNamedEntries(), first);
    }

    // =========================================================================
    // Workspaces.Slots validators
    // =========================================================================

    void slotTargets_canonicaliseIdAndName()
    {
        const QString group = ConfigDefaults::workspacesSlotsGroup();
        const QUuid id = QUuid::createUuid();
        for (int slot = 1; slot <= ConfigDefaults::WorkspaceSlotCount; ++slot) {
            const QString idKey = ConfigDefaults::workspaceSlotTargetIdKey(slot);
            const QString nameKey = ConfigDefaults::workspaceSlotTargetKey(slot);
            QCOMPARE(validate(group, idKey, QStringLiteral("garbage")).toString(), QString());
            QCOMPARE(validate(group, idKey, id.toString(QUuid::WithoutBraces)).toString(), id.toString());
            QCOMPARE(validate(group, nameKey, QStringLiteral("  Work ")).toString(), QStringLiteral("Work"));
            QCOMPARE(
                validate(group, nameKey, QString(QStringLiteral("a") + QChar(0x202E) + QStringLiteral("b"))).toString(),
                QString());
        }
    }

    // =========================================================================
    // Trigger-list validators
    // =========================================================================

    /// An entry missing either field is dropped, an empty map included. A
    /// missing field used to read as 0, and modifier 0 means "any modifier"
    /// to the drag readers, so {mouseButton: 1} alone widened to "left button
    /// with anything held".
    void triggerList_dropsIncompleteEntries()
    {
        const QVariantList out =
            validate(ConfigDefaults::snappingBehaviorZoneSpanGroup(), ConfigDefaults::triggersKey(),
                     QVariantList{
                         QVariantMap(),
                         QVariantMap{{ConfigDefaults::triggerModifierField(), 2}},
                         QVariantMap{{ConfigDefaults::triggerMouseButtonField(), 1}},
                         trigger(static_cast<int>(DragModifier::Alt), 0),
                     })
                .toList();
        QCOMPARE(out, (QVariantList{trigger(static_cast<int>(DragModifier::Alt), 0)}));
    }

    /// The wheel lists drop AlwaysActive, and the cap counts KEPT entries, so
    /// a list led by entries the filter drops still keeps a valid chord past
    /// them. Capping first would keep the dropped leaders' slots and lose it.
    void wheelTriggerList_filtersBeforeCap()
    {
        const int cap = ConfigDefaults::maxTriggersPerAction();
        QVariantList raw;
        for (int i = 0; i < cap; ++i) {
            raw.append(trigger(static_cast<int>(DragModifier::AlwaysActive), 0));
        }
        raw.append(trigger(static_cast<int>(DragModifier::Meta), 0));
        for (const QString& group :
             {ConfigDefaults::scrollingWheelFocusGroup(), ConfigDefaults::scrollingWheelViewGroup()}) {
            const QVariantList out = validate(group, ConfigDefaults::triggersKey(), raw).toList();
            QCOMPARE(out, (QVariantList{trigger(static_cast<int>(DragModifier::Meta), 0)}));
        }
    }

    // =========================================================================
    // Schema placement
    // =========================================================================

    /// The workspace groups and chord rows are declared, and the chord rows
    /// come after every older Shortcuts.Global row, so the schema dump keeps
    /// its earlier order.
    void schema_declaresWorkspaceRowsAfterTheOlderGlobals()
    {
        const PhosphorConfig::Schema schema = buildSettingsSchema();
        for (const QString& group : {ConfigDefaults::workspacesBehaviorGroup(), ConfigDefaults::workspacesNamedGroup(),
                                     ConfigDefaults::workspacesSlotsGroup()}) {
            QVERIFY2(schema.groups.contains(group), qPrintable(group));
        }
        QCOMPARE(schema.groups.value(ConfigDefaults::workspacesSlotsGroup()).size(),
                 2 * ConfigDefaults::WorkspaceSlotCount);

        const QVector<PhosphorConfig::KeyDef> globals = schema.groups.value(ConfigDefaults::shortcutsGlobalGroup());
        QStringList order;
        for (const PhosphorConfig::KeyDef& def : globals) {
            order.append(def.key);
        }
        QStringList workspaceKeys{
            ConfigDefaults::workspaceFocusUpKey(),           ConfigDefaults::workspaceFocusDownKey(),
            ConfigDefaults::workspaceMoveWindowUpKey(),      ConfigDefaults::workspaceMoveWindowDownKey(),
            ConfigDefaults::workspaceMoveColumnUpKey(),      ConfigDefaults::workspaceMoveColumnDownKey(),
            ConfigDefaults::workspaceReorderUpKey(),         ConfigDefaults::workspaceReorderDownKey(),
            ConfigDefaults::workspaceMoveToMonitorLeftKey(), ConfigDefaults::workspaceMoveToMonitorRightKey(),
        };
        for (int slot = 1; slot <= ConfigDefaults::WorkspaceSlotCount; ++slot) {
            workspaceKeys.append(ConfigDefaults::workspaceMoveSlotKey(slot));
            workspaceKeys.append(ConfigDefaults::workspaceFocusSlotKey(slot));
        }
        const qsizetype anchor = order.indexOf(ConfigDefaults::rotateVirtualScreensCounterclockwiseKey());
        QVERIFY(anchor >= 0);
        QStringList misplaced;
        for (const QString& key : std::as_const(workspaceKeys)) {
            const qsizetype at = order.indexOf(key);
            if (at <= anchor) {
                misplaced.append(QStringLiteral("%1 at %2").arg(key).arg(at));
            }
        }
        QVERIFY2(misplaced.isEmpty(), qPrintable(misplaced.join(QLatin1String("; "))));
    }

    // =========================================================================
    // Settings: change gating
    // =========================================================================

    /// The named list compares canonical before and after, so handing over a
    /// list that canonicalises to what is stored (padding, an unknown field)
    /// announces nothing, and a real change announces once.
    void namedEntriesSetter_announcesOnlyCanonicalChanges()
    {
        IsolatedConfigGuard guard;
        Settings settings;
        const QString id = QUuid::createUuid().toString();
        settings.setWorkspacesNamedEntries({namedEntry(id, QStringLiteral("Web"))});

        QSignalSpy entriesSpy(&settings, &Settings::workspacesNamedEntriesChanged);
        QSignalSpy changedSpy(&settings, &Settings::settingsChanged);
        QVariantMap padded = namedEntry(id, QStringLiteral("  Web  "));
        padded.insert(QStringLiteral("unknownField"), true);
        settings.setWorkspacesNamedEntries({padded});
        QCOMPARE(entriesSpy.count(), 0);
        QCOMPARE(changedSpy.count(), 0);

        settings.setWorkspacesNamedEntries({namedEntry(id, QStringLiteral("Browser"))});
        QCOMPARE(entriesSpy.count(), 1);
        QCOMPARE(changedSpy.count(), 1);
    }

    void chordSetters_announceOnlyRealChanges()
    {
        IsolatedConfigGuard guard;
        Settings settings;
        WorkspaceNotifySpies spies(&settings);

        settings.setWorkspaceFocusUpShortcut(settings.workspaceFocusUpShortcut());
        settings.setWorkspaceMoveSlotShortcut(0, settings.workspaceMoveSlotShortcut(0));
        // Out of range on both ends: nothing written, nothing announced.
        settings.setWorkspaceMoveSlotShortcut(-1, QStringLiteral("Meta+1"));
        settings.setWorkspaceFocusSlotShortcut(ConfigDefaults::WorkspaceSlotCount, QStringLiteral("Meta+1"));
        QCOMPARE(spies.fired(), QStringList());

        settings.setWorkspaceMoveSlotShortcut(3, QStringLiteral("Meta+Shift+4"));
        QCOMPARE(spies.fired(), firedOnce("workspaceMoveSlot4ShortcutChanged"));
        QCOMPARE(settings.workspaceMoveSlot4Shortcut(), QStringLiteral("Meta+Shift+4"));
    }

    // =========================================================================
    // Settings: slot targets
    // =========================================================================

    void setWorkspaceSlotTarget_storesIdAndCurrentName()
    {
        IsolatedConfigGuard guard;
        Settings settings;
        const QUuid web = QUuid::createUuid();
        const QUuid mail = QUuid::createUuid();
        settings.setWorkspacesNamedEntries(
            {namedEntry(web.toString(), QStringLiteral("Web")), namedEntry(mail.toString(), QStringLiteral("Mail"))});

        WorkspaceNotifySpies spies(&settings);
        QSignalSpy changedSpy(&settings, &Settings::settingsChanged);

        // An id the list carries: both halves written, one announcement.
        settings.setWorkspaceSlotTarget(0, web.toString());
        QCOMPARE(settings.workspaceSlotTargetId(0), web.toString());
        QCOMPARE(settings.workspaceSlotTargetName(0), QStringLiteral("Web"));
        QCOMPARE(spies.fired(), firedOnce("workspaceSlot1TargetChanged"));
        QCOMPARE(changedSpy.count(), 1);

        // The same id in another spelling is the same id: nothing moves.
        spies.clear();
        changedSpy.clear();
        settings.setWorkspaceSlotTarget(0, web.toString(QUuid::WithoutBraces).toUpper());
        QCOMPARE(spies.fired(), QStringList());
        QCOMPARE(changedSpy.count(), 0);

        // An id no entry carries, and text that is no id at all, are refused.
        settings.setWorkspaceSlotTarget(0, QUuid::createUuid().toString());
        settings.setWorkspaceSlotTarget(0, QStringLiteral("Web"));
        QCOMPARE(settings.workspaceSlotTargetId(0), web.toString());
        QCOMPARE(settings.workspaceSlotTargetName(0), QStringLiteral("Web"));
        QCOMPARE(spies.fired(), QStringList());
        QCOMPARE(changedSpy.count(), 0);

        // A rename keeps the id, so the slot still names the old text until
        // it is assigned again, which refreshes the name and announces it.
        settings.setWorkspacesNamedEntries({namedEntry(web.toString(), QStringLiteral("Browser")),
                                            namedEntry(mail.toString(), QStringLiteral("Mail"))});
        QCOMPARE(settings.workspaceSlotTargetName(0), QStringLiteral("Web"));
        spies.clear();
        changedSpy.clear();
        settings.setWorkspaceSlotTarget(0, web.toString());
        QCOMPARE(settings.workspaceSlotTargetId(0), web.toString());
        QCOMPARE(settings.workspaceSlotTargetName(0), QStringLiteral("Browser"));
        QCOMPARE(spies.fired(), firedOnce("workspaceSlot1TargetChanged"));
        QCOMPARE(changedSpy.count(), 1);

        // An empty id clears both halves.
        spies.clear();
        settings.setWorkspaceSlotTarget(0, QString());
        QCOMPARE(settings.workspaceSlotTargetId(0), QString());
        QCOMPARE(settings.workspaceSlotTargetName(0), QString());
        QCOMPARE(spies.fired(), firedOnce("workspaceSlot1TargetChanged"));

        // The property write reads only the id: the name comes from the list.
        spies.clear();
        QVERIFY(settings.setProperty("workspaceSlot2Target",
                                     QVariantMap{{ConfigDefaults::namedEntryIdField(), mail.toString()},
                                                 {ConfigDefaults::namedEntryNameField(), QStringLiteral("Ignored")}}));
        QCOMPARE(settings.workspaceSlotTargetName(1), QStringLiteral("Mail"));
        QCOMPARE(
            settings.property("workspaceSlot2Target").toMap().value(ConfigDefaults::namedEntryIdField()).toString(),
            mail.toString());
        QCOMPARE(spies.fired(), firedOnce("workspaceSlot2TargetChanged"));
    }

    // =========================================================================
    // Settings: per-property NOTIFYs on the bulk paths
    // =========================================================================

    /// Every workspace chord and slot target is its own Q_PROPERTY with its
    /// own NOTIFY: that is what QML binds and what the bulk paths' generic
    /// property walk announces through.
    void workspaceProperties_eachHaveTheirOwnNotify()
    {
        IsolatedConfigGuard guard;
        Settings settings;
        QStringList expected{
            QStringLiteral("workspaceFocusUpShortcut"),           QStringLiteral("workspaceFocusDownShortcut"),
            QStringLiteral("workspaceMoveWindowUpShortcut"),      QStringLiteral("workspaceMoveWindowDownShortcut"),
            QStringLiteral("workspaceMoveColumnUpShortcut"),      QStringLiteral("workspaceMoveColumnDownShortcut"),
            QStringLiteral("workspaceReorderUpShortcut"),         QStringLiteral("workspaceReorderDownShortcut"),
            QStringLiteral("workspaceMoveToMonitorLeftShortcut"), QStringLiteral("workspaceMoveToMonitorRightShortcut"),
        };
        for (int slot = 1; slot <= ConfigDefaults::WorkspaceSlotCount; ++slot) {
            expected.append(QStringLiteral("workspaceFocusSlot%1Shortcut").arg(slot));
            expected.append(QStringLiteral("workspaceMoveSlot%1Shortcut").arg(slot));
            expected.append(QStringLiteral("workspaceSlot%1Target").arg(slot));
        }
        const QMetaObject* mo = settings.metaObject();
        QStringList problems;
        QSet<QByteArray> notifies;
        for (const QString& name : std::as_const(expected)) {
            const int index = mo->indexOfProperty(name.toLatin1().constData());
            if (index < 0) {
                problems.append(name + QStringLiteral(": no property"));
                continue;
            }
            const QMetaProperty prop = mo->property(index);
            if (!prop.hasNotifySignal() || !prop.isWritable()) {
                problems.append(name + QStringLiteral(": not writable with a NOTIFY"));
                continue;
            }
            const QByteArray notify = prop.notifySignal().methodSignature();
            if (notifies.contains(notify)) {
                problems.append(name + QStringLiteral(": shares ") + QString::fromLatin1(notify));
            }
            notifies.insert(notify);
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1String("; "))));
    }

    // =========================================================================
    // Preserving reload merges the named list per entry
    // =========================================================================

    /// The daemon rewrites one declaration on its own (a pinned workspace
    /// moved by a verb), while the settings app holds an unsaved edit to
    /// another. The preserving reload keeps the local edit and takes the
    /// daemon's rewrite of the entry the user did not touch. Putting the whole
    /// local list back would revert A's position on the next Save.
    void reloadPreservingLocalEdits_mergesNamedEntriesPerEntry()
    {
        IsolatedConfigGuard guard;
        const QString a = QUuid::createUuid().toString();
        const QString b = QUuid::createUuid().toString();
        Settings settings;
        settings.setWorkspacesNamedEntries({namedEntry(a, QStringLiteral("A")), namedEntry(b, QStringLiteral("B"))});
        QVERIFY(settings.save());

        settings.setWorkspacesNamedEntries(
            {namedEntry(a, QStringLiteral("A")), namedEntry(b, QStringLiteral("B local"))});
        writeNamedFromAnotherProcess(
            {namedEntry(a, QStringLiteral("A"), QString(), 2), namedEntry(b, QStringLiteral("B"))});

        settings.reloadPreservingLocalEdits();
        QCOMPARE(namesAndPositions(settings.workspacesNamedEntries()),
                 (QStringList{QStringLiteral("A@2"), QStringLiteral("B local@-1")}));
    }

    /// A row the user removed stays removed, whatever disk did to the others.
    void reloadPreservingLocalEdits_keepsALocalRemoval()
    {
        IsolatedConfigGuard guard;
        const QString a = QUuid::createUuid().toString();
        const QString b = QUuid::createUuid().toString();
        Settings settings;
        settings.setWorkspacesNamedEntries({namedEntry(a, QStringLiteral("A")), namedEntry(b, QStringLiteral("B"))});
        QVERIFY(settings.save());

        settings.setWorkspacesNamedEntries({namedEntry(a, QStringLiteral("A"))});
        writeNamedFromAnotherProcess(
            {namedEntry(a, QStringLiteral("A"), QString(), 2), namedEntry(b, QStringLiteral("B"))});

        settings.reloadPreservingLocalEdits();
        QCOMPARE(namesAndPositions(settings.workspacesNamedEntries()), (QStringList{QStringLiteral("A@2")}));
    }

    /// A row the user added stays where it was added, after the row it
    /// follows locally, and the untouched rows take disk's version.
    void reloadPreservingLocalEdits_placesALocalAdditionAfterItsLocalPredecessor()
    {
        IsolatedConfigGuard guard;
        const QString a = QUuid::createUuid().toString();
        const QString b = QUuid::createUuid().toString();
        const QString c = QUuid::createUuid().toString();
        Settings settings;
        settings.setWorkspacesNamedEntries({namedEntry(a, QStringLiteral("A")), namedEntry(b, QStringLiteral("B"))});
        QVERIFY(settings.save());

        settings.setWorkspacesNamedEntries({namedEntry(a, QStringLiteral("A")), namedEntry(c, QStringLiteral("C")),
                                            namedEntry(b, QStringLiteral("B"))});
        writeNamedFromAnotherProcess(
            {namedEntry(a, QStringLiteral("A"), QString(), 2), namedEntry(b, QStringLiteral("B"))});

        settings.reloadPreservingLocalEdits();
        QCOMPARE(namesAndPositions(settings.workspacesNamedEntries()),
                 (QStringList{QStringLiteral("A@2"), QStringLiteral("C@-1"), QStringLiteral("B@-1")}));
    }

    /// The settings page stamps an id on every id-less entry when it writes
    /// the list, while the file still holds those entries id-less. The stamp
    /// alone must not read as "removed and re-added": the daemon's move of the
    /// stamped entry survives, and the entry keeps the id the page gave it.
    void reloadPreservingLocalEdits_idStampKeepsTheDaemonRewrite()
    {
        IsolatedConfigGuard guard;
        const QString x = QUuid::createUuid().toString();
        const QString y = QUuid::createUuid().toString();
        Settings settings;
        settings.setWorkspacesNamedEntries(
            {namedEntry(QVariant(), QStringLiteral("A")), namedEntry(QVariant(), QStringLiteral("B"))});
        QVERIFY(settings.save());

        settings.setWorkspacesNamedEntries({namedEntry(x, QStringLiteral("A")), namedEntry(y, QStringLiteral("B"))});
        writeNamedFromAnotherProcess(
            {namedEntry(QVariant(), QStringLiteral("A"), QString(), 2), namedEntry(QVariant(), QStringLiteral("B"))});

        settings.reloadPreservingLocalEdits();
        const QVariantList merged = settings.workspacesNamedEntries();
        QCOMPARE(namesAndPositions(merged), (QStringList{QStringLiteral("A@2"), QStringLiteral("B@-1")}));
        QCOMPARE(entryIds(merged), (QStringList{x, y}));
    }

    /// A stamped entry the daemon renamed in place (a rename made in KWin)
    /// stays one workspace: the stamp is not an edit that keeps the old name
    /// alive beside the new one.
    void reloadPreservingLocalEdits_idStampDoesNotDuplicateARenamedEntry()
    {
        IsolatedConfigGuard guard;
        const QString x = QUuid::createUuid().toString();
        Settings settings;
        settings.setWorkspacesNamedEntries({namedEntry(QVariant(), QStringLiteral("A"))});
        QVERIFY(settings.save());

        settings.setWorkspacesNamedEntries({namedEntry(x, QStringLiteral("A"))});
        writeNamedFromAnotherProcess({namedEntry(QVariant(), QStringLiteral("Alpha"))});

        settings.reloadPreservingLocalEdits();
        QCOMPARE(namesAndPositions(settings.workspacesNamedEntries()), (QStringList{QStringLiteral("Alpha@-1")}));
    }

    /// One entry changed on both sides, in different fields: the user's chord
    /// and the daemon's move both survive.
    void reloadPreservingLocalEdits_mergesOneEntryPerField()
    {
        IsolatedConfigGuard guard;
        const QString a = QUuid::createUuid().toString();
        Settings settings;
        settings.setWorkspacesNamedEntries({namedEntry(a, QStringLiteral("A"))});
        QVERIFY(settings.save());

        QVariantMap withChord = namedEntry(a, QStringLiteral("A"));
        withChord[ConfigDefaults::namedEntryFocusShortcutField()] = QStringLiteral("Meta+Ctrl+F1");
        settings.setWorkspacesNamedEntries({withChord});
        writeNamedFromAnotherProcess({namedEntry(a, QStringLiteral("A"), QString(), 2)});

        settings.reloadPreservingLocalEdits();
        const QVariantList merged = settings.workspacesNamedEntries();
        QCOMPARE(namesAndPositions(merged), (QStringList{QStringLiteral("A@2")}));
        QCOMPARE(merged.constFirst().toMap().value(ConfigDefaults::namedEntryFocusShortcutField()).toString(),
                 QStringLiteral("Meta+Ctrl+F1"));
    }

    /// A row the user added under a name another process gave an existing
    /// entry meanwhile yields to that entry, wherever the added row sits.
    void reloadPreservingLocalEdits_collidingLocalAdditionYieldsToDisk()
    {
        IsolatedConfigGuard guard;
        const QString a = QUuid::createUuid().toString();
        const QString b = QUuid::createUuid().toString();
        const QString c = QUuid::createUuid().toString();
        Settings settings;
        settings.setWorkspacesNamedEntries({namedEntry(a, QStringLiteral("A")), namedEntry(b, QStringLiteral("B"))});
        QVERIFY(settings.save());

        // Added between the two, so list order alone would keep it.
        settings.setWorkspacesNamedEntries({namedEntry(a, QStringLiteral("A")), namedEntry(c, QStringLiteral("Work")),
                                            namedEntry(b, QStringLiteral("B"))});
        writeNamedFromAnotherProcess({namedEntry(a, QStringLiteral("A")), namedEntry(b, QStringLiteral("Work"))});

        settings.reloadPreservingLocalEdits();
        const QVariantList merged = settings.workspacesNamedEntries();
        QCOMPARE(namesAndPositions(merged), (QStringList{QStringLiteral("A@-1"), QStringLiteral("Work@-1")}));
        QCOMPARE(entryIds(merged), (QStringList{a, b}));
    }

    /// A local rename to a name another process gave a different entry goes
    /// back to the entry's own name, and neither declaration is lost.
    void reloadPreservingLocalEdits_collidingLocalRenameKeepsBothEntries()
    {
        IsolatedConfigGuard guard;
        const QString a = QUuid::createUuid().toString();
        const QString b = QUuid::createUuid().toString();
        Settings settings;
        settings.setWorkspacesNamedEntries({namedEntry(a, QStringLiteral("A")), namedEntry(b, QStringLiteral("B"))});
        QVERIFY(settings.save());

        settings.setWorkspacesNamedEntries({namedEntry(a, QStringLiteral("A")), namedEntry(b, QStringLiteral("Work"))});
        writeNamedFromAnotherProcess({namedEntry(a, QStringLiteral("Work")), namedEntry(b, QStringLiteral("B"))});

        settings.reloadPreservingLocalEdits();
        const QVariantList merged = settings.workspacesNamedEntries();
        QCOMPARE(namesAndPositions(merged), (QStringList{QStringLiteral("Work@-1"), QStringLiteral("B@-1")}));
        QCOMPARE(entryIds(merged), (QStringList{a, b}));
    }

    /// A factory reset whose commit is refused puts the unsaved edits back
    /// through the same per-entry merge, so the daemon's rewrite of an entry
    /// the user did not touch is not reverted by the put-back.
    void reset_refusedCommitMergesTheNamedEntries()
    {
        class FailingCommitBackend : public PhosphorConfig::JsonBackend
        {
        public:
            using PhosphorConfig::JsonBackend::JsonBackend;
            bool failCommits = false;
            bool commit() override
            {
                return failCommits ? false : PhosphorConfig::JsonBackend::commit();
            }
        };

        IsolatedConfigGuard guard;
        FailingCommitBackend backend(ConfigDefaults::configFilePath());
        Settings settings(&backend, nullptr, nullptr, nullptr);
        const QString a = QUuid::createUuid().toString();
        const QString b = QUuid::createUuid().toString();
        settings.setWorkspacesNamedEntries({namedEntry(a, QStringLiteral("A")), namedEntry(b, QStringLiteral("B"))});
        QVERIFY(settings.save());

        settings.setWorkspacesNamedEntries(
            {namedEntry(a, QStringLiteral("A")), namedEntry(b, QStringLiteral("B local"))});
        writeNamedFromAnotherProcess(
            {namedEntry(a, QStringLiteral("A"), QString(), 2), namedEntry(b, QStringLiteral("B"))});

        backend.failCommits = true;
        QVERIFY(!settings.reset());
        QCOMPARE(namesAndPositions(settings.workspacesNamedEntries()),
                 (QStringList{QStringLiteral("A@2"), QStringLiteral("B local@-1")}));
    }

    /// load() reverting an unsaved chord edit announces that chord only.
    void load_announcesOnlyTheMovedWorkspaceProperty()
    {
        IsolatedConfigGuard guard;
        Settings settings;
        settings.setWorkspaceReorderUpShortcut(QStringLiteral("Meta+Alt+U"));
        QVERIFY(settings.save());
        settings.setWorkspaceReorderUpShortcut(QStringLiteral("Meta+Alt+J"));

        WorkspaceNotifySpies spies(&settings);
        QVERIFY(spies.size() > 0);
        settings.load();
        QCOMPARE(settings.workspaceReorderUpShortcut(), QStringLiteral("Meta+Alt+U"));
        QCOMPARE(spies.fired(), firedOnce("workspaceReorderUpShortcutChanged"));
    }

    /// Per-page Discard of one slot chord announces that slot only.
    void discardKeys_announcesOnlyTheMovedSlot()
    {
        IsolatedConfigGuard guard;
        Settings settings;
        QVERIFY(settings.save());
        settings.setWorkspaceMoveSlotShortcut(2, QStringLiteral("Meta+Shift+3"));
        settings.setWorkspaceFocusSlotShortcut(5, QStringLiteral("Meta+6"));

        WorkspaceNotifySpies spies(&settings);
        settings.discardKeys({{ConfigDefaults::shortcutsGlobalGroup(), ConfigDefaults::workspaceMoveSlotKey(3)}});
        QCOMPARE(settings.workspaceMoveSlot3Shortcut(), ConfigDefaults::workspaceMoveSlotShortcut());
        // The edit outside the discarded list is kept and stays silent.
        QCOMPARE(settings.workspaceFocusSlot6Shortcut(), QStringLiteral("Meta+6"));
        QCOMPARE(spies.fired(), firedOnce("workspaceMoveSlot3ShortcutChanged"));
    }

    /// Per-page Reset of one slot chord announces that slot only.
    void resetKeys_announcesOnlyTheMovedSlot()
    {
        IsolatedConfigGuard guard;
        Settings settings;
        settings.setWorkspaceFocusSlotShortcut(4, QStringLiteral("Meta+5"));
        settings.setWorkspaceFocusSlotShortcut(7, QStringLiteral("Meta+8"));
        QVERIFY(settings.save());

        WorkspaceNotifySpies spies(&settings);
        settings.resetKeys({{ConfigDefaults::shortcutsGlobalGroup(), ConfigDefaults::workspaceFocusSlotKey(5)}});
        QCOMPARE(settings.workspaceFocusSlot5Shortcut(), ConfigDefaults::workspaceFocusSlotShortcut());
        QCOMPARE(settings.workspaceFocusSlot8Shortcut(), QStringLiteral("Meta+8"));
        QCOMPARE(spies.fired(), firedOnce("workspaceFocusSlot5ShortcutChanged"));
    }

    /// A staged profile blob that moves one slot target announces that
    /// target once, though both of its keys moved.
    void applyConfigOverlayStaged_announcesOnlyTheMovedTarget()
    {
        IsolatedConfigGuard guard;
        Settings settings;
        settings.load();

        const QString id = QUuid::createUuid().toString();
        QJsonObject blob = settings.exportConfigToJson();
        QJsonObject slots = blob.value(ConfigDefaults::workspacesSlotsGroup()).toObject();
        slots.insert(ConfigDefaults::workspaceSlotTargetIdKey(7), id);
        slots.insert(ConfigDefaults::workspaceSlotTargetKey(7), QStringLiteral("Web"));
        blob.insert(ConfigDefaults::workspacesSlotsGroup(), slots);

        WorkspaceNotifySpies spies(&settings);
        QVERIFY(settings.applyConfigOverlayStaged(blob));
        QCOMPARE(settings.workspaceSlotTargetId(6), id);
        QCOMPARE(settings.workspaceSlotTargetName(6), QStringLiteral("Web"));
        QCOMPARE(spies.fired(), firedOnce("workspaceSlot7TargetChanged"));
    }

    // =========================================================================
    // Settings: machine-scoped keys
    // =========================================================================

    void isMachineScopedKey_coversHardwareAndConsentOnly()
    {
        QVERIFY(Settings::isMachineScopedKey(ConfigDefaults::renderingGroup(), ConfigDefaults::gpuKey()));
        QVERIFY(Settings::isMachineScopedKey(ConfigDefaults::workspacesBehaviorGroup(),
                                             ConfigDefaults::manageKWinPerOutputKey()));
        // The rebind toggle is a preference and rides profiles.
        QVERIFY(!Settings::isMachineScopedKey(ConfigDefaults::workspacesBehaviorGroup(),
                                              ConfigDefaults::rebindKWinDesktopShortcutsKey()));
        QVERIFY(!Settings::isMachineScopedKey(ConfigDefaults::workspacesBehaviorGroup(), ConfigDefaults::enabledKey()));
        QVERIFY(!Settings::isMachineScopedKey(ConfigDefaults::renderingGroup(), ConfigDefaults::backendKey()));
        // The predicate is per group: the same key name elsewhere is not it.
        QVERIFY(!Settings::isMachineScopedKey(ConfigDefaults::workspacesNamedGroup(),
                                              ConfigDefaults::manageKWinPerOutputKey()));

        QJsonObject blob{
            {ConfigDefaults::renderingGroup(),
             QJsonObject{{ConfigDefaults::gpuKey(), QStringLiteral("10de:1e84")},
                         {ConfigDefaults::backendKey(), QStringLiteral("vulkan")}}},
            {ConfigDefaults::workspacesBehaviorGroup(),
             QJsonObject{{ConfigDefaults::manageKWinPerOutputKey(), true},
                         {ConfigDefaults::rebindKWinDesktopShortcutsKey(), false}}},
        };
        Settings::stripMachineScopedKeys(blob);
        const QJsonObject rendering = blob.value(ConfigDefaults::renderingGroup()).toObject();
        const QJsonObject workspaces = blob.value(ConfigDefaults::workspacesBehaviorGroup()).toObject();
        QVERIFY(!rendering.contains(ConfigDefaults::gpuKey()));
        QVERIFY(rendering.contains(ConfigDefaults::backendKey()));
        QVERIFY(!workspaces.contains(ConfigDefaults::manageKWinPerOutputKey()));
        QVERIFY(workspaces.contains(ConfigDefaults::rebindKWinDesktopShortcutsKey()));
    }
};

QTEST_MAIN(TestSettingsWorkspaces)
#include "test_settings_workspaces.moc"
