// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Handshake.h>
#include <PhosphorChannel/Ring.h>

#include <QTest>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace PhosphorChannel;

namespace {

// The header layout offsets that the tests use, as the ring lays them out.
constexpr std::size_t HeaderBytes = 512;
constexpr std::size_t MagicAt = 0;
constexpr std::size_t VersionAt = 4;
constexpr std::size_t CapacityAt = 8;
constexpr std::size_t WritePosAt = 64;
constexpr std::size_t ReadPosAt = 128;
constexpr std::size_t ReaderClosedAt = 324;
constexpr std::uint32_t RingMagic = 0x52434850;
constexpr std::uint32_t FrameRecord = 1;
constexpr std::uint32_t SkipRecord = 2;

struct Ring
{
    std::unique_ptr<RingWriter> writer;
    std::unique_ptr<RingReader> reader;
};

/// A writer and the reader that a handshake gave for it, in this process.
Ring connected(std::size_t capacity, std::string codec = "t.v1")
{
    Ring ring;
    ring.writer = RingWriter::create(capacity, std::move(codec), nullptr);
    if (!ring.writer)
        return ring;
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
        return ring;
    std::thread offer([&] {
        offerRing(pair[0], *ring.writer, 5000, nullptr);
    });
    ring.reader = acceptRing(pair[1], AcceptOptions(), 5000, nullptr);
    offer.join();
    ::close(pair[0]);
    ::close(pair[1]);
    return ring;
}

/// A ring that the test writes by hand, as a writer that breaks the rules
/// would: the memfd, its mapping, and the writer's end of a wake pair.
struct RawRing
{
    int memoryFd = -1;
    int wake[2] = {-1, -1};
    char* map = nullptr;
    std::size_t capacity = 0;

    RawRing(std::size_t dataBytes, bool sealed = true, std::size_t fileBytes = 0)
    {
        capacity = dataBytes;
        const std::size_t size = fileBytes ? fileBytes : HeaderBytes + dataBytes;
        memoryFd = ::memfd_create("raw-ring", MFD_CLOEXEC | (sealed ? MFD_ALLOW_SEALING : 0));
        if (memoryFd < 0 || ::ftruncate(memoryFd, off_t(size)) != 0)
            return;
        if (sealed)
            ::fcntl(memoryFd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
        map = static_cast<char*>(::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, memoryFd, 0));
        if (map == MAP_FAILED)
            map = nullptr;
        ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wake);
        if (!map)
            return;
        put32(MagicAt, RingMagic);
        put32(VersionAt, ProtocolVersion);
        put64(CapacityAt, capacity);
        mapped = size;
    }
    ~RawRing()
    {
        if (map)
            ::munmap(map, mapped);
        for (const int fd : {memoryFd, wake[0], wake[1]}) {
            if (fd >= 0)
                ::close(fd);
        }
    }
    RawRing(const RawRing&) = delete;
    RawRing& operator=(const RawRing&) = delete;

    void put32(std::size_t at, std::uint32_t v)
    {
        std::memcpy(map + at, &v, sizeof v);
    }
    void put64(std::size_t at, std::uint64_t v)
    {
        std::memcpy(map + at, &v, sizeof v);
    }
    std::uint32_t get32(std::size_t at) const
    {
        std::uint32_t v = 0;
        std::memcpy(&v, map + at, sizeof v);
        return v;
    }
    /// A record at @p offset of the data area.
    void record(std::size_t offset, std::uint32_t length, std::uint32_t kind)
    {
        put32(HeaderBytes + offset, length);
        put32(HeaderBytes + offset + 4, kind);
    }

    /// A reader of it. The reader takes a copy of the memfd and the
    /// reader's end of the wake pair.
    std::unique_ptr<RingReader> attach(std::string* error, std::size_t maxCapacity = RingWriter::MaxCapacity)
    {
        const int peer = wake[1];
        wake[1] = -1;
        return RingReader::attach(::dup(memoryFd), peer, "t.v1", maxCapacity, error);
    }

    std::size_t mapped = 0;
};

bool readableNow(int fd)
{
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, 0) == 1;
}

std::string frameOf(std::size_t size, int seed)
{
    std::string frame(size, '\0');
    for (std::size_t i = 0; i < size; ++i)
        frame[i] = char((seed * 131 + int(i)) & 0xff);
    return frame;
}

