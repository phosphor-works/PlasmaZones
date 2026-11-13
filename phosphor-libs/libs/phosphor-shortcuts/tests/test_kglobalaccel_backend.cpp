// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// OPT-IN live test for KGlobalAccelBackend against a real kglobalaccel
// daemon. Guarded by PHOSPHORSHORTCUTS_KGLOBALACCEL_TESTS=1 following the
// same three-valued-verdict pattern as the GPU-gated tests:
//   1. Env gate unset            → QSKIP (default; CI and plain ctest runs).
//   2. Gate set, no live daemon  → QSKIP with reason (the factory fell back
//      or read-back cannot engage).
//   3. Gate set, daemon reachable → real assertions run.
//
// Shortcuts registered here are transient (persistent=false) where the case
// allows, so the backend purges their kglobalshortcutsrc record on unregister
// and on destruction. The park cases need a PERSISTENT id, because only
// those go through the suspend path; a RecordCleanup forgets each one when
// the case ends, so an opted-in run leaves no residue in the user's System
// Settings even if an assertion fails mid-case. Sequences use
// Meta+Shift with F28 or a higher function key, which collide with nothing
// real.
//
// What this pins that FakeBackend cannot: the behaviour of the REAL backend
// against kglobalacceld. The read-back contract (currentTriggers engages
// after a live setShortcut, follows an updateShortcut, and disengages after
// unregisterShortcut), a park and revive that leaves the client's action
// tables sound, the write-through of a chord changed while parked, the
// a(ai) marshalling of the foreign-rebind pair, and the record enumeration
// and purge used to clear a crashed run's leftovers.

#include <PhosphorShortcuts/Factory.h>
#include <PhosphorShortcuts/IBackend.h>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QKeySequence>
#include <QSignalSpy>
#include <QTest>

#include <optional>

namespace PhosphorShortcuts::tests {

class TestKGlobalAccelBackend : public QObject
{
    Q_OBJECT

private:
    bool gateOpen() const
    {
        return qEnvironmentVariable("PHOSPHORSHORTCUTS_KGLOBALACCEL_TESTS") == QLatin1String("1");
    }

    /// Deletes the case's record whatever happens to the case.
    struct RecordCleanup
    {
        IBackend* backend;
        QString id;
        ~RecordCleanup()
        {
            backend->forgetShortcut(id);
        }
    };

    /// Registers @p id and reports whether a live service took the grab.
    static bool registerLive(IBackend& backend, const QString& id, const QKeySequence& seq, bool persistent)
    {
        backend.registerShortcut(id, seq, seq, QStringLiteral("PhosphorShortcuts live test"), persistent);
        backend.flush();
        const std::optional<QStringList> readBack = backend.currentTriggers(id);
        return readBack.has_value() && !readBack->isEmpty();
    }

    /// Asks kglobalacceld to fire @p id exactly as a key press would.
    static void invokeLive(const QString& id)
    {
        QDBusMessage call = QDBusMessage::createMethodCall(
            QStringLiteral("org.kde.kglobalaccel"), QStringLiteral("/component/") + QCoreApplication::applicationName(),
            QStringLiteral("org.kde.kglobalaccel.Component"), QStringLiteral("invokeShortcut"));
        call << id;
        QDBusConnection::sessionBus().call(call, QDBus::Block, 1000);
    }

private Q_SLOTS:
    void initTestCase()
    {
        // KGlobalAccelBackend asserts a non-empty applicationName (it is
        // the persistent component key) — set it before any construction.
        QCoreApplication::setApplicationName(QStringLiteral("phosphorshortcuts_kga_test"));
    }

    void liveReadbackFollowsRegisterUpdateUnregister()
    {
        if (!gateOpen()) {
            QSKIP("Set PHOSPHORSHORTCUTS_KGLOBALACCEL_TESTS=1 to run against a live kglobalaccel daemon");
        }

        const auto backend = createBackend(BackendHint::KGlobalAccel, nullptr);
        QVERIFY(backend != nullptr);

        const QString id = QStringLiteral("pz.test.kga.readback");
        const QKeySequence first(QStringLiteral("Meta+Shift+F35"));
        const QKeySequence second(QStringLiteral("Meta+Shift+F34"));

        backend->registerShortcut(id, first, first, QStringLiteral("PhosphorShortcuts live test"),
                                  /*persistent=*/false);
        backend->flush();

        const std::optional<QStringList> afterRegister = backend->currentTriggers(id);
        if (!afterRegister.has_value() || afterRegister->isEmpty()) {
            // The KGlobalAccel hint fell through (lib built without it) or
            // no daemon answered the grab — nothing meaningful to assert.
            backend->unregisterShortcut(id);
            QSKIP("kglobalaccel daemon unavailable in this session — live read-back not exercisable");
        }
        QVERIFY(afterRegister->contains(first.toString(QKeySequence::PortableText)));

        backend->updateShortcut(id, first, second);
        backend->flush();
        const std::optional<QStringList> afterUpdate = backend->currentTriggers(id);
        QVERIFY(afterUpdate.has_value());
        QVERIFY(afterUpdate->contains(second.toString(QKeySequence::PortableText)));

        backend->unregisterShortcut(id);
        // Unknown id after unregister → cannot-report, NOT engaged-empty:
        // the caller's own stored value is the only remaining answer.
        QVERIFY(!backend->currentTriggers(id).has_value());
    }

