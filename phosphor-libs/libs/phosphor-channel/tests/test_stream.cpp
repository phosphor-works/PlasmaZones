// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Stream.h>

#include <QTest>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace PhosphorChannel;

static_assert(!std::is_copy_constructible_v<StreamWriter> && !std::is_copy_assignable_v<StreamWriter>);
static_assert(!std::is_copy_constructible_v<StreamReader> && !std::is_copy_assignable_v<StreamReader>);

namespace {

int socketPair(int& other)
{
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
        return -1;
    other = fds[1];
    return fds[0];
}

/// Runs both streams until @p done, up to five seconds.
bool pumpUntil(Stream& a, Stream& b, const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done())
            return true;
        a.pump(5);
        b.pump(5);
    }
    return done();
}

/// Runs both streams for @p rounds rounds.
void pumpFor(Stream& a, Stream& b, int rounds)
{
    for (int i = 0; i < rounds; ++i) {
        a.pump(5);
        b.pump(5);
    }
}

std::string frameOf(std::size_t size, int seed)
{
    std::string frame(size, '\0');
    for (std::size_t i = 0; i < size; ++i)
        frame[i] = char((seed * 31 + int(i)) & 0xff);
    return frame;
}

// The packets of the stream, as the library writes them, for a peer that
// breaks the rules.
constexpr std::uint8_t HelloType = 1;
constexpr std::uint8_t OpenType = 2;
constexpr std::uint8_t DataType = 3;
constexpr std::uint8_t CreditType = 4;
constexpr std::uint8_t CloseType = 5;
constexpr std::uint8_t ReleaseType = 6;
constexpr std::uint8_t ErrorType = 7;
constexpr std::uint32_t MaxPayload = 1 << 20;

void put32(std::string& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(char((v >> (8 * i)) & 0xff));
}

void put64(std::string& out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(char((v >> (8 * i)) & 0xff));
}

std::string packet(std::uint8_t type, std::uint8_t flags, std::uint32_t id, const std::string& payload)
{
    std::string out{char(type), char(flags), 0, 0};
    put32(out, id);
    put32(out, std::uint32_t(payload.size()));
    return out + payload;
}

std::string hello(std::uint32_t version, std::uint32_t maxChannels = 64, std::uint32_t maxChannelId = 4096)
{
    std::string payload = "PHCHSTRM";
    put32(payload, version);
    put32(payload, maxChannels);
    put32(payload, maxChannelId);
    return packet(HelloType, 0, 0, payload);
}

std::string credit(std::uint32_t id, std::uint64_t bytes)
{
    std::string payload;
    put64(payload, bytes);
    return packet(CreditType, 0, id, payload);
}

std::string open(std::uint32_t id, const std::string& codec = "t.v1")
{
    return packet(OpenType, 0, id, codec);
}

/// Everything that @p fd holds now, within a second.
std::string drainRaw(int fd)
{
    std::string in;
    char buffer[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 50) != 1)
            break;
        const ssize_t n = ::recv(fd, buffer, sizeof buffer, MSG_DONTWAIT);
        if (n <= 0)
            break;
        in.append(buffer, std::size_t(n));
    }
    return in;
}

struct Packet
{
    std::uint8_t type;
    std::uint32_t id;
    std::string payload;
};

std::vector<Packet> parse(const std::string& bytes)
{
    std::vector<Packet> packets;
    std::size_t at = 0;
    const auto get32 = [&](std::size_t p) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= std::uint32_t(std::uint8_t(bytes[p + i])) << (8 * i);
        return v;
    };
    while (bytes.size() - at >= 12) {
        const std::uint32_t length = get32(at + 8);
        if (bytes.size() - at < 12 + length)
            break;
        packets.push_back({std::uint8_t(bytes[at]), get32(at + 4), bytes.substr(at + 12, length)});
        at += 12 + length;
    }
    return packets;
}

/// A stream with a window of 4096 bytes that reads @p bytes from a peer
/// that breaks the rules, after @p setup. Returns its error.
std::string errorFor(const std::string& bytes, const std::function<void(Stream&)>& setup = {}, bool endInput = false)
{
    int raw = -1;
    const int fd = socketPair(raw);
    Stream::Options small;
    small.window = 4096;
    small.maxChannels = 2;
    small.maxChannelId = 16;
    Stream stream(fd, fd, small);
    if (setup)
        setup(stream);
    ::send(raw, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (endInput)
        ::shutdown(raw, SHUT_WR);
    for (int i = 0; i < 50 && stream.open(); ++i)
        stream.pump(10);
    ::close(raw);
    return stream.error();
}

} // namespace

class TestStream : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void framesArriveInOrder();
    void bothDirectionsUseTheSameId();
    void fullWindow_wouldBlock_thenOnWritable();
    void frameOverTheWindow_isTooLarge();
    void emptyFrames_costAgainstTheWindow();
    void peerWindow_isCappedByThisEnd();
    void smallFrameOvertakesALargeOne();
    void worksOverTwoPipes();
    void pipeWithoutReader_failsWithoutSigpipe();
    void close_deliversFramesThenClosed();
    void channelId_opensAgainAfterItsEnd();
    void failedReopen_keepsTheOldWriter();
    void maxChannels_countsOpenChannels();
    void openWriter_refusesMoreChannelsThanThePeerReads();
    void channelIds_stayBelowThePeersLimit();
    void reopenBeforeRelease_isRefused();
    void endOfInput_closesReaders();
    void endOfInput_reachesOnReadable();
    void failure_reachesOnReadable();
    void shutdown_endsADuplicatedSocket();
    void shutdown_keepsItsTimeoutOnADuplicatedSocket();
    void writersBeforeTheHello_failPastThePeersLimits();
    void callbacks_keepThePacketOrder();
    void callbacks_mayThrow();
    void cleanShutdown_endsBothWithoutError();
    void failure_reachesTheOtherEnd();
    void versionRefusal_sendsThisEndsHello();
    void halfClose_keepsTheOutputGoing();
    void callbacks_mayPumpAndDestroyTheStream();
    void write_drainsReadersThroughOnReadable();
    void write_timesOutAndClosesWhenThePeerGoes();
    void bothEndsFlushAtOnce();
    void flushZero_writesWhatFits();
    void failedStream_wantsNoWriteAndReadsNoMore();
    void closedFd_failsInsteadOfSpinning();
    void refusesFdsThatCannotCarryAStream();
    void destruction_restoresFlagsAndKeepsStdioTaken();
    void refusesAnotherVersionAndNamesBoth();
    void refusesAPeerThatBreaksTheRules_data();
    void refusesAPeerThatBreaksTheRules();
};

