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
#include <set>
#include <string>
#include <string_view>

// Several channels over one connected byte stream: a stream socket, or the
// standard input and output of a process such as ssh, which passes no file
// descriptors. The ssh process must not allocate a terminal (no -t): a
// terminal rewrites the bytes. Each end begins with a hello that carries the
// protocol version, and refuses the other end's version when it is not its
// own.
//
// Each channel has its own flow control. The reader grants a window of
// bytes, and the writer sends no more than the window until the reader has
// read frames and returned their bytes. Each frame costs its size plus
// Stream::FrameCost against the window. A full channel returns WouldBlock and
// holds back no other channel. Frames go out in fragments, one fragment of
// each channel in turn, so a large frame on one channel does not wait in
// front of a small one on another for more than a few fragments, plus what
// the kernel and the transport (ssh) already hold.
//
// A channel ends when its writer closes it and its reader has read every
// frame (read() returned Closed). Then its id is free in that direction, and
// the writer may open it again. The other end may open at most
// Options::maxChannels channels at a time, with ids below
// Options::maxChannelId, and the hello tells it both limits. Owners reuse ids
// within the limit, as file descriptors are reused.
//
// tryWrite(), readable() and writable() never block. write(), pump(),
// flush() and shutdown() block with their own poll. The owner polls readFd()
// for input and writeFd() for output while wantsWrite(), and calls readable()
// and writable(). Any call can queue output, reads included (a read returns
// credit), so the owner checks wantsWrite() before each poll. Frames that
// wait in a reader keep the window full and readFd() quiet: after each call
// into the stream, the owner reads every reader until read() returns
// something other than Frame, or does so in onReadable.
//
// Destroying the stream closes its file descriptors at once, and what still
// waits to go out is lost. shutdown() ends a stream cleanly.
//
// All values on the wire are little-endian, so two computers of any byte
// order agree.

namespace PhosphorChannel {

class Stream;

/// The writing end of one channel of a stream. The stream owns it. The
/// pointer stays valid until the stream is destroyed, or until openWriter()
/// opens its id again after the channel ended.
class PHOSPHORCHANNEL_EXPORT StreamWriter
{
public:
    StreamWriter(const StreamWriter&) = delete;
    StreamWriter& operator=(const StreamWriter&) = delete;

    std::uint32_t id() const
    {
        return m_id;
    }
    const std::string& codec() const
    {
        return m_codec;
    }

    /// Queues one frame without waiting. WouldBlock when the frame and its
    /// cost do not fit the window that the reader has left: onWritable says
    /// when it may. Closed after close() or once the stream cannot write.
    WriteResult tryWrite(std::string_view frame);
    /// Waits up to @p timeoutMs (-1: no limit) for the window, and runs the
    /// stream meanwhile, callbacks included. Two ends that both wait in
    /// write() get no window unless their callbacks read what arrives.
    /// Closed once the input ended, since the window comes back through it.
    WriteResult write(std::string_view frame, int timeoutMs);
    /// The largest frame: the window less Stream::FrameCost. Zero before the
    /// reader granted a window.
    std::size_t maxFrame() const;
    /// No frame follows. The queued ones still go out.
    void close();

private:
    friend class Stream;
    StreamWriter(Stream& stream, std::uint32_t id, std::string codec);

    Stream& m_stream;
    std::uint32_t m_id;
    std::string m_codec;
    std::size_t m_window = 0; ///< what this end uses of the reader's grant
    std::size_t m_credit = 0; ///< what is left of it
    std::deque<std::string> m_queue; ///< frames, the first one partly sent
    std::size_t m_sent = 0; ///< bytes of m_queue.front() sent
    bool m_granted = false; ///< the reader granted the window
    bool m_closing = false;
    bool m_closeSent = false;
    bool m_released = false; ///< the reader read every frame: the id is free
    bool m_blocked = false; ///< a write returned WouldBlock
};

/// The reading end of one channel of a stream. The stream owns it. The
/// pointer stays valid until the stream is destroyed, or until the other end
/// opens its id again after read() returned Closed.
class PHOSPHORCHANNEL_EXPORT StreamReader
{
public:
    StreamReader(const StreamReader&) = delete;
    StreamReader& operator=(const StreamReader&) = delete;

    std::uint32_t id() const
    {
        return m_id;
    }
    const std::string& codec() const
    {
        return m_codec;
    }

