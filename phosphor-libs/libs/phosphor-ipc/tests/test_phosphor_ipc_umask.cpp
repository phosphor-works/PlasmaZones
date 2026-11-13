// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// IpcRouter::start makes its socket file 0600 without the process umask.
// The umask is one for the whole process, so a router that set it, even
// for an instant, changed the mode of every file and directory that other
// threads made meanwhile. This binary defines umask() itself, which takes
// the place of the C library's for the router library too, and counts the
// calls.

#include "ipctesthelpers.h"

#include <PhosphorIpc/IpcRouter.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QtCore/qtclasshelpermacros.h>

#include <atomic>
#include <cstring>

#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
std::atomic<int> umaskCalls{0};

// The process umask through the system call, which nothing interposes.
mode_t setUmask(mode_t mask)
{
    return static_cast<mode_t>(::syscall(SYS_umask, mask));
}
} // namespace

// Default visibility: the tree builds with -fvisibility=hidden, and a hidden
// symbol is not exported.
extern "C" __attribute__((visibility("default"))) mode_t umask(mode_t mask) noexcept
{
    ++umaskCalls;
    return setUmask(mask);
}

using namespace PhosphorIpc;
using PhosphorIpcTests::RouterFixture;

class TestPhosphorIpcUmask : public QObject
{
    Q_OBJECT
public:
    Q_DISABLE_COPY_MOVE(TestPhosphorIpcUmask)
    TestPhosphorIpcUmask() = default;
private Q_SLOTS:
    void start_doesNotTouchTheUmask();
    void start_socketIs0600_whateverTheUmask();
    void start_keepsLiveListener_leavesNothingBehind();
    void start_relativeNameGoesWhereQtPutsIt();
    void stop_removesOnlyItsOwnSocket();
};

void TestPhosphorIpcUmask::start_doesNotTouchTheUmask()
{
    RouterFixture fx;
    QVERIFY(fx.valid());
    // The interposition works: the global lookup, which binds the router
    // library's calls, finds this umask() (the binary exports it, see
    // CMakeLists.txt), and it counts.
    void* const found = ::dlsym(RTLD_DEFAULT, "umask");
    mode_t (*bound)(mode_t) = nullptr;
    static_assert(sizeof(bound) == sizeof(found));
    std::memcpy(&bound, &found, sizeof(bound));
    QVERIFY(bound == &::umask);
    umaskCalls = 0;
    const mode_t previous = ::umask(022);
    ::umask(previous);
    QCOMPARE(umaskCalls.load(), 2);

    umaskCalls = 0;
    QVERIFY(fx.router.start(fx.sockPath));
    QCOMPARE(umaskCalls.load(), 0);
    fx.router.stop();
    QCOMPARE(umaskCalls.load(), 0);
}

void TestPhosphorIpcUmask::start_socketIs0600_whateverTheUmask()
{
    for (const mode_t mask : {mode_t(0), mode_t(022), mode_t(077)}) {
        RouterFixture fx;
        QVERIFY(fx.valid());
        const mode_t previous = setUmask(mask);
        const bool started = fx.router.start(fx.sockPath);
        setUmask(previous);
        QVERIFY(started);
        struct stat info{};
        QCOMPARE(::lstat(QFile::encodeName(fx.sockPath).constData(), &info), 0);
        QVERIFY(S_ISSOCK(info.st_mode));
        QCOMPARE(info.st_mode & 07777, mode_t(0600));
        // The socket listens at the path, and the private directory that
        // it was bound in is gone.
        QLocalSocket client;
        QVERIFY(PhosphorIpcTests::connectClient(client, fx.sockPath));
        QCOMPARE(QDir(fx.dir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot),
                 QStringList{QStringLiteral("test.sock")});
    }
}

void TestPhosphorIpcUmask::start_keepsLiveListener_leavesNothingBehind()
{
    // The stale, busy and not-a-socket cases are in test_phosphor_ipc_e2e.
    RouterFixture fx;
    QVERIFY(fx.valid());
    QVERIFY(fx.router.start(fx.sockPath));
    QLocalSocket client;
    QVERIFY(PhosphorIpcTests::connectClient(client, fx.sockPath));

    // A live listener keeps its path, and its socket file.
    struct stat live{};
    QCOMPARE(::lstat(QFile::encodeName(fx.sockPath).constData(), &live), 0);
    IpcRouter second;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("listen\\(\\) failed")));
    QVERIFY(!second.start(fx.sockPath));
    struct stat after{};
    QCOMPARE(::lstat(QFile::encodeName(fx.sockPath).constData(), &after), 0);
    QCOMPARE(after.st_ino, live.st_ino);
    QLocalSocket again;
    QVERIFY(PhosphorIpcTests::connectClient(again, fx.sockPath));
    // The failed start left no private directory behind.
    QCOMPARE(QDir(fx.dir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot),
             QStringList{QStringLiteral("test.sock")});
}

void TestPhosphorIpcUmask::start_relativeNameGoesWhereQtPutsIt()
{
    // A name that is not a path goes where QLocalServer puts it.
    const QString name = QStringLiteral("phosphor-ipc-test-%1.sock").arg(QUuid::createUuid().toString(QUuid::Id128));
    QString expected;
    {
        QLocalServer probe;
        QVERIFY(probe.listen(name));
        expected = probe.fullServerName();
    }
    QVERIFY(!QFileInfo::exists(expected));

    IpcRouter router;
    QVERIFY(router.start(name));
    QCOMPARE(router.socketPath(), name);
    struct stat info{};
    QCOMPARE(::lstat(QFile::encodeName(expected).constData(), &info), 0);
    QCOMPARE(info.st_mode & 07777, mode_t(0600));
    QLocalSocket client;
    QVERIFY(PhosphorIpcTests::connectClient(client, name));
    router.stop();
    QVERIFY(!QFileInfo::exists(expected));
}

void TestPhosphorIpcUmask::stop_removesOnlyItsOwnSocket()
{
    RouterFixture fx;
    QVERIFY(fx.valid());
    QVERIFY(fx.router.start(fx.sockPath));
    fx.router.stop();
    QVERIFY(!QFileInfo::exists(fx.sockPath));

    // A file that took the path meanwhile is not the router's to remove.
    QVERIFY(fx.router.start(fx.sockPath));
    QVERIFY(QFile::remove(fx.sockPath));
    QFile other(fx.sockPath);
    QVERIFY(other.open(QIODevice::WriteOnly));
    other.write("other");
    other.close();
    fx.router.stop();
    QVERIFY(QFileInfo::exists(fx.sockPath));
}

QTEST_GUILESS_MAIN(TestPhosphorIpcUmask)
#include "test_phosphor_ipc_umask.moc"
