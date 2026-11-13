// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Pins the teardown contract of ShortcutManager::unregisterShortcuts()
// (discussion #851): releasing the registration on the daemon stop() path
// must NOT call IBackend::unregisterShortcut for the persistent catalog
// entries. On KGlobalAccel that call purges the id's on-disk
// kglobalshortcutsrc record — the store holding the user's System Settings
// customisations — so a sweep there wiped every customised binding on each
// service restart. Teardown releases grabs by destroying the backend, whose
// destructor preserves persistent records.
//
// The explicit adhoc release path is the intended purge and must keep
// purging; the second case pins that the fix did not overreach.
//
// The same store is at risk from a second direction: parking a shortcut
// family whose feature is switched off. That park routes through
// IBackend::suspendShortcut, which releases the grab WITHOUT touching the
// persistent record — the whole point of the separate verb. RecordingBackend
// therefore records suspends apart from unregisters, so a park that
// regressed into a purge shows up as a failure here rather than as a silent
// wipe of the user's chords. Note the base class's default suspendShortcut
// forwards to unregisterShortcut, which is right for the grab-only backends
// (Portal, D-Bus) that own no persistent record but would be exactly the
// wrong answer for KGlobalAccel — so this fake overrides it, as the real
// KGlobalAccel backend does.
//
// The park cases also pin WHICH ids park: exactly the dynamic-workspaces
// family members that hold a chord, never another family's, and the same ids
// come back when the family is switched on. A backend that cannot release a
// grab per id (Portal) must never register the parked ids at all.

#include "daemon/controllers/shortcutmanager.h"
#include "daemon/controllers/shortcutmanager_ids.h"

#include "config/settings.h"
#include "helpers/IsolatedConfigGuard.h"

#include <PhosphorShortcuts/IBackend.h>

#include <QKeySequence>
#include <QSet>
#include <QStringList>
#include <QTest>

#include <memory>

using PlasmaZones::Settings;
using PlasmaZones::ShortcutManager;
using PlasmaZones::TestHelpers::IsolatedConfigGuard;

namespace {

// Records calls into test-owned lists (passed by pointer) so the record
// survives the backend's destruction — unregisterShortcuts() destroys the
// injected backend as part of teardown.
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

    // Overridden away from the base default (which forwards to
    // unregisterShortcut) so a park is distinguishable from a purge, the
    // same split KGlobalAccelBackend implements.
    void suspendShortcut(const QString& id) override
    {
        m_suspends->append(id);
    }

    void flush() override
    {
        Q_EMIT ready();
    }

    /// False models PortalBackend, which cannot give a grab back per id.
    bool suspendReleasesGrab() const override
    {
        return m_suspendReleasesGrab;
    }

    void setSuspendReleasesGrab(bool releases)
    {
        m_suspendReleasesGrab = releases;
    }

private:
    QStringList* m_registers;
    QStringList* m_unregisters;
    QStringList* m_suspends;
    bool m_suspendReleasesGrab = true;
};

QSet<QString> toSet(const QStringList& ids)
{
    return QSet<QString>(ids.cbegin(), ids.cend());
}

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

/// The dynamic-workspaces ids that hold a chord under the shipped defaults.
/// The rest of the family (window moves, monitor hand-offs, both slot
/// families) ships unbound, and an unbound id is never registered, so it has
/// no grab to park.
QSet<QString> boundWorkspaceIdsByDefault()
{
    using namespace PlasmaZones::ShortcutIds;
    return {QLatin1String(kIdWorkspaceFocusUp),      QLatin1String(kIdWorkspaceFocusDown),
            QLatin1String(kIdWorkspaceMoveColumnUp), QLatin1String(kIdWorkspaceMoveColumnDown),
            QLatin1String(kIdWorkspaceReorderUp),    QLatin1String(kIdWorkspaceReorderDown)};
}

QString describe(const QSet<QString>& ids)
{
    QStringList sorted(ids.cbegin(), ids.cend());
    sorted.sort();
    return sorted.join(QStringLiteral(", "));
}

} // namespace

