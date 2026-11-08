// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Stream.h>

#include "Fd.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace PhosphorChannel {

namespace {

// A packet: type (1 byte), flags (1), zero (2), channel id (4), payload
// length (4), then the payload. Little-endian.
constexpr std::size_t PacketHeader = 12;
constexpr std::uint8_t Hello = 1; ///< "PHCHSTRM" and the version
constexpr std::uint8_t Open = 2; ///< the codec tag of a channel that the sender writes
constexpr std::uint8_t Data = 3; ///< a fragment of a frame
constexpr std::uint8_t Credit = 4; ///< bytes that the sender, a reader, grants the writer
constexpr std::uint8_t Close = 5; ///< no frame follows on the channel
constexpr std::uint8_t EndOfFrame = 1; ///< the flag of the last fragment of a frame

constexpr std::string_view HelloMagic = "PHCHSTRM";
// The largest payload that this end reads, whatever the other end's fragment.
constexpr std::size_t MaxPayload = std::size_t(1) << 20;
// How much of the output waits in front of a new packet at most, in
// fragments: a keystroke queued behind a large frame waits for this, not for
// the frame.
constexpr std::size_t OutputFragments = 2;

void put32(std::string& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back(char((v >> (8 * i)) & 0xff));
}

void put64(std::string& out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) out.push_back(char((v >> (8 * i)) & 0xff));
}

std::uint32_t get32(const char* p)
{
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::uint32_t(std::uint8_t(p[i])) << (8 * i);
    return v;
}

std::uint64_t get64(const char* p)
{
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= std::uint64_t(std::uint8_t(p[i])) << (8 * i);
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

bool isSocket(int fd)
{
    struct stat info{};
    return ::fstat(fd, &info) == 0 && S_ISSOCK(info.st_mode);
}

} // namespace

StreamWriter::StreamWriter(Stream& stream, std::uint32_t id, std::string codec)
    : m_stream(stream)
    , m_id(id)
    , m_codec(std::move(codec))
{
}

WriteResult StreamWriter::tryWrite(std::string_view frame)
{
    if (m_closing || !m_stream.open()) return WriteResult::Closed;
    if (m_window != 0 && frame.size() > m_window) return WriteResult::TooLarge;
    if (m_window == 0 || frame.size() > m_credit) {
        m_blocked = true;
        return WriteResult::WouldBlock;
    }
    m_credit -= frame.size();
    m_queue.emplace_back(frame);
    // Out now when the fd takes it: the owner polls for output only while
    // wantsWrite().
    m_stream.writable();
    return WriteResult::Written;
}

WriteResult StreamWriter::write(std::string_view frame, int timeoutMs)
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
        if (!m_stream.pump(wait) && !m_stream.open()) return WriteResult::Closed;
    }
}

void StreamWriter::close()
{
    if (m_closing) return;
    m_closing = true;
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
    if (m_frames.empty()) return m_closed || !m_stream.open() ? ReadResult::Closed : ReadResult::Empty;
    frame = std::move(m_frames.front());
    m_frames.pop_front();
    m_buffered -= frame.size();
    m_stream.returnCredit(*this, frame.size());
    return ReadResult::Frame;
}

Stream::Stream(int readFd, int writeFd, Options options)
    : m_options(options)
    , m_readFd(readFd)
    , m_writeFd(writeFd)
{
    m_options.fragment = std::clamp<std::size_t>(m_options.fragment, 1024, MaxPayload);
    m_options.window = std::max<std::size_t>(m_options.window, 1);
    if (m_readFd < 0 || m_writeFd < 0 || !setNonBlocking(m_readFd) || !setNonBlocking(m_writeFd)) {
        fail("the stream needs two open file descriptors");
        return;
    }
    m_writeIsSocket = isSocket(m_writeFd);
    std::string hello(HelloMagic);
    put32(hello, ProtocolVersion);
    queueControl(Hello, 0, 0, hello);
}

Stream::~Stream()
{
    if (m_readFd >= 0) ::close(m_readFd);
    if (m_writeFd >= 0 && m_writeFd != m_readFd) ::close(m_writeFd);
}

void Stream::fail(std::string reason)
{
    if (m_error.empty()) m_error = std::move(reason);
}

StreamWriter* Stream::openWriter(std::uint32_t id, std::string codec, std::string* error)
{
    const auto refuse = [error](std::string text) -> StreamWriter* {
        if (error) *error = std::move(text);
        return nullptr;
    };
    if (!open()) return refuse("the stream is not open: " + (m_error.empty() ? std::string("its input ended") : m_error));
    if (!validCodec(codec)) return refuse("the codec tag is not 1 to 64 printable ASCII characters without spaces");
    if (m_writers.count(id)) return refuse("channel " + std::to_string(id) + " is open for writing");
    queueControl(Open, 0, id, codec);
    auto& writer = m_writers[id];
    writer.reset(new StreamWriter(*this, id, std::move(codec)));
    writable();
    return writer.get();
}