void TestStream::framesArriveInOrder()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream a(fa), b(fb);
    std::vector<std::string> opened;
    b.onReader = [&](StreamReader& reader) {
        opened.push_back(reader.codec());
    };
    StreamWriter* writer = a.openWriter(1, "t.v1", nullptr);
    QVERIFY(writer);
    QVERIFY(pumpUntil(a, b, [&] {
        return b.reader(1) && writer->maxFrame() > 0;
    }));
    QVERIFY(a.ready() && b.ready());
    QCOMPARE(opened, std::vector<std::string>{"t.v1"});
    for (int i = 0; i < 100; ++i)
        QCOMPARE(writer->tryWrite(frameOf(std::size_t(i * 97), i)), WriteResult::Written);
    int received = 0;
    bool inOrder = true;
    std::string frame;
    QVERIFY(pumpUntil(a, b, [&] {
        while (b.reader(1)->read(frame) == ReadResult::Frame) {
            inOrder = inOrder && frame == frameOf(std::size_t(received * 97), received);
            ++received;
        }
        return received == 100;
    }));
    QVERIFY(inOrder);
}

void TestStream::bothDirectionsUseTheSameId()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream a(fa), b(fb);
    StreamWriter* toB = a.openWriter(7, "a.v1", nullptr);
    StreamWriter* toA = b.openWriter(7, "b.v1", nullptr);
    QVERIFY(toA && toB);
    std::string error;
    QVERIFY(!a.openWriter(7, "a.v1", &error));
    QVERIFY2(error.find("open for writing") != std::string::npos, error.c_str());
    QVERIFY(pumpUntil(a, b, [&] {
        return toA->maxFrame() && toB->maxFrame();
    }));
    QCOMPARE(toB->tryWrite("for b"), WriteResult::Written);
    QCOMPARE(toA->tryWrite("for a"), WriteResult::Written);
    std::string atA, atB;
    QVERIFY(pumpUntil(a, b, [&] {
        if (atA.empty())
            a.reader(7)->read(atA);
        if (atB.empty())
            b.reader(7)->read(atB);
        return !atA.empty() && !atB.empty();
    }));
    QCOMPARE(atA, std::string("for a"));
    QCOMPARE(atB, std::string("for b"));
    QCOMPARE(a.reader(7)->codec(), std::string("b.v1"));
}

void TestStream::fullWindow_wouldBlock_thenOnWritable()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream::Options small;
    small.window = 64 * 1024;
    Stream a(fa), b(fb, fb, small);
    StreamWriter* writer = a.openWriter(1, "t.v1", nullptr);
    // Before the reader granted a window, nothing can go.
    QCOMPARE(writer->tryWrite("early"), WriteResult::WouldBlock);
    int wakes = 0;
    a.onWritable = [&](StreamWriter& w) {
        QCOMPARE(&w, writer);
        ++wakes;
    };
    QVERIFY(pumpUntil(a, b, [&] {
        return writer->maxFrame() == small.window - Stream::FrameCost;
    }));
    QCOMPARE(wakes, 1);
    QCOMPARE(writer->tryWrite(frameOf(40 * 1024, 1)), WriteResult::Written);
    QCOMPARE(writer->tryWrite(frameOf(40 * 1024, 2)), WriteResult::WouldBlock);
    // The reader does not read: the window stays full, however long it runs.
    pumpFor(a, b, 20);
    QCOMPARE(writer->tryWrite(frameOf(40 * 1024, 2)), WriteResult::WouldBlock);
    std::string frame;
    QCOMPARE(b.reader(1)->read(frame), ReadResult::Frame);
    QVERIFY(frame == frameOf(40 * 1024, 1));
    QVERIFY(pumpUntil(a, b, [&] {
        return wakes == 2;
    }));
    QCOMPARE(writer->tryWrite(frameOf(40 * 1024, 2)), WriteResult::Written);
}

void TestStream::frameOverTheWindow_isTooLarge()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream::Options small;
    small.window = 4096;
    Stream a(fa), b(fb, fb, small);
    StreamWriter* writer = a.openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] {
        return writer->maxFrame() > 0;
    }));
    QCOMPARE(writer->maxFrame(), 4096 - Stream::FrameCost);
    QCOMPARE(writer->tryWrite(std::string(writer->maxFrame() + 1, 'x')), WriteResult::TooLarge);
    QCOMPARE(writer->tryWrite(std::string(writer->maxFrame(), 'x')), WriteResult::Written);
}

void TestStream::emptyFrames_costAgainstTheWindow()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream::Options small;
    small.window = 4096;
    Stream a(fa), b(fb, fb, small);
    StreamWriter* writer = a.openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] {
        return writer->maxFrame() > 0;
    }));
    // Empty frames are not free: the window holds a bounded number of them.
    std::size_t written = 0;
    while (writer->tryWrite("") == WriteResult::Written)
        ++written;
    QCOMPARE(written, std::size_t(4096 / Stream::FrameCost));
    // A peer that floods them anyway breaks the window.
    const std::string bytes = hello(ProtocolVersion) + open(1) + [] {
        std::string flood;
        for (int i = 0; i < 100; ++i)
            flood += packet(DataType, 1, 1, "");
        return flood;
    }();
    const std::string error = errorFor(bytes);
    QVERIFY2(error.find("past the window") != std::string::npos, error.c_str());
}

void TestStream::peerWindow_isCappedByThisEnd()
{
    int raw = -1;
    const int fd = socketPair(raw);
    Stream::Options small;
    small.window = 4096;
    Stream stream(fd, fd, small);
    StreamWriter* writer = stream.openWriter(1, "t.v1", nullptr);
    // A reader that grants a vast window and reads nothing.
    const std::string bytes = hello(ProtocolVersion) + credit(1, std::uint64_t(1) << 60);
    ::send(raw, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    for (int i = 0; i < 20 && writer->maxFrame() == 0; ++i)
        stream.pump(10);
    QCOMPARE(writer->maxFrame(), 4096 - Stream::FrameCost);
    QCOMPARE(writer->tryWrite(std::string(writer->maxFrame(), 'x')), WriteResult::Written);
    QCOMPARE(writer->tryWrite("more"), WriteResult::WouldBlock);
    ::close(raw);
}

void TestStream::smallFrameOvertakesALargeOne()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream::Options wide;
    wide.window = 8 << 20;
    Stream a(fa, fa, wide), b(fb, fb, wide);
    StreamWriter* state = a.openWriter(1, "state.v1", nullptr);
    StreamWriter* input = a.openWriter(2, "input.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] {
        return state->maxFrame() && input->maxFrame();
    }));
    QCOMPARE(state->tryWrite(frameOf(6 << 20, 3)), WriteResult::Written);
    QCOMPARE(input->tryWrite("key"), WriteResult::Written);
    std::string key;
    std::string large;
    bool keyFirst = false;
    QVERIFY(pumpUntil(a, b, [&] {
        if (b.reader(2) && b.reader(2)->read(key) == ReadResult::Frame) {
            keyFirst = b.reader(1)->read(large) == ReadResult::Empty;
            return true;
        }
        return false;
    }));
    QCOMPARE(key, std::string("key"));
    QVERIFY(keyFirst);
    QVERIFY(pumpUntil(a, b, [&] {
        return b.reader(1)->read(large) == ReadResult::Frame;
    }));
    QVERIFY(large == frameOf(6 << 20, 3));
}

