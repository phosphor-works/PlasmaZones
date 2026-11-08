// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Stream.h>

#include <QTest>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace PhosphorChannel;

namespace {

int socketPair(int& other)
{
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) return -1;
    other = fds[1];
    return fds[0];
}

/// Runs both streams until @p done, up to five seconds.
bool pumpUntil(Stream& a, Stream& b, const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        a.pump(5);
        b.pump(5);
    }
    return done();
}

std::string frameOf(std::size_t size, int seed)
{
    std::string frame(size, '\0');
    for (std::size_t i = 0; i < size; ++i) frame[i] = char((seed * 31 + int(i)) & 0xff);
    return frame;
}

// The packets of the stream, as the library writes them, for a peer that
// breaks the rules.
void put32(std::string& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back(char((v >> (8 * i)) & 0xff));
}

std::string packet(std::uint8_t type, std::uint8_t flags, std::uint32_t id, const std::string& payload)
{
    std::string out{char(type), char(flags), 0, 0};
    put32(out, id);
    put32(out, std::uint32_t(payload.size()));
    return out + payload;
}

std::string hello(std::uint32_t version)
{
    std::string payload = "PHCHSTRM";
    put32(payload, version);
    return packet(1, 0, 0, payload);
}

/// Runs both streams for @p rounds rounds.
void pumpFor(Stream& a, Stream& b, int rounds)
{
    for (int i = 0; i < rounds; ++i) {
        a.pump(5);
        b.pump(5);
    }
}

/// Feeds @p bytes to a fresh stream with a window of 4096 bytes, and returns
/// its error.
std::string errorFor(const std::string& bytes)
{
    int raw = -1;
    const int fd = socketPair(raw);
    Stream::Options small;
    small.window = 4096;
    Stream stream(fd, fd, small);
    ::send(raw, bytes.data(), bytes.size(), 0);
    for (int i = 0; i < 50 && stream.open(); ++i) stream.pump(10);
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
    void smallFrameOvertakesALargeOne();
    void worksOverTwoPipes();
    void close_deliversFramesThenClosed();
    void endOfInput_closesReaders();
    void refusesAnotherVersionAndNamesBoth();
    void refusesAPeerThatBreaksTheRules();
};

void TestStream::framesArriveInOrder()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream a(fa), b(fb);
    std::vector<std::string> opened;
    b.onReader = [&](StreamReader& reader) { opened.push_back(reader.codec()); };
    StreamWriter* writer = a.openWriter(1, "t.v1", nullptr);
    QVERIFY(writer);
    QVERIFY(pumpUntil(a, b, [&] { return b.reader(1) && writer->maxFrame() > 0; }));
    QVERIFY(a.ready() && b.ready());
    QCOMPARE(opened, std::vector<std::string>{"t.v1"});
    for (int i = 0; i < 100; ++i) QCOMPARE(writer->tryWrite(frameOf(std::size_t(i * 97), i)), WriteResult::Written);
    int received = 0;
    std::string frame;
    QVERIFY(pumpUntil(a, b, [&] {
        while (b.reader(1)->read(frame) == ReadResult::Frame) {
            if (frame != frameOf(std::size_t(received * 97), received)) return true;
            ++received;
        }
        return received == 100;
    }));
    QCOMPARE(received, 100);
}

void TestStream::bothDirectionsUseTheSameId()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream a(fa), b(fb);
    StreamWriter* toB = a.openWriter(7, "a.v1", nullptr);
    StreamWriter* toA = b.openWriter(7, "b.v1", nullptr);
    QVERIFY(toA && toB);
    QVERIFY(!a.openWriter(7, "a.v1", nullptr));
    QVERIFY(pumpUntil(a, b, [&] { return toA->maxFrame() && toB->maxFrame(); }));
    QCOMPARE(toB->tryWrite("for b"), WriteResult::Written);
    QCOMPARE(toA->tryWrite("for a"), WriteResult::Written);
    std::string atA, atB;
    QVERIFY(pumpUntil(a, b, [&] {
        if (atA.empty()) a.reader(7)->read(atA);
        if (atB.empty()) b.reader(7)->read(atB);
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
    QVERIFY(pumpUntil(a, b, [&] { return writer->maxFrame() == small.window; }));
    QCOMPARE(wakes, 1);
    QCOMPARE(writer->tryWrite(frameOf(40 * 1024, 1)), WriteResult::Written);
    QCOMPARE(writer->tryWrite(frameOf(40 * 1024, 2)), WriteResult::WouldBlock);
    // The reader does not read: the window stays full, however long it runs.
    pumpFor(a, b, 20);
    QCOMPARE(writer->tryWrite(frameOf(40 * 1024, 2)), WriteResult::WouldBlock);
    std::string frame;
    QCOMPARE(b.reader(1)->read(frame), ReadResult::Frame);
    QVERIFY(frame == frameOf(40 * 1024, 1));
    QVERIFY(pumpUntil(a, b, [&] { return wakes == 2; }));
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
    QVERIFY(pumpUntil(a, b, [&] { return writer->maxFrame() > 0; }));
    QCOMPARE(writer->tryWrite(std::string(4097, 'x')), WriteResult::TooLarge);
    QCOMPARE(writer->tryWrite(std::string(4096, 'x')), WriteResult::Written);
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
    QVERIFY(pumpUntil(a, b, [&] { return state->maxFrame() && input->maxFrame(); }));
    QCOMPARE(state->tryWrite(frameOf(6 << 20, 3)), WriteResult::Written);
    QCOMPARE(input->tryWrite("key"), WriteResult::Written);
    std::string frame;
    bool keyFirst = false;
    QVERIFY(pumpUntil(a, b, [&] {
        if (b.reader(2) && b.reader(2)->read(frame) == ReadResult::Frame) {
            keyFirst = b.reader(1)->read(frame) == ReadResult::Empty;
            return true;
        }
        return false;
    }));
    QVERIFY(keyFirst);
    QVERIFY(pumpUntil(a, b, [&] { return b.reader(1)->read(frame) == ReadResult::Frame; }));
    QVERIFY(frame == frameOf(6 << 20, 3));
}

void TestStream::worksOverTwoPipes()
{
    // As ssh carries it: one pipe each way, no socket.
    int up[2], down[2];
    QVERIFY(::pipe2(up, O_CLOEXEC) == 0 && ::pipe2(down, O_CLOEXEC) == 0);
    Stream a(down[0], up[1], Stream::Options());
    Stream b(up[0], down[1], Stream::Options());
    StreamWriter* writer = a.openWriter(3, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] { return writer->maxFrame() > 0; }));
    QCOMPARE(writer->tryWrite(frameOf(200000, 9)), WriteResult::Written);
    std::string frame;
    QVERIFY(pumpUntil(a, b, [&] { return b.reader(3)->read(frame) == ReadResult::Frame; }));
    QVERIFY(frame == frameOf(200000, 9));
}

