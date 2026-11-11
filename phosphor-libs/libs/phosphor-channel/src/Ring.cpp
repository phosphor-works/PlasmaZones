// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Ring.h>

#include "Fd.h"

#include <atomic>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/magic.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace PhosphorChannel {

namespace {

// The layout of the shared memory. Positions count bytes from the start of
// the ring and only grow; the offset in the data area is the position modulo
// the capacity. Each position has a cache line of its own, which only its end
// writes for every frame. The two wake requests and the closed flags sit on
// lines that are written only when an end waits or closes.
constexpr std::uint32_t RingMagic = 0x52434850; // "PHCR"
constexpr std::size_t HeaderBytes = 512;
constexpr std::size_t MagicAt = 0;
constexpr std::size_t VersionAt = 4;
constexpr std::size_t CapacityAt = 8;
constexpr std::size_t WritePosAt = 64;
constexpr std::size_t ReadPosAt = 128;
constexpr std::size_t ReaderWaitsAt = 192; ///< u32: the reader asks for a data signal
constexpr std::size_t WriterWantsAt = 256; ///< u64: the bytes that a waiting writer needs, 0 when it does not wait
constexpr std::size_t WriterClosedAt = 320;
constexpr std::size_t ReaderClosedAt = 324;

// A record: a length and a kind, then the payload, padded to 8 bytes. A skip
// record fills the rest of the data area before the wrap.
constexpr std::size_t RecordHeader = 8;
constexpr std::uint32_t FrameRecord = 1;
constexpr std::uint32_t SkipRecord = 2;

// The two processes share these fields, so their atomics must work without
// a lock: a lock would live in each process alone.
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
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

/// A frame's record is at most half the ring rounded up to 8, and a skip
/// before a record is at most the record less 8 bytes, so the two together
/// take at most twice the record less 8, which is at most the ring. So a
/// skip and a record fit an empty ring wherever the wrap falls.
std::size_t maxFrameOf(std::size_t capacity)
{
    return capacity / 2 - RecordHeader;
}

/// Wakes the other end. False when it went away. A full socket already wakes
/// it, so that counts as a success.
bool signal(int fd)
{
    const char byte = 1;
    while (true) {
        if (::send(fd, &byte, 1, MSG_DONTWAIT | MSG_NOSIGNAL) >= 0)
            return true;
        if (errno == EINTR)
            continue;
        return errno == EAGAIN;
    }
}

/// Reads the signals that wait. False when the other end went away.
bool drain(int fd)
{
    char buffer[256];
    for (int round = 0; round < 16; ++round) {
        const ssize_t n = ::recv(fd, buffer, sizeof buffer, MSG_DONTWAIT);
        if (n > 0)
            continue;
        if (n == 0)
            return false;
        if (errno == EINTR)
            continue;
        return errno == EAGAIN;
    }
    return true;
}

} // namespace

std::unique_ptr<RingWriter> RingWriter::create(std::size_t capacity, std::string codec, std::string* error)
{
    if (!validCodec(codec)) {
        setError(error, codecRule());
        return nullptr;
    }
    if (capacity < MinCapacity || capacity > MaxCapacity) {
        setError(error,
                 "the ring capacity must be " + std::to_string(MinCapacity) + " to " + std::to_string(MaxCapacity)
                     + " bytes");
        return nullptr;
    }
    std::unique_ptr<RingWriter> ring(new RingWriter);
    ring->m_owner = ::getpid();
    ring->m_codec = std::move(codec);
    ring->m_capacity = align8(capacity);
    ring->m_mapSize = HeaderBytes + ring->m_capacity;
#ifdef MFD_NOEXEC_SEAL
    ring->m_memoryFd = ::memfd_create("phosphor-channel", MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);
    // Kernels before 6.3 do not know the flag.
    if (ring->m_memoryFd < 0 && errno == EINVAL)
#endif
        ring->m_memoryFd = ::memfd_create("phosphor-channel", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (ring->m_memoryFd < 0) {
        setError(error, std::string("cannot create the ring's memory: ") + std::strerror(errno));
        return nullptr;
    }
    int wake[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, wake) != 0) {
        setError(error, std::string("cannot create the ring's wake sockets: ") + std::strerror(errno));
        return nullptr;
    }
    ring->m_wakeFd = wake[0];
    ring->m_peerWakeFd = wake[1];
    if (::ftruncate(ring->m_memoryFd, off_t(ring->m_mapSize)) != 0) {
        setError(error, std::string("cannot size the ring's memory: ") + std::strerror(errno));
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
        // A child after fork() leaves the ring of its parent alone.
        if (::getpid() == m_owner)
            close();
        ::munmap(m_map, m_mapSize);
    }
    for (const int fd : {m_memoryFd, m_wakeFd, m_peerWakeFd}) {
        if (fd >= 0)
            ::close(fd);
    }
}

