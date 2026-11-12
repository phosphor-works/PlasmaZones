// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Handshake.h>

#include <QTest>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace PhosphorChannel;

namespace {

// The words of the handshake, as the library writes them.
constexpr std::uint32_t Magic = 0x48434850;
constexpr std::uint32_t RingOffer = 1;
constexpr std::uint32_t Refused = 1;

struct SocketPair
{
    int a = -1;
    int b = -1;
    explicit SocketPair(int type = SOCK_STREAM)
    {
        int fds[2];
        if (::socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, fds) == 0) {
            a = fds[0];
            b = fds[1];
        }
    }
    ~SocketPair()
    {
        if (a >= 0)
            ::close(a);
        if (b >= 0)
            ::close(b);
    }
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;
};

/// An offer as a writer that breaks the rules would send it: @p header, the
/// codec, and @p fds with the first bytes.
bool sendOffer(int socket, const std::uint32_t (&header)[4], const std::string& codec, const std::vector<int>& fds)
{
    std::string message(reinterpret_cast<const char*>(header), sizeof header);
    message += codec;
    iovec io{message.data(), message.size()};
    std::vector<char> control(CMSG_SPACE(sizeof(int) * fds.size()));
    msghdr out{};
    out.msg_iov = &io;
    out.msg_iovlen = 1;
    if (!fds.empty()) {
        out.msg_control = control.data();
        out.msg_controllen = control.size();
        cmsghdr* c = CMSG_FIRSTHDR(&out);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        std::memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
    }
    return ::sendmsg(socket, &out, MSG_NOSIGNAL) == ssize_t(message.size());
}

/// The answer that acceptRing sent, as the writer reads it, within two seconds.
bool readAnswer(int socket, std::uint32_t (&reply)[3])
{
    pollfd p{socket, POLLIN, 0};
    if (::poll(&p, 1, 2000) != 1)
        return false;
    return ::recv(socket, reply, sizeof reply, MSG_WAITALL) == ssize_t(sizeof reply);
}

std::size_t openFds()
{
    std::size_t count = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator("/proc/self/fd"))
        ++count;
    return count;
}

} // namespace

class TestHandshake : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void ringCrossesTheSocket();
    void ringCrossesASocketThatPassesCredentials();
    void ringCrossesASocketThatPassesPidfds();
    void ringIsOfferedOnce();
    void reader_refusesAnotherVersionAndNamesBoth();
    void writer_namesBothVersionsOfARefusal();
    void writer_reportsARefusalOfItsOwnVersion();
    void reader_refusesAnOfferWithoutFds();
    void reader_refusesExtraFdsAndClosesThem();
    void reader_answersWhenFdsWereDropped();
    void reader_refusesABadCodec();
    void reader_refusesACodecItDoesNotRead();
    void reader_refusesARingLargerThanItTakes();
    void bothRefuseAnInvalidSocket();
    void bothRefuseASocketOfRecords();
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
    std::thread offer([&] {
        offered = offerRing(pair.a, *writer, 5000, &offerError);
    });
    std::string error;
    auto reader = acceptRing(pair.b, AcceptOptions(), 5000, &error);
    offer.join();
    QVERIFY2(reader, error.c_str());
    QVERIFY2(offered, offerError.c_str());
    QCOMPARE(reader->codec(), std::string("afterglow.state.v1"));
    QCOMPARE(reader->capacity(), writer->capacity());
    QCOMPARE(writer->tryWrite("through the ring"), WriteResult::Written);
    std::string frame;
    QCOMPARE(reader->read(frame), ReadResult::Frame);
    QCOMPARE(frame, std::string("through the ring"));
}

void TestHandshake::ringCrossesASocketThatPassesCredentials()
{
    // A socket of a service with PassCredentials: the kernel adds the
    // credentials to the fds of the offer.
    SocketPair pair;
    const int on = 1;
    QVERIFY(::setsockopt(pair.b, SOL_SOCKET, SO_PASSCRED, &on, sizeof on) == 0);
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    QVERIFY(writer);
    bool offered = false;
    std::string offerError;
    std::thread offer([&] {
        offered = offerRing(pair.a, *writer, 5000, &offerError);
    });
    std::string error;
    auto reader = acceptRing(pair.b, AcceptOptions(), 5000, &error);
    offer.join();
    QVERIFY2(reader, error.c_str());
    QVERIFY2(offered, offerError.c_str());
}

