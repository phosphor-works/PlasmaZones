// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Handshake.h>

#include "Fd.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace PhosphorChannel {

namespace {

// The offer: magic, version, kind, codec length, the codec, with the memfd
// and the reader's wake socket. The answer: magic, version, status.
constexpr std::uint32_t HandshakeMagic = 0x48434850; // "PHCH"
constexpr std::uint32_t RingOffer = 1;
constexpr std::uint32_t Accepted = 0;
constexpr std::uint32_t Refused = 1;
constexpr std::uint32_t UnknownCodec = 2;
constexpr std::size_t RingFds = 2;

// Room for the ring's fds and the control messages that a socket may add to
// them: credentials with SO_PASSCRED, a pidfd with SO_PASSPIDFD, a security
// label of a usual length with SO_PASSSEC. A longer label, or more fds, sets
// MSG_CTRUNC, which refuses the offer.
constexpr std::size_t ControlBytes = 512;

// The control message of SO_PASSPIDFD. glibc names it only from 2.39, while
// the kernel headers that enable SO_PASSPIDFD come earlier: the kernel's value,
// the same on every architecture, so that no build leaks the pidfd.
#ifdef SCM_PIDFD
constexpr int PidfdMessage = SCM_PIDFD;
static_assert(SCM_PIDFD == 0x04);
#else
constexpr int PidfdMessage = 0x04;
#endif

std::string socketFailed()
{
    return std::string("the handshake socket failed: ") + std::strerror(errno);
}

/// Refuses a socket that cannot carry a handshake.
bool checkSocket(int socket, std::string* error)
{
    if (socket < 0) {
        setError(error, "the handshake socket is not valid");
        return false;
    }
    if (!isUnixStreamSocket(socket)) {
        setError(error, "the handshake needs a connected Unix stream socket");
        return false;
    }
    return true;
}

/// Waits for @p events on @p socket, with the error for a timeout or a failure.
bool waitSocket(int socket, short events, const Deadline& deadline, std::string* error)
{
    switch (waitFor(socket, events, deadline)) {
    case Wait::Ready:
        return true;
    case Wait::TimedOut:
        setError(error, "the handshake timed out");
        return false;
    case Wait::Failed:
        setError(error, socketFailed());
        return false;
    }
    return false;
}

bool sendAll(int socket, const void* bytes, std::size_t size, const Deadline& deadline, std::string* error)
{
    const char* p = static_cast<const char*>(bytes);
    while (size) {
        const ssize_t n = ::send(socket, p, size, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN) {
            if (!waitSocket(socket, POLLOUT, deadline, error))
                return false;
            continue;
        }
        if (n <= 0) {
            setError(error, socketFailed());
            return false;
        }
        p += n;
        size -= std::size_t(n);
    }
    return true;
}

/// The fds that came with a message: the first RingFds of them, and how many
/// came in all.
struct ReceivedFds
{
    std::array<int, RingFds> fds{-1, -1};
    std::size_t count = 0;

    void take(int fd)
    {
        if (count < RingFds)
            fds[count] = fd;
        else
            ::close(fd);
        ++count;
    }

    void close()
    {
        for (std::size_t i = 0; i < std::min(count, RingFds); ++i)
            ::close(fds[i]);
        count = 0;
    }
};

/// Reads exactly @p size bytes. Fds that come with them go to @p fds; with no
/// @p fds, they are closed.
bool receiveAll(int socket, void* bytes, std::size_t size, const Deadline& deadline, std::string* error,
                ReceivedFds* fds = nullptr)
{
    char* p = static_cast<char*>(bytes);
    while (size) {
        if (!waitSocket(socket, POLLIN, deadline, error))
            return false;
        iovec io{p, size};
        alignas(cmsghdr) char control[ControlBytes] = {};
        msghdr message{};
        message.msg_iov = &io;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof control;
        const ssize_t n = ::recvmsg(socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        // A failed recvmsg leaves the control buffer as it was: no message.
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            setError(error, socketFailed());
            return false;
        }
        for (cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
            if (c->cmsg_level != SOL_SOCKET)
                continue;
            const bool rights = c->cmsg_type == SCM_RIGHTS;
            const bool pidfd = c->cmsg_type == PidfdMessage;
            if (!rights && !pidfd)
                continue;
            const std::size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (std::size_t i = 0; i < count; ++i) {
                int fd = -1;
                std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
                if (rights && fds)
                    fds->take(fd);
                else
                    ::close(fd);
            }
        }
        if (message.msg_flags & MSG_CTRUNC) {
            setError(error,
                     "file descriptors of the handshake were dropped: too many came, or this process has no free "
                     "descriptor");
            return false;
        }
        if (n == 0) {
            setError(error, "the other end closed the handshake socket");
            return false;
        }
        p += n;
        size -= std::size_t(n);
    }
    return true;
}

bool answer(int socket, std::uint32_t status, const Deadline& deadline, std::string* error = nullptr)
{
    const std::uint32_t reply[3] = {HandshakeMagic, ProtocolVersion, status};
    return sendAll(socket, reply, sizeof reply, deadline, error);
}

} // namespace