void TestStream::close_deliversFramesThenClosed()
{
    int fb = -1;
    const int fa = socketPair(fb);
    Stream a(fa), b(fb);
    StreamWriter* writer = a.openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(a, b, [&] { return writer->maxFrame() > 0; }));
    QCOMPARE(writer->tryWrite("last"), WriteResult::Written);
    writer->close();
    QCOMPARE(writer->tryWrite("after"), WriteResult::Closed);
    std::string frame;
    QVERIFY(pumpUntil(a, b, [&] { return b.reader(1)->read(frame) == ReadResult::Frame; }));
    QCOMPARE(frame, std::string("last"));
    QVERIFY(pumpUntil(a, b, [&] { return b.reader(1)->read(frame) == ReadResult::Closed; }));
    // The stream itself still works.
    QVERIFY(a.open() && b.open());
}

void TestStream::endOfInput_closesReaders()
{
    int fb = -1;
    const int fa = socketPair(fb);
    auto a = std::make_unique<Stream>(fa);
    Stream b(fb);
    StreamWriter* writer = a->openWriter(1, "t.v1", nullptr);
    QVERIFY(pumpUntil(*a, b, [&] { return writer->maxFrame() > 0; }));
    QCOMPARE(writer->tryWrite("before the end"), WriteResult::Written);
    QVERIFY(a->flush(2000));
    a.reset();
    std::string frame;
    for (int i = 0; i < 50 && b.open(); ++i) b.pump(10);
    QVERIFY(!b.open());
    QVERIFY2(b.error().empty(), b.error().c_str());
    QCOMPARE(b.reader(1)->read(frame), ReadResult::Frame);
    QCOMPARE(frame, std::string("before the end"));
    QCOMPARE(b.reader(1)->read(frame), ReadResult::Closed);
}

void TestStream::refusesAnotherVersionAndNamesBoth()
{
    const std::string error = errorFor(hello(ProtocolVersion + 8));
    QVERIFY2(error.find(std::to_string(ProtocolVersion + 8)) != std::string::npos, error.c_str());
    QVERIFY2(error.find(std::to_string(ProtocolVersion)) != std::string::npos, error.c_str());
}

void TestStream::refusesAPeerThatBreaksTheRules()
{
    const std::string open = packet(2, 0, 1, "t.v1");
    const std::string credit8 = std::string("\x00\x00\x00\x00\x00\x00\x00\x01", 8);
    const std::pair<const char*, std::string> cases[] = {
        {"hello", packet(2, 0, 1, "t.v1")},
        {"not open", hello(ProtocolVersion) + packet(3, 1, 9, "data")},
        {"twice", hello(ProtocolVersion) + open + open},
        {"window", hello(ProtocolVersion) + open + packet(3, 0, 1, std::string(4097, 'x'))},
        {"credit", hello(ProtocolVersion) + packet(4, 0, 5, credit8)},
        {"unknown type", hello(ProtocolVersion) + packet(77, 0, 0, "")},
        {"malformed", hello(ProtocolVersion) + std::string("\x03\x00\x01\x00", 4) + std::string(8, '\0')},
        {"within a frame", hello(ProtocolVersion) + open + packet(3, 0, 1, "part") + packet(5, 0, 1, "")},
    };
    for (const auto& [expected, bytes] : cases) {
        const std::string error = errorFor(bytes);
        QVERIFY2(error.find(expected) != std::string::npos, (std::string(expected) + ": " + error).c_str());
    }
}

QTEST_GUILESS_MAIN(TestStream)
#include "test_stream.moc"
