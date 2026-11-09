// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorChannel/Channel.h>
#include <PhosphorChannel/phosphorchannel_export.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <sys/types.h>

// The shared-memory ring: a memfd that both processes map, sealed so that its
// size can never change (F_SEAL_SHRINK, F_SEAL_GROW, F_SEAL_SEAL). It cannot
// be sealed against writes, because both ends write their positions. The
// write position, the read position and each end's wake request sit on cache
// lines of their own, and the two closed flags share one. The writer writes
// whole frames, and a skip record pads the wrap.
//
// A connected pair of Unix sockets wakes the ends, one socket in each
// process: the writer sends a byte when it wrote to a reader that waits, and
// the reader sends one when it freed the space that a waiting writer asked
// for. While frames wait, neither end makes a system call. Each process holds
// only its own socket, so neither can change how the other's blocks. When
// the other process exits, its socket closes: the wake fd becomes readable.
// Once an end drains it (clearSpaceSignal(), clearDataSignal(), or the wait
// in write()) or fails to signal, its writes say Closed, and its reads say
// Closed after the last frame.
//
// The other process can change the shared memory at any time. The reader
// checks each position and length that it reads, copies each frame out
// before it returns it, and reports ReadResult::Corrupt for a ring that
// breaks the layout.
//
// A ring belongs to the process that made or accepted it. After fork() the
// child must exec or _exit: its copy of the object leaves the ring alone
// when it is destroyed, but it must not read or write.

namespace PhosphorChannel {

class RingWriter;
PHOSPHORCHANNEL_EXPORT bool offerRing(int socket, RingWriter& ring, int timeoutMs, std::string* error);

class PHOSPHORCHANNEL_EXPORT RingWriter
{
public:
    /// The smallest and the largest data area, in bytes.
    static constexpr std::size_t MinCapacity = 4096;
    static constexpr std::size_t MaxCapacity = std::size_t(1) << 30;

    /// A ring of @p capacity bytes (rounded up to 8) for @p codec. Nullptr
    /// with @p error for a bad codec or capacity, or a failed memfd.
    static std::unique_ptr<RingWriter> create(std::size_t capacity, std::string codec, std::string* error);
    ~RingWriter();
    RingWriter(const RingWriter&) = delete;
    RingWriter& operator=(const RingWriter&) = delete;

    /// Writes one frame without waiting. Closed once the reader closed the
    /// ring, or once this end saw it go away (see spaceFd()).
    WriteResult tryWrite(std::string_view frame);
    /// Waits up to @p timeoutMs (-1: no limit) for space. WouldBlock after
    /// the timeout. A reader that exits ends the wait with Closed.
    WriteResult write(std::string_view frame, int timeoutMs);
    /// The largest frame that the ring takes.
    std::size_t maxFrame() const;
    std::size_t capacity() const
    {
        return m_capacity;
    }
    const std::string& codec() const
    {
        return m_codec;
    }

    /// Readable when the reader freed the space that a WouldBlock asked for,
    /// closed the ring, or went away. Call clearSpaceSignal() before the
    /// tryWrite() that it wakes: a signal cleared after that retry can be the
    /// one that its WouldBlock asked for, and then nothing wakes the writer.
    int spaceFd() const
    {
        return m_wakeFd;
    }
    void clearSpaceSignal();

    /// Tells the reader that no frame follows. Frames written before still
    /// reach it.
    void close();

    /// The memfd of the ring. The ring owns it: never close it. A mapping of
    /// a dup() of it sees the ring, which the tests use to break the layout.
    int memoryFd() const
    {
        return m_memoryFd;
    }

private:
    friend bool offerRing(int socket, RingWriter& ring, int timeoutMs, std::string* error);
    RingWriter() = default;
    bool fits(std::uint64_t readPos, std::size_t bytes) const;
    /// Gives up the reader's socket once the offer of it went out.
    void handedOver();

    int m_memoryFd = -1;
    int m_wakeFd = -1; ///< this end's socket of the wake pair
    int m_peerWakeFd = -1; ///< the reader's socket, until offerRing() hands it over
    void* m_map = nullptr;
    std::size_t m_mapSize = 0;
    std::size_t m_capacity = 0;
    std::uint64_t m_writePos = 0;
    std::uint64_t m_readPosSeen = 0; ///< the last read position loaded from the ring
    pid_t m_owner = 0;
    bool m_closed = false;
    bool m_broken = false; ///< the reader broke the layout or went away
    bool m_offered = false;
    std::string m_codec;
};

class PHOSPHORCHANNEL_EXPORT RingReader
{
public:
    /// Maps the ring of a writer. Takes both fds, also on a failure. Refuses
    /// a memfd that is not plain shared memory, lacks the size seals, is
    /// larger than @p maxCapacity, or does not match its header, and a wake
    /// fd that is not a Unix stream socket. @p codec is what the handshake
    /// said.
    static std::unique_ptr<RingReader> attach(int memoryFd, int wakeFd, std::string codec, std::size_t maxCapacity,
                                              std::string* error);
    ~RingReader();
    RingReader(const RingReader&) = delete;
    RingReader& operator=(const RingReader&) = delete;

    /// The next frame in @p frame, without waiting. Closed once the writer
    /// closed the ring, or this end saw it go away (see dataFd()), and every
    /// frame was read, and after close().
    ReadResult read(std::string& frame);
    const std::string& codec() const
    {
        return m_codec;
    }
    std::size_t capacity() const
    {
        return m_capacity;
    }

    /// Readable when the writer wrote after a read() returned Empty, closed
    /// the ring, or went away. Only Empty asks the writer for a signal: a
    /// reader that stops before Empty while frames wait gets no signal for
    /// them, so it reads until Empty before it waits. Call clearDataSignal()
    /// before the reads that drain the ring.
    int dataFd() const
    {
        return m_wakeFd;
    }
    void clearDataSignal();

    /// Tells the writer that nothing reads any more: its writes return
    /// Closed. Later reads return Closed too.
    void close();

private:
    RingReader() = default;

    int m_memoryFd = -1;
    int m_wakeFd = -1;
    void* m_map = nullptr;
    std::size_t m_mapSize = 0;
    std::size_t m_capacity = 0;
    std::uint64_t m_readPos = 0;
    std::uint64_t m_writePosSeen = 0; ///< the last write position loaded from the ring
    pid_t m_owner = 0;
    bool m_corrupt = false;
    bool m_closed = false;
    bool m_writerGone = false;
    std::string m_codec;
};

} // namespace PhosphorChannel