std::size_t RingWriter::maxFrame() const
{
    return maxFrameOf(m_capacity);
}

bool RingWriter::fits(std::uint64_t readPos, std::size_t bytes) const
{
    const std::uint64_t used = m_writePos - readPos;
    return used <= m_capacity && m_capacity - used >= bytes;
}

WriteResult RingWriter::tryWrite(std::string_view frame)
{
    if (m_closed || m_broken)
        return WriteResult::Closed;
    if (field<std::uint32_t>(m_map, ReaderClosedAt).load(std::memory_order_acquire)) {
        m_broken = true;
        return WriteResult::Closed;
    }
    if (frame.size() > maxFrame())
        return WriteResult::TooLarge;
    const std::size_t need = RecordHeader + align8(frame.size());
    const std::size_t offset = m_writePos % m_capacity;
    const std::size_t tail = m_capacity - offset;
    const std::size_t skip = need > tail ? tail : 0;
    auto readPos = field<std::uint64_t>(m_map, ReadPosAt);
    // The read position seen last is never ahead of the real one, so it can
    // only say "full" wrongly, never "fits": load the real one only then.
    if (!fits(m_readPosSeen, skip + need)) {
        m_readPosSeen = readPos.load(std::memory_order_seq_cst);
        if (m_writePos - m_readPosSeen > m_capacity) {
            // A reader that claims to have read more than was written broke
            // the ring: nothing it says can be trusted.
            m_broken = true;
            return WriteResult::Closed;
        }
        if (!fits(m_readPosSeen, skip + need)) {
            // Said before the second look: a read between the two sees it and
            // signals, so the writer never sleeps through the space it waits for.
            auto wants = field<std::uint64_t>(m_map, WriterWantsAt);
            wants.store(skip + need, std::memory_order_seq_cst);
            m_readPosSeen = readPos.load(std::memory_order_seq_cst);
            if (!fits(m_readPosSeen, skip + need))
                return WriteResult::WouldBlock;
            wants.store(0, std::memory_order_relaxed);
        }
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
    if (!frame.empty())
        std::memcpy(area + at + RecordHeader, frame.data(), frame.size());
    m_writePos += skip + need;
    field<std::uint64_t>(m_map, WritePosAt).store(m_writePos, std::memory_order_seq_cst);
    // A load first: the locked exchange runs only when the reader waits.
    auto readerWaits = field<std::uint32_t>(m_map, ReaderWaitsAt);
    if (readerWaits.load(std::memory_order_seq_cst) && readerWaits.exchange(0, std::memory_order_seq_cst)
        && !signal(m_wakeFd)) {
        m_broken = true;
    }
    return WriteResult::Written;
}

WriteResult RingWriter::write(std::string_view frame, int timeoutMs)
{
    const Deadline deadline(timeoutMs);
    while (true) {
        const WriteResult result = tryWrite(frame);
        if (result != WriteResult::WouldBlock)
            return result;
        switch (waitFor(m_wakeFd, POLLIN, deadline)) {
        case Wait::Ready:
            clearSpaceSignal();
            break;
        case Wait::TimedOut:
            return WriteResult::WouldBlock;
        case Wait::Failed:
            m_broken = true;
            return WriteResult::Closed;
        }
    }
}

void RingWriter::clearSpaceSignal()
{
    if (!drain(m_wakeFd))
        m_broken = true;
}

void RingWriter::close()
{
    if (m_closed)
        return;
    m_closed = true;
    field<std::uint32_t>(m_map, WriterClosedAt).store(1, std::memory_order_seq_cst);
    signal(m_wakeFd);
}

void RingWriter::handedOver()
{
    if (m_peerWakeFd >= 0)
        ::close(m_peerWakeFd);
    m_peerWakeFd = -1;
}

std::unique_ptr<RingReader> RingReader::attach(int memoryFd, int wakeFd, std::string codec, std::size_t maxCapacity,
                                               std::string* error)
{
    std::unique_ptr<RingReader> ring(new RingReader);
    ring->m_owner = ::getpid();
    ring->m_memoryFd = memoryFd;
    ring->m_wakeFd = wakeFd;
    ring->m_codec = std::move(codec);
    if (memoryFd < 0 || wakeFd < 0) {
        setError(error, "the ring came without its file descriptors");
        return nullptr;
    }
    if (!validCodec(ring->m_codec)) {
        setError(error, codecRule());
        return nullptr;
    }
    // Each process holds only its own end of the wake pair, and every call
    // on it passes MSG_DONTWAIT, so the writer cannot make it block.
    if (!isUnixStreamSocket(wakeFd)) {
        setError(error, "the ring's wake descriptor is not a Unix stream socket");
        return nullptr;
    }
    // Plain shared memory: huge pages could run out under the mapping.
    struct statfs filesystem{};
    if (::fstatfs(memoryFd, &filesystem) != 0 || filesystem.f_type != TMPFS_MAGIC) {
        setError(error, "the ring's memory is not plain shared memory");
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
    if (std::size_t(info.st_size) - HeaderBytes > maxCapacity) {
        setError(error,
                 "the ring has " + std::to_string(std::size_t(info.st_size) - HeaderBytes)
                     + " bytes, and this end takes at most " + std::to_string(maxCapacity));
        return nullptr;
    }
    const std::size_t mapSize = std::size_t(info.st_size);
    void* map = ::mmap(nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, memoryFd, 0);
    if (map == MAP_FAILED) {
        setError(error, std::string("cannot map the ring: ") + std::strerror(errno));
        return nullptr;
    }
    // Refused memory is left as it was: the mapping becomes the reader's
    // only after every check, so that its destructor writes nothing into it.
    const auto refuse = [&](std::string text) -> std::unique_ptr<RingReader> {
        ::munmap(map, mapSize);
        setError(error, std::move(text));
        return nullptr;
    };
    if (field<std::uint32_t>(map, MagicAt).load() != RingMagic)
        return refuse("the memory is not a phosphor-channel ring");
    if (const std::uint32_t version = field<std::uint32_t>(map, VersionAt).load(); version != ProtocolVersion) {
        return refuse(versionMismatch(version));
    }
    // The size of the mapping decides, never the header: the writer can
    // change the header at any time.
    const std::size_t capacity = mapSize - HeaderBytes;
    if (field<std::uint64_t>(map, CapacityAt).load() != capacity || capacity % 8 != 0) {
        return refuse("the ring's header does not match its size");
    }
    // Records start at multiples of 8, so a record header never crosses the
    // end of the data area.
    const std::uint64_t readPos = field<std::uint64_t>(map, ReadPosAt).load();
    if (readPos % 8 != 0)
        return refuse("the ring's read position is not on a record");
    if (field<std::uint32_t>(map, ReaderClosedAt).load() != 0)
        return refuse("the ring already had a reader");
    ring->m_map = map;
    ring->m_mapSize = mapSize;
    ring->m_capacity = capacity;
    ring->m_readPos = readPos;
    ring->m_writePosSeen = readPos;
    return ring;
}

RingReader::~RingReader()
{
    if (m_map) {
        // A child after fork() leaves the ring of its parent alone.
        if (::getpid() == m_owner)
            close();
        ::munmap(m_map, m_mapSize);
    }
    for (const int fd : {m_memoryFd, m_wakeFd}) {
        if (fd >= 0)
            ::close(fd);
    }
}

void RingReader::clearDataSignal()
{
    if (!drain(m_wakeFd))
        m_writerGone = true;
}

void RingReader::close()
{
    m_closed = true;
    if (!m_map || field<std::uint32_t>(m_map, ReaderClosedAt).load() != 0)
        return;
    field<std::uint32_t>(m_map, ReaderClosedAt).store(1, std::memory_order_seq_cst);
    signal(m_wakeFd);
}

ReadResult RingReader::read(std::string& frame)
{
    if (m_corrupt)
        return ReadResult::Corrupt;
    if (m_closed)
        return ReadResult::Closed;
    auto writePos = field<std::uint64_t>(m_map, WritePosAt);
    auto readPos = field<std::uint64_t>(m_map, ReadPosAt);
    const auto corrupt = [this] {
        m_corrupt = true;
        return ReadResult::Corrupt;
    };
    while (true) {
        // The write position seen last is never ahead of the real one: while
        // it shows frames, they are there, and no load is needed.
        std::uint64_t w = m_writePosSeen;
        if (w == m_readPos) {
            w = writePos.load(std::memory_order_seq_cst);
            if (w == m_readPos) {
                // Said before the second look, as the writer does for space.
                auto waits = field<std::uint32_t>(m_map, ReaderWaitsAt);
                waits.store(1, std::memory_order_seq_cst);
                w = writePos.load(std::memory_order_seq_cst);
                if (w == m_readPos) {
                    // The writer closes after its last frame: with the flag
                    // set, a third look at the position finds every frame. A
                    // writer that went away wrote all it will.
                    if (field<std::uint32_t>(m_map, WriterClosedAt).load(std::memory_order_seq_cst) == 0
                        && !m_writerGone) {
                        return ReadResult::Empty;
                    }
                    w = writePos.load(std::memory_order_seq_cst);
                    if (w == m_readPos) {
                        m_closed = true;
                        return ReadResult::Closed;
                    }
                }
                waits.store(0, std::memory_order_relaxed);
            }
            m_writePosSeen = w;
        }
        const std::uint64_t used = w - m_readPos;
        if (used > m_capacity || used % 8 != 0)
            return corrupt();
        const std::size_t offset = m_readPos % m_capacity;
        const char* const area = data(m_map);
        std::uint32_t header[2];
        std::memcpy(header, area + offset, sizeof header);
        if (header[1] == SkipRecord) {
            // A writer pads only a wrap that a frame follows, so a skip is
            // never the last record.
            const std::size_t tail = m_capacity - offset;
            if (tail >= used)
                return corrupt();
            m_readPos += tail;
            readPos.store(m_readPos, std::memory_order_seq_cst);
            continue;
        }
        if (header[1] != FrameRecord || header[0] > maxFrameOf(m_capacity))
            return corrupt();
        const std::size_t need = RecordHeader + align8(header[0]);
        if (need > m_capacity - offset || need > used)
            return corrupt();
        // A copy: the writer may write the area again once the position moves.
        frame.assign(area + offset + RecordHeader, header[0]);
        m_readPos += need;
        readPos.store(m_readPos, std::memory_order_seq_cst);
        // Wake a waiting writer only once the space it asked for is free.
        auto wants = field<std::uint64_t>(m_map, WriterWantsAt);
        if (const std::uint64_t bytes = wants.load(std::memory_order_seq_cst); bytes
            && m_capacity - (m_writePosSeen - m_readPos) >= bytes && wants.exchange(0, std::memory_order_seq_cst)
            && !signal(m_wakeFd)) {
            m_writerGone = true;
        }
        return ReadResult::Frame;
    }
}

} // namespace PhosphorChannel