/// The reader's frames until Empty, Closed or Corrupt, waiting on its fd up
/// to five seconds whenever it is Empty.
ReadResult drainUntilEnd(RingReader& reader, std::vector<std::string>& frames)
{
    std::string frame;
    while (true) {
        const ReadResult result = reader.read(frame);
        if (result == ReadResult::Frame) {
            frames.push_back(frame);
            continue;
        }
        if (result != ReadResult::Empty)
            return result;
        pollfd p{reader.dataFd(), POLLIN, 0};
        if (::poll(&p, 1, 5000) != 1)
            return ReadResult::Empty;
        reader.clearDataSignal();
    }
}

} // namespace

class TestRing : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void create_refusesBadCodecAndCapacity();
    void framesArriveInOrder();
    void wrapUsesSkipRecords();
    void largestFrameFitsWhereverTheWrapFalls_data();
    void largestFrameFitsWhereverTheWrapFalls();
    void fullRing_wouldBlock_thenSpaceSignal();
    void spaceSignal_waitsForTheSpaceAsked();
    void dataSignal_onlyAfterEmpty();
    void close_deliversWaitingFramesThenClosed();
    void readerClose_makesWritesAndReadsClosed();
    void write_timesOut();
    void write_wokenByReaderClose();
    void readerExit_endsAWaitingWrite();
    void writerExit_endsReadsAfterTheLastFrame();
    void failedOffer_stillSeesTheReaderExit();
    void forkedChild_leavesTheRingAlone();
    void writer_closesOnABrokenReadPosition();
    void attach_refusesWhatIsNotARing_data();
    void attach_refusesWhatIsNotARing();
    void attach_leavesRefusedMemoryAlone();
    void read_reportsCorruptRings_data();
    void read_reportsCorruptRings();
    void read_staysCorrupt();
    void twoThreads_keepOrderUnderBackpressure();
};

void TestRing::create_refusesBadCodecAndCapacity()
{
    std::string error;
    QVERIFY(!RingWriter::create(1 << 16, "", &error));
    QVERIFY(!RingWriter::create(1 << 16, "has space", &error));
    QVERIFY(!RingWriter::create(1 << 16, std::string(MaxCodecBytes + 1, 'a'), &error));
    QVERIFY(error.find(std::to_string(MaxCodecBytes)) != std::string::npos);
    QVERIFY(!RingWriter::create(RingWriter::MinCapacity - 1, "t.v1", &error));
    QVERIFY(error.find("capacity") != std::string::npos);
    QVERIFY(!RingWriter::create(RingWriter::MaxCapacity + 1, "t.v1", &error));
    QVERIFY(error.find("capacity") != std::string::npos);
    auto ring = RingWriter::create(RingWriter::MinCapacity + 1, "t.v1", &error);
    QVERIFY(ring);
    QCOMPARE(ring->codec(), std::string("t.v1"));
    QCOMPARE(ring->capacity(), RingWriter::MinCapacity + 8);
}

void TestRing::framesArriveInOrder()
{
    Ring ring = connected(1 << 16);
    QVERIFY(ring.writer && ring.reader);
    QCOMPARE(ring.reader->codec(), std::string("t.v1"));
    std::string frame;
    QCOMPARE(ring.reader->read(frame), ReadResult::Empty);
    for (const char* text : {"one", "", "three"})
        QCOMPARE(ring.writer->tryWrite(text), WriteResult::Written);
    for (const char* text : {"one", "", "three"}) {
        QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
        QCOMPARE(frame, std::string(text));
    }
    QCOMPARE(ring.reader->read(frame), ReadResult::Empty);
}

void TestRing::wrapUsesSkipRecords()
{
    // Sizes that do not divide the ring: the wrap falls at every offset.
    Ring ring = connected(RingWriter::MinCapacity);
    QVERIFY(ring.writer && ring.reader);
    std::string frame;
    for (int i = 0; i < 4000; ++i) {
        const std::string sent = frameOf(std::size_t(1 + (i * 37) % 900), i);
        QCOMPARE(ring.writer->tryWrite(sent), WriteResult::Written);
        QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
        QVERIFY(frame == sent);
    }
}