void TestStream::worksOverTwoPipes()
{
    // As ssh carries it: one pipe each way, no socket.
    int up[2], down[2];
    QVERIFY(::pipe2(up, O_CLOEXEC) == 0 && ::pipe2(down, O_CLOEXEC) == 0);
    Stream a(down[0], up[1], Stream::Options());
    Stream b(up[0], down[1], Stream::Options());
    StreamWriter* writer = a.openWriter(3, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] {
        return writer->maxFrame() > 0;
    }));
    QCOMPARE(writer->tryWrite(frameOf(200000, 9)), WriteResult::Written);
    std::string frame;
    QVERIFY(pumpUntil(a, b, [&] {
        return b.reader(3)->read(frame) == ReadResult::Frame;
    }));
    QVERIFY(frame == frameOf(200000, 9));
}

void TestStream::pipeWithoutReader_failsWithoutSigpipe()
{
    // ssh exited: the pipe that fed it has no reader. A write must fail, not
    // end the process with SIGPIPE.
    int up[2], down[2];
    QVERIFY(::pipe2(up, O_CLOEXEC) == 0 && ::pipe2(down, O_CLOEXEC) == 0);
    ::close(up[0]);
    ::close(down[1]);
    Stream a(down[0], up[1], Stream::Options());
    std::string error;
    // The Open is what finds the output dead: the call itself says so.
    QVERIFY(!a.openWriter(1, "t.v1", &error));
    QVERIFY2(error.find("output failed") != std::string::npos, error.c_str());
    QVERIFY2(a.error().find("output failed") != std::string::npos, a.error().c_str());
}

void TestStream::close_deliversFramesThenClosed()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream a(fa), b(fb);
    StreamWriter* writer = a.openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] {
        return writer->maxFrame() > 0;
    }));
    QCOMPARE(writer->tryWrite("last"), WriteResult::Written);
    writer->close();
    QCOMPARE(writer->tryWrite("after"), WriteResult::Closed);
    std::string frame;
    QVERIFY(pumpUntil(a, b, [&] {
        return b.reader(1)->read(frame) == ReadResult::Frame;
    }));
    QCOMPARE(frame, std::string("last"));
    QVERIFY(pumpUntil(a, b, [&] {
        return b.reader(1)->read(frame) == ReadResult::Closed;
    }));
    // The stream itself still works.
    QVERIFY(a.open() && b.open());
}

void TestStream::channelId_opensAgainAfterItsEnd()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream a(fa), b(fb);
    StreamWriter* writer = a.openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] {
        return writer->maxFrame() > 0;
    }));
    QCOMPARE(writer->tryWrite("first life"), WriteResult::Written);
    writer->close();
    pumpFor(a, b, 5);
    // The reader has not read the end yet: the id is still taken.
    std::string error;
    QVERIFY(!a.openWriter(1, "t.v2", &error));
    QVERIFY2(error.find("still closing") != std::string::npos, error.c_str());
    std::string frame;
    QCOMPARE(b.reader(1)->read(frame), ReadResult::Frame);
    QCOMPARE(b.reader(1)->read(frame), ReadResult::Closed);
    StreamWriter* again = nullptr;
    QVERIFY(pumpUntil(a, b, [&] {
        return (again = a.openWriter(1, "t.v2", nullptr)) != nullptr;
    }));
    QVERIFY(pumpUntil(a, b, [&] {
        return b.reader(1)->codec() == "t.v2" && again->maxFrame() > 0;
    }));
    QCOMPARE(again->tryWrite("second life"), WriteResult::Written);
    QVERIFY(pumpUntil(a, b, [&] {
        return b.reader(1)->read(frame) == ReadResult::Frame;
    }));
    QCOMPARE(frame, std::string("second life"));
}

void TestStream::failedReopen_keepsTheOldWriter()
{
    int raw = -1;
    const int fd = socketPair(raw);
    Stream stream(fd);
    StreamWriter* writer = stream.openWriter(1, "t.v1", nullptr);
    const std::string start = hello(ProtocolVersion) + credit(1, 4096);
    ::send(raw, start.data(), start.size(), MSG_NOSIGNAL);
    for (int i = 0; i < 20 && writer->maxFrame() == 0; ++i)
        stream.pump(10);
    QVERIFY(writer->maxFrame() > 0);
    writer->close();
    QVERIFY(stream.flush(1000));
    // The other end read the channel to its end: the id is free.
    const std::string release = packet(ReleaseType, 0, 1, "");
    ::send(raw, release.data(), release.size(), MSG_NOSIGNAL);
    for (int i = 0; i < 5; ++i)
        stream.pump(10);
    QVERIFY2(stream.open(), stream.error().c_str());
    // The other end stops reading, so the Open of the id finds the output
    // dead: nothing opened, and the old writer is still the one there. A
    // freed old writer still reads back in a plain build; the sanitizer
    // build that CI runs is what sees it.
    ::shutdown(raw, SHUT_RD);
    std::string error;
    QVERIFY(!stream.openWriter(1, "t.v2", &error));
    QVERIFY2(error.find("output failed") != std::string::npos, error.c_str());
    QCOMPARE(writer->codec(), std::string("t.v1"));
    QCOMPARE(writer->tryWrite("x"), WriteResult::Closed);
    ::close(raw);
}

void TestStream::maxChannels_countsOpenChannels()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream::Options two;
    two.maxChannels = 2;
    Stream a(fa), b(fb, fb, two);
    // Channels that come and go: a lifetime of ten, two at a time at most.
    for (std::uint32_t id = 10; id < 20; ++id) {
        StreamWriter* writer = nullptr;
        QVERIFY(pumpUntil(a, b, [&] {
            return (writer = a.openWriter(id, "t.v1", nullptr)) != nullptr;
        }));
        QVERIFY(pumpUntil(a, b, [&] {
            return writer->maxFrame() > 0;
        }));
        QCOMPARE(writer->tryWrite("hello"), WriteResult::Written);
        writer->close();
        std::string frame;
        QVERIFY(pumpUntil(a, b, [&] {
            return b.reader(id) && b.reader(id)->read(frame) == ReadResult::Frame;
        }));
        QVERIFY(pumpUntil(a, b, [&] {
            return b.reader(id)->read(frame) == ReadResult::Closed;
        }));
    }
    QVERIFY2(a.open() && b.open(), b.error().c_str());
}

