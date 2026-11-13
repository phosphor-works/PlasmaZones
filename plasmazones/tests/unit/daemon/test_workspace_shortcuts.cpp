// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The dynamic-workspaces shortcut surface of ShortcutManager, driven through
// an injected backend that records what reaches the platform:
//
//   - parity: every workspace chord key in the Shortcuts.Global schema has a
//     Settings Q_PROPERTY, a registered action, a catalog entry, and is in
//     the gated family (and no other id is), so a row added to one of the
//     schema, the registration table, the slot loops or the catalog alone
//     fails here instead of shipping unbindable or uncatalogued;
//   - the gate: parked rows leave the cheatsheet and the shell catalog, and
//     the focus pair parks while KWin keeps its stock chords;
//   - the all-unassigned slot families fold into one row each;
//   - the named-workspace chords: adhoc ids the collision guard must not
//     refuse, listed on the sheet under Workspaces while bound.

#include "daemon/controllers/shortcutmanager.h"
#include "daemon/controllers/shortcutmanager_ids.h"

#include "config/configdefaults.h"
#include "config/settings.h"
#include "config/settingsschema.h"
#include "helpers/IsolatedConfigGuard.h"

#include <PhosphorShortcuts/IBackend.h>

#include <QKeySequence>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QVariantList>

#include <memory>

using PlasmaZones::ConfigDefaults;
using PlasmaZones::Settings;
using PlasmaZones::ShortcutManager;
using PlasmaZones::TestHelpers::IsolatedConfigGuard;
namespace Ids = PlasmaZones::ShortcutIds;

namespace {

/// Records registrations, purges and parks into test-owned lists, which must
/// be declared before the manager that owns this backend.
class RecordingBackend : public PhosphorShortcuts::IBackend
{
    Q_OBJECT
public:
    RecordingBackend(QStringList* registers, QStringList* unregisters, QStringList* suspends)
        : m_registers(registers)
        , m_unregisters(unregisters)
        , m_suspends(suspends)
    {
    }

    void registerShortcut(const QString& id, const QKeySequence& /*defaultSeq*/, const QKeySequence& /*currentSeq*/,
                          const QString& /*description*/, bool /*persistent*/) override
    {
        m_registers->append(id);
    }

    void updateShortcut(const QString& /*id*/, const QKeySequence& /*defaultSeq*/,
                        const QKeySequence& /*newTrigger*/) override
    {
    }

    void unregisterShortcut(const QString& id) override
    {
        m_unregisters->append(id);
    }

    void suspendShortcut(const QString& id) override
    {
        m_suspends->append(id);
    }

    void flush() override
    {
        Q_EMIT ready();
    }

    /// True makes the registry hand every id to the backend, chord or not
    /// (the D-Bus trigger backend's behaviour), so the registration surface
    /// can be read off the recorded list in full.
    bool registersUnboundIds() const override
    {
        return m_registersUnboundIds;
    }

