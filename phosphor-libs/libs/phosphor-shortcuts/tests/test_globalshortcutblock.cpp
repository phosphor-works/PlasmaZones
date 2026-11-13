// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// GlobalShortcutBlock against a stand-in org.kde.kglobalaccel that this test
// registers on the session bus and that records every blockGlobalShortcuts
// call. ctest runs it on a private bus, where the name is free. Run by hand on
// a desktop session, the real kglobalacceld owns the name and the test skips
// rather than suspend the user's shortcuts.
//
// Pins the per-process hold count (overlapping holds, only the last release
// unblocks), the failed block that takes no hold, the start-up release that
// sends whatever the process believes, and the quit hook that gives back
// every hold still outstanding.

#include <PhosphorShortcuts/GlobalShortcutBlock.h>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusError>
#include <QObject>
#include <QTest>

namespace {

const QString kService = QStringLiteral("org.kde.kglobalaccel");
const QString kPath = QStringLiteral("/kglobalaccel");

} // namespace

// External linkage: TestGlobalShortcutBlock holds one as a member, and a
// member whose type lives in an anonymous namespace trips -Wsubobject-linkage.
class FakeKGlobalAccel : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KGlobalAccel")

public:
    QList<bool> calls;
    bool failNext = false;

public Q_SLOTS:
    void blockGlobalShortcuts(bool block)
    {
        if (failNext) {
            failNext = false;
            sendErrorReply(QDBusError::Failed, QStringLiteral("refused by the test"));
            return;
        }
        calls.append(block);
    }
};

class TestGlobalShortcutBlock : public QObject
{
    Q_OBJECT

private:
    FakeKGlobalAccel m_fake;

private Q_SLOTS:
    void initTestCase()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            QSKIP("no session bus");
        }
        if (bus.interface()->isServiceRegistered(kService)) {
            QSKIP("a real kglobalaccel owns the name on this bus; refusing to suspend its shortcuts");
        }
        QVERIFY(bus.registerObject(kPath, &m_fake, QDBusConnection::ExportAllSlots));
        QVERIFY(bus.registerService(kService));
        QVERIFY(PhosphorShortcuts::canBlockGlobalShortcuts());
    }

    void init()
    {
        m_fake.calls.clear();
        m_fake.failNext = false;
    }

    void cleanup()
    {
        // Leave no hold for the next case, whatever this one did.
        while (PhosphorShortcuts::globalShortcutsBlocked()) {
            PhosphorShortcuts::setGlobalShortcutsBlocked(false);
        }
    }

    /// Two recorders overlapping in one process: the first hold blocks, the
    /// second needs no call, and only giving back the last hold unblocks.
    void overlappingHolds_onlyTheLastReleaseUnblocks()
    {
        using namespace PhosphorShortcuts;
        QVERIFY(setGlobalShortcutsBlocked(true));
        QVERIFY(setGlobalShortcutsBlocked(true));
        QCOMPARE(m_fake.calls, (QList<bool>{true}));
        QVERIFY(globalShortcutsBlocked());

        QVERIFY(setGlobalShortcutsBlocked(false));
        QCOMPARE(m_fake.calls, (QList<bool>{true}));
        QVERIFY(globalShortcutsBlocked());

        QVERIFY(setGlobalShortcutsBlocked(false));
        QCOMPARE(m_fake.calls, (QList<bool>{true, false}));
        QVERIFY(!globalShortcutsBlocked());

        // A release with no hold outstanding sends nothing.
        QVERIFY(setGlobalShortcutsBlocked(false));
        QCOMPARE(m_fake.calls, (QList<bool>{true, false}));
    }

    /// A block the service refuses takes no hold, so the caller's matching
    /// release (which it should not send, but may) cannot unblock anything.
    void refusedBlock_takesNoHold()
    {
        using namespace PhosphorShortcuts;
        m_fake.failNext = true;
        QVERIFY(!setGlobalShortcutsBlocked(true));
        QVERIFY(!globalShortcutsBlocked());
        QVERIFY(setGlobalShortcutsBlocked(false));
        QVERIFY(m_fake.calls.isEmpty());
    }

    /// The start-up release sends the unblock with no hold taken, because a
    /// crashed previous run is exactly the case this process knows nothing
    /// about. It refuses while the process holds a block of its own.
    void strandedRelease_sendsUnblockButNeverUnderAHold()
    {
        using namespace PhosphorShortcuts;
        QVERIFY(releaseStrandedGlobalShortcutBlock());
        QCOMPARE(m_fake.calls, (QList<bool>{false}));

        QVERIFY(setGlobalShortcutsBlocked(true));
        QVERIFY(!releaseStrandedGlobalShortcutBlock());
        QCOMPARE(m_fake.calls, (QList<bool>{false, true}));
        QVERIFY(globalShortcutsBlocked());
    }

    /// An application quitting mid-capture gives back every hold it still
    /// has, with one unblock.
    void quit_releasesEveryOutstandingHold()
    {
        using namespace PhosphorShortcuts;
        QVERIFY(setGlobalShortcutsBlocked(true));
        QVERIFY(setGlobalShortcutsBlocked(true));
        QVERIFY(QMetaObject::invokeMethod(QCoreApplication::instance(), "aboutToQuit", Qt::DirectConnection));
        QCOMPARE(m_fake.calls, (QList<bool>{true, false}));
        QVERIFY(!globalShortcutsBlocked());
    }
};

QTEST_GUILESS_MAIN(TestGlobalShortcutBlock)
#include "test_globalshortcutblock.moc"