void TestStream::openWriter_refusesMoreChannelsThanThePeerReads()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream::Options two;
    two.maxChannels = 2;
    Stream a(fa), b(fb, fb, two);
    QVERIFY(pumpUntil(a, b, [&] {
        return a.ready();
    }));
    QVERIFY(a.openWriter(0, "t.v1", nullptr));
    QVERIFY(a.openWriter(1, "t.v1", nullptr));
    std::string error;
    QVERIFY(!a.openWriter(2, "t.v1", &error));
    QVERIFY2(error.find("at most 2") != std::string::npos, error.c_str());
    pumpFor(a, b, 5);
    QVERIFY(a.open() && b.open());
}

void TestStream::channelIds_stayBelowThePeersLimit()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream::Options eight;
    eight.maxChannelId = 8;
    Stream a(fa), b(fb, fb, eight);
    QVERIFY(pumpUntil(a, b, [&] {
        return a.ready();
    }));
    std::string error;
    QVERIFY(!a.openWriter(8, "t.v1", &error));
    QVERIFY2(error.find("ids below 8") != std::string::npos, error.c_str());
    QVERIFY(a.openWriter(7, "t.v1", nullptr));
    pumpFor(a, b, 5);
    QVERIFY(a.open() && b.open());
    // Opened before the limit was known: refused once it is, by either end.
    int fd = -1;
    const int fc = socketPair(fd);
    Stream c(fc), d(fd, fd, eight);
    QVERIFY(c.openWriter(100, "t.v1", nullptr));
    pumpFor(c, d, 20);
    QVERIFY(!c.open() && !d.open());
    QVERIFY2((c.error() + d.error()).find("ids below 8") != std::string::npos, (c.error() + " / " + d.error()).c_str());
}

void TestStream::reopenBeforeRelease_isRefused()
{
    int raw = -1;
    const int fd = socketPair(raw);
    Stream::Options wide;
    wide.window = 4 << 20;
    Stream stream(fd, fd, wide);
    StreamWriter* writer = stream.openWriter(5, "t.v1", nullptr);
    const std::string start = hello(ProtocolVersion) + credit(5, 4 << 20);
    ::send(raw, start.data(), start.size(), MSG_NOSIGNAL);
    for (int i = 0; i < 20 && writer->maxFrame() == 0; ++i)
        stream.pump(10);
    QVERIFY(writer->maxFrame() > 0);
    // The other end reads nothing: the output backs up, and what this end
    // queues waits.
    QCOMPARE(writer->tryWrite(std::string(writer->maxFrame(), 'x')), WriteResult::Written);
    QVERIFY(stream.wantsWrite());
    const std::string first = open(1) + packet(CloseType, 0, 1, "");
    ::send(raw, first.data(), first.size(), MSG_NOSIGNAL);
    for (int i = 0; i < 20 && !stream.reader(1); ++i)
        stream.pump(10);
    QVERIFY(stream.reader(1));
    std::string frame;
    QVERIFY(pumpUntil(stream, stream, [&] {
        return stream.reader(1)->read(frame) == ReadResult::Closed;
    }));
    // The Release waits behind the output: the other end cannot have it yet.
    const std::string again = open(1);
    ::send(raw, again.data(), again.size(), MSG_NOSIGNAL);
    for (int i = 0; i < 20 && stream.error().empty(); ++i)
        stream.pump(10);
    QVERIFY2(stream.error().find("again before this end released it") != std::string::npos, stream.error().c_str());
    ::close(raw);
}

void TestStream::endOfInput_closesReaders()
{
    int fb = -1;
    const int fa = socketPair(fb);
    auto a = std::make_unique<Stream>(fa);
    Stream b(fb);
    StreamWriter* writer = a->openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(*a, b, [&] {
        return writer->maxFrame() > 0;
    }));
    QCOMPARE(writer->tryWrite("before the end"), WriteResult::Written);
    QVERIFY(a->flush(2000));
    a.reset();
    std::string frame;
    for (int i = 0; i < 50 && b.open(); ++i)
        b.pump(10);
    QVERIFY(!b.open());
    QVERIFY2(b.error().empty(), b.error().c_str());
    QCOMPARE(b.reader(1)->read(frame), ReadResult::Frame);
    QCOMPARE(frame, std::string("before the end"));
    QCOMPARE(b.reader(1)->read(frame), ReadResult::Closed);
    // Reading after the end sends nothing to an end that is gone.
    QVERIFY2(b.error().empty(), b.error().c_str());
}

void TestStream::endOfInput_reachesOnReadable()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream a(fa);
    Stream b(fb);
    // b reads only in onReadable, as the header allows.
    std::vector<std::string> frames;
    bool sawClosed = false;
    b.onReadable = [&](StreamReader& reader) {
        std::string frame;
        ReadResult result;
        while ((result = reader.read(frame)) == ReadResult::Frame)
            frames.push_back(frame);
        sawClosed = sawClosed || result == ReadResult::Closed;
    };
    StreamWriter* writer = a.openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] {
        return writer->maxFrame() > 0;
    }));
    QCOMPARE(writer->tryWrite("one"), WriteResult::Written);
    QCOMPARE(writer->tryWrite("two"), WriteResult::Written);
    QVERIFY(pumpUntil(a, b, [&] {
        return frames.size() == 2;
    }));
    QVERIFY(!sawClosed);
    // a ends cleanly without closing its channel: only the end of the
    // input tells.
    bool aDone = false;
    std::thread closer([&] {
        aDone = a.shutdown(3000);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (b.open() && std::chrono::steady_clock::now() < deadline)
        b.pump(10);
    const bool bDone = b.shutdown(3000);
    // Joined before any check can return.
    closer.join();
    QVERIFY2(b.error().empty(), b.error().c_str());
    QVERIFY(sawClosed);
    QVERIFY(bDone);
    QVERIFY(aDone);
    QCOMPARE(frames, (std::vector<std::string>{"one", "two"}));
}

void TestStream::failure_reachesOnReadable()
{
    int up[2], down[2];
    QVERIFY(::pipe2(up, O_CLOEXEC) == 0 && ::pipe2(down, O_CLOEXEC) == 0);
    const int intoB = ::dup(up[1]);
    Stream a(down[0], up[1], Stream::Options());
    Stream b(up[0], down[1], Stream::Options());
    ReadResult last = ReadResult::Frame;
    b.onReadable = [&](StreamReader& reader) {
        std::string frame;
        while ((last = reader.read(frame)) == ReadResult::Frame) { }
    };
    StreamWriter* writer = a.openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] {
        return writer->maxFrame() > 0 && !a.wantsWrite();
    }));
    // A broken packet that a never sent: b fails, and a reader that b's owner
    // reads only in onReadable still hears of it.
    const std::string broken = std::string("\x03\x00\x01\x00", 4) + std::string(8, '\0');
    QCOMPARE(::write(intoB, broken.data(), broken.size()), ssize_t(broken.size()));
    for (int i = 0; i < 50 && b.error().empty(); ++i)
        b.pump(10);
    QVERIFY2(b.error().find("malformed") != std::string::npos, b.error().c_str());
    QCOMPARE(last, ReadResult::Corrupt);
    ::close(intoB);
}