    /// The next complete frame, without waiting. The bytes of the frames
    /// read go back to the writer's window in batches: once a quarter of the
    /// window was read, or when no frame waits. Closed once the writer closed
    /// the channel and every frame was read, which frees the id; Corrupt
    /// once the other end broke the protocol and every frame before it was
    /// read.
    ReadResult read(std::string& frame);

private:
    friend class Stream;
    StreamReader(Stream& stream, std::uint32_t id, std::string codec);

    Stream& m_stream;
    std::uint32_t m_id;
    std::string m_codec;
    std::deque<std::string> m_frames;
    std::string m_partial;
    std::size_t m_peerCredit = 0; ///< what the writer may still send
    std::size_t m_unreturned = 0; ///< read, and not yet returned to the writer
    std::size_t m_creditToSend = 0; ///< returned, and not yet sent
    bool m_inFrame = false; ///< a frame began and did not end
    bool m_closed = false; ///< the writer closed the channel
    bool m_released = false; ///< every frame was read and the writer was told
};

class PHOSPHORCHANNEL_EXPORT Stream
{
public:
    /// What a frame costs against the window on top of its size, so that a
    /// window also bounds the number of small frames that wait.
    static constexpr std::size_t FrameCost = 64;
    /// The smallest window.
    static constexpr std::size_t MinWindow = 1024;

    struct Options
    {
        /// The window, in bytes, that this end grants each channel that it
        /// reads. The largest frame of such a channel is the window less
        /// FrameCost. It also caps the window that this end uses on a channel
        /// that it writes, whatever the other end grants. At least MinWindow.
        /// The other end can make this end hold up to maxChannels times the
        /// window in frames, plus what the allocator rounds up.
        std::size_t window = std::size_t(4) << 20;
        /// The largest fragment that this end sends: 1 KiB to 1 MiB.
        std::size_t fragment = 16 * 1024;
        /// The most channels that the other end may have open at once. At
        /// least 1.
        std::size_t maxChannels = 64;
        /// The ids of the channels that the other end opens are below this.
        /// An ended channel keeps a small record until its id opens again,
        /// so this bounds what the other end can make this end keep. At
        /// least 1.
        std::uint32_t maxChannelId = 4096;
    };

    /// Takes @p readFd and @p writeFd (the same fd for a socket), makes them
    /// non-blocking, and queues the hello. The non-blocking flag lives in the
    /// open file description, which other processes may share, as a shell
    /// shares its terminal: hand in fds that nothing else uses. Destruction
    /// restores the flag and closes the fds, except that it points a taken
    /// fd 0, 1 or 2 at /dev/null, so that the number stays taken.
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
    /// id that is open or still closing, an id at or past the other end's
    /// maxChannelId, a bad codec, more channels than the other end takes, or
    /// a stream that cannot write. Before ready() the other end's limits are
    /// unknown: a writer opened then past one of them fails the stream when
    /// the hello comes. Opening an id again after its channel ended
    /// invalidates the old writer.
    StreamWriter* openWriter(std::uint32_t id, std::string codec, std::string* error);
    /// The channel @p id that the other end opened, or nullptr.
    StreamReader* reader(std::uint32_t id);

    // The callbacks run after the stream handled what it read, never in the
    // middle of a packet, in the order of the packets. They may call into the
    // stream, and may destroy it: the call that ran them then returns false,
    // or Closed, at once. A callback that throws leaves the call that ran it;
    // the callbacks after it run in the next readable() or pump().

    /// The other end opened a channel.
    std::function<void(StreamReader&)> onReader;
    /// A frame arrived on a reader, its writer closed it, or the stream
    /// ended or failed: read it until read() returns something other than
    /// Frame. A failure in the owner's own call sets error() at once:
    /// tryWrite() returns Closed and openWriter() nullptr, while read() still
    /// returns the frame that it took. Its onReadable comes with the next
    /// readable() or pump(), which run it even while readFd() is quiet: an
    /// owner with its own poll calls readable() once error() is set, without
    /// waiting for readFd().
    std::function<void(StreamReader&)> onReadable;
    /// A writer that returned WouldBlock may write again.
    std::function<void(StreamWriter&)> onWritable;

