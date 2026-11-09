// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Stream.h>

#include "Fd.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace PhosphorChannel {

namespace {

// A packet: type (1 byte), flags (1), zero (2), channel id (4), payload
// length (4), then the payload. Little-endian.
constexpr std::size_t PacketHeader = 12;
constexpr std::uint8_t Hello = 1; ///< "PHCHSTRM", the version, the most channels and the id limit of the sender
constexpr std::uint8_t Open = 2; ///< the codec tag of a channel that the sender writes
constexpr std::uint8_t Data = 3; ///< a fragment of a frame
constexpr std::uint8_t Credit = 4; ///< bytes that the sender, a reader, grants the writer
constexpr std::uint8_t Close = 5; ///< no frame follows on the channel
constexpr std::uint8_t Release = 6; ///< the sender, a reader, read every frame of a closed channel: its id is free
constexpr std::uint8_t Error = 7; ///< why the sender failed; nothing follows
constexpr std::uint8_t EndOfFrame = 1; ///< the flag of the last fragment of a frame

constexpr std::string_view HelloMagic = "PHCHSTRM";
// The hello of this version: the magic, the version, the most channels that
// the sender reads at a time, and the limit of the ids that it reads. A later
// version may add to it, so a longer hello is read, not refused.
constexpr std::size_t HelloBytes = HelloMagic.size() + 12;
constexpr std::size_t HelloPacket = PacketHeader + HelloBytes;
// The largest payload that this end reads, whatever the other end's fragment.
constexpr std::size_t MaxPayload = std::size_t(1) << 20;
// The longest reason that an Error packet carries.
constexpr std::size_t MaxReasonBytes = 256;
// How much of the output waits in user space in front of a new packet at
// most, in fragments. The kernel's buffer and the transport add to it.
constexpr std::size_t OutputFragments = 2;
// What one readable() reads at most: a peer that keeps the fd full does not
// keep the owner from its other work.
constexpr std::size_t ReadChunk = 64 * 1024;
constexpr int ReadRounds = 16;

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

std::uint32_t get32(const char* p)
{
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= std::uint32_t(std::uint8_t(p[i])) << (8 * i);
    return v;
}

std::uint64_t get64(const char* p)
{
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= std::uint64_t(std::uint8_t(p[i])) << (8 * i);
    return v;
}

void packet(std::string& out, std::uint8_t type, std::uint8_t flags, std::uint32_t id, std::string_view payload)
{
    out.push_back(char(type));
    out.push_back(char(flags));
    out.push_back(0);
    out.push_back(0);
    put32(out, id);
    put32(out, std::uint32_t(payload.size()));
    out.append(payload);
}

/// Text from the other end, made safe to show: printable ASCII only.
std::string printable(std::string_view text, std::size_t limit)
{
    std::string out;
    for (const char c : text.substr(0, limit))
        out.push_back(c >= ' ' && c <= '~' ? c : '?');
    return out;
}

std::string channel(std::uint32_t id)
{
    return "channel " + std::to_string(id);
}

/// Blocks SIGPIPE in this thread around a write to a pipe, and takes back a
/// SIGPIPE that the write raised, so that a reader that went away is an
/// EPIPE and not the end of the process. The signal disposition, which the
/// whole process shares, stays as it is.
class SigpipeBlock
{
public:
    SigpipeBlock()
    {
        sigemptyset(&m_pipe);
        sigaddset(&m_pipe, SIGPIPE);
        sigset_t pending;
        sigpending(&pending);
        m_wasPending = sigismember(&pending, SIGPIPE) == 1;
        m_blocked = pthread_sigmask(SIG_BLOCK, &m_pipe, &m_old) == 0;
    }
    ~SigpipeBlock()
    {
        if (!m_blocked)
            return;
        if (!m_wasPending) {
            sigset_t pending;
            sigpending(&pending);
            if (sigismember(&pending, SIGPIPE) == 1) {
                const timespec now{0, 0};
                while (sigtimedwait(&m_pipe, nullptr, &now) < 0 && errno == EINTR) { }
            }
        }
        pthread_sigmask(SIG_SETMASK, &m_old, nullptr);
    }
    SigpipeBlock(const SigpipeBlock&) = delete;
    SigpipeBlock& operator=(const SigpipeBlock&) = delete;

private:
    sigset_t m_pipe;
    sigset_t m_old;
    bool m_wasPending = false;
    bool m_blocked = false;
};

/// Gives back an fd that the stream took: its old status flags, then a close,
/// except that fds 0 to 2 point at /dev/null instead, so that the next file
/// that the process opens does not take the place of its standard streams.
void giveBack(int fd, int flags)
{
    if (fd < 0)
        return;
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags);
    if (fd <= 2) {
        const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null >= 0) {
            ::dup2(null, fd);
            ::close(null);
            return;
        }
    }
    ::close(fd);
}

} // namespace