void TestStream::shutdown_endsADuplicatedSocket()
{
    // One socket on two fds, as socket activation gives a service its
    // standard input and output.
    int fb = -1;
    const int fa = socketPair(fb);
    Stream a(fa, ::dup(fa), Stream::Options());
    Stream b(fb);
    QVERIFY(pumpUntil(a, b, [&] {
        return a.ready() && b.ready();
    }));
    bool aDone = false;
    std::thread closer([&] {
        aDone = a.shutdown(3000);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (b.open() && std::chrono::steady_clock::now() < deadline)
        b.pump(10);
    const bool bDone = b.shutdown(3000);
    closer.join();
    QVERIFY2(aDone, a.error().c_str());
    QVERIFY2(bDone, b.error().c_str());
}

void TestStream::shutdown_keepsItsTimeoutOnADuplicatedSocket()
{
    int raw = -1;
    const int fd = socketPair(raw);
    Stream a(fd, ::dup(fd), Stream::Options());
    const std::string theirs = hello(ProtocolVersion);
    ::send(raw, theirs.data(), theirs.size(), MSG_NOSIGNAL);
    for (int i = 0; i < 20 && !a.ready(); ++i)
        a.pump(10);
    QVERIFY(a.ready());
    bool done = true;
    std::atomic<bool> finished{false};
    auto took = std::chrono::milliseconds(0);
    std::thread closer([&] {
        const auto start = std::chrono::steady_clock::now();
        done = a.shutdown(300);
        took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        finished = true;
    });
    // a ended its output; the other end still talks, and does not end.
    const timeval limit{2, 0};
    ::setsockopt(raw, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
    char buffer[256];
    while (::recv(raw, buffer, sizeof buffer, 0) > 0) { }
    const std::string more = open(1);
    ::send(raw, more.data(), more.size(), MSG_NOSIGNAL);
    // A shutdown that blocks in a read is freed, so that it fails the check
    // below instead of hanging the binary.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (!finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!finished)
        ::shutdown(raw, SHUT_RDWR);
    closer.join();
    QVERIFY(!done);
    QVERIFY2(took < std::chrono::milliseconds(1500), std::to_string(took.count()).c_str());
    ::close(raw);
}

void TestStream::writersBeforeTheHello_failPastThePeersLimits()
{
    {
        int raw = -1;
        const int fd = socketPair(raw);
        Stream stream(fd);
        QVERIFY(stream.openWriter(100, "t.v1", nullptr));
        const std::string theirs = hello(ProtocolVersion, 64, 8);
        ::send(raw, theirs.data(), theirs.size(), MSG_NOSIGNAL);
        for (int i = 0; i < 20 && stream.error().empty(); ++i)
            stream.pump(10);
        QVERIFY2(stream.error().find("this end opened channel 100") != std::string::npos, stream.error().c_str());
        ::close(raw);
    }
    {
        int raw = -1;
        const int fd = socketPair(raw);
        Stream stream(fd);
        for (std::uint32_t id = 0; id < 3; ++id)
            QVERIFY(stream.openWriter(id, "t.v1", nullptr));
        const std::string theirs = hello(ProtocolVersion, 2);
        ::send(raw, theirs.data(), theirs.size(), MSG_NOSIGNAL);
        for (int i = 0; i < 20 && stream.error().empty(); ++i)
            stream.pump(10);
        QVERIFY2(stream.error().find("this end opened 3 channels") != std::string::npos, stream.error().c_str());
        ::close(raw);
    }
}

void TestStream::callbacks_keepThePacketOrder()
{
    int raw = -1;
    const int fd = socketPair(raw);
    Stream stream(fd);
    std::vector<std::string> seen;
    // The first callback runs the stream again while new packets wait: the
    // owner still hears of a reader before its frames.
    stream.onReader = [&](StreamReader& reader) {
        seen.push_back("reader " + std::to_string(reader.id()));
        if (reader.id() == 1) {
            const std::string data = packet(DataType, 1, 2, "x");
            ::send(raw, data.data(), data.size(), MSG_NOSIGNAL);
            stream.pump(100);
        }
    };
    stream.onReadable = [&](StreamReader& reader) {
        seen.push_back("readable " + std::to_string(reader.id()));
    };
    const std::string bytes = hello(ProtocolVersion) + open(1) + open(2);
    ::send(raw, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    for (int i = 0; i < 20 && seen.size() < 3; ++i)
        stream.pump(10);
    QCOMPARE(seen, (std::vector<std::string>{"reader 1", "reader 2", "readable 2"}));
    ::close(raw);
}

void TestStream::callbacks_mayThrow()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream a(fa), b(fb);
    StreamWriter* first = a.openWriter(1, "t.v1", nullptr);
    StreamWriter* second = a.openWriter(2, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] {
        return first->maxFrame() && second->maxFrame();
    }));
    std::vector<std::uint32_t> heard;
    bool thrown = false;
    b.onReadable = [&](StreamReader& reader) {
        heard.push_back(reader.id());
        if (!thrown) {
            thrown = true;
            throw std::runtime_error("the owner's own failure");
        }
    };
    QCOMPARE(first->tryWrite("one"), WriteResult::Written);
    QCOMPARE(second->tryWrite("two"), WriteResult::Written);
    QVERIFY(a.flush(1000));
    bool caught = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (heard.size() < 2 && std::chrono::steady_clock::now() < deadline) {
        try {
            b.pump(10);
        } catch (const std::runtime_error&) {
            caught = true;
        }
    }
    QVERIFY(caught);
    // The callback after the one that threw still came, with the next call.
    QCOMPARE(heard, (std::vector<std::uint32_t>{1, 2}));
    QVERIFY2(b.open(), b.error().c_str());
}

void TestStream::cleanShutdown_endsBothWithoutError()
{
    int fb = -1;
    const int fa = socketPair(fb);
    auto a = std::make_unique<Stream>(fa);
    Stream b(fb);
    StreamWriter* toB = a->openWriter(1, "t.v1", nullptr);
    StreamWriter* toA = b.openWriter(2, "t.v1", nullptr);
    QVERIFY(pumpUntil(*a, b, [&] {
        return toB->maxFrame() > 0 && toA->maxFrame() > 0;
    }));
    QCOMPARE(toB->tryWrite("last"), WriteResult::Written);
    // a ends its stream and is destroyed at once, as an owner would do.
    // Meanwhile b, which still writes after a's end, sends a late frame: a
    // must read it before it closes, or the connection resets and b reads a
    // failure.
    bool aDone = false;
    std::string aError;
    std::thread closer([&] {
        aDone = a->shutdown(5000);
        aError = a->error();
        a.reset();
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (b.open() && std::chrono::steady_clock::now() < deadline)
        b.pump(10);
    QCOMPARE(toA->tryWrite("late"), WriteResult::Written);
    const bool bDone = b.shutdown(5000);
    closer.join();
    QVERIFY2(aDone, aError.c_str());
    QVERIFY2(bDone, b.error().c_str());
    QVERIFY2(b.error().empty(), b.error().c_str());
    std::string frame;
    QCOMPARE(b.reader(1)->read(frame), ReadResult::Frame);
    QCOMPARE(frame, std::string("last"));
    QCOMPARE(toA->tryWrite("after"), WriteResult::Closed);
}

void TestStream::failure_reachesTheOtherEnd()
{
    // Over two pipes, so that the test can put bytes into a's input.
    int up[2], down[2];
    QVERIFY(::pipe2(up, O_CLOEXEC) == 0 && ::pipe2(down, O_CLOEXEC) == 0);
    const int intoA = ::dup(down[1]);
    Stream a(down[0], up[1], Stream::Options());
    Stream b(up[0], down[1], Stream::Options());
    QVERIFY(pumpUntil(a, b, [&] {
        return a.ready() && b.ready() && !a.wantsWrite() && !b.wantsWrite();
    }));
    // A broken packet that b never sent: a fails, and b, which did nothing
    // wrong, learns why instead of waiting.
    const std::string broken = std::string("\x03\x00\x01\x00", 4) + std::string(8, '\0');
    QCOMPARE(::write(intoA, broken.data(), broken.size()), ssize_t(broken.size()));
    QVERIFY(pumpUntil(a, b, [&] {
        return !b.error().empty();
    }));
    QVERIFY2(a.error().find("malformed packet") != std::string::npos, a.error().c_str());
    QVERIFY2(b.error().find("stopped the stream") != std::string::npos, b.error().c_str());
    QVERIFY2(b.error().find("malformed packet") != std::string::npos, b.error().c_str());
    ::close(intoA);
}

void TestStream::versionRefusal_sendsThisEndsHello()
{
    int raw = -1;
    const int fd = socketPair(raw);
    // The other end's hello is there before this end ever wrote.
    const std::string theirs = hello(ProtocolVersion + 8);
    ::send(raw, theirs.data(), theirs.size(), MSG_NOSIGNAL);
    Stream stream(fd);
    for (int i = 0; i < 5 && stream.open(); ++i)
        stream.pump(10);
    QVERIFY(!stream.open());
    const std::vector<Packet> packets = parse(drainRaw(raw));
    // The refusing end still sends its hello, so the other end names both
    // versions too, then the reason.
    QVERIFY(packets.size() >= 2);
    QCOMPARE(packets.front().type, HelloType);
    QCOMPARE(packets.front().payload.substr(8, 4), [] {
        std::string v;
        put32(v, ProtocolVersion);
        return v;
    }());
    QCOMPARE(packets.back().type, ErrorType);
    QVERIFY(packets.back().payload.find(std::to_string(ProtocolVersion + 8)) != std::string::npos);
    ::close(raw);
}

void TestStream::halfClose_keepsTheOutputGoing()
{
    int raw = -1;
    const int fd = socketPair(raw);
    Stream::Options wide;
    wide.window = 1 << 20;
    Stream stream(fd, fd, wide);
    StreamWriter* writer = stream.openWriter(1, "t.v1", nullptr);
    const std::string bytes = hello(ProtocolVersion) + credit(1, 1 << 20);
    ::send(raw, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    for (int i = 0; i < 20 && writer->maxFrame() == 0; ++i)
        stream.pump(10);
    QVERIFY(writer->maxFrame() > 0);
    QCOMPARE(writer->tryWrite(frameOf(600000, 1)), WriteResult::Written);
    // The other end has said all it will, and still reads.
    ::shutdown(raw, SHUT_WR);
    std::string received;
    char buffer[65536];
    for (int i = 0; i < 500 && received.size() < 600000; ++i) {
        stream.pump(5);
        const ssize_t n = ::recv(raw, buffer, sizeof buffer, MSG_DONTWAIT);
        if (n > 0)
            received.append(buffer, std::size_t(n));
    }
    QVERIFY2(stream.error().empty(), stream.error().c_str());
    QVERIFY(!stream.open());
    QVERIFY(received.size() > 600000);
    QVERIFY(!stream.wantsWrite());
    ::close(raw);
}

void TestStream::callbacks_mayPumpAndDestroyTheStream()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream a(fa);
    auto b = std::make_unique<Stream>(fb);
    StreamWriter* first = a.openWriter(1, "t.v1", nullptr);
    StreamWriter* second = a.openWriter(2, "t.v1", nullptr);
    QVERIFY(first && second);
    int opened = 0;
    // A callback that runs the stream again, with new bytes waiting, sees
    // each packet once.
    b->onReader = [&](StreamReader&) {
        if (++opened == 1) {
            a.openWriter(3, "t.v1", nullptr);
            a.flush(1000);
        }
        b->pump(0);
    };
    QVERIFY(pumpUntil(a, *b, [&] {
        return opened == 3 && first->maxFrame() && second->maxFrame();
    }));
    QVERIFY2(b->open(), b->error().c_str());
    // A callback that destroys the stream ends the call that ran it.
    b->onReadable = [&](StreamReader&) {
        b.reset();
    };
    QCOMPARE(first->tryWrite("one"), WriteResult::Written);
    QCOMPARE(second->tryWrite("two"), WriteResult::Written);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (b && std::chrono::steady_clock::now() < deadline) {
        a.pump(5);
        if (b)
            b->pump(5);
    }
    QVERIFY(!b);
}

void TestStream::write_drainsReadersThroughOnReadable()
{
    // Two ends that both wait in write(), with windows smaller than what
    // each sends: only reads in onReadable return the windows.
    int fb = -1;
    const int fa = socketPair(fb);
    Stream::Options small;
    small.window = 64 * 1024;
    Stream a(fa, fa, small), b(fb, fb, small);
    StreamWriter* toB = a.openWriter(1, "t.v1", nullptr);
    StreamWriter* toA = b.openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] {
        return toA->maxFrame() && toB->maxFrame();
    }));
    std::atomic<int> atA{0}, atB{0};
    a.onReadable = [&](StreamReader& reader) {
        std::string frame;
        while (reader.read(frame) == ReadResult::Frame)
            ++atA;
    };
    b.onReadable = [&](StreamReader& reader) {
        std::string frame;
        while (reader.read(frame) == ReadResult::Frame)
            ++atB;
    };
    std::atomic<bool> aWrote{true};
    std::thread other([&] {
        for (int i = 0; i < 10; ++i)
            aWrote = aWrote && toB->write(frameOf(40 * 1024, i), 5000) == WriteResult::Written;
        a.flush(5000);
    });
    bool bWrote = true;
    for (int i = 0; i < 10; ++i)
        bWrote = bWrote && toA->write(frameOf(40 * 1024, i), 5000) == WriteResult::Written;
    b.flush(5000);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (atB < 10 && std::chrono::steady_clock::now() < deadline)
        b.pump(5);
    other.join();
    while (atA < 10 && std::chrono::steady_clock::now() < deadline)
        a.pump(5);
    QVERIFY(aWrote);
    QVERIFY(bWrote);
    QCOMPARE(atA.load(), 10);
    QCOMPARE(atB.load(), 10);
}

