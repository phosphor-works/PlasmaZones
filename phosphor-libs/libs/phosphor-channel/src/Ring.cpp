// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Ring.h>

#include "Fd.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace PhosphorChannel {

namespace {

// The layout of the shared memory. Positions count bytes from the start of
// the ring and only grow; the offset in the data area is the position modulo
// the capacity. Each end writes only its own cache line.
constexpr std::uint32_t RingMagic = 0x52434850; // "PHCR"
constexpr std::size_t HeaderBytes = 256;
constexpr std::size_t MagicAt = 0;
constexpr std::size_t VersionAt = 4;
constexpr std::size_t CapacityAt = 8;
// The writer's line.
constexpr std::size_t WritePosAt = 64;
constexpr std::size_t ReaderSleepsAt = 72; ///< the reader waits on the data fd
constexpr std::size_t WriterClosedAt = 76;
// The reader's line.
constexpr std::size_t ReadPosAt = 128;
constexpr std::size_t WriterSleepsAt = 136; ///< the writer waits on the space fd
constexpr std::size_t ReaderClosedAt = 140;

// A record: a length and a kind, then the payload, padded to 8 bytes. A skip
// record fills the rest of the data area before the wrap.
constexpr std::size_t RecordHeader = 8;
constexpr std::uint32_t FrameRecord = 1;
constexpr std::uint32_t SkipRecord = 2;

static_assert(std::atomic_ref<std::uint64_t>::required_alignment <= 8);
static_assert(std::atomic_ref<std::uint32_t>::required_alignment <= 4);

template<typename T>
std::atomic_ref<T> field(void* map, std::size_t at)
{
    return std::atomic_ref<T>(*reinterpret_cast<T*>(static_cast<char*>(map) + at));
}

char* data(void* map)
{
    return static_cast<char*>(map) + HeaderBytes;
}

std::size_t align8(std::size_t n)
{
    return (n + 7) & ~std::size_t(7);
}

/// A frame and its record take at most half the ring. Then a frame fits an
/// empty ring wherever the wrap falls: the skip before it is shorter than
/// the frame's record.
std::size_t maxFrameOf(std::size_t capacity)
{
    return capacity / 2 - RecordHeader;
}

void signal(int fd)
{
    const std::uint64_t one = 1;
    while (::write(fd, &one, sizeof one) < 0 && errno == EINTR) { }
}

void clear(int fd)
{
    std::uint64_t count = 0;
    while (::read(fd, &count, sizeof count) < 0 && errno == EINTR) { }
}

} // namespace