    void setRegistersUnboundIds(bool registers)
    {
        m_registersUnboundIds = registers;
    }

private:
    QStringList* m_registers;
    QStringList* m_unregisters;
    QStringList* m_suspends;
    bool m_registersUnboundIds = false;
};

QSet<QString> workspaceIdsIn(const QStringList& ids)
{
    QSet<QString> out;
    for (const QString& id : ids) {
        if (id.startsWith(QLatin1String("workspace_"))) {
            out.insert(id);
        }
    }
    return out;
}

QSet<QString> toSet(const QStringList& ids)
{
    return QSet<QString>(ids.cbegin(), ids.cend());
}

QString describe(const QSet<QString>& ids)
{
    QStringList sorted(ids.cbegin(), ids.cend());
    sorted.sort();
    return sorted.join(QStringLiteral(", "));
}

/// "WorkspaceMoveColumnUp" → "workspace_move_column_up".
QString snakeCase(const QString& camel)
{
    QString out;
    out.reserve(camel.size() + 8);
    for (int i = 0; i < camel.size(); ++i) {
        const QChar c = camel.at(i);
        if (c.isUpper() && i > 0) {
            out.append(QLatin1Char('_'));
        }
        out.append(c.toLower());
    }
    return out;
}

/// The action id a Shortcuts.Global workspace key registers under. The slot
/// keys carry their 1-based number with no separator ("WorkspaceMoveSlot3"),
/// the ids carry it after an underscore ("workspace_move_slot_3").
QString actionIdForSchemaKey(const QString& key)
{
    static const QRegularExpression slotKey(QStringLiteral("^(Workspace(?:Move|Focus)Slot)(\\d+)$"));
    const QRegularExpressionMatch m = slotKey.match(key);
    if (m.hasMatch()) {
        return snakeCase(m.captured(1)) + QLatin1Char('_') + m.captured(2);
    }
    return snakeCase(key);
}

QVariantMap rowById(const QVariantList& rows, const QString& id)
{
    for (const QVariant& v : rows) {
        const QVariantMap row = v.toMap();
        if (row.value(QStringLiteral("id")).toString() == id) {
            return row;
        }
    }
    return {};
}

QSet<QString> workspaceRowIds(const QVariantList& rows)
{
    QSet<QString> out;
    for (const QVariant& v : rows) {
        const QString id = v.toMap().value(QStringLiteral("id")).toString();
        if (id.startsWith(QLatin1String("workspace_"))) {
            out.insert(id);
        }
    }
    return out;
}

} // namespace

class TestWorkspaceShortcuts : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<IsolatedConfigGuard> m_configGuard;