void TestRing::largestFrameFitsWhereverTheWrapFalls_data()
{
    QTest::addColumn<int>("capacity");
    QTest::newRow("a multiple of 16") << int(RingWriter::MinCapacity);
    // Half of it is not a multiple of 8, so the record rounds past half.
    QTest::newRow("8 past a multiple of 16") << int(RingWriter::MinCapacity + 8);
}

void TestRing::largestFrameFitsWhereverTheWrapFalls()
{
    QFETCH(int, capacity);
    Ring ring = connected(std::size_t(capacity));
    QVERIFY(ring.writer && ring.reader);
    QCOMPARE(ring.writer->tryWrite(std::string(ring.writer->maxFrame() + 1, 'x')), WriteResult::TooLarge);
    std::string frame;
    // Each pass moves the positions on by a different amount, so the largest
    // frame meets the wrap at many offsets.
    for (std::size_t shift = 0; shift < ring.writer->maxFrame(); shift += 8 * 13) {
        QCOMPARE(ring.writer->tryWrite(std::string(shift, 's')), WriteResult::Written);
        QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
        // The ring is empty again, with its positions somewhere new.
        const std::string largest = frameOf(ring.writer->maxFrame(), int(shift));
        QCOMPARE(ring.writer->tryWrite(largest), WriteResult::Written);
        QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
        QVERIFY(frame == largest);
    }
}

void TestRing::fullRing_wouldBlock_thenSpaceSignal()
{
    Ring ring = connected(RingWriter::MinCapacity);
    QVERIFY(ring.writer && ring.reader);
    int written = 0;
    while (ring.writer->tryWrite(frameOf(500, written)) == WriteResult::Written)
        ++written;
    QVERIFY(written > 0);
    QCOMPARE(ring.writer->tryWrite(frameOf(500, written)), WriteResult::WouldBlock);
    QVERIFY(!readableNow(ring.writer->spaceFd()));
    std::string frame;
    QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
    QVERIFY(frame == frameOf(500, 0));
    // The writer asked for the space of one frame: the read freed it.
    QVERIFY(readableNow(ring.writer->spaceFd()));
    ring.writer->clearSpaceSignal();
    QVERIFY(!readableNow(ring.writer->spaceFd()));
    QCOMPARE(ring.writer->tryWrite(frameOf(500, written)), WriteResult::Written);
    // Nothing was lost or reordered by the refusal.
    for (int i = 1; i <= written; ++i) {
        QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
        QVERIFY(frame == frameOf(500, i));
    }
}

void TestRing::spaceSignal_waitsForTheSpaceAsked()
{
    Ring ring = connected(1 << 16);
    QVERIFY(ring.writer && ring.reader);
    int small = 0;
    while (ring.writer->tryWrite("x") == WriteResult::Written)
        ++small;
    const std::string large(ring.writer->maxFrame(), 'L');
    QCOMPARE(ring.writer->tryWrite(large), WriteResult::WouldBlock);
    std::string frame;
    // One small frame frees far less than the large one needs: no signal.
    QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
    QVERIFY(!readableNow(ring.writer->spaceFd()));
    int read = 1;
    while (!readableNow(ring.writer->spaceFd())) {
        QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
        ++read;
    }
    // Signalled once the space was free, before the ring was empty.
    QVERIFY(read < small);
    ring.writer->clearSpaceSignal();
    QCOMPARE(ring.writer->tryWrite(large), WriteResult::Written);
}

void TestRing::dataSignal_onlyAfterEmpty()
{
    Ring ring = connected(1 << 16);
    QVERIFY(ring.writer && ring.reader);
    // The reader has not looked yet: no signal for a busy reader.
    QCOMPARE(ring.writer->tryWrite("a"), WriteResult::Written);
    QVERIFY(!readableNow(ring.reader->dataFd()));
    std::string frame;
    QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
    // A read that found a frame asks for nothing.
    QCOMPARE(ring.writer->tryWrite("b"), WriteResult::Written);
    QVERIFY(!readableNow(ring.reader->dataFd()));
    QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
    QCOMPARE(ring.reader->read(frame), ReadResult::Empty);
    // Empty asked: the next write wakes it.
    QCOMPARE(ring.writer->tryWrite("c"), WriteResult::Written);
    QVERIFY(readableNow(ring.reader->dataFd()));
    ring.reader->clearDataSignal();
    QVERIFY(!readableNow(ring.reader->dataFd()));
    QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
    QCOMPARE(frame, std::string("c"));
}