void TestHandshake::ringCrossesASocketThatPassesPidfds()
{
#ifdef SO_PASSPIDFD
    // The kernel adds a pidfd of the sender to the offer: the reader must
    // close it, not keep it.
    SocketPair pair;
    const int on = 1;
    if (::setsockopt(pair.b, SOL_SOCKET, SO_PASSPIDFD, &on, sizeof on) != 0)
        QSKIP("this kernel does not pass pidfds");
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    QVERIFY(writer);
    const std::size_t before = openFds();
    bool offered = false;
    std::thread offer([&] {
        offered = offerRing(pair.a, *writer, 5000, nullptr);
    });
    std::string error;
    auto reader = acceptRing(pair.b, AcceptOptions(), 5000, &error);
    offer.join();
    QVERIFY2(reader, error.c_str());
    QVERIFY(offered);
    // The reader holds the memfd and its wake socket, and the writer gave up
    // its copy of that socket: one more fd in all.
    QCOMPARE(openFds(), before + 1);
#else
    QSKIP("these headers have no SO_PASSPIDFD");
#endif
}

void TestHandshake::ringIsOfferedOnce()
{
    SocketPair pair;
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    QVERIFY(writer);
    bool offered = false;
    std::thread offer([&] {
        offered = offerRing(pair.a, *writer, 5000, nullptr);
    });
    auto reader = acceptRing(pair.b, AcceptOptions(), 5000, nullptr);
    offer.join();
    QVERIFY(reader && offered);
    // A second reader on a ring of one reader would steal its frames.
    SocketPair again;
    std::string error;
    QVERIFY(!offerRing(again.a, *writer, 1000, &error));
    QVERIFY2(error.find("offered already") != std::string::npos, error.c_str());
}

void TestHandshake::reader_refusesAnotherVersionAndNamesBoth()
{
    SocketPair pair;
    QVERIFY(sendOffer(pair.a, {Magic, ProtocolVersion + 4, RingOffer, 4}, "t.v1", {}));
    std::string error;
    QVERIFY(!acceptRing(pair.b, AcceptOptions(), 2000, &error));
    QVERIFY2(error.find(std::to_string(ProtocolVersion + 4)) != std::string::npos, error.c_str());
    QVERIFY2(error.find(std::to_string(ProtocolVersion)) != std::string::npos, error.c_str());
    // It answered with its own version, so the offering end can name both.
    std::uint32_t reply[3] = {};
    QVERIFY(readAnswer(pair.a, reply));
    QCOMPARE(reply[0], Magic);
    QCOMPARE(reply[1], ProtocolVersion);
    QCOMPARE(reply[2], Refused);
}

void TestHandshake::writer_namesBothVersionsOfARefusal()
{
    SocketPair pair;
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    QVERIFY(writer);
    std::thread other([&] {
        char offer[16 + 4];
        ::recv(pair.b, offer, sizeof offer, MSG_WAITALL);
        const std::uint32_t reply[3] = {Magic, ProtocolVersion + 2, Refused};
        ::send(pair.b, reply, sizeof reply, MSG_NOSIGNAL);
    });
    std::string error;
    const bool offered = offerRing(pair.a, *writer, 2000, &error);
    other.join();
    QVERIFY(!offered);
    QVERIFY2(error.find(std::to_string(ProtocolVersion + 2)) != std::string::npos, error.c_str());
    QVERIFY2(error.find(std::to_string(ProtocolVersion)) != std::string::npos, error.c_str());
}

void TestHandshake::writer_reportsARefusalOfItsOwnVersion()
{
    SocketPair pair;
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    QVERIFY(writer);
    std::thread other([&] {
        char offer[16 + 4];
        ::recv(pair.b, offer, sizeof offer, MSG_WAITALL);
        const std::uint32_t reply[3] = {Magic, ProtocolVersion, Refused};
        ::send(pair.b, reply, sizeof reply, MSG_NOSIGNAL);
    });
    std::string error;
    const bool offered = offerRing(pair.a, *writer, 2000, &error);
    other.join();
    QVERIFY(!offered);
    QVERIFY2(error.find("refused") != std::string::npos, error.c_str());
}

void TestHandshake::reader_refusesAnOfferWithoutFds()
{
    SocketPair pair;
    QVERIFY(sendOffer(pair.a, {Magic, ProtocolVersion, RingOffer, 4}, "t.v1", {}));
    std::string error;
    QVERIFY(!acceptRing(pair.b, AcceptOptions(), 2000, &error));
    QVERIFY2(error.find("malformed") != std::string::npos, error.c_str());
}

void TestHandshake::reader_refusesExtraFdsAndClosesThem()
{
    SocketPair pair;
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    QVERIFY(writer);
    const int extra = ::dup(writer->memoryFd());
    const std::size_t before = openFds();
    QVERIFY(sendOffer(pair.a, {Magic, ProtocolVersion, RingOffer, 4}, "t.v1", {extra, extra, extra, extra}));
    std::string error;
    QVERIFY(!acceptRing(pair.b, AcceptOptions(), 2000, &error));
    QVERIFY2(error.find("malformed") != std::string::npos, error.c_str());
    // Every fd that came is closed again.
    QCOMPARE(openFds(), before);
    ::close(extra);
}