bool offerRing(int socket, RingWriter& ring, int timeoutMs, std::string* error)
{
    if (!checkSocket(socket, error))
        return false;
    if (ring.m_offered) {
        setError(error, "the ring was offered already, and a reader may hold it");
        return false;
    }
    const Deadline deadline(timeoutMs);
    const std::uint32_t header[4] = {HandshakeMagic, ProtocolVersion, RingOffer, std::uint32_t(ring.codec().size())};
    std::string message(reinterpret_cast<const char*>(header), sizeof header);
    message += ring.codec();
    const int fds[RingFds] = {ring.m_memoryFd, ring.m_peerWakeFd};
    iovec io{message.data(), message.size()};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof fds)] = {};
    msghdr out{};
    out.msg_iov = &io;
    out.msg_iovlen = 1;
    out.msg_control = control;
    out.msg_controllen = sizeof control;
    cmsghdr* c = CMSG_FIRSTHDR(&out);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof fds);
    std::memcpy(CMSG_DATA(c), fds, sizeof fds);
    ssize_t sent = -1;
    while (true) {
        sent = ::sendmsg(socket, &out, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent >= 0)
            break;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN) {
            if (!waitSocket(socket, POLLOUT, deadline, error))
                return false;
            continue;
        }
        setError(error, std::string("cannot send the ring: ") + std::strerror(errno));
        return false;
    }
    // The fds went with the first bytes: from here on a reader may hold the
    // ring. The writer gives up the reader's socket now, whatever the answer:
    // a reader that took it and dies then closes the last copy, and the
    // writer sees it go.
    ring.m_offered = true;
    ring.handedOver();
    if (std::size_t(sent) < message.size()
        && !sendAll(socket, message.data() + sent, message.size() - std::size_t(sent), deadline, error)) {
        return false;
    }
    std::uint32_t reply[3] = {};
    if (!receiveAll(socket, reply, sizeof reply, deadline, error))
        return false;
    if (reply[0] != HandshakeMagic) {
        setError(error, "the other end answered with something that is not a phosphor-channel handshake");
        return false;
    }
    if (reply[1] != ProtocolVersion) {
        setError(error, versionMismatch(reply[1]));
        return false;
    }
    switch (reply[2]) {
    case Accepted:
        return true;
    case Refused:
        setError(error, "the other end refused the ring");
        return false;
    case UnknownCodec:
        setError(error, "the other end does not read codec " + ring.codec());
        return false;
    default:
        setError(error, "the other end answered with the unknown status " + std::to_string(reply[2]));
        return false;
    }
}

std::unique_ptr<RingReader> acceptRing(int socket, const AcceptOptions& options, int timeoutMs, std::string* error)
{
    if (!checkSocket(socket, error))
        return nullptr;
    const Deadline deadline(timeoutMs);
    std::uint32_t header[4] = {};
    ReceivedFds fds;
    // Every refusal of a phosphor-channel offer is answered, so that the
    // writer does not wait out its timeout. The answer is best effort: the
    // socket may be what failed.
    const auto refuse = [&](std::uint32_t status) -> std::unique_ptr<RingReader> {
        fds.close();
        answer(socket, status, deadline);
        return nullptr;
    };
    if (!receiveAll(socket, header, sizeof header, deadline, error, &fds))
        return refuse(Refused);
    if (header[0] != HandshakeMagic) {
        fds.close();
        setError(error, "the other end sent something that is not a phosphor-channel handshake");
        return nullptr;
    }
    if (header[1] != ProtocolVersion) {
        // Answered with this end's version, so the other end names both too.
        setError(error, versionMismatch(header[1]));
        return refuse(Refused);
    }
    if (header[2] != RingOffer || header[3] == 0 || header[3] > MaxCodecBytes || fds.count != RingFds) {
        setError(error, "the ring offer is malformed");
        return refuse(Refused);
    }
    std::string codec(header[3], '\0');
    if (!receiveAll(socket, codec.data(), codec.size(), deadline, error))
        return refuse(Refused);
    if (!validCodec(codec)) {
        setError(error, "the ring offer has a bad codec tag");
        return refuse(Refused);
    }
    if (!options.codecs.empty()
        && std::find(options.codecs.begin(), options.codecs.end(), codec) == options.codecs.end()) {
        setError(error, "the other end offered codec " + codec + ", which this end does not read");
        return refuse(UnknownCodec);
    }
    auto ring = RingReader::attach(fds.fds[0], fds.fds[1], std::move(codec), options.maxCapacity, error);
    // attach took the fds.
    fds.count = 0;
    if (!ring)
        return refuse(Refused);
    std::string answerError;
    if (!answer(socket, Accepted, deadline, &answerError)) {
        // The writer did not learn that it has a reader: drop the ring.
        setError(error, "cannot answer the ring's writer: " + answerError);
        return nullptr;
    }
    return ring;
}

} // namespace PhosphorChannel