void TestStream::write_timesOutAndClosesWhenThePeerGoes()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream::Options small;
    small.window = 4096;
    Stream a(fa);
    auto b = std::make_unique<Stream>(fb, fb, small);
    StreamWriter* writer = a.openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, *b, [&] {
        return writer->maxFrame() > 0;
    }));
    QCOMPARE(writer->tryWrite(std::string(writer->maxFrame(), 'x')), WriteResult::Written);
    // b reads nothing: no window comes back.
    const auto start = std::chrono::steady_clock::now();
    QCOMPARE(writer->write("more", 100), WriteResult::WouldBlock);
    QVERIFY(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(90));
    b.reset();
    QCOMPARE(writer->write("more", 5000), WriteResult::Closed);
    QVERIFY(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
}

void TestStream::bothEndsFlushAtOnce()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream::Options wide;
    wide.window = 8 << 20;
    Stream a(fa, fa, wide), b(fb, fb, wide);
    StreamWriter* toB = a.openWriter(1, "t.v1", nullptr);
    StreamWriter* toA = b.openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] {
        return toA->maxFrame() && toB->maxFrame();
    }));
    // Far more than the kernel holds, queued on both ends.
    QCOMPARE(toB->tryWrite(frameOf(3 << 20, 1)), WriteResult::Written);
    QCOMPARE(toA->tryWrite(frameOf(3 << 20, 2)), WriteResult::Written);
    // Each end keeps running its stream after its own flush, as an owner
    // would, so that the other end's output still drains.
    std::atomic<bool> aDone{false}, bDone{false};
    const auto flushThenServe = [](Stream& stream, std::atomic<bool>& done, const std::atomic<bool>& otherDone) {
        const bool flushed = stream.flush(5000);
        done = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!otherDone && std::chrono::steady_clock::now() < deadline)
            stream.pump(5);
        return flushed;
    };
    bool aFlushed = false;
    std::thread other([&] {
        aFlushed = flushThenServe(a, aDone, bDone);
    });
    const bool bFlushed = flushThenServe(b, bDone, aDone);
    other.join();
    QVERIFY(aFlushed);
    QVERIFY(bFlushed);
}