StreamWriter::StreamWriter(Stream& stream, std::uint32_t id, std::string codec)
    : m_stream(stream)
    , m_id(id)
    , m_codec(std::move(codec))
{
}

std::size_t StreamWriter::maxFrame() const
{
    return m_granted && m_window > Stream::FrameCost ? m_window - Stream::FrameCost : 0;
}

WriteResult StreamWriter::tryWrite(std::string_view frame)
{
    if (m_closing || m_released || !m_stream.canWrite())
        return WriteResult::Closed;
    if (m_granted && frame.size() > maxFrame())
        return WriteResult::TooLarge;
    if (!m_granted || frame.size() + Stream::FrameCost > m_credit) {
        m_blocked = true;
        return WriteResult::WouldBlock;
    }
    m_credit -= frame.size() + Stream::FrameCost;
    m_queue.emplace_back(frame);
    m_stream.m_sending.insert(m_id);
    // Out now when the fd takes it: the owner polls for output only while
    // wantsWrite().
    m_stream.writable();
    return m_stream.canWrite() ? WriteResult::Written : WriteResult::Closed;
}

WriteResult StreamWriter::write(std::string_view frame, int timeoutMs)
{
    const Deadline deadline(timeoutMs);
    // The callbacks that pump() runs may destroy the stream and this writer.
    const auto alive = m_stream.m_alive;
    while (true) {
        const WriteResult result = tryWrite(frame);
        if (result != WriteResult::WouldBlock)
            return result;
        // Credit comes only through the input: once it ended, none will.
        if (m_stream.m_ended)
            return WriteResult::Closed;
        if (deadline.expired())
            return WriteResult::WouldBlock;
        m_stream.pump(deadline.remainingMs());
        if (!*alive)
            return WriteResult::Closed;
        if (!m_stream.canWrite())
            return WriteResult::Closed;
    }
}

void StreamWriter::close()
{
    if (m_closing)
        return;
    m_closing = true;
    m_blocked = false;
    m_stream.m_sending.insert(m_id);
    m_stream.writable();
}

StreamReader::StreamReader(Stream& stream, std::uint32_t id, std::string codec)
    : m_stream(stream)
    , m_id(id)
    , m_codec(std::move(codec))
{
}

ReadResult StreamReader::read(std::string& frame)
{
    if (!m_frames.empty()) {
        frame = std::move(m_frames.front());
        m_frames.pop_front();
        m_stream.returnCredit(*this, frame.size() + Stream::FrameCost);
        return ReadResult::Frame;
    }
    if (m_stream.m_peerBroke)
        return ReadResult::Corrupt;
    if (m_closed) {
        m_stream.release(*this);
        return ReadResult::Closed;
    }
    return m_stream.m_error.empty() && !m_stream.m_ended ? ReadResult::Empty : ReadResult::Closed;
}