StreamReader* Stream::reader(std::uint32_t id)
{
    const auto it = m_readers.find(id);
    return it == m_readers.end() ? nullptr : it->second.get();
}

void Stream::queueControl(std::uint8_t type, std::uint8_t flags, std::uint32_t id, std::string_view payload)
{
    packet(m_control, type, flags, id, payload);
}

void Stream::returnCredit(StreamReader& reader, std::size_t bytes)
{
    // Returned in batches: one Credit packet for each frame would cost more
    // than small frames.
    std::size_t& returned = m_returned[reader.m_id];
    returned += bytes;
    if (returned * 4 < m_options.window && !reader.m_frames.empty()) return;
    if (returned == 0) return;
    std::string payload;
    put64(payload, returned);
    returned = 0;
    queueControl(Credit, 0, reader.m_id, payload);
    writable();
}

bool Stream::wantsWrite() const
{
    if (m_outHead < m_out.size() || !m_control.empty()) return true;
    for (const auto& [id, writer] : m_writers) {
        if (!writer->m_queue.empty() || (writer->m_closing && !writer->m_closeSent)) return true;
    }
    return false;
}

void Stream::schedule()
{
    // Control first: hellos, opens and credit are small, and a writer on
    // the other end may wait for them.
    if (!m_control.empty()) {
        m_out.append(m_control);
        m_control.clear();
    }
    // Then one fragment of each writer in turn, while the output is short.
    const std::size_t limit = OutputFragments * (m_options.fragment + PacketHeader);
    bool progress = true;
    while (m_out.size() - m_outHead < limit && progress) {
        progress = false;
        auto it = m_writers.lower_bound(m_nextTurn);
        for (std::size_t n = 0; n < m_writers.size() && m_out.size() - m_outHead < limit; ++n, ++it) {
            if (it == m_writers.end()) it = m_writers.begin();
            StreamWriter& writer = *it->second;
            if (!writer.m_queue.empty()) {
                const std::string& frame = writer.m_queue.front();
                const std::size_t size = std::min(m_options.fragment, frame.size() - writer.m_sent);
                const bool last = writer.m_sent + size == frame.size();
                packet(m_out, Data, last ? EndOfFrame : 0, writer.m_id, std::string_view(frame).substr(writer.m_sent, size));
                writer.m_sent += size;
                if (last) {
                    writer.m_queue.pop_front();
                    writer.m_sent = 0;
                }
                progress = true;
            } else if (writer.m_closing && !writer.m_closeSent) {
                packet(m_out, Close, 0, writer.m_id, {});
                writer.m_closeSent = true;
                progress = true;
            }
            m_nextTurn = it->first + 1;
        }
    }
}