class TestShortcutManagerTeardown : public QObject
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

    void teardownDoesNotPurgePersistentBindings()
    {
        // The recorded lists outlive the manager: the backend it owns writes
        // into them until the manager is gone.
        QStringList registers;
        QStringList unregisters;
        QStringList suspends;
        Settings settings;
        ShortcutManager manager(&settings);
        manager.setBackendForTesting(std::make_unique<RecordingBackend>(&registers, &unregisters, &suspends));

        manager.registerShortcuts();
        QVERIFY2(!registers.isEmpty(), "expected the catalog to reach the backend on registerShortcuts()");

        // The workspace family is inactive until the daemon's controller
        // runs, so it parks on the first settle. The exact set doubles as the
        // positive control: the purge check below would also pass if nothing
        // had been parked at all.
        QCOMPARE(toSet(suspends), boundWorkspaceIdsByDefault());
        QVERIFY2(unregisters.isEmpty(),
                 qPrintable(QStringLiteral("parking a disabled family purged %1 persistent binding(s), e.g. \"%2\" — "
                                           "a park must release the grab without touching the on-disk record")
                                .arg(unregisters.size())
                                .arg(unregisters.value(0))));

        manager.unregisterShortcuts();
        QVERIFY2(unregisters.isEmpty(),
                 qPrintable(QStringLiteral("teardown purged %1 persistent binding(s) from the backend, e.g. \"%2\" — "
                                           "on KGlobalAccel this deletes the user's customised shortcut")
                                .arg(unregisters.size())
                                .arg(unregisters.value(0))));
    }

    /// The park is register-then-suspend on a backend that can release a
    /// grab, so the parked ids keep their platform record (the System
    /// Settings row). Switching the family on registers exactly those ids
    /// again, and switching it off parks exactly those again.
    void workspaceFamilyParksAndResumesExactlyItsBoundIds()
    {
        QStringList registers;
        QStringList unregisters;
        QStringList suspends;
        Settings settings;
        ShortcutManager manager(&settings);
        manager.setBackendForTesting(std::make_unique<RecordingBackend>(&registers, &unregisters, &suspends));
        manager.registerShortcuts();

        const QSet<QString> expected = boundWorkspaceIdsByDefault();
        // Registered first: that is what gives the parked ids a record.
        const QSet<QString> registeredWorkspaceIds = workspaceIdsIn(registers);
        QVERIFY2(registeredWorkspaceIds == expected,
                 qPrintable(QStringLiteral("registered workspace ids: %1").arg(describe(registeredWorkspaceIds))));
        QCOMPARE(toSet(suspends), expected);

        registers.clear();
        suspends.clear();
        manager.setWorkspaceFamilyActive(true);
        QVERIFY(manager.workspaceFamilyActive());
        QVERIFY2(toSet(registers) == expected,
                 qPrintable(QStringLiteral("resumed ids: %1").arg(describe(toSet(registers)))));
        QVERIFY(suspends.isEmpty());

        registers.clear();
        manager.setWorkspaceFamilyActive(false);
        QCOMPARE(toSet(suspends), expected);
        QVERIFY(registers.isEmpty());
        QVERIFY(unregisters.isEmpty());
    }

    /// A backend that cannot release a grab per id (Portal) must never be
    /// handed a parked id: registering it would hold a grab for the whole
    /// session with nothing to fire. It gets the ids only once the family
    /// is on.
    void portalLikeBackendNeverRegistersParkedIds()
    {
        QStringList registers;
        QStringList unregisters;
        QStringList suspends;
        Settings settings;
        ShortcutManager manager(&settings);
        auto backend = std::make_unique<RecordingBackend>(&registers, &unregisters, &suspends);
        backend->setSuspendReleasesGrab(false);
        manager.setBackendForTesting(std::move(backend));
        manager.registerShortcuts();

        // Positive control: the rest of the table did reach the backend.
        QVERIFY2(!registers.isEmpty(), "expected the catalog to reach the backend on registerShortcuts()");
        const QSet<QString> registeredWorkspaceIds = workspaceIdsIn(registers);
        QVERIFY2(registeredWorkspaceIds.isEmpty(),
                 qPrintable(QStringLiteral("parked workspace ids reached a backend that cannot release them: %1")
                                .arg(describe(registeredWorkspaceIds))));
        QVERIFY(suspends.isEmpty());

        registers.clear();
        manager.setWorkspaceFamilyActive(true);
        QCOMPARE(toSet(registers), boundWorkspaceIdsByDefault());
        QVERIFY(unregisters.isEmpty());
    }

    void explicitAdhocReleaseStillPurges()
    {
        QStringList registers;
        QStringList unregisters;
        QStringList suspends;
        Settings settings;
        ShortcutManager manager(&settings);
        manager.setBackendForTesting(std::make_unique<RecordingBackend>(&registers, &unregisters, &suspends));
        manager.registerShortcuts();

        const QString id = QStringLiteral("pz.test.adhoc.escape");
        manager.registerAdhocShortcut(id, QKeySequence(QStringLiteral("Escape")), QStringLiteral("Test grab"), [] { });
        QVERIFY(registers.contains(id));

        manager.unregisterAdhocShortcut(id);
        QCOMPARE(unregisters, QStringList{id});
    }
};

QTEST_MAIN(TestShortcutManagerTeardown)
#include "test_shortcutmanager_teardown.moc"