Stream::Stream(int readFd, int writeFd, Options options)
    : m_options(options)
    , m_readFd(readFd)
    , m_writeFd(writeFd)
    , m_readBuffer(new char[ReadChunk])
    , m_alive(std::make_shared<bool>(true))
{
    m_options.fragment = std::clamp<std::size_t>(m_options.fragment, 1024, MaxPayload);
    m_options.window = std::max(m_options.window, MinWindow);
    m_options.maxChannels = std::clamp<std::size_t>(m_options.maxChannels, 1, UINT32_MAX);
    m_options.maxChannelId = std::max<std::uint32_t>(m_options.maxChannelId, 1);
    if (m_readFd < 0 || m_writeFd < 0) {
        fail("the stream needs an open file descriptor for input and one for output (the same one for a socket)",
             Farewell::Skip);
        return;
    }
    m_readFlags = ::fcntl(m_readFd, F_GETFL);
    m_writeFlags = m_writeFd == m_readFd ? m_readFlags : ::fcntl(m_writeFd, F_GETFL);
    if (m_readFlags < 0 || m_writeFlags < 0) {
        fail(std::string("the stream's file descriptors are not open: ") + std::strerror(errno), Farewell::Skip);
        return;
    }
    if (::isatty(m_readFd) || ::isatty(m_writeFd)) {
        fail("the stream's input or output is a terminal, which does not carry raw bytes", Farewell::Skip);
        return;
    }
    for (const int fd : {m_readFd, m_writeFd}) {
        struct stat info{};
        int type = 0;
        socklen_t length = sizeof type;
        if (::fstat(fd, &info) == 0 && S_ISSOCK(info.st_mode)
            && (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 || type != SOCK_STREAM)) {
            fail("the stream needs a byte-stream socket, and a socket of records cuts frames", Farewell::Skip);
            return;
        }
    }
    struct stat info{};
    m_writeIsSocket = ::fstat(m_writeFd, &info) == 0 && S_ISSOCK(info.st_mode);
    if (!setNonBlocking(m_readFd) || !setNonBlocking(m_writeFd)) {
        fail(std::string("cannot make the stream non-blocking: ") + std::strerror(errno), Farewell::Skip);
        return;
    }
    std::string hello(HelloMagic);
    put32(hello, ProtocolVersion);
    put32(hello, std::uint32_t(m_options.maxChannels));
    put32(hello, m_options.maxChannelId);
    queueControl(Hello, 0, hello);
}

Stream::~Stream()
{
    *m_alive = false;
    giveBack(m_readFd, m_readFlags);
    if (m_writeFd != m_readFd)
        giveBack(m_writeFd, m_writeFlags);
}

void Stream::fail(std::string reason, Farewell farewell)
{
    if (!m_error.empty())
        return;
    // The callbacks that wait still run: frames that came before the failure
    // can be read, and an owner that reads in onReadable learns of them.
    m_error = std::move(reason);
    announceEnd();
    if (farewell == Farewell::Send)
        sendFarewell();
}

void Stream::announceEnd()
{
    // Every reader now reads to its end, Closed or Corrupt: an owner that
    // reads in onReadable learns of it.
    for (const auto& [id, reader] : m_readers) {
        if (!reader->m_released)
            m_events.push_back({EventKind::Readable, id});
    }
}

void Stream::refusePeer(std::string reason)
{
    m_peerBroke = true;
    fail(std::move(reason));
}