    void parkAndReviveThenPress_reachesTheLiveAction()
    {
        // A park used to cut the KGlobalAccel client's own destroyed()
        // handler along with ours, so the client kept the freed QAction in
        // its tables. A revived id then had two candidates for one name and
        // a press resolved to the freed one. After a full off/on cycle a
        // press must fire the live action exactly once.
        if (!gateOpen()) {
            QSKIP("Set PHOSPHORSHORTCUTS_KGLOBALACCEL_TESTS=1 to run against a live kglobalaccel daemon");
        }
        const auto backend = createBackend(BackendHint::KGlobalAccel, nullptr);
        QVERIFY(backend != nullptr);
        const QString id = QStringLiteral("pz.test.kga.park");
        const QKeySequence seq(QStringLiteral("Meta+Shift+F33"));
        RecordCleanup cleanup{backend.get(), id};
        if (!registerLive(*backend, id, seq, /*persistent=*/true)) {
            QSKIP("kglobalaccel daemon unavailable in this session");
        }
        QSignalSpy activated(backend.get(), &IBackend::activated);

        for (int cycle = 0; cycle < 2; ++cycle) {
            backend->suspendShortcut(id);
            QVERIFY(!backend->currentTriggers(id).has_value());
            backend->registerShortcut(id, seq, seq, QStringLiteral("PhosphorShortcuts live test"), true);
            backend->flush();
        }

        invokeLive(id);
        QTRY_COMPARE(activated.count(), 1);
        QCOMPARE(activated.constFirst().constFirst().toString(), id);
        QTest::qWait(100);
        QCOMPARE(activated.count(), 1);
    }

    void storeWhileParked_isWhatTheRevivalGrabs()
    {
        if (!gateOpen()) {
            QSKIP("Set PHOSPHORSHORTCUTS_KGLOBALACCEL_TESTS=1 to run against a live kglobalaccel daemon");
        }
        const auto backend = createBackend(BackendHint::KGlobalAccel, nullptr);
        QVERIFY(backend != nullptr);
        const QString id = QStringLiteral("pz.test.kga.store");
        const QKeySequence original(QStringLiteral("Meta+Shift+F32"));
        const QKeySequence changed(QStringLiteral("Meta+Shift+F31"));
        RecordCleanup cleanup{backend.get(), id};
        if (!registerLive(*backend, id, original, /*persistent=*/true)) {
            QSKIP("kglobalaccel daemon unavailable in this session");
        }

        backend->suspendShortcut(id);
        backend->storeShortcut(id, changed);
        // Revive the way a consumer does, with the sequence it had before:
        // the autoloading registration must hand back the stored chord.
        backend->registerShortcut(id, original, original, QStringLiteral("PhosphorShortcuts live test"), true);
        backend->flush();

        const std::optional<QStringList> readBack = backend->currentTriggers(id);
        QVERIFY(readBack.has_value());
        QCOMPARE(*readBack, QStringList{changed.toString(QKeySequence::PortableText)});
    }

    void foreignRoundTrip_preservesTheSequence()
    {
        // Exercises the a(ai) marshalling both ways on an action of our own
        // component, never on a real one.
        if (!gateOpen()) {
            QSKIP("Set PHOSPHORSHORTCUTS_KGLOBALACCEL_TESTS=1 to run against a live kglobalaccel daemon");
        }
        const auto backend = createBackend(BackendHint::KGlobalAccel, nullptr);
        QVERIFY(backend != nullptr);
        const QString id = QStringLiteral("pz.test.kga.foreign");
        RecordCleanup cleanup{backend.get(), id};
        if (!registerLive(*backend, id, QKeySequence(QStringLiteral("Meta+Shift+F30")), /*persistent=*/false)) {
            QSKIP("kglobalaccel daemon unavailable in this session");
        }

        const QKeySequence chord(QStringLiteral("Meta+Shift+F29"));
        QVERIFY(backend->setForeignShortcuts(QCoreApplication::applicationName(), id, {chord}));
        const std::optional<QList<QKeySequence>> read =
            backend->foreignShortcuts(QCoreApplication::applicationName(), id);
        QVERIFY(read.has_value());
        QCOMPARE(*read, QList<QKeySequence>{chord});
    }

    void storedIdsListAndForgetDropsAParkedRecord()
    {
        if (!gateOpen()) {
            QSKIP("Set PHOSPHORSHORTCUTS_KGLOBALACCEL_TESTS=1 to run against a live kglobalaccel daemon");
        }
        const auto backend = createBackend(BackendHint::KGlobalAccel, nullptr);
        QVERIFY(backend != nullptr);
        const QString id = QStringLiteral("pz.test.kga.forget");
        RecordCleanup cleanup{backend.get(), id};
        if (!registerLive(*backend, id, QKeySequence(QStringLiteral("Meta+Shift+F28")), /*persistent=*/true)) {
            QSKIP("kglobalaccel daemon unavailable in this session");
        }
        std::optional<QStringList> stored = backend->storedShortcutIds();
        QVERIFY(stored.has_value());
        QVERIFY(stored->contains(id));

        // Parked, so the backend no longer holds it: forget takes the raw path.
        backend->suspendShortcut(id);
        backend->forgetShortcut(id);
        stored = backend->storedShortcutIds();
        QVERIFY(stored.has_value());
        QVERIFY(!stored->contains(id));
    }
};

} // namespace PhosphorShortcuts::tests

QTEST_MAIN(PhosphorShortcuts::tests::TestKGlobalAccelBackend)
#include "test_kglobalaccel_backend.moc"
