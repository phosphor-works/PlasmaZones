// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Ring.h>

#include <QTest>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace PhosphorChannel;

namespace {

/// A reader of @p writer's ring in this process, on copies of its fds, as a
/// handshake would give it to another process.
std::unique_ptr<RingReader> readerOf(const RingWriter& writer, std::string* error = nullptr)
{
    std::string ignored;
    return RingReader::attach(::dup(writer.memoryFd()), ::dup(writer.dataFd()), ::dup(writer.spaceFd()), writer.codec(),
                              error ? error : &ignored);
}

bool readableNow(int fd)
{
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, 0) == 1;
}

std::string frameOf(std::size_t size, int seed)
{
    std::string frame(size, '\0');
    for (std::size_t i = 0; i < size; ++i) frame[i] = char((seed * 131 + int(i)) & 0xff);
    return frame;
}

// The fields of the header that the tests change, as the ring lays them out.
constexpr std::size_t WritePosAt = 64;
constexpr std::size_t HeaderBytes = 256;

} // namespace

class TestRing : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void create_refusesBadCodecAndCapacity();
    void framesArriveInOrder();
    void wrapUsesSkipRecords();
    void largestFrameFitsWhereverTheWrapFalls();
    void fullRing_wouldBlock_thenSpaceSignal();
    void dataSignal_onlyWhenTheReaderSleeps();
    void close_deliversWaitingFramesThenClosed();
    void readerClose_makesWritesClosed();
    void attach_refusesAnUnsealedMemfd();
    void attach_refusesAnotherVersion();
    void read_reportsCorruptPositionsAndRecords();
    void twoThreads_keepOrderUnderBackpressure();
};

void TestRing::create_refusesBadCodecAndCapacity()
{
    std::string error;
    QVERIFY(!RingWriter::create(1 << 16, "", &error));
    QVERIFY(!RingWriter::create(1 << 16, "has space", &error));
    QVERIFY(!RingWriter::create(1 << 16, std::string(MaxCodecBytes + 1, 'a'), &error));
    QVERIFY(!RingWriter::create(RingWriter::MinCapacity - 1, "t.v1", &error));
    QVERIFY(error.find("capacity") != std::string::npos);
    auto ring = RingWriter::create(RingWriter::MinCapacity + 1, "t.v1", &error);
    QVERIFY(ring);
    QCOMPARE(ring->codec(), std::string("t.v1"));
}

void TestRing::framesArriveInOrder()
{
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    QVERIFY(writer);
    auto reader = readerOf(*writer);
    QVERIFY(reader);
    QCOMPARE(reader->codec(), std::string("t.v1"));
    std::string frame;
    QCOMPARE(reader->read(frame), ReadResult::Empty);
    for (const char* text : {"one", "", "three"}) QCOMPARE(writer->tryWrite(text), WriteResult::Written);
    for (const char* text : {"one", "", "three"}) {
        QCOMPARE(reader->read(frame), ReadResult::Frame);
        QCOMPARE(frame, std::string(text));
    }
    QCOMPARE(reader->read(frame), ReadResult::Empty);
}

void TestRing::wrapUsesSkipRecords()
{
    // Sizes that do not divide the ring: the wrap falls at every offset.
    auto writer = RingWriter::create(RingWriter::MinCapacity, "t.v1", nullptr);
    auto reader = readerOf(*writer);
    QVERIFY(writer && reader);
    std::string frame;
    for (int i = 0; i < 4000; ++i) {
        const std::string sent = frameOf(std::size_t(1 + (i * 37) % 900), i);
        QCOMPARE(writer->tryWrite(sent), WriteResult::Written);
        QCOMPARE(reader->read(frame), ReadResult::Frame);
        QVERIFY(frame == sent);
    }
}

void TestRing::largestFrameFitsWhereverTheWrapFalls()
{
    auto writer = RingWriter::create(RingWriter::MinCapacity, "t.v1", nullptr);
    auto reader = readerOf(*writer);
    QVERIFY(writer && reader);
    QCOMPARE(writer->tryWrite(std::string(writer->maxFrame() + 1, 'x')), WriteResult::TooLarge);
    std::string frame;
    // Each pass moves the positions on by a different amount, so the largest
    // frame meets the wrap at many offsets.
    for (std::size_t shift = 0; shift < writer->maxFrame(); shift += 8 * 13) {
        QCOMPARE(writer->tryWrite(std::string(shift, 's')), WriteResult::Written);
        QCOMPARE(reader->read(frame), ReadResult::Frame);
        // The ring is empty again, with its positions somewhere new.
        const std::string largest = frameOf(writer->maxFrame(), int(shift));
        QCOMPARE(writer->tryWrite(largest), WriteResult::Written);
        QCOMPARE(reader->read(frame), ReadResult::Frame);
        QVERIFY(frame == largest);
    }
}