private Q_SLOTS:
    void init()
    {
        m_configGuard = std::make_unique<IsolatedConfigGuard>();
    }

    void cleanup()
    {
        m_configGuard.reset();
    }

    /// Schema, Settings, registration, gating and catalog agree on the
    /// workspace chord family, row for row.
    void workspaceChordsKeepParityAcrossSchemaRegistrationAndCatalog()
    {
        QSet<QString> expectedIds;
        QStringList missingProperties;
        QStringList registeredIds;
        QStringList unregisters;
        QStringList parkedIds;
        Settings settings;
        const QMetaObject* mo = settings.metaObject();

        const auto& schema = PlasmaZones::cachedSettingsSchema();
        const auto group = schema.groups.value(ConfigDefaults::shortcutsGlobalGroup());
        QVERIFY2(!group.isEmpty(), "Schema group Shortcuts.Global is empty: the schema lookup itself is broken.");
        for (const auto& def : group) {
            if (!def.key.startsWith(QLatin1String("Workspace"))) {
                continue;
            }
            expectedIds.insert(actionIdForSchemaKey(def.key));
            // The Q_PROPERTY is what the settings app's recorders bind and
            // what the D-Bus registry contract test reflects over.
            QString property = def.key;
            property[0] = property.at(0).toLower();
            property += QStringLiteral("Shortcut");
            if (mo->indexOfProperty(property.toLatin1().constData()) < 0) {
                missingProperties.append(property);
            }
        }
        // Vacuity guard: the verbs plus two full slot families.
        QVERIFY2(expectedIds.size() >= 2 * ConfigDefaults::WorkspaceSlotCount,
                 qPrintable(QStringLiteral("only %1 workspace chord keys in the schema").arg(expectedIds.size())));
        QVERIFY2(missingProperties.isEmpty(),
                 qPrintable(QStringLiteral("workspace chord keys with no Settings Q_PROPERTY: %1")
                                .arg(missingProperties.join(QStringLiteral(", ")))));

        ShortcutManager manager(&settings);
        auto backend = std::make_unique<RecordingBackend>(&registeredIds, &unregisters, &parkedIds);
        backend->setRegistersUnboundIds(true);
        manager.setBackendForTesting(std::move(backend));
        manager.registerShortcuts();

        // Registration: every id reaches the backend with this backend, so
        // the recorded list is the whole surface.
        const QSet<QString> registered = workspaceIdsIn(registeredIds);
        QVERIFY2(registered == expectedIds,
                 qPrintable(QStringLiteral("only in the schema: %1 | only registered: %2")
                                .arg(describe(QSet(expectedIds).subtract(registered)),
                                     describe(QSet(registered).subtract(expectedIds)))));

        // Gating: the family is inactive, so exactly the workspace ids park.
        // A row missing its workspaceGated flag would hold a grab with the
        // feature off; any other id parking would lose a working chord.
        QVERIFY2(toSet(parkedIds) == expectedIds,
                 qPrintable(QStringLiteral("parked: %1").arg(describe(toSet(parkedIds)))));
        QVERIFY(unregisters.isEmpty());

        // Catalog: with the family on, every member has a row filed under
        // Workspaces. An id the catalog does not know lands in the "Other"
        // bucket (category order 99) instead.
        manager.setWorkspaceFamilyActive(true);
        const QVariantList catalog = manager.shortcutCatalog();
        QStringList uncatalogued;
        for (const QString& id : std::as_const(expectedIds)) {
            const QVariantMap row = rowById(catalog, id);
            if (row.isEmpty() || row.value(QStringLiteral("categoryOrder")).toInt() != 5) {
                uncatalogued.append(id);
            }
        }
        uncatalogued.sort();
        QVERIFY2(uncatalogued.isEmpty(),
                 qPrintable(QStringLiteral("workspace actions with no Workspaces catalog row: %1")
                                .arg(uncatalogued.join(QStringLiteral(", ")))));
    }

    /// A parked family holds no grab, so neither the sheet nor the shell's
    /// uncompressed catalog may list it. The family appears once it is on.
    void parkedWorkspaceRowsLeaveTheSheetAndCatalog()
    {
        QStringList registers;
        QStringList unregisters;
        QStringList suspends;
        Settings settings;
        ShortcutManager manager(&settings);
        manager.setBackendForTesting(std::make_unique<RecordingBackend>(&registers, &unregisters, &suspends));
        manager.registerShortcuts();

        QVERIFY(!manager.workspaceFamilyActive());
        QVERIFY2(!manager.shortcutCatalog().isEmpty(), "catalog is empty: the producer itself is broken");
        QCOMPARE(describe(workspaceRowIds(manager.cheatsheetModel())), QString());
        QCOMPARE(describe(workspaceRowIds(manager.shortcutCatalog())), QString());

        QSignalSpy changed(&manager, &ShortcutManager::cheatsheetModelChanged);
        manager.setWorkspaceFamilyActive(true);
        QCOMPARE(changed.count(), 1);
        const QSet<QString> catalogued = workspaceRowIds(manager.shortcutCatalog());
        QVERIFY(catalogued.contains(QLatin1String(Ids::kIdWorkspaceFocusUp)));
        QVERIFY(catalogued.contains(Ids::workspaceMoveSlotId(0)));
    }

    /// With the stock takeover off, KWin keeps Switch One Desktop Up/Down and
    /// wins the key, so the focus pair parks while its chord is still the
    /// stock one. Moving the chord, or turning the takeover on, revives it.
    void focusPairParksWhileKWinKeepsItsStockChords()
    {
        QStringList registers;
        QStringList unregisters;
        QStringList suspends;
        Settings settings;
        QVERIFY(settings.workspacesRebindKWinShortcuts());
        ShortcutManager manager(&settings);
        manager.setBackendForTesting(std::make_unique<RecordingBackend>(&registers, &unregisters, &suspends));
        manager.registerShortcuts();
        manager.setWorkspaceFamilyActive(true);

        const QString focusUp = QLatin1String(Ids::kIdWorkspaceFocusUp);
        const QString focusDown = QLatin1String(Ids::kIdWorkspaceFocusDown);
        QVERIFY(!rowById(manager.cheatsheetModel(), focusUp).isEmpty());

        suspends.clear();
        registers.clear();
        settings.setWorkspacesRebindKWinShortcuts(false);
        QCOMPARE(toSet(suspends), (QSet<QString>{focusUp, focusDown}));
        QVERIFY(rowById(manager.cheatsheetModel(), focusUp).isEmpty());
        QVERIFY(rowById(manager.cheatsheetModel(), focusDown).isEmpty());
        // The rest of the family is untouched by the takeover switch.
        QVERIFY(!rowById(manager.shortcutCatalog(), QLatin1String(Ids::kIdWorkspaceReorderUp)).isEmpty());

        // A chord of the user's own is not KWin's, so it fires: revived even
        // with the takeover still off. Its partner keeps the stock chord and
        // stays parked.
        suspends.clear();
        settings.setWorkspaceFocusUpShortcut(QStringLiteral("Meta+Ctrl+Alt+F11"));
        QCOMPARE(registers, QStringList{focusUp});
        QVERIFY(suspends.isEmpty());
        QVERIFY(!rowById(manager.cheatsheetModel(), focusUp).isEmpty());
        QVERIFY(rowById(manager.cheatsheetModel(), focusDown).isEmpty());

        registers.clear();
        settings.setWorkspacesRebindKWinShortcuts(true);
        QCOMPARE(registers, QStringList{focusDown});
        QVERIFY(!rowById(manager.cheatsheetModel(), focusDown).isEmpty());
        QVERIFY(unregisters.isEmpty());
    }

    /// Both slot families ship unbound. Each folds into ONE unassigned row
    /// rather than nine empty ones.
    void unboundSlotFamiliesFoldToOneRowEach()
    {
        QStringList registers;
        QStringList unregisters;
        QStringList suspends;
        Settings settings;
        ShortcutManager manager(&settings);
        manager.setBackendForTesting(std::make_unique<RecordingBackend>(&registers, &unregisters, &suspends));
        manager.registerShortcuts();
        manager.setWorkspaceFamilyActive(true);

        const QVariantList sheet = manager.cheatsheetModel();
        for (const QString& first : {Ids::workspaceMoveSlotId(0), Ids::workspaceFocusSlotId(0)}) {
            const QVariantMap row = rowById(sheet, first);
            QVERIFY2(!row.isEmpty(), qPrintable(first));
            QCOMPARE(row.value(QStringLiteral("assigned")).toBool(), false);
            QCOMPARE(row.value(QStringLiteral("triggers")).toStringList(), QStringList());
            QVERIFY2(row.value(QStringLiteral("label")).toString().endsWith(QLatin1String("1-9")),
                     qPrintable(row.value(QStringLiteral("label")).toString()));
        }
        QStringList unfolded;
        for (int i = 1; i < ConfigDefaults::WorkspaceSlotCount; ++i) {
            for (const QString& member : {Ids::workspaceMoveSlotId(i), Ids::workspaceFocusSlotId(i)}) {
                if (!rowById(sheet, member).isEmpty()) {
                    unfolded.append(member);
                }
            }
        }
        QVERIFY2(
            unfolded.isEmpty(),
            qPrintable(QStringLiteral("slot members kept their own row: %1").arg(unfolded.join(QLatin1Char(' ')))));
        // The uncompressed catalog still carries every member.
        QVERIFY(!rowById(manager.shortcutCatalog(), Ids::workspaceMoveSlotId(8)).isEmpty());
    }

    /// The named-workspace chords are adhoc by design. The collision guard
    /// that protects the settings-driven ids must let them through on both
    /// halves, or no named chord ever binds and none is ever released.
    void namedChordsReachTheBackendBothWays()
    {
        QStringList registers;
        QStringList unregisters;
        QStringList suspends;
        Settings settings;
        ShortcutManager manager(&settings);
        manager.setBackendForTesting(std::make_unique<RecordingBackend>(&registers, &unregisters, &suspends));
        manager.registerShortcuts();

        const QString key = QStringLiteral("{6f9619ff-8b86-d011-b42d-00cf4fc964ff}");
        const QString focusId = Ids::workspaceNamedFocusId(key);
        const QString moveId = Ids::workspaceNamedMoveId(key);
        registers.clear();
        manager.registerAdhocShortcut(focusId, QKeySequence(QStringLiteral("Meta+F1")),
                                      QStringLiteral("Focus Workspace \"Web\""), [] { });
        manager.registerAdhocShortcut(moveId, QKeySequence(QStringLiteral("Meta+Shift+F1")),
                                      QStringLiteral("Move Window to Workspace \"Web\""), [] { });
        QCOMPARE(registers, (QStringList{focusId, moveId}));

        manager.unregisterAdhocShortcut(focusId);
        manager.unregisterAdhocShortcut(moveId);
        QCOMPARE(unregisters, (QStringList{focusId, moveId}));
    }

    /// A bound named chord is a row of its own under Workspaces in both
    /// catalog views, labelled with its registration description, and
    /// binding or releasing it republishes the sheet.
    void namedChordsAreListedWhileBound()
    {
        QStringList registers;
        QStringList unregisters;
        QStringList suspends;
        Settings settings;
        ShortcutManager manager(&settings);
        manager.setBackendForTesting(std::make_unique<RecordingBackend>(&registers, &unregisters, &suspends));
        manager.registerShortcuts();

        const QString id = Ids::workspaceNamedFocusId(QStringLiteral("{6f9619ff-8b86-d011-b42d-00cf4fc964ff}"));
        const QString description = QStringLiteral("Focus Workspace \"Web\"");
        QSignalSpy changed(&manager, &ShortcutManager::cheatsheetModelChanged);
        manager.registerAdhocShortcut(id, QKeySequence(QStringLiteral("Meta+F1")), description, [] { });
        QCOMPARE(changed.count(), 1);

        // Listed whatever the family gate says: the chord holds its own grab.
        QVERIFY(!manager.workspaceFamilyActive());
        for (const QVariantList& rows : {manager.cheatsheetModel(), manager.shortcutCatalog()}) {
            const QVariantMap row = rowById(rows, id);
            QVERIFY2(!row.isEmpty(), "the bound named chord has no catalog row");
            QCOMPARE(row.value(QStringLiteral("category")).toString(), QStringLiteral("Workspaces"));
            QCOMPARE(row.value(QStringLiteral("categoryOrder")).toInt(), 5);
            QCOMPARE(row.value(QStringLiteral("label")).toString(), description);
            QCOMPARE(row.value(QStringLiteral("triggers")).toStringList(), QStringList{QStringLiteral("Meta+F1")});
        }

        manager.unregisterAdhocShortcut(id);
        QCOMPARE(changed.count(), 2);
        QVERIFY(rowById(manager.cheatsheetModel(), id).isEmpty());
        QVERIFY(rowById(manager.shortcutCatalog(), id).isEmpty());
    }

    /// The transient grabs (the picker's Escape and arrows) are adhoc too
    /// but never catalogued: binding one must neither list it nor
    /// republish the sheet.
    void otherAdhocGrabsStayOffTheSheet()
    {
        QStringList registers;
        QStringList unregisters;
        QStringList suspends;
        Settings settings;
        ShortcutManager manager(&settings);
        manager.setBackendForTesting(std::make_unique<RecordingBackend>(&registers, &unregisters, &suspends));
        manager.registerShortcuts();

        const QString id = QStringLiteral("pz.test.adhoc.escape");
        QSignalSpy changed(&manager, &ShortcutManager::cheatsheetModelChanged);
        manager.registerAdhocShortcut(id, QKeySequence(QStringLiteral("Escape")), QStringLiteral("Test grab"), [] { });
        QVERIFY(registers.contains(id));
        QCOMPARE(changed.count(), 0);
        QVERIFY(rowById(manager.shortcutCatalog(), id).isEmpty());
        manager.unregisterAdhocShortcut(id);
        QCOMPARE(changed.count(), 0);
    }
};

QTEST_MAIN(TestWorkspaceShortcuts)
#include "test_workspace_shortcuts.moc"