void TestHandshake::reader_answersWhenFdsWereDropped()
{
    SocketPair pair;
    // More fds than any control buffer of the reader holds: the kernel
    // drops them, and the reader says so and answers.
    std::vector<int> many(200, pair.a);
    const std::size_t before = openFds();
    QVERIFY(sendOffer(pair.a, {Magic, ProtocolVersion, RingOffer, 4}, "t.v1", many));
    std::string error;
    QVERIFY(!acceptRing(pair.b, AcceptOptions(), 2000, &error));
    QVERIFY2(error.find("dropped") != std::string::npos, error.c_str());
    // The fds that the kernel did install are closed again.
    QCOMPARE(openFds(), before);
    std::uint32_t reply[3] = {};
    QVERIFY(readAnswer(pair.a, reply));
    QCOMPARE(reply[2], Refused);
}

void TestHandshake::reader_refusesABadCodec()
{
    SocketPair pair;
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    QVERIFY(writer);
    int wake[2];
    QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wake) == 0);
    QVERIFY(sendOffer(pair.a, {Magic, ProtocolVersion, RingOffer, 4}, "t v1", {writer->memoryFd(), wake[1]}));
    ::close(wake[0]);
    ::close(wake[1]);
    std::string error;
    QVERIFY(!acceptRing(pair.b, AcceptOptions(), 2000, &error));
    QVERIFY2(error.find("codec") != std::string::npos, error.c_str());
}

void TestHandshake::reader_refusesACodecItDoesNotRead()
{
    SocketPair pair;
    auto writer = RingWriter::create(1 << 16, "afterglow.state.v2", nullptr);
    QVERIFY(writer);
    bool offered = true;
    std::string offerError;
    std::thread offer([&] {
        offered = offerRing(pair.a, *writer, 5000, &offerError);
    });
    AcceptOptions options;
    options.codecs = {"afterglow.state.v1"};
    std::string error;
    auto reader = acceptRing(pair.b, options, 5000, &error);
    offer.join();
    QVERIFY(!reader);
    QVERIFY(!offered);
    QVERIFY2(error.find("afterglow.state.v2") != std::string::npos, error.c_str());
    // The writer learns why, not only that it was refused.
    QVERIFY2(offerError.find("does not read codec afterglow.state.v2") != std::string::npos, offerError.c_str());
}

void TestHandshake::reader_refusesARingLargerThanItTakes()
{
    SocketPair pair;
    auto writer = RingWriter::create(1 << 20, "t.v1", nullptr);
    QVERIFY(writer);
    bool offered = true;
    std::string offerError;
    std::thread offer([&] {
        offered = offerRing(pair.a, *writer, 5000, &offerError);
    });
    AcceptOptions options;
    options.maxCapacity = 1 << 16;
    std::string error;
    auto reader = acceptRing(pair.b, options, 5000, &error);
    offer.join();
    QVERIFY(!reader);
    QVERIFY(!offered);
    QVERIFY2(error.find("takes at most") != std::string::npos, error.c_str());
    QVERIFY2(offerError.find("refused") != std::string::npos, offerError.c_str());
}

void TestHandshake::bothRefuseAnInvalidSocket()
{
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    QVERIFY(writer);
    std::string error;
    QVERIFY(!acceptRing(-1, AcceptOptions(), -1, &error));
    QVERIFY2(error.find("not valid") != std::string::npos, error.c_str());
    QVERIFY(!offerRing(-1, *writer, -1, &error));
    QVERIFY2(error.find("not valid") != std::string::npos, error.c_str());
}

void TestHandshake::bothRefuseASocketOfRecords()
{
    // A socket of records cuts the offer at its first read, and both ends
    // would wait for the rest forever.
    SocketPair pair(SOCK_SEQPACKET);
    QVERIFY(pair.a >= 0);
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    QVERIFY(writer);
    std::string error;
    QVERIFY(!offerRing(pair.a, *writer, -1, &error));
    QVERIFY2(error.find("Unix stream socket") != std::string::npos, error.c_str());
    QVERIFY(!acceptRing(pair.b, AcceptOptions(), -1, &error));
    QVERIFY2(error.find("Unix stream socket") != std::string::npos, error.c_str());
}

void TestHandshake::timesOut()
{
    SocketPair pair;
    std::string error;
    QVERIFY(!acceptRing(pair.b, AcceptOptions(), 50, &error));
    QVERIFY2(error.find("timed out") != std::string::npos, error.c_str());
}

QTEST_GUILESS_MAIN(TestHandshake)
#include "test_handshake.moc"