void TestRing::close_deliversWaitingFramesThenClosed()
{
    Ring ring = connected(1 << 16);
    QVERIFY(ring.writer && ring.reader);
    QCOMPARE(ring.writer->tryWrite("last"), WriteResult::Written);
    ring.writer->close();
    QCOMPARE(ring.writer->tryWrite("after"), WriteResult::Closed);
    QVERIFY(readableNow(ring.reader->dataFd()));
    std::string frame;
    QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
    QCOMPARE(frame, std::string("last"));
    QCOMPARE(ring.reader->read(frame), ReadResult::Closed);
    QCOMPARE(ring.reader->read(frame), ReadResult::Closed);
}

void TestRing::readerClose_makesWritesAndReadsClosed()
{
    Ring ring = connected(1 << 16);
    QVERIFY(ring.writer && ring.reader);
    QCOMPARE(ring.writer->tryWrite("unread"), WriteResult::Written);
    ring.reader->close();
    QVERIFY(readableNow(ring.writer->spaceFd()));
    QCOMPARE(ring.writer->tryWrite("x"), WriteResult::Closed);
    std::string frame;
    QCOMPARE(ring.reader->read(frame), ReadResult::Closed);
}

void TestRing::write_timesOut()
{
    Ring ring = connected(RingWriter::MinCapacity);
    QVERIFY(ring.writer && ring.reader);
    while (ring.writer->tryWrite(frameOf(500, 1)) == WriteResult::Written) { }
    const auto start = std::chrono::steady_clock::now();
    QCOMPARE(ring.writer->write(frameOf(500, 1), 100), WriteResult::WouldBlock);
    QVERIFY(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(90));
}