    int readFd() const
    {
        return m_readFd;
    }
    /// -1 once the stream gave back an output fd separate from its input:
    /// after shutdown() got past its flush and ended the output, or after a
    /// failure that it tried to report to the other end. It reports every
    /// failure except one of its output, a closed fd that pump()'s poll
    /// found, the other end's own stop, and a refusal of the fds in the
    /// constructor. With one fd for both, as for a socket, this stays that
    /// fd. Otherwise the fd stays open until the stream is destroyed. Stop
    /// watching the old number once this is -1: it may be reused.
    int writeFd() const
    {
        return m_writeFd;
    }
    bool wantsWrite() const;
    /// Reads what the fd has, up to a budget, and handles it. Poll again
    /// while the fd stays readable. False when the input ended or the
    /// stream failed: error() says which.
    bool readable();
    /// Writes what waits. False when the stream failed.
    bool writable();
    /// One round of poll on readFd(), and on writeFd() while wantsWrite(),
    /// up to @p timeoutMs. False when the stream failed, or its input ended
    /// and nothing waits to go out.
    bool pump(int timeoutMs);
    /// Writes until nothing waits, up to @p timeoutMs, and reads meanwhile.
    /// False on a failure or the timeout: error() is empty after a timeout.
    bool flush(int timeoutMs);
    /// Ends the stream cleanly: flushes, ends this end's output, and reads
    /// until the other end ends too, up to @p timeoutMs. False on a failure or
    /// the timeout. Once it ended the output, no writer can write.
    bool shutdown(int timeoutMs);

    /// The hello of the other end arrived, with this end's version.
    bool ready() const
    {
        return m_ready;
    }
    /// The stream works: no failure and no end of its input. After the other
    /// end ends its output, this end may still write until it fails.
    bool open() const
    {
        return m_error.empty() && !m_ended;
    }
    /// Why the stream failed. Empty while it works, and after a clean end.
    const std::string& error() const
    {
        return m_error;
    }

private:
    friend class StreamWriter;
    friend class StreamReader;

    enum class Farewell {
        Send,
        Skip
    };
    enum class EventKind {
        Opened,
        Readable,
        Writable
    };
    struct Event
    {
        EventKind kind;
        std::uint32_t id;
    };

    bool canWrite() const
    {
        return m_error.empty() && !m_outputEnded;
    }
    void fail(std::string reason, Farewell farewell = Farewell::Send);
    void refusePeer(std::string reason);
    void sendFarewell();
    void endOutput();
    bool parseInput();
    bool handlePacket(std::uint8_t type, std::uint8_t flags, std::uint32_t id, std::string_view payload);
    bool handleHello(std::uint8_t type, std::string_view payload);
    std::string helloMissing() const;
    bool dispatch();
    void queueControl(std::uint8_t type, std::uint32_t id, std::string_view payload);
    void schedule();
    void returnCredit(StreamReader& reader, std::size_t bytes);
    void release(StreamReader& reader);
    void announceEnd();
    long writeOut(const char* bytes, std::size_t size);

    Options m_options;
    int m_readFd = -1;
    int m_writeFd = -1;
    int m_readFlags = -1; ///< the file status flags that readFd had
    int m_writeFlags = -1;
    bool m_writeIsSocket = false; ///< send() with MSG_NOSIGNAL, so that a closed peer is an error and not SIGPIPE
    std::unique_ptr<char[]> m_readBuffer;
    std::string m_in;
    std::size_t m_inHead = 0;
    std::string m_out; ///< packets ready for the fd
    std::size_t m_outHead = 0;
    std::string m_control; ///< packets that go before the next fragment
    std::set<std::uint32_t> m_creditDue; ///< readers with credit to send
    std::set<std::uint32_t> m_releaseDue; ///< readers whose Release waits to go out
    std::set<std::uint32_t> m_sending; ///< writers with a frame or a close to send
    std::map<std::uint32_t, std::unique_ptr<StreamWriter>> m_writers;
    std::map<std::uint32_t, std::unique_ptr<StreamReader>> m_readers;
    std::deque<Event> m_events; ///< callbacks that wait for the end of the packets
    std::shared_ptr<bool> m_alive; ///< false once the stream is destroyed
    std::uint32_t m_nextTurn = 0; ///< the writer whose fragment goes next
    std::size_t m_openReaders = 0; ///< readers not released
    std::size_t m_openWriters = 0; ///< writers not released
    std::size_t m_peerMaxChannels = 0; ///< what the other end's hello said
    std::uint32_t m_peerMaxChannelId = 0;
    bool m_ready = false;
    bool m_helloScheduled = false; ///< the hello left m_control for m_out
    bool m_helloWritten = false; ///< the hello reached the fd
    bool m_ended = false; ///< the input ended
    bool m_outputEnded = false; ///< this end ended its output
    bool m_peerBroke = false; ///< the other end broke the protocol
    std::string m_error;
};

} // namespace PhosphorChannel
