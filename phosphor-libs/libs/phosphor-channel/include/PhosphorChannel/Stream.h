// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorChannel/Channel.h>
#include <PhosphorChannel/phosphorchannel_export.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

// Several channels over one connected byte stream: a Unix socket, or the
// standard input and output of a process such as ssh, which passes no file
// descriptors. Each end begins with a hello that carries the protocol
// version, and refuses the other end's version when it is not its own.
//
// Each channel has its own flow control. The reader grants a window of
// bytes, and the writer sends no more than the window until the reader has
// read frames and returned their bytes. A full channel returns WouldBlock and
// holds back no other channel. Frames go out in fragments, one fragment of
// each channel in turn, so a large frame on one channel does not delay a
// small one on another.
//
// The stream never blocks. The owner polls readFd() for input and
// writeFd() for output while wantsWrite(), and calls readable() and
// writable(). write() and flush() block with their own poll.
//
// All values on the wire are little-endian, so two computers of any byte
// order agree.

namespace PhosphorChannel {

class Stream;

/// The writing end of one channel of a stream.
class PHOSPHORCHANNEL_EXPORT StreamWriter
{
public:
    std::uint32_t id() const { return m_id; }
    const std::string& codec() const { return m_codec; }

    /// Queues one frame without waiting. WouldBlock when the frame does not
    /// fit the window that the reader has left: Stream::onWritable says when
    /// it may.
    WriteResult tryWrite(std::string_view frame);
    /// Waits up to @p timeoutMs (-1: no limit) for the window, and runs the
    /// stream meanwhile.
    WriteResult write(std::string_view frame, int timeoutMs);
    /// The largest frame: the window of the reader, once it is known. Zero
    /// before the reader granted a window.
    std::size_t maxFrame() const { return m_window; }
    /// No frame follows. The queued ones still go out.
    void close();

private:
    friend class Stream;
    StreamWriter(Stream& stream, std::uint32_t id, std::string codec);

    Stream& m_stream;
    std::uint32_t m_id;
    std::string m_codec;
    std::size_t m_window = 0; ///< what the reader granted in all
    std::size_t m_credit = 0; ///< what is left of it
    std::deque<std::string> m_queue; ///< frames, the first one partly sent
    std::size_t m_sent = 0; ///< bytes of m_queue.front() sent
    bool m_closing = false;
    bool m_closeSent = false;
    bool m_blocked = false; ///< a write returned WouldBlock
};

/// The reading end of one channel of a stream.
class PHOSPHORCHANNEL_EXPORT StreamReader
{
public:
    std::uint32_t id() const { return m_id; }
    const std::string& codec() const { return m_codec; }

    /// The next complete frame, without waiting. Reading it returns its bytes
    /// to the writer's window.
    ReadResult read(std::string& frame);

private:
    friend class Stream;
    StreamReader(Stream& stream, std::uint32_t id, std::string codec);

    Stream& m_stream;
    std::uint32_t m_id;
    std::string m_codec;
    std::deque<std::string> m_frames;
    std::string m_partial;
    std::size_t m_buffered = 0; ///< bytes in m_frames and m_partial
    bool m_closed = false;
};

class PHOSPHORCHANNEL_EXPORT Stream
{
public:
    struct Options
    {
        /// The window that this end grants each channel that it reads, in
        /// bytes. It is also the largest frame of such a channel.
        std::size_t window = std::size_t(4) << 20;
        /// The largest fragment that this end sends.
        std::size_t fragment = 16 * 1024;
        /// The most channels that the other end may open.
        std::size_t maxChannels = 64;
    };

    /// Takes @p readFd and @p writeFd (the same fd for a socket), makes them
    /// non-blocking, and queues the hello.
    Stream(int readFd, int writeFd, Options options);
    explicit Stream(int fd)
        : Stream(fd, fd, Options())
    {
    }
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    /// Opens channel @p id for writing. The ids of each direction are apart:
    /// the other end may write on the same id. Nullptr with @p error for an
    /// id in use, a bad codec or a stream that failed.
    StreamWriter* openWriter(std::uint32_t id, std::string codec, std::string* error);
    /// The channel @p id that the other end opened, or nullptr.
    StreamReader* reader(std::uint32_t id);

    /// The other end opened a channel.
    std::function<void(StreamReader&)> onReader;
    /// A writer that returned WouldBlock may write again.
    std::function<void(StreamWriter&)> onWritable;

    int readFd() const { return m_readFd; }
    int writeFd() const { return m_writeFd; }
    bool wantsWrite() const;
    /// Reads what the fd has and handles it. False when the stream ended or
    /// failed: error() says why.
    bool readable();
    /// Writes what waits. False when the stream failed.
    bool writable();
    /// One round of poll on both fds, up to @p timeoutMs. False when the
    /// stream ended or failed.
    bool pump(int timeoutMs);
    /// Writes until nothing waits, up to @p timeoutMs. False on a failure or
    /// the timeout.
    bool flush(int timeoutMs);

    /// The hello of the other end arrived, with this end's version.
    bool ready() const { return m_ready; }
    /// The stream works: no failure and no end of its input.
    bool open() const { return m_error.empty() && !m_ended; }
    /// Why the stream failed. Empty while it works, and after a clean end.
    const std::string& error() const { return m_error; }

private:
    friend class StreamWriter;
    friend class StreamReader;

    void fail(std::string reason);
    bool handlePacket(std::uint8_t type, std::uint8_t flags, std::uint32_t id, std::string_view payload);
    void queueControl(std::uint8_t type, std::uint8_t flags, std::uint32_t id, std::string_view payload);
    void schedule();
    void returnCredit(StreamReader& reader, std::size_t bytes);

    Options m_options;
    int m_readFd = -1;
    int m_writeFd = -1;
    bool m_writeIsSocket = false; ///< send() with MSG_NOSIGNAL, so that a closed peer is an error and not SIGPIPE
    std::string m_in;
    std::size_t m_inHead = 0;
    std::string m_out; ///< packets ready for the fd
    std::size_t m_outHead = 0;
    std::string m_control; ///< packets that go before the next fragment
    std::map<std::uint32_t, std::unique_ptr<StreamWriter>> m_writers;
    std::map<std::uint32_t, std::unique_ptr<StreamReader>> m_readers;
    std::map<std::uint32_t, std::size_t> m_returned; ///< credit read but not yet returned, by channel
    std::uint32_t m_nextTurn = 0; ///< the writer whose fragment goes next
    bool m_ready = false;
    bool m_ended = false;
    std::string m_error;
};

} // namespace PhosphorChannel
