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

// The shared-memory ring: a memfd that both processes map, sealed so that its
// size can never change (F_SEAL_SHRINK, F_SEAL_GROW, F_SEAL_SEAL). It cannot
// be sealed against writes, because both ends write their positions. The
// header keeps the write and the read position on separate cache lines. The
// writer writes whole frames, and a skip record pads the wrap.
//
// Two eventfds wake the ends: the data fd wakes the reader after a write, and
// the space fd wakes the writer after a read freed space. An end signals only
// when the other one said that it sleeps, so a busy ring makes no system call.
//
// The other process can change the shared memory at any time. The reader
// checks each position and length that it reads, copies each frame out
// before it returns it, and reports ReadResult::Corrupt for a ring that
// breaks the layout. A crash of the other process sets no flag in the ring:
// the owner of the channel learns it from its control connection.

namespace PhosphorChannel {

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

    /// Writes one frame without waiting.
    WriteResult tryWrite(std::string_view frame);
    /// Waits up to @p timeoutMs (-1: no limit) for space. WouldBlock after
    /// the timeout.
    WriteResult write(std::string_view frame, int timeoutMs);
    /// The largest frame that the ring takes.
    std::size_t maxFrame() const;
    const std::string& codec() const { return m_codec; }

    /// Readable when the reader freed space after a WouldBlock. Read it with
    /// clearSpaceSignal().
    int spaceFd() const { return m_spaceFd; }
    void clearSpaceSignal();

    /// Tells the reader that no frame follows. Frames written before still
    /// reach it.
    void close();

    /// For offerRing: the memfd and the two eventfds.
    int memoryFd() const { return m_memoryFd; }
    int dataFd() const { return m_dataFd; }

private:
    RingWriter() = default;

    int m_memoryFd = -1;
    int m_dataFd = -1;
    int m_spaceFd = -1;
    void* m_map = nullptr;
    std::size_t m_mapSize = 0;
    std::size_t m_capacity = 0;
    std::uint64_t m_writePos = 0;
    bool m_closed = false;
    std::string m_codec;
};

class PHOSPHORCHANNEL_EXPORT RingReader
{
public:
    /// Maps the ring of a writer. Takes the three fds, also on a failure.
    /// Refuses a memfd without the size seals or with a size that the header
    /// does not state. @p codec is what the handshake said.
    static std::unique_ptr<RingReader> attach(int memoryFd, int dataFd, int spaceFd, std::string codec, std::string* error);
    ~RingReader();
    RingReader(const RingReader&) = delete;
    RingReader& operator=(const RingReader&) = delete;

    /// The next frame in @p frame, without waiting.
    ReadResult read(std::string& frame);
    const std::string& codec() const { return m_codec; }

    /// Readable after the writer wrote. Read it with clearDataSignal()
    /// before the reads that drain the ring.
    int dataFd() const { return m_dataFd; }
    void clearDataSignal();

    /// Tells the writer that nothing reads any more: its writes return Closed.
    void close();

private:
    RingReader() = default;

    int m_memoryFd = -1;
    int m_dataFd = -1;
    int m_spaceFd = -1;
    void* m_map = nullptr;
    std::size_t m_mapSize = 0;
    std::size_t m_capacity = 0;
    std::uint64_t m_readPos = 0;
    bool m_corrupt = false;
    std::string m_codec;
};

} // namespace PhosphorChannel