void Stream::sendFarewell()
{
    // Tells the other end why, so that it does not wait for a stream that
    // will never write again. Only what keeps the packets whole goes before
    // the reason: the rest of a packet that is half out, or the hello if it
    // never went. Best effort: no waiting, and the output may be what failed.
    if (m_outputEnded || m_writeFd < 0)
        return;
    std::string out;
    if (!m_helloWritten) {
        if (m_helloScheduled)
            out.append(m_out, m_outHead, HelloPacket - m_outHead);
        else
            out.append(m_control, 0, HelloPacket);
    } else if (m_outHead < m_out.size()) {
        // m_out starts at a packet: walk to the one that m_outHead is in.
        std::size_t start = 0;
        while (start < m_outHead) {
            const std::size_t end = start + PacketHeader + get32(m_out.data() + start + 8);
            if (end > m_outHead) {
                out.append(m_out, m_outHead, end - m_outHead);
                break;
            }
            start = end;
        }
    }
    packet(out, Error, 0, 0, std::string_view(m_error).substr(0, MaxReasonBytes));
    std::size_t done = 0;
    while (done < out.size()) {
        const long n = writeOut(out.data() + done, out.size() - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += std::size_t(n);
    }
    m_out.clear();
    m_outHead = 0;
    m_control.clear();
    endOutput();
}

void Stream::endOutput()
{
    if (m_outputEnded)
        return;
    m_outputEnded = true;
    // On the socket, not the fd: another fd of the same socket, such as the
    // input of Stream(0, 1) under socket activation, must not keep it open.
    if (m_writeIsSocket)
        ::shutdown(m_writeFd, SHUT_WR);
    if (m_writeFd != m_readFd) {
        giveBack(m_writeFd, m_writeFlags);
        m_writeFd = -1;
        // giveBack restored the flags of an open file description that the
        // input may share, a dup of one socket: the input must stay
        // non-blocking, or shutdown()'s reads could block past its timeout.
        if (!m_ended)
            setNonBlocking(m_readFd);
    }
}

long Stream::writeOut(const char* bytes, std::size_t size)
{
    if (m_writeIsSocket)
        return ::send(m_writeFd, bytes, size, MSG_NOSIGNAL | MSG_DONTWAIT);
    ssize_t n = -1;
    int saved = 0;
    {
        const SigpipeBlock block;
        n = ::write(m_writeFd, bytes, size);
        saved = errno;
    }
    errno = saved;
    return n;
}

StreamWriter* Stream::openWriter(std::uint32_t id, std::string codec, std::string* error)
{
    const auto refuse = [error](std::string text) -> StreamWriter* {
        setError(error, std::move(text));
        return nullptr;
    };
    if (!canWrite())
        return refuse("the stream cannot write: " + (m_error.empty() ? std::string("its output ended") : m_error));
    if (!validCodec(codec))
        return refuse(codecRule());
    const auto it = m_writers.find(id);
    if (it != m_writers.end() && !it->second->m_released) {
        return refuse(channel(id)
                      + (it->second->m_closing ? " is still closing: the other end has not read all of it"
                                               : " is open for writing"));
    }
    if (m_peerMaxChannelId && id >= m_peerMaxChannelId)
        return refuse("the other end reads channel ids below " + std::to_string(m_peerMaxChannelId));
    if (m_peerMaxChannels && m_openWriters >= m_peerMaxChannels) {
        return refuse("the other end reads at most " + std::to_string(m_peerMaxChannels) + " channels at a time");
    }
    // Made before the Open is queued, so that a failed allocation of the
    // writer queues nothing.
    std::unique_ptr<StreamWriter> made(new StreamWriter(*this, id, std::move(codec)));
    queueControl(Open, id, made->m_codec);
    writable();
    // The Open itself may be what found the output dead. Then nothing opened,
    // and the writer of an ended channel on this id stays as it was.
    if (!canWrite())
        return refuse("the stream cannot write: " + m_error);
    // The Open went through m_control, which needs no writer: the record comes
    // now. A writer whose channel ended goes, since its id opens again.
    auto& writer = m_writers[id];
    writer = std::move(made);
    ++m_openWriters;
    return writer.get();
}

StreamReader* Stream::reader(std::uint32_t id)
{
    const auto it = m_readers.find(id);
    return it == m_readers.end() ? nullptr : it->second.get();
}

void Stream::queueControl(std::uint8_t type, std::uint32_t id, std::string_view payload)
{
    packet(m_control, type, 0, id, payload);
}

void Stream::returnCredit(StreamReader& reader, std::size_t bytes)
{
    // Credit is for a writer that will write more: not after it closed, and
    // not after the input ended.
    if (reader.m_closed || reader.m_released || m_ended || !m_error.empty())
        return;
    // Returned in batches: one Credit packet for each frame would cost more
    // than small frames.
    reader.m_unreturned += bytes;
    if (reader.m_unreturned * 4 < m_options.window && !reader.m_frames.empty())
        return;
    reader.m_peerCredit += reader.m_unreturned;
    reader.m_creditToSend += reader.m_unreturned;
    reader.m_unreturned = 0;
    m_creditDue.insert(reader.m_id);
    writable();
}

void Stream::release(StreamReader& reader)
{
    if (reader.m_released || m_ended || !canWrite())
        return;
    reader.m_released = true;
    reader.m_creditToSend = 0;
    reader.m_partial = std::string();
    --m_openReaders;
    m_creditDue.erase(reader.m_id);
    // One pending Release per id: an id opens again only after its Release
    // went out, so releases cannot pile up while the other end does not read.
    m_releaseDue.insert(reader.m_id);
    writable();
}

bool Stream::wantsWrite() const
{
    if (!canWrite())
        return false;
    return m_outHead < m_out.size() || !m_control.empty() || !m_creditDue.empty() || !m_releaseDue.empty()
        || !m_sending.empty();
}

void Stream::schedule()
{
    // Control first: hellos, opens, releases and credit are small, and a
    // writer on the other end may wait for them. Releases and credit go as one
    // packet per channel, however often they were queued, so that they cannot
    // pile up.
    if (!m_control.empty()) {
        m_out.append(m_control);
        m_control.clear();
        m_helloScheduled = true;
    }
    for (const std::uint32_t id : m_releaseDue)
        packet(m_out, Release, 0, id, {});
    m_releaseDue.clear();
    for (const std::uint32_t id : m_creditDue) {
        StreamReader* const reader = this->reader(id);
        if (!reader || reader->m_creditToSend == 0)
            continue;
        std::string payload;
        put64(payload, reader->m_creditToSend);
        reader->m_creditToSend = 0;
        packet(m_out, Credit, 0, id, payload);
    }
    m_creditDue.clear();
    // Then one fragment of each writer in turn, while the output is short.
    // Only the writers with something to send take part.
    const std::size_t limit = OutputFragments * (m_options.fragment + PacketHeader);
    while (m_out.size() - m_outHead < limit && !m_sending.empty()) {
        auto it = m_sending.lower_bound(m_nextTurn);
        for (std::size_t n = m_sending.size(); n > 0 && m_out.size() - m_outHead < limit; --n) {
            if (it == m_sending.end())
                it = m_sending.begin();
            StreamWriter& writer = *m_writers.at(*it);
            m_nextTurn = writer.m_id + 1;
            if (!writer.m_queue.empty()) {
                const std::string& frame = writer.m_queue.front();
                const std::size_t size = std::min(m_options.fragment, frame.size() - writer.m_sent);
                const bool last = writer.m_sent + size == frame.size();
                packet(m_out, Data, last ? EndOfFrame : 0, writer.m_id,
                       std::string_view(frame).substr(writer.m_sent, size));
                writer.m_sent += size;
                if (last) {
                    writer.m_queue.pop_front();
                    writer.m_sent = 0;
                }
            } else if (writer.m_closing && !writer.m_closeSent) {
                packet(m_out, Close, 0, writer.m_id, {});
                writer.m_closeSent = true;
            }
            if (writer.m_queue.empty() && (!writer.m_closing || writer.m_closeSent))
                it = m_sending.erase(it);
            else
                ++it;
        }
    }
}

bool Stream::writable()
{
    if (!canWrite())
        return false;
    while (true) {
        if (m_outHead == m_out.size()) {
            m_out.clear();
            m_outHead = 0;
            schedule();
            if (m_out.empty())
                return true;
        }
        const long n = writeOut(m_out.data() + m_outHead, m_out.size() - m_outHead);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return true;
        if (n <= 0) {
            fail(std::string("the stream's output failed: ") + std::strerror(n < 0 ? errno : EPIPE), Farewell::Skip);
            return false;
        }
        m_outHead += std::size_t(n);
        // The first output begins with the hello.
        if (!m_helloWritten && m_helloScheduled && m_outHead >= HelloPacket)
            m_helloWritten = true;
    }
}

bool Stream::readable()
{
    // Callbacks still waiting run first, before anything new: left by one
    // that threw, by the dispatch that this call is nested in, or by a
    // failure outside readable().
    if (!m_events.empty() && !dispatch())
        return false;
    if (!m_error.empty() || m_ended)
        return false;
    for (int round = 0; round < ReadRounds && m_error.empty(); ++round) {
        const ssize_t n = ::read(m_readFd, m_readBuffer.get(), ReadChunk);
        if (n < 0 && errno == EINTR) {
            --round;
            continue;
        }
        if (n < 0 && errno == EAGAIN)
            break;
        if (n < 0) {
            fail(std::string("the stream's input failed: ") + std::strerror(errno));
            break;
        }
        if (n == 0) {
            m_ended = true;
            break;
        }
        m_in.append(m_readBuffer.get(), std::size_t(n));
        // Handled as it comes: a fast peer must not grow the buffer without bound.
        if (!parseInput())
            break;
    }
    if (m_ended && m_error.empty()) {
        if (!m_ready) {
            fail(
                "the stream ended before the other end's hello: the other end is not a phosphor-channel stream, or it "
                "exited");
        } else if (m_inHead != m_in.size()) {
            fail("the stream ended within a packet");
        } else {
            for (const auto& [id, reader] : m_readers) {
                if (reader->m_inFrame) {
                    fail("the stream ended within a frame on " + channel(id));
                    break;
                }
            }
        }
        // A clean end: a failure above already announced it.
        if (m_error.empty())
            announceEnd();
    }
    // Credit and releases that the packets queued go out now.
    if (m_error.empty() && wantsWrite())
        writable();
    if (!dispatch())
        return false;
    return m_error.empty() && !m_ended;
}

bool Stream::parseInput()
{
    while (m_error.empty()) {
        const std::size_t available = m_in.size() - m_inHead;
        const char* p = m_in.data() + m_inHead;
        // A stream that does not begin with a hello is refused at its first
        // byte: a login banner shorter than a packet would wait forever.
        if (!m_ready && available >= 1 && std::uint8_t(p[0]) != Hello) {
            fail(helloMissing());
            break;
        }
        if (available < PacketHeader)
            break;
        const std::uint8_t type = std::uint8_t(p[0]);
        const std::uint8_t flags = std::uint8_t(p[1]);
        const std::uint32_t id = get32(p + 4);
        const std::uint32_t length = get32(p + 8);
        if (p[2] != 0 || p[3] != 0 || length > MaxPayload) {
            if (m_ready)
                refusePeer("the other end sent a malformed packet");
            else
                fail(helloMissing());
            break;
        }
        if (available < PacketHeader + length)
            break;
        // Consumed before it is handled: nothing that handling does can see
        // the same packet again.
        m_inHead += PacketHeader + length;
        if (!handlePacket(type, flags, id, std::string_view(p + PacketHeader, length)))
            break;
    }
    if (m_inHead == m_in.size()) {
        m_in.clear();
        m_inHead = 0;
    } else if (m_inHead > ReadChunk) {
        m_in.erase(0, m_inHead);
        m_inHead = 0;
    }
    return m_error.empty();
}

std::string Stream::helloMissing() const
{
    const std::string start = printable(std::string_view(m_in).substr(m_inHead), 32);
    return "the other end did not begin with a phosphor-channel hello. It sent \"" + start
        + "\" first, perhaps from a login script that writes to the terminal.";
}

bool Stream::handleHello(std::uint8_t type, std::string_view payload)
{
    if (type != Hello || payload.size() < HelloMagic.size() + 4 || payload.substr(0, HelloMagic.size()) != HelloMagic) {
        fail("the other end did not begin with a phosphor-channel hello");
        return false;
    }
    // The version first, so that a later version with a longer hello is
    // named, not called malformed.
    const std::uint32_t version = get32(payload.data() + HelloMagic.size());
    if (version != ProtocolVersion) {
        fail(versionMismatch(version));
        return false;
    }
    if (payload.size() < HelloBytes) {
        refusePeer("the other end sent a hello that is too short");
        return false;
    }
    m_peerMaxChannels = get32(payload.data() + HelloMagic.size() + 4);
    m_peerMaxChannelId = get32(payload.data() + HelloMagic.size() + 8);
    if (m_peerMaxChannels == 0 || m_peerMaxChannelId == 0) {
        refusePeer("the other end reads no channels");
        return false;
    }
    m_ready = true;
    // Writers opened before the hello came could not know its limits.
    if (m_openWriters > m_peerMaxChannels) {
        fail("this end opened " + std::to_string(m_openWriters) + " channels, and the other end reads at most "
             + std::to_string(m_peerMaxChannels) + " at a time");
        return false;
    }
    if (!m_writers.empty() && m_writers.rbegin()->first >= m_peerMaxChannelId) {
        fail("this end opened " + channel(m_writers.rbegin()->first) + ", and the other end reads channel ids below "
             + std::to_string(m_peerMaxChannelId));
        return false;
    }
    return true;
}

bool Stream::handlePacket(std::uint8_t type, std::uint8_t flags, std::uint32_t id, std::string_view payload)
{
    if (!m_ready)
        return handleHello(type, payload);
    if (flags & ~(type == Data ? EndOfFrame : 0)) {
        refusePeer("the other end sent a packet of type " + std::to_string(type) + " with unknown flags");
        return false;
    }
    switch (type) {
    case Hello:
        refusePeer("the other end sent a second hello");
        return false;
    case Open: {
        if (!validCodec(payload)) {
            refusePeer("the other end opened " + channel(id) + " with a bad codec tag");
            return false;
        }
        if (id >= m_options.maxChannelId) {
            refusePeer("the other end opened " + channel(id) + ", and this end reads channel ids below "
                       + std::to_string(m_options.maxChannelId));
            return false;
        }
        const auto it = m_readers.find(id);
        if (it != m_readers.end() && !it->second->m_released) {
            refusePeer("the other end opened " + channel(id) + " twice");
            return false;
        }
        if (m_releaseDue.count(id)) {
            refusePeer("the other end opened " + channel(id) + " again before this end released it");
            return false;
        }
        if (m_openReaders >= m_options.maxChannels) {
            refusePeer("the other end opened more than the " + std::to_string(m_options.maxChannels)
                       + " channels that this end reads at a time");
            return false;
        }
        // A channel whose id ended may open again: the old reader goes.
        auto& reader = m_readers[id];
        reader.reset(new StreamReader(*this, id, std::string(payload)));
        reader->m_peerCredit = m_options.window;
        reader->m_creditToSend = m_options.window;
        ++m_openReaders;
        m_creditDue.insert(id);
        m_events.push_back({EventKind::Opened, id});
        return true;
    }
    case Data: {
        StreamReader* const reader = this->reader(id);
        if (!reader || reader->m_released || reader->m_closed) {
            refusePeer("the other end sent data on " + channel(id) + ", which is not open");
            return false;
        }
        // The window bounds what waits here, whatever the other end does.
        const std::size_t cost = payload.size() + (reader->m_inFrame ? 0 : FrameCost);
        if (cost > reader->m_peerCredit) {
            refusePeer("the other end wrote past the window of " + channel(id));
            return false;
        }
        reader->m_peerCredit -= cost;
        if (flags & EndOfFrame) {
            // A frame in one fragment goes straight to the queue.
            if (reader->m_partial.empty())
                reader->m_frames.emplace_back(payload);
            else
                reader->m_frames.push_back(std::move(reader->m_partial.append(payload)));
            reader->m_partial = std::string();
            reader->m_inFrame = false;
            m_events.push_back({EventKind::Readable, id});
        } else {
            reader->m_partial.append(payload);
            reader->m_inFrame = true;
        }
        return true;
    }
    case Credit: {
        if (payload.size() != 8) {
            refusePeer("the other end sent a malformed credit on " + channel(id));
            return false;
        }
        const auto it = m_writers.find(id);
        if (it == m_writers.end() || it->second->m_released) {
            refusePeer("the other end granted credit on " + channel(id) + ", which this end does not write");
            return false;
        }
        StreamWriter& writer = *it->second;
        const std::uint64_t bytes = get64(payload.data());
        if (!writer.m_granted) {
            // The first grant is the window. This end uses no more of it than
            // its own window, so that a reader cannot make it queue without bound.
            if (bytes == 0) {
                refusePeer("the other end granted an empty window on " + channel(id));
                return false;
            }
            writer.m_window = std::size_t(std::min<std::uint64_t>(bytes, m_options.window));
            writer.m_credit = writer.m_window;
            writer.m_granted = true;
        } else {
            // Later grants return what was read, and never take the credit
            // past the window.
            if (bytes > writer.m_window - writer.m_credit) {
                refusePeer("the other end granted more than the window of " + channel(id));
                return false;
            }
            writer.m_credit += std::size_t(bytes);
        }
        if (writer.m_blocked) {
            writer.m_blocked = false;
            m_events.push_back({EventKind::Writable, id});
        }
        return true;
    }
    case Close: {
        StreamReader* const reader = this->reader(id);
        if (!payload.empty()) {
            refusePeer("the other end sent a malformed close on " + channel(id));
            return false;
        }
        if (!reader || reader->m_released) {
            refusePeer("the other end closed " + channel(id) + ", which is not open");
            return false;
        }
        if (reader->m_closed) {
            refusePeer("the other end closed " + channel(id) + " twice");
            return false;
        }
        if (reader->m_inFrame) {
            refusePeer("the other end closed " + channel(id) + " within a frame");
            return false;
        }
        reader->m_closed = true;
        m_events.push_back({EventKind::Readable, id});
        return true;
    }
    case Release: {
        const auto it = m_writers.find(id);
        if (!payload.empty() || it == m_writers.end() || it->second->m_released || !it->second->m_closeSent) {
            refusePeer("the other end released " + channel(id) + ", which this end did not close");
            return false;
        }
        it->second->m_released = true;
        --m_openWriters;
        return true;
    }
    case Error:
        // The reason is the other end's own words, where "the other end" means this one.
        fail("the other end stopped the stream, saying: \"" + printable(payload, MaxReasonBytes) + "\"",
             Farewell::Skip);
        return false;
    default:
        refusePeer("the other end sent a packet of unknown type " + std::to_string(type));
        return false;
    }
}

bool Stream::dispatch()
{
    // One queue, taken from the front: a callback that runs the stream again
    // adds behind what still waits, so the order holds, and a callback that
    // throws leaves the rest for the next call.
    const auto alive = m_alive;
    while (!m_events.empty()) {
        const Event event = m_events.front();
        m_events.pop_front();
        switch (event.kind) {
        case EventKind::Opened:
            if (StreamReader* const reader = this->reader(event.id); reader && onReader)
                onReader(*reader);
            break;
        case EventKind::Readable:
            if (StreamReader* const reader = this->reader(event.id); reader && onReadable)
                onReadable(*reader);
            break;
        case EventKind::Writable: {
            const auto it = m_writers.find(event.id);
            if (canWrite() && it != m_writers.end() && !it->second->m_released && onWritable)
                onWritable(*it->second);
            break;
        }
        }
        if (!*alive)
            return false;
    }
    return true;
}

bool Stream::pump(int timeoutMs)
{
    // Callbacks still waiting run now, without waiting for input: left by one
    // that threw, by the dispatch that this call is nested in, or by a
    // failure outside readable().
    if (!m_events.empty()) {
        if (!dispatch())
            return false;
        return m_error.empty() && (!m_ended || wantsWrite());
    }
    if (!m_error.empty())
        return false;
    const bool wantIn = !m_ended;
    const bool wantOut = wantsWrite();
    if (!wantIn && !wantOut)
        return false;
    pollfd fds[2] = {};
    nfds_t count = 0;
    int in = -1;
    int out = -1;
    if (wantIn) {
        in = int(count);
        fds[count++] = {m_readFd, POLLIN, 0};
    }
    if (wantOut) {
        if (wantIn && m_writeFd == m_readFd) {
            fds[in].events |= POLLOUT;
            out = in;
        } else {
            out = int(count);
            fds[count++] = {m_writeFd, POLLOUT, 0};
        }
    }
    const int ready = ::poll(fds, count, timeoutMs);
    if (ready < 0 && errno != EINTR) {
        fail(std::string("the stream's poll failed: ") + std::strerror(errno));
        dispatch();
        return false;
    }
    if (ready <= 0)
        return true;
    for (nfds_t i = 0; i < count; ++i) {
        if (fds[i].revents & POLLNVAL) {
            fail("the stream's file descriptor is not open", Farewell::Skip);
            dispatch();
            return false;
        }
    }
    const auto alive = m_alive;
    if (in >= 0 && (fds[in].revents & (POLLIN | POLLHUP | POLLERR))) {
        readable();
        if (!*alive)
            return false;
    }
    if (out >= 0 && (fds[out].revents & (POLLOUT | POLLERR | POLLHUP)))
        writable();
    // A failure of the output announced the readers' end: tell them now.
    if (!dispatch())
        return false;
    return m_error.empty() && (!m_ended || wantsWrite());
}

bool Stream::flush(int timeoutMs)
{
    const Deadline deadline(timeoutMs);
    const auto alive = m_alive;
    while (true) {
        // A write before the first look at the time: flush(0) writes what fits.
        if (!writable())
            return false;
        if (!wantsWrite())
            return true;
        if (deadline.expired())
            return false;
        // pump() reads as well, so that two ends that flush at once do not
        // wait for each other.
        pump(deadline.remainingMs());
        if (!*alive || !m_error.empty())
            return false;
    }
}

bool Stream::shutdown(int timeoutMs)
{
    const Deadline deadline(timeoutMs);
    const auto alive = m_alive;
    if (!flush(deadline.remainingMs()))
        return false;
    endOutput();
    // Read until the other end ends too: closing with its bytes unread would
    // reset the connection, and the other end would read a failure.
    while (!m_ended) {
        if (!m_error.empty() || deadline.expired())
            return false;
        pump(deadline.remainingMs());
        if (!*alive)
            return false;
    }
    return m_error.empty();
}

} // namespace PhosphorChannel