void TestStream::flushZero_writesWhatFits()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream a(fa);
    QVERIFY(a.wantsWrite());
    // The hello waits: flush(0) writes it without waiting.
    QVERIFY(a.flush(0));
    QVERIFY(!a.wantsWrite());
    char byte = 0;
    QCOMPARE(::recv(fb, &byte, 1, MSG_DONTWAIT), ssize_t(1));
    ::close(fb);
}

void TestStream::failedStream_wantsNoWriteAndReadsNoMore()
{
    int raw = -1;
    const int fd = socketPair(raw);
    Stream stream(fd);
    int opened = 0;
    stream.onReader = [&](StreamReader&) {
        ++opened;
    };
    // The other end opens three channels and stops reading: the credit for
    // them cannot go out, and the stream fails in the call that reads them.
    const std::string bytes = hello(ProtocolVersion) + open(1) + open(2) + open(3);
    ::shutdown(raw, SHUT_RD);
    ::send(raw, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    pollfd p{fd, POLLIN, 0};
    QCOMPARE(::poll(&p, 1, 2000), 1);
    const bool kept = stream.readable();
    QVERIFY(!stream.error().empty());
    QVERIFY(!kept);
    QVERIFY(!stream.readable());
    QVERIFY(!stream.wantsWrite());
    // The channels opened before the failure were still announced.
    QCOMPARE(opened, 3);
    ::close(raw);
}

void TestStream::closedFd_failsInsteadOfSpinning()
{
    int up[2], down[2];
    QVERIFY(::pipe2(up, O_CLOEXEC) == 0 && ::pipe2(down, O_CLOEXEC) == 0);
    Stream a(down[0], up[1], Stream::Options());
    Stream b(up[0], down[1], Stream::Options());
    StreamWriter* writer = a.openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] {
        return b.reader(1) && writer->maxFrame() > 0;
    }));
    ReadResult last = ReadResult::Frame;
    b.onReadable = [&](StreamReader& reader) {
        std::string frame;
        while ((last = reader.read(frame)) == ReadResult::Frame) { }
    };
    // The owner closed b's input behind its back: b fails instead of
    // spinning, and its readers hear of it in the same call.
    ::close(up[0]);
    const bool pumped = b.pump(1000);
    // The number gets a file again, so that b's destructor closes that and not
    // a number that something else reused.
    const int null = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    ::dup2(null, up[0]);
    ::close(null);
    QVERIFY(!pumped);
    QVERIFY2(b.error().find("not open") != std::string::npos, b.error().c_str());
    QCOMPARE(last, ReadResult::Closed);
}