std::unique_ptr<RingWriter> RingWriter::create(std::size_t capacity, std::string codec, std::string* error)
{
    if (!validCodec(codec)) {
        setError(error, "the codec tag is not 1 to 64 printable ASCII characters without spaces");
        return nullptr;
    }
    if (capacity < MinCapacity || capacity > MaxCapacity) {
        setError(error, "the ring capacity must be " + std::to_string(MinCapacity) + " to " + std::to_string(MaxCapacity) + " bytes");
        return nullptr;
    }
    std::unique_ptr<RingWriter> ring(new RingWriter);
    ring->m_codec = std::move(codec);
    ring->m_capacity = align8(capacity);
    ring->m_mapSize = HeaderBytes + ring->m_capacity;
    ring->m_memoryFd = ::memfd_create("phosphor-channel", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    ring->m_dataFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ring->m_spaceFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (ring->m_memoryFd < 0 || ring->m_dataFd < 0 || ring->m_spaceFd < 0
        || ::ftruncate(ring->m_memoryFd, off_t(ring->m_mapSize)) != 0) {
        setError(error, std::string("cannot create the ring: ") + std::strerror(errno));
        return nullptr;
    }
    // Sealed before the reader sees it: a reader must never find the
    // mapping cut short (a SIGBUS) or grown.
    if (::fcntl(ring->m_memoryFd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
        setError(error, std::string("cannot seal the ring: ") + std::strerror(errno));
        return nullptr;
    }
    void* map = ::mmap(nullptr, ring->m_mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, ring->m_memoryFd, 0);
    if (map == MAP_FAILED) {
        setError(error, std::string("cannot map the ring: ") + std::strerror(errno));
        return nullptr;
    }
    ring->m_map = map;
    // ftruncate gave zeros: the positions start at 0.
    field<std::uint32_t>(map, MagicAt).store(RingMagic);
    field<std::uint32_t>(map, VersionAt).store(ProtocolVersion);
    field<std::uint64_t>(map, CapacityAt).store(ring->m_capacity);
    return ring;
}

RingWriter::~RingWriter()
{
    if (m_map) {
        close();
        ::munmap(m_map, m_mapSize);
    }
    for (const int fd : {m_memoryFd, m_dataFd, m_spaceFd}) {
        if (fd >= 0) ::close(fd);
    }
}

std::size_t RingWriter::maxFrame() const
{
    return maxFrameOf(m_capacity);
}

WriteResult RingWriter::tryWrite(std::string_view frame)
{
    if (m_closed || field<std::uint32_t>(m_map, ReaderClosedAt).load(std::memory_order_acquire)) return WriteResult::Closed;
    if (frame.size() > maxFrame()) return WriteResult::TooLarge;
    const std::size_t need = RecordHeader + align8(frame.size());
    const std::size_t offset = m_writePos % m_capacity;
    const std::size_t tail = m_capacity - offset;
    const std::size_t skip = need > tail ? tail : 0;
    auto readPos = field<std::uint64_t>(m_map, ReadPosAt);
    const auto fits = [&] {
        const std::uint64_t used = m_writePos - readPos.load(std::memory_order_seq_cst);
        // A reader that claims to have read more than was written breaks the
        // ring: nothing it says can be trusted, so nothing more is written.
        return used <= m_capacity && m_capacity - used >= skip + need;
    };
    if (!fits()) {
        // Said before the second look: a read between the two sees it and
        // signals, so the writer never sleeps through the space it waits for.
        field<std::uint32_t>(m_map, WriterSleepsAt).store(1, std::memory_order_seq_cst);
        if (!fits()) return WriteResult::WouldBlock;
        field<std::uint32_t>(m_map, WriterSleepsAt).store(0, std::memory_order_relaxed);
    }
    char* const area = data(m_map);
    std::uint32_t header[2];
    std::size_t at = offset;
    if (skip) {
        header[0] = 0;
        header[1] = SkipRecord;
        std::memcpy(area + at, header, sizeof header);
        at = 0;
    }
    header[0] = std::uint32_t(frame.size());
    header[1] = FrameRecord;
    std::memcpy(area + at, header, sizeof header);
    if (!frame.empty()) std::memcpy(area + at + RecordHeader, frame.data(), frame.size());
    m_writePos += skip + need;
    field<std::uint64_t>(m_map, WritePosAt).store(m_writePos, std::memory_order_seq_cst);
    if (field<std::uint32_t>(m_map, ReaderSleepsAt).exchange(0, std::memory_order_seq_cst)) signal(m_dataFd);
    return WriteResult::Written;
}

WriteResult RingWriter::write(std::string_view frame, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    while (true) {
        const WriteResult result = tryWrite(frame);
        if (result != WriteResult::WouldBlock) return result;
        int wait = -1;
        if (timeoutMs >= 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) return WriteResult::WouldBlock;
            wait = int(left.count());
        }
        if (!waitReadable(m_spaceFd, wait)) return WriteResult::WouldBlock;
        clearSpaceSignal();
    }
}

void RingWriter::clearSpaceSignal()
{
    clear(m_spaceFd);
}

void RingWriter::close()
{
    if (m_closed) return;
    m_closed = true;
    field<std::uint32_t>(m_map, WriterClosedAt).store(1, std::memory_order_seq_cst);
    signal(m_dataFd);
}

std::unique_ptr<RingReader> RingReader::attach(int memoryFd, int dataFd, int spaceFd, std::string codec, std::string* error)
{
    std::unique_ptr<RingReader> ring(new RingReader);
    ring->m_memoryFd = memoryFd;
    ring->m_dataFd = dataFd;
    ring->m_spaceFd = spaceFd;
    ring->m_codec = std::move(codec);
    if (memoryFd < 0 || dataFd < 0 || spaceFd < 0) {
        setError(error, "the ring came without its file descriptors");
        return nullptr;
    }
    const int seals = ::fcntl(memoryFd, F_GET_SEALS);
    if (seals < 0 || (seals & (F_SEAL_SHRINK | F_SEAL_GROW)) != (F_SEAL_SHRINK | F_SEAL_GROW)) {
        setError(error, "the ring is not sealed against a change of its size");
        return nullptr;
    }
    struct stat info{};
    if (::fstat(memoryFd, &info) != 0 || info.st_size < off_t(HeaderBytes + RingWriter::MinCapacity)
        || info.st_size > off_t(HeaderBytes + RingWriter::MaxCapacity)) {
        setError(error, "the ring has a size out of range");
        return nullptr;
    }
    ring->m_mapSize = std::size_t(info.st_size);
    void* map = ::mmap(nullptr, ring->m_mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, memoryFd, 0);
    if (map == MAP_FAILED) {
        setError(error, std::string("cannot map the ring: ") + std::strerror(errno));
        return nullptr;
    }
    ring->m_map = map;
    if (field<std::uint32_t>(map, MagicAt).load() != RingMagic) {
        setError(error, "the memory is not a phosphor-channel ring");
        return nullptr;
    }
    if (const std::uint32_t version = field<std::uint32_t>(map, VersionAt).load(); version != ProtocolVersion) {
        setError(error, versionMismatch(version));
        return nullptr;
    }
    // The size of the mapping decides, never the header: the writer can
    // change the header at any time.
    ring->m_capacity = ring->m_mapSize - HeaderBytes;
    if (field<std::uint64_t>(map, CapacityAt).load() != ring->m_capacity || ring->m_capacity % 8 != 0) {
        setError(error, "the ring's header does not match its size");
        return nullptr;
    }
    ring->m_readPos = field<std::uint64_t>(map, ReadPosAt).load();
    return ring;
}

RingReader::~RingReader()
{
    if (m_map) {
        close();
        ::munmap(m_map, m_mapSize);
    }
    for (const int fd : {m_memoryFd, m_dataFd, m_spaceFd}) {
        if (fd >= 0) ::close(fd);
    }
}

void RingReader::clearDataSignal()
{
    clear(m_dataFd);
}

void RingReader::close()
{
    if (!m_map || field<std::uint32_t>(m_map, ReaderClosedAt).load() != 0) return;
    field<std::uint32_t>(m_map, ReaderClosedAt).store(1, std::memory_order_seq_cst);
    signal(m_spaceFd);
}

ReadResult RingReader::read(std::string& frame)
{
    if (m_corrupt) return ReadResult::Corrupt;
    auto writePos = field<std::uint64_t>(m_map, WritePosAt);
    auto readPos = field<std::uint64_t>(m_map, ReadPosAt);
    const auto corrupt = [this] {
        m_corrupt = true;
        return ReadResult::Corrupt;
    };
    while (true) {
        std::uint64_t w = writePos.load(std::memory_order_seq_cst);
        if (w == m_readPos) {
            // Said before the second look, as the writer does for space.
            field<std::uint32_t>(m_map, ReaderSleepsAt).store(1, std::memory_order_seq_cst);
            w = writePos.load(std::memory_order_seq_cst);
            if (w == m_readPos) {
                // The writer closes after its last frame: with the flag set,
                // a second look at the position finds every frame.
                if (field<std::uint32_t>(m_map, WriterClosedAt).load(std::memory_order_seq_cst) == 0) return ReadResult::Empty;
                if (writePos.load(std::memory_order_seq_cst) == m_readPos) return ReadResult::Closed;
                continue;
            }
            field<std::uint32_t>(m_map, ReaderSleepsAt).store(0, std::memory_order_relaxed);
        }
        const std::uint64_t used = w - m_readPos;
        if (used > m_capacity || used % 8 != 0) return corrupt();
        const std::size_t offset = m_readPos % m_capacity;
        const char* const area = data(m_map);
        std::uint32_t header[2];
        std::memcpy(header, area + offset, sizeof header);
        if (header[1] == SkipRecord) {
            const std::size_t tail = m_capacity - offset;
            if (tail > used) return corrupt();
            m_readPos += tail;
            readPos.store(m_readPos, std::memory_order_seq_cst);
            continue;
        }
        if (header[1] != FrameRecord || header[0] > maxFrameOf(m_capacity)) return corrupt();
        const std::size_t need = RecordHeader + align8(header[0]);
        if (need > m_capacity - offset || need > used) return corrupt();
        // A copy: the writer may write the area again once the position moves.
        frame.assign(area + offset + RecordHeader, header[0]);
        m_readPos += need;
        readPos.store(m_readPos, std::memory_order_seq_cst);
        if (field<std::uint32_t>(m_map, WriterSleepsAt).exchange(0, std::memory_order_seq_cst)) signal(m_spaceFd);
        return ReadResult::Frame;
    }
}

} // namespace PhosphorChannel