void TestRing::fullRing_wouldBlock_thenSpaceSignal()
{
    auto writer = RingWriter::create(RingWriter::MinCapacity, "t.v1", nullptr);
    auto reader = readerOf(*writer);
    QVERIFY(writer && reader);
    int written = 0;
    while (writer->tryWrite(frameOf(500, written)) == WriteResult::Written) ++written;
    QVERIFY(written > 0);
    QCOMPARE(writer->tryWrite(frameOf(500, written)), WriteResult::WouldBlock);
    QVERIFY(!readableNow(writer->spaceFd()));
    std::string frame;
    QCOMPARE(reader->read(frame), ReadResult::Frame);
    QVERIFY(frame == frameOf(500, 0));
    // The writer said it sleeps: the read woke it.
    QVERIFY(readableNow(writer->spaceFd()));
    writer->clearSpaceSignal();
    QVERIFY(!readableNow(writer->spaceFd()));
    QCOMPARE(writer->tryWrite(frameOf(500, written)), WriteResult::Written);
    // Nothing was lost or reordered by the refusal.
    for (int i = 1; i <= written; ++i) {
        QCOMPARE(reader->read(frame), ReadResult::Frame);
        QVERIFY(frame == frameOf(500, i));
    }
}

void TestRing::dataSignal_onlyWhenTheReaderSleeps()
{
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    auto reader = readerOf(*writer);
    QVERIFY(writer && reader);
    // The reader has not looked yet: no signal for a busy reader.
    QCOMPARE(writer->tryWrite("a"), WriteResult::Written);
    QVERIFY(!readableNow(reader->dataFd()));
    std::string frame;
    QCOMPARE(reader->read(frame), ReadResult::Frame);
    QCOMPARE(reader->read(frame), ReadResult::Empty);
    // Now it sleeps, and the next write wakes it.
    QCOMPARE(writer->tryWrite("b"), WriteResult::Written);
    QVERIFY(readableNow(reader->dataFd()));
    reader->clearDataSignal();
    QCOMPARE(reader->read(frame), ReadResult::Frame);
    QCOMPARE(frame, std::string("b"));
}

void TestRing::close_deliversWaitingFramesThenClosed()
{
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    auto reader = readerOf(*writer);
    QVERIFY(writer && reader);
    QCOMPARE(writer->tryWrite("last"), WriteResult::Written);
    writer->close();
    QCOMPARE(writer->tryWrite("after"), WriteResult::Closed);
    QVERIFY(readableNow(reader->dataFd()));
    std::string frame;
    QCOMPARE(reader->read(frame), ReadResult::Frame);
    QCOMPARE(frame, std::string("last"));
    QCOMPARE(reader->read(frame), ReadResult::Closed);
}

void TestRing::readerClose_makesWritesClosed()
{
    auto writer = RingWriter::create(1 << 16, "t.v1", nullptr);
    auto reader = readerOf(*writer);
    QVERIFY(writer && reader);
    reader->close();
    QVERIFY(readableNow(writer->spaceFd()));
    QCOMPARE(writer->tryWrite("x"), WriteResult::Closed);
}

void TestRing::attach_refusesAnUnsealedMemfd()
{
    auto writer = RingWriter::create(RingWriter::MinCapacity, "t.v1", nullptr);
    QVERIFY(writer);
    // A copy of the ring in a memfd that its owner could still shrink: a
    // mapping of it could end in SIGBUS.
    const int copy = ::memfd_create("unsealed", MFD_CLOEXEC);
    QVERIFY(copy >= 0);
    QVERIFY(::ftruncate(copy, off_t(HeaderBytes + RingWriter::MinCapacity)) == 0);
    void* source = ::mmap(nullptr, HeaderBytes, PROT_READ, MAP_SHARED, writer->memoryFd(), 0);
    QVERIFY(source != MAP_FAILED);
    QVERIFY(::pwrite(copy, source, HeaderBytes, 0) == ssize_t(HeaderBytes));
    ::munmap(source, HeaderBytes);
    std::string error;
    QVERIFY(!RingReader::attach(copy, ::dup(writer->dataFd()), ::dup(writer->spaceFd()), "t.v1", &error));
    QVERIFY2(error.find("sealed") != std::string::npos, error.c_str());
}