void TestStream::refusesFdsThatCannotCarryAStream()
{
    {
        Stream stream(-1, -1, Stream::Options());
        QVERIFY(!stream.open());
    }
    {
        // A socket of records cuts every packet longer than one read.
        int fds[2];
        QVERIFY(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) == 0);
        Stream stream(fds[0]);
        QVERIFY2(stream.error().find("byte-stream socket") != std::string::npos, stream.error().c_str());
        ::close(fds[1]);
    }
    {
        // ssh -t: a terminal rewrites the bytes.
        const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        QVERIFY(master >= 0);
        QVERIFY(::grantpt(master) == 0 && ::unlockpt(master) == 0);
        const int terminal = ::open(::ptsname(master), O_RDWR | O_NOCTTY | O_CLOEXEC);
        QVERIFY(terminal >= 0);
        Stream stream(terminal);
        QVERIFY2(stream.error().find("terminal") != std::string::npos, stream.error().c_str());
        ::close(master);
    }
}

void TestStream::destruction_restoresFlagsAndKeepsStdioTaken()
{
    int fds[2];
    QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    const int other = ::dup(fds[0]);
    {
        Stream stream(fds[0]);
        QVERIFY(::fcntl(other, F_GETFL) & O_NONBLOCK);
    }
    // The description is shared: what the stream changed is undone.
    QVERIFY(!(::fcntl(other, F_GETFL) & O_NONBLOCK));
    ::close(other);
    // A stream on standard input, as the far end of ssh runs one: the fd
    // stays taken, so nothing opened later lands on it.
    // Standard input comes back, and the fds go, however the test ends.
    struct RestoreStdin
    {
        int saved;
        int socket;
        ~RestoreStdin()
        {
            ::dup2(saved, 0);
            ::close(saved);
            ::close(socket);
        }
    } restore{::dup(0), fds[1]};
    QVERIFY(::dup2(fds[1], 0) == 0);
    {
        Stream stream(0, 0, Stream::Options());
    }
    char target[64] = {};
    QVERIFY(::readlink("/proc/self/fd/0", target, sizeof target - 1) > 0);
    QCOMPARE(std::string(target), std::string("/dev/null"));
}

void TestStream::refusesAnotherVersionAndNamesBoth()
{
    const std::string error = errorFor(hello(ProtocolVersion + 8));
    QVERIFY2(error.find(std::to_string(ProtocolVersion + 8)) != std::string::npos, error.c_str());
    QVERIFY2(error.find(std::to_string(ProtocolVersion)) != std::string::npos, error.c_str());
    // A later version may say more in its hello: still named, not malformed.
    std::string longer = "PHCHSTRM";
    put32(longer, ProtocolVersion + 1);
    put32(longer, 64);
    longer += "capabilities";
    const std::string later = errorFor(packet(HelloType, 0, 0, longer));
    QVERIFY2(later.find(std::to_string(ProtocolVersion + 1)) != std::string::npos, later.c_str());
}

void TestStream::refusesAPeerThatBreaksTheRules_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<QByteArray>("expected");
    QTest::addColumn<bool>("writesChannel5");
    QTest::addColumn<bool>("endInput");
    const auto row = [](const char* name, const std::string& bytes, const char* expected, bool writes = false,
                        bool end = false) {
        QTest::newRow(name) << QByteArray(bytes.data(), qsizetype(bytes.size())) << QByteArray(expected) << writes
                            << end;
    };
    const std::string h = hello(ProtocolVersion);
    std::string tooLong{char(DataType), 0, 0, 0};
    put32(tooLong, 1);
    put32(tooLong, MaxPayload + 1);
    row("no hello", open(1), "did not begin with a phosphor-channel hello");
    row("a login banner", "hi\n", "It sent \"hi?\"");
    row("a second hello", h + h, "second hello");
    row("a short hello", packet(HelloType, 0, 0, std::string("PHCHSTRM\x01\x00\x00\x00", 12)), "too short");
    row("data on no channel", h + packet(DataType, 1, 9, "data"), "data on channel 9, which is not open");
    row("an open twice", h + open(1) + open(1), "opened channel 1 twice");
    row("a bad codec", h + open(1, "has space"), "bad codec tag");
    row("too many channels", h + open(1) + open(2) + open(3), "more than the 2 channels");
    row("an id past the limit", h + open(16), "reads channel ids below 16");
    row("past the window", h + open(1) + packet(DataType, 0, 1, std::string(4097, 'x')), "past the window");
    row("credit on no writer", h + credit(5, 8), "which this end does not write");
    row("an empty window", h + credit(5, 0), "empty window", true);
    row("credit past the window", h + credit(5, 100) + credit(5, 1), "more than the window", true);
    row("a malformed credit", h + packet(CreditType, 0, 5, "short"), "malformed credit", true);
    row("an unknown type", h + packet(77, 0, 0, ""), "unknown type 77");
    row("unknown flags", h + open(1) + packet(DataType, 2, 1, "x"), "unknown flags");
    row("nonzero reserved bytes", h + std::string("\x03\x00\x01\x00", 4) + std::string(8, '\0'), "malformed packet");
    row("a payload past the limit", h + tooLong, "malformed packet");
    row("a close of no channel", h + packet(CloseType, 0, 4, ""), "closed channel 4, which is not open");
    row("a close twice", h + open(1) + packet(CloseType, 0, 1, "") + packet(CloseType, 0, 1, ""),
        "closed channel 1 twice");
    row("a close within a frame", h + open(1) + packet(DataType, 0, 1, "part") + packet(CloseType, 0, 1, ""),
        "within a frame");
    row("data after a close", h + open(1) + packet(CloseType, 0, 1, "") + packet(DataType, 1, 1, "x"),
        "which is not open");
    row("a release of no closed channel", h + packet(ReleaseType, 0, 5, ""), "did not close", true);
    row("an end before the hello", "", "before the other end's hello", false, true);
    row("an end within a packet", h + std::string(5, '\0'), "within a packet", false, true);
    row("an end within a frame", h + open(1) + packet(DataType, 0, 1, "part"), "within a frame on channel 1", false,
        true);
    row("the other end's failure", h + packet(ErrorType, 0, 0, "it broke"), "saying: \"it broke\"");
}

void TestStream::refusesAPeerThatBreaksTheRules()
{
    QFETCH(QByteArray, bytes);
    QFETCH(QByteArray, expected);
    QFETCH(bool, writesChannel5);
    QFETCH(bool, endInput);
    const auto setup = [&](Stream& stream) {
        if (writesChannel5)
            stream.openWriter(5, "t.v1", nullptr);
    };
    const std::string error = errorFor(bytes.toStdString(), setup, endInput);
    QVERIFY2(error.find(expected.toStdString()) != std::string::npos, error.c_str());
}

QTEST_GUILESS_MAIN(TestStream)
#include "test_stream.moc"