void TestRing::write_wokenByReaderClose()
{
    Ring ring = connected(RingWriter::MinCapacity);
    QVERIFY(ring.writer && ring.reader);
    while (ring.writer->tryWrite(frameOf(500, 1)) == WriteResult::Written) { }
    std::atomic<int> result{-1};
    const auto start = std::chrono::steady_clock::now();
    std::thread blocked([&] {
        result = int(ring.writer->write(frameOf(500, 1), 5000));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ring.reader->close();
    blocked.join();
    QCOMPARE(WriteResult(result.load()), WriteResult::Closed);
    QVERIFY(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

void TestRing::readerExit_endsAWaitingWrite()
{
    auto writer = RingWriter::create(RingWriter::MinCapacity, "t.v1", nullptr);
    QVERIFY(writer);
    int pair[2];
    QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    const pid_t child = ::fork();
    QVERIFY(child >= 0);
    if (child == 0) {
        // A reader that takes the ring and dies without a word.
        ::close(pair[0]);
        auto reader = acceptRing(pair[1], AcceptOptions(), 5000, nullptr);
        ::_exit(reader ? 0 : 1);
    }
    ::close(pair[1]);
    std::string error;
    const bool offered = offerRing(pair[0], *writer, 5000, &error);
    ::close(pair[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    QVERIFY2(offered, error.c_str());
    QVERIFY(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    while (writer->tryWrite(frameOf(500, 1)) == WriteResult::Written) { }
    const auto start = std::chrono::steady_clock::now();
    QCOMPARE(writer->write(frameOf(500, 1), 5000), WriteResult::Closed);
    QVERIFY(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

void TestRing::writerExit_endsReadsAfterTheLastFrame()
{
    int pair[2];
    QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    const pid_t child = ::fork();
    QVERIFY(child >= 0);
    if (child == 0) {
        // A writer that writes, then dies without closing the ring.
        ::close(pair[1]);
        auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
        if (!writer || !offerRing(pair[0], *writer, 5000, nullptr))
            ::_exit(1);
        writer->write("first", 5000);
        writer->write("last", 5000);
        ::_exit(0);
    }
    ::close(pair[0]);
    std::string error;
    auto reader = acceptRing(pair[1], AcceptOptions(), 5000, &error);
    ::close(pair[1]);
    std::vector<std::string> frames;
    const ReadResult end = reader ? drainUntilEnd(*reader, frames) : ReadResult::Corrupt;
    // Reaped before any check can return.
    int status = 0;
    ::waitpid(child, &status, 0);
    QVERIFY2(reader, error.c_str());
    QCOMPARE(end, ReadResult::Closed);
    QCOMPARE(frames, (std::vector<std::string>{"first", "last"}));
}

void TestRing::failedOffer_stillSeesTheReaderExit()
{
    auto writer = RingWriter::create(RingWriter::MinCapacity, "t.v1", nullptr);
    QVERIFY(writer);
    int pair[2];
    QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);
    int go[2];
    QVERIFY(::pipe2(go, O_CLOEXEC) == 0);
    const pid_t child = ::fork();
    QVERIFY(child >= 0);
    if (child == 0) {
        // A reader that answers only after the writer gave up, takes the ring
        // anyway, and dies.
        ::close(pair[0]);
        ::close(go[1]);
        char byte = 0;
        if (::read(go[0], &byte, 1) != 1)
            ::_exit(2);
        auto reader = acceptRing(pair[1], AcceptOptions(), 5000, nullptr);
        ::_exit(reader ? 0 : 1);
    }
    ::close(pair[1]);
    ::close(go[0]);
    std::string error;
    const bool offered = offerRing(pair[0], *writer, 100, &error);
    const char byte = 1;
    const bool told = ::write(go[1], &byte, 1) == 1;
    ::close(go[1]);
    int status = 0;
    ::waitpid(child, &status, 0);
    ::close(pair[0]);
    QVERIFY(told);
    QVERIFY(!offered);
    QVERIFY2(error.find("timed out") != std::string::npos, error.c_str());
    QVERIFY(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    // The writer gave up the reader's socket with the offer, so the dead
    // reader closed its last copy.
    while (writer->tryWrite(frameOf(500, 1)) == WriteResult::Written) { }
    const auto start = std::chrono::steady_clock::now();
    QCOMPARE(writer->write(frameOf(500, 1), 5000), WriteResult::Closed);
    QVERIFY(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

void TestRing::forkedChild_leavesTheRingAlone()
{
    Ring ring = connected(1 << 16);
    QVERIFY(ring.writer && ring.reader);
    const pid_t child = ::fork();
    QVERIFY(child >= 0);
    if (child == 0) {
        // The child's copies go, as they would before an exec.
        ring.writer.reset();
        ring.reader.reset();
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    QCOMPARE(ring.writer->tryWrite("still open"), WriteResult::Written);
    std::string frame;
    QCOMPARE(ring.reader->read(frame), ReadResult::Frame);
    QCOMPARE(frame, std::string("still open"));
    // Neither copy said that its end closed.
    QCOMPARE(ring.reader->read(frame), ReadResult::Empty);
}

void TestRing::writer_closesOnABrokenReadPosition()
{
    Ring ring = connected(RingWriter::MinCapacity);
    QVERIFY(ring.writer && ring.reader);
    const int copy = ::dup(ring.writer->memoryFd());
    void* map = ::mmap(nullptr, HeaderBytes, PROT_READ | PROT_WRITE, MAP_SHARED, copy, 0);
    ::close(copy);
    QVERIFY(map != MAP_FAILED);
    // A reader that claims to have read what was never written.
    const std::uint64_t ahead = 1 << 20;
    std::memcpy(static_cast<char*>(map) + ReadPosAt, &ahead, sizeof ahead);
    ::munmap(map, HeaderBytes);
    WriteResult result = WriteResult::Written;
    for (int i = 0; i < 100 && result == WriteResult::Written; ++i)
        result = ring.writer->tryWrite(frameOf(500, i));
    QCOMPARE(result, WriteResult::Closed);
    QCOMPARE(ring.writer->tryWrite("x"), WriteResult::Closed);
}

void TestRing::attach_refusesWhatIsNotARing_data()
{
    QTest::addColumn<QByteArray>("kind");
    QTest::addColumn<QByteArray>("expected");
    QTest::newRow("no fds") << QByteArray("no fds") << QByteArray("without its file descriptors");
    QTest::newRow("bad codec") << QByteArray("bad codec") << QByteArray("codec tag");
    QTest::newRow("wake fd is a pipe") << QByteArray("pipe") << QByteArray("Unix stream socket");
    QTest::newRow("unsealed") << QByteArray("unsealed") << QByteArray("sealed");
    QTest::newRow("too small") << QByteArray("too small") << QByteArray("out of range");
    QTest::newRow("larger than taken") << QByteArray("larger than taken") << QByteArray("takes at most");
    QTest::newRow("bad magic") << QByteArray("magic") << QByteArray("not a phosphor-channel ring");
    QTest::newRow("another version") << QByteArray("version")
                                     << QByteArray(std::to_string(ProtocolVersion + 7).c_str());
    QTest::newRow("header capacity") << QByteArray("capacity") << QByteArray("does not match its size");
    QTest::newRow("unaligned read position") << QByteArray("unaligned") << QByteArray("not on a record");
    QTest::newRow("reader closed") << QByteArray("reader closed") << QByteArray("already had a reader");
}

void TestRing::attach_refusesWhatIsNotARing()
{
    QFETCH(QByteArray, kind);
    QFETCH(QByteArray, expected);
    std::string error;
    std::unique_ptr<RingReader> reader;
    if (kind == "no fds") {
        reader = RingReader::attach(-1, -1, "t.v1", RingWriter::MaxCapacity, &error);
    } else if (kind == "bad codec") {
        RawRing raw(RingWriter::MinCapacity);
        reader =
            RingReader::attach(::dup(raw.memoryFd), ::dup(raw.wake[1]), "has space", RingWriter::MaxCapacity, &error);
    } else if (kind == "pipe") {
        RawRing raw(RingWriter::MinCapacity);
        int pipe[2];
        QVERIFY(::pipe2(pipe, O_CLOEXEC) == 0);
        ::close(pipe[1]);
        reader = RingReader::attach(::dup(raw.memoryFd), pipe[0], "t.v1", RingWriter::MaxCapacity, &error);
    } else if (kind == "unsealed") {
        // A ring whose owner could still shrink it: a mapping of it could end in SIGBUS.
        RawRing raw(RingWriter::MinCapacity, false);
        reader = raw.attach(&error);
    } else if (kind == "too small") {
        RawRing raw(RingWriter::MinCapacity - 8);
        reader = raw.attach(&error);
    } else if (kind == "larger than taken") {
        RawRing raw(1 << 16);
        reader = raw.attach(&error, RingWriter::MinCapacity);
    } else {
        RawRing raw(RingWriter::MinCapacity);
        if (kind == "magic")
            raw.put32(MagicAt, 0x12345678);
        if (kind == "version")
            raw.put32(VersionAt, ProtocolVersion + 7);
        if (kind == "capacity")
            raw.put64(CapacityAt, RingWriter::MinCapacity * 2);
        if (kind == "unaligned")
            raw.put64(ReadPosAt, 4);
        if (kind == "reader closed")
            raw.put32(ReaderClosedAt, 1);
        reader = raw.attach(&error);
    }
    QVERIFY(!reader);
    QVERIFY2(error.find(expected.toStdString()) != std::string::npos, error.c_str());
    if (kind == "version")
        QVERIFY2(error.find(std::to_string(ProtocolVersion)) != std::string::npos, error.c_str());
}

void TestRing::attach_leavesRefusedMemoryAlone()
{
    RawRing raw(RingWriter::MinCapacity);
    raw.put32(VersionAt, ProtocolVersion + 1);
    std::string error;
    QVERIFY(!raw.attach(&error));
    // A refused ring of another layout must not be written into.
    QCOMPARE(raw.get32(ReaderClosedAt), 0u);
}

void TestRing::read_reportsCorruptRings_data()
{
    QTest::addColumn<QByteArray>("kind");
    QTest::newRow("a whole ring and more unread") << QByteArray("ahead");
    QTest::newRow("a write position off a record") << QByteArray("unaligned");
    QTest::newRow("a record of an unknown kind") << QByteArray("kind");
    QTest::newRow("a length over the largest frame") << QByteArray("too long");
    QTest::newRow("a length past what was written") << QByteArray("past written");
    QTest::newRow("a record past the end of the area") << QByteArray("past the end");
    QTest::newRow("a skip at the start of the area") << QByteArray("skip at start");
    QTest::newRow("a skip as the last record") << QByteArray("skip last");
}

void TestRing::read_reportsCorruptRings()
{
    QFETCH(QByteArray, kind);
    const std::size_t capacity = RingWriter::MinCapacity;
    RawRing raw(capacity);
    // The past-the-end and skip-last cases need the reader near the end of the area.
    const std::size_t nearEnd = capacity - 16;
    if (kind == "past the end" || kind == "skip last")
        raw.put64(ReadPosAt, nearEnd);
    std::string error;
    auto reader = raw.attach(&error);
    QVERIFY2(reader, error.c_str());
    if (kind == "ahead") {
        raw.record(0, 4, FrameRecord);
        raw.put64(WritePosAt, capacity * 3);
    } else if (kind == "unaligned") {
        raw.record(0, 4, FrameRecord);
        raw.put64(WritePosAt, 12);
    } else if (kind == "kind") {
        raw.record(0, 4, 99);
        raw.put64(WritePosAt, 16);
    } else if (kind == "too long") {
        raw.record(0, std::uint32_t(capacity / 2), FrameRecord);
        raw.put64(WritePosAt, capacity);
    } else if (kind == "past written") {
        raw.record(0, 2000, FrameRecord);
        raw.put64(WritePosAt, 16);
    } else if (kind == "past the end") {
        // Plenty "written", and a record that would run past the mapping.
        raw.record(nearEnd, 1000, FrameRecord);
        raw.put64(WritePosAt, nearEnd + 2048);
    } else if (kind == "skip at start") {
        raw.record(0, 0, SkipRecord);
        raw.record(8, 4, FrameRecord);
        raw.put64(WritePosAt, 24);
    } else if (kind == "skip last") {
        raw.record(nearEnd, 0, SkipRecord);
        raw.put64(WritePosAt, capacity);
    }
    std::string frame;
    QCOMPARE(reader->read(frame), ReadResult::Corrupt);
}

void TestRing::read_staysCorrupt()
{
    RawRing raw(RingWriter::MinCapacity);
    std::string error;
    auto reader = raw.attach(&error);
    QVERIFY2(reader, error.c_str());
    raw.record(0, 4, 99);
    raw.put64(WritePosAt, 16);
    std::string frame;
    QCOMPARE(reader->read(frame), ReadResult::Corrupt);
    // Repaired: a good frame now. Nothing after a break can be trusted, so
    // the reader stays corrupt.
    raw.record(0, 4, FrameRecord);
    std::memcpy(raw.map + HeaderBytes + 8, "good", 4);
    QCOMPARE(reader->read(frame), ReadResult::Corrupt);
}

void TestRing::twoThreads_keepOrderUnderBackpressure()
{
    Ring ring = connected(RingWriter::MinCapacity * 2);
    QVERIFY(ring.writer && ring.reader);
    constexpr int Frames = 50000;
    std::atomic<bool> writeFailed{false};
    std::thread producer([&] {
        for (int i = 0; i < Frames; ++i) {
            if (ring.writer->write(frameOf(std::size_t(1 + i % 700), i), 5000) != WriteResult::Written) {
                writeFailed = true;
                return;
            }
        }
        ring.writer->close();
    });
    int received = 0;
    bool inOrder = true;
    std::string frame;
    while (true) {
        const ReadResult result = ring.reader->read(frame);
        if (result == ReadResult::Frame) {
            inOrder = inOrder && frame == frameOf(std::size_t(1 + received % 700), received);
            ++received;
            continue;
        }
        if (result != ReadResult::Empty)
            break;
        pollfd p{ring.reader->dataFd(), POLLIN, 0};
        if (::poll(&p, 1, 5000) != 1)
            break;
        ring.reader->clearDataSignal();
    }
    producer.join();
    QVERIFY(!writeFailed);
    QVERIFY(inOrder);
    QCOMPARE(received, Frames);
    QCOMPARE(ring.reader->read(frame), ReadResult::Closed);
}

QTEST_GUILESS_MAIN(TestRing)
#include "test_ring.moc"