bool Stream::writable()
{
    if (!m_error.empty()) return false;
    while (true) {
        if (m_outHead == m_out.size()) {
            m_out.clear();
            m_outHead = 0;
            schedule();
            if (m_out.empty()) return true;
        }
        const char* p = m_out.data() + m_outHead;
        const std::size_t size = m_out.size() - m_outHead;
        const ssize_t n = m_writeIsSocket ? ::send(m_writeFd, p, size, MSG_NOSIGNAL) : ::write(m_writeFd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return true;
        if (n <= 0) {
            fail(std::string("the stream's output failed: ") + std::strerror(n < 0 ? errno : EPIPE));
            return false;
        }
        m_outHead += std::size_t(n);
    }
}

bool Stream::readable()
{
    if (!m_error.empty()) return false;
    if (m_ended) return false;
    char buffer[65536];
    while (true) {
        const ssize_t n = ::read(m_readFd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) break;
        if (n < 0) {
            fail(std::string("the stream's input failed: ") + std::strerror(errno));
            return false;
        }
        if (n == 0) {
            m_ended = true;
            break;
        }
        m_in.append(buffer, std::size_t(n));
        // Handled as it comes: a fast peer must not grow the buffer without bound.
        while (m_in.size() - m_inHead >= PacketHeader) {
            const char* p = m_in.data() + m_inHead;
            const std::uint8_t type = std::uint8_t(p[0]);
            const std::uint8_t flags = std::uint8_t(p[1]);
            const std::uint32_t id = get32(p + 4);
            const std::uint32_t length = get32(p + 8);
            if (p[2] != 0 || p[3] != 0 || length > MaxPayload) {
                fail("the other end sent a malformed packet");
                return false;
            }
            if (m_in.size() - m_inHead < PacketHeader + length) break;
            if (!handlePacket(type, flags, id, std::string_view(p + PacketHeader, length))) return false;
            m_inHead += PacketHeader + length;
        }
        if (m_inHead == m_in.size()) {
            m_in.clear();
            m_inHead = 0;
        } else if (m_inHead > sizeof buffer) {
            m_in.erase(0, m_inHead);
            m_inHead = 0;
        }
    }
    if (m_ended) {
        if (m_inHead != m_in.size()) fail("the stream ended within a packet");
        for (auto& [id, reader] : m_readers) reader->m_closed = true;
        return false;
    }
    return true;
}

bool Stream::handlePacket(std::uint8_t type, std::uint8_t flags, std::uint32_t id, std::string_view payload)
{
    if (!m_ready) {
        if (type != Hello || payload.size() != HelloMagic.size() + 4 || payload.substr(0, HelloMagic.size()) != HelloMagic) {
            fail("the other end did not begin with a phosphor-channel hello");
            return false;
        }
        const std::uint32_t version = get32(payload.data() + HelloMagic.size());
        if (version != ProtocolVersion) {
            fail(versionMismatch(version));
            return false;
        }
        m_ready = true;
        return true;
    }
    switch (type) {
    case Open: {
        if (m_readers.count(id) || !validCodec(payload) || m_readers.size() >= m_options.maxChannels) {
            fail("the other end opened channel " + std::to_string(id) + " twice, with a bad codec tag, or past the limit of "
                 + std::to_string(m_options.maxChannels) + " channels");
            return false;
        }
        auto& reader = m_readers[id];
        reader.reset(new StreamReader(*this, id, std::string(payload)));
        std::string credit;
        put64(credit, m_options.window);
        queueControl(Credit, 0, id, credit);
        if (onReader) onReader(*reader);
        writable();
        return true;
    }
    case Data: {
        StreamReader* reader = this->reader(id);
        if (!reader || reader->m_closed) {
            fail("the other end sent data on channel " + std::to_string(id) + ", which is not open");
            return false;
        }
        // The window bounds what waits here, whatever the other end does.
        if (reader->m_buffered + payload.size() > m_options.window) {
            fail("the other end wrote past the window of channel " + std::to_string(id));
            return false;
        }
        reader->m_partial.append(payload);
        reader->m_buffered += payload.size();
        if (flags & EndOfFrame) {
            reader->m_frames.push_back(std::move(reader->m_partial));
            reader->m_partial.clear();
        }
        return true;
    }
    case Credit: {
        const auto it = m_writers.find(id);
        if (it == m_writers.end() || payload.size() != 8) {
            fail("the other end granted credit on channel " + std::to_string(id) + ", which this end does not write");
            return false;
        }
        StreamWriter& writer = *it->second;
        const std::uint64_t bytes = get64(payload.data());
        // The first grant is the window. Later ones return what was read, and
        // never take the credit past the window.
        if (writer.m_window == 0) writer.m_window = std::size_t(bytes);
        if (bytes > writer.m_window - writer.m_credit) {
            fail("the other end granted more than the window of channel " + std::to_string(id));
            return false;
        }
        writer.m_credit += std::size_t(bytes);
        if (writer.m_blocked) {
            writer.m_blocked = false;
            if (onWritable) onWritable(writer);
        }
        return true;
    }
    case Close: {
        StreamReader* reader = this->reader(id);
        if (!reader || reader->m_closed || !reader->m_partial.empty()) {
            fail("the other end closed channel " + std::to_string(id) + " within a frame, or twice");
            return false;
        }
        reader->m_closed = true;
        return true;
    }
    default:
        fail("the other end sent a packet of unknown type " + std::to_string(type));
        return false;
    }
}

bool Stream::pump(int timeoutMs)
{
    if (!open()) return false;
    pollfd fds[2] = {{m_readFd, POLLIN, 0}, {m_writeFd, POLLOUT, 0}};
    const nfds_t count = wantsWrite() ? (m_readFd == m_writeFd ? 1 : 2) : 1;
    if (count == 1 && wantsWrite()) fds[0].events |= POLLOUT;
    const int ready = ::poll(fds, count, timeoutMs);
    if (ready < 0 && errno != EINTR) {
        fail(std::string("poll failed: ") + std::strerror(errno));
        return false;
    }
    if (ready <= 0) return true;
    bool ok = true;
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) ok = readable();
    if (ok && (((fds[0].revents & POLLOUT)) || (count == 2 && (fds[1].revents & (POLLOUT | POLLERR | POLLHUP))))) ok = writable();
    return ok && open();
}

bool Stream::flush(int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    while (wantsWrite()) {
        int wait = -1;
        if (timeoutMs >= 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) return false;
            wait = int(left.count());
        }
        if (!writable()) return false;
        if (!wantsWrite()) break;
        pollfd out{m_writeFd, POLLOUT, 0};
        if (::poll(&out, 1, wait) < 0 && errno != EINTR) return false;
    }
    return m_error.empty();
}

} // namespace PhosphorChannel
