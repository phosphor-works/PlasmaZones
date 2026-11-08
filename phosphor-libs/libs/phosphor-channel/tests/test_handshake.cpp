// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Handshake.h>

#include <QTest>

#include <cstdint>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

using namespace PhosphorChannel;

namespace {

// The words of the handshake, as the library writes them.
constexpr std::uint32_t Magic = 0x48434850;
constexpr std::uint32_t RingOffer = 1;

struct SocketPair
{
    int a = -1;
    int b = -1;
    SocketPair()
    {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0) {
            a = fds[0];
            b = fds[1];
        }
    }
    ~SocketPair()
    {
        if (a >= 0) ::close(a);
        if (b >= 0) ::close(b);
    }
};

} // namespace

class TestHandshake : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void ringCrossesTheSocket();
    void reader_refusesAnotherVersionAndNamesBoth();
    void writer_namesBothVersionsOfARefusal();
    void reader_refusesAnOfferWithoutFds();
    void timesOut();
};

void TestHandshake::ringCrossesTheSocket()
{
    SocketPair pair;
    QVERIFY(pair.a >= 0);
    auto writer = RingWriter::create(1 << 16, "afterglow.state.v1", nullptr);
    QVERIFY(writer);
    bool offered = false;
    std::string offerError;
    std::thread offer([&] { offered = offerRing(pair.a, *writer, 5000, &offerError); });
    std::string error;
    auto reader = acceptRing(pair.b, 5000, &error);
    offer.join();
    QVERIFY2(reader, error.c_str());
    QVERIFY2(offered, offerError.c_str());
    QCOMPARE(reader->codec(), std::string("afterglow.state.v1"));
    QCOMPARE(writer->tryWrite("through the ring"), WriteResult::Written);
    std::string frame;
    QCOMPARE(reader->read(frame), ReadResult::Frame);
    QCOMPARE(frame, std::string("through the ring"));
}

void TestHandshake::reader_refusesAnotherVersionAndNamesBoth()
{
    SocketPair pair;
    const std::uint32_t offer[4] = {Magic, ProtocolVersion + 4, RingOffer, 4};
    QVERIFY(::send(pair.a, offer, sizeof offer, 0) == ssize_t(sizeof offer));
    QVERIFY(::send(pair.a, "t.v1", 4, 0) == 4);
    std::string error;
    QVERIFY(!acceptRing(pair.b, 2000, &error));
    QVERIFY2(error.find(std::to_string(ProtocolVersion + 4)) != std::string::npos, error.c_str());
    QVERIFY2(error.find(std::to_string(ProtocolVersion)) != std::string::npos, error.c_str());
    // It answered with its own version, so the offering end can name both.
    std::uint32_t reply[3] = {};
    QVERIFY(::recv(pair.a, reply, sizeof reply, MSG_WAITALL) == ssize_t(sizeof reply));
    QCOMPARE(reply[0], Magic);
    QCOMPARE(reply[1], ProtocolVersion);
    QVERIFY(reply[2] != 0);
}

void TestHandshake::writer_namesBothVersionsOfARefusal()
{
    SocketPair pair;
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    QVERIFY(writer);
    std::thread other([&] {
        char offer[16 + 4];
        ::recv(pair.b, offer, sizeof offer, MSG_WAITALL);
        const std::uint32_t reply[3] = {Magic, ProtocolVersion + 2, 1};
        ::send(pair.b, reply, sizeof reply, 0);
    });
    std::string error;
    QVERIFY(!offerRing(pair.a, *writer, 2000, &error));
    other.join();
    QVERIFY2(error.find(std::to_string(ProtocolVersion + 2)) != std::string::npos, error.c_str());
    QVERIFY2(error.find(std::to_string(ProtocolVersion)) != std::string::npos, error.c_str());
}

void TestHandshake::reader_refusesAnOfferWithoutFds()
{
    SocketPair pair;
    const std::uint32_t offer[4] = {Magic, ProtocolVersion, RingOffer, 4};
    QVERIFY(::send(pair.a, offer, sizeof offer, 0) == ssize_t(sizeof offer));
    QVERIFY(::send(pair.a, "t.v1", 4, 0) == 4);
    std::string error;
    QVERIFY(!acceptRing(pair.b, 2000, &error));
    QVERIFY2(error.find("malformed") != std::string::npos, error.c_str());
}

void TestHandshake::timesOut()
{
    SocketPair pair;
    std::string error;
    QVERIFY(!acceptRing(pair.b, 50, &error));
    QVERIFY2(error.find("timed out") != std::string::npos, error.c_str());
}

QTEST_GUILESS_MAIN(TestHandshake)
#include "test_handshake.moc"