void TestRing::attach_refusesAnotherVersion()
{
    auto writer = RingWriter::create(RingWriter::MinCapacity, "t.v1", nullptr);
    QVERIFY(writer);
    void* map = ::mmap(nullptr, HeaderBytes, PROT_READ | PROT_WRITE, MAP_SHARED, writer->memoryFd(), 0);
    QVERIFY(map != MAP_FAILED);
    const std::uint32_t other = ProtocolVersion + 7;
    std::memcpy(static_cast<char*>(map) + 4, &other, sizeof other);
    ::munmap(map, HeaderBytes);
    std::string error;
    QVERIFY(!readerOf(*writer, &error));
    QVERIFY2(error.find(std::to_string(other)) != std::string::npos, error.c_str());
    QVERIFY2(error.find(std::to_string(ProtocolVersion)) != std::string::npos, error.c_str());
}

void TestRing::read_reportsCorruptPositionsAndRecords()
{
    for (int kind = 0; kind < 3; ++kind) {
        auto writer = RingWriter::create(RingWriter::MinCapacity, "t.v1", nullptr);
        auto reader = readerOf(*writer);
        QVERIFY(writer && reader);
        QCOMPARE(writer->tryWrite("good"), WriteResult::Written);
        void* map = ::mmap(nullptr, HeaderBytes + RingWriter::MinCapacity, PROT_READ | PROT_WRITE, MAP_SHARED,
                           writer->memoryFd(), 0);
        QVERIFY(map != MAP_FAILED);
        char* base = static_cast<char*>(map);
        if (kind == 0) {
            // A write position past a whole ring of unread data.
            const std::uint64_t far = RingWriter::MinCapacity * 3;
            std::memcpy(base + WritePosAt, &far, sizeof far);
        } else if (kind == 1) {
            // A record of an unknown kind.
            const std::uint32_t bad = 99;
            std::memcpy(base + HeaderBytes + 4, &bad, sizeof bad);
        } else {
            // A length longer than the data that was written.
            const std::uint32_t longer = 2000;
            std::memcpy(base + HeaderBytes, &longer, sizeof longer);
        }
        ::munmap(map, HeaderBytes + RingWriter::MinCapacity);
        std::string frame;
        QCOMPARE(reader->read(frame), ReadResult::Corrupt);
        // It stays corrupt: nothing after it can be trusted.
        QCOMPARE(reader->read(frame), ReadResult::Corrupt);
    }
}

void TestRing::twoThreads_keepOrderUnderBackpressure()
{
    auto writer = RingWriter::create(RingWriter::MinCapacity * 2, "t.v1", nullptr);
    auto reader = readerOf(*writer);
    QVERIFY(writer && reader);
    constexpr int Frames = 50000;
    std::atomic<bool> writeFailed{false};
    std::thread producer([&] {
        for (int i = 0; i < Frames; ++i) {
            if (writer->write(frameOf(std::size_t(1 + i % 700), i), 5000) != WriteResult::Written) {
                writeFailed = true;
                return;
            }
        }
        writer->close();
    });
    int received = 0;
    bool inOrder = true;
    std::string frame;
    while (true) {
        const ReadResult result = reader->read(frame);
        if (result == ReadResult::Frame) {
            inOrder = inOrder && frame == frameOf(std::size_t(1 + received % 700), received);
            ++received;
            continue;
        }
        if (result != ReadResult::Empty) break;
        pollfd p{reader->dataFd(), POLLIN, 0};
        if (::poll(&p, 1, 5000) != 1) break;
        reader->clearDataSignal();
    }
    producer.join();
    QVERIFY(!writeFailed);
    QVERIFY(inOrder);
    QCOMPARE(received, Frames);
    QCOMPARE(reader->read(frame), ReadResult::Closed);
}

QTEST_GUILESS_MAIN(TestRing)
#include "test_ring.moc"
