// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <PhosphorChannel/Handshake.h>

#include "Fd.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

namespace PhosphorChannel {

namespace {

// The offer: magic, version, kind, codec length, the codec, with the memfd,
// the data fd and the space fd. The answer: magic, version, status.
constexpr std::uint32_t HandshakeMagic = 0x48434850; // "PHCH"
constexpr std::uint32_t RingOffer = 1;
constexpr std::uint32_t Accepted = 0;
constexpr std::uint32_t Refused = 1;
constexpr std::size_t RingFds = 3;

using Clock = std::chrono::steady_clock;

int left(Clock::time_point deadline, int timeoutMs)
{
    if (timeoutMs < 0) return -1;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return ms > 0 ? int(ms) : 0;
}

bool sendAll(int socket, const void* bytes, std::size_t size, std::string* error)
{
    const char* p = static_cast<const char*>(bytes);
    while (size) {
        const ssize_t n = ::send(socket, p, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            pollfd w{socket, POLLOUT, 0};
            ::poll(&w, 1, 1000);
            continue;
        }
        if (n <= 0) {
            setError(error, std::string("the handshake socket failed: ") + std::strerror(errno));
            return false;
        }
        p += n;
        size -= std::size_t(n);
    }
    return true;
}

/// Reads exactly @p size bytes. The fds that come with them, at most
/// @p maxFds, go to @p fds; the ones over the limit are closed.
bool receiveAll(int socket, void* bytes, std::size_t size, Clock::time_point deadline, int timeoutMs, std::string* error,
                std::array<int, RingFds>* fds = nullptr, std::size_t* fdCount = nullptr)
{
    char* p = static_cast<char*>(bytes);
    while (size) {
        if (!waitReadable(socket, left(deadline, timeoutMs))) {
            setError(error, "the handshake timed out");
            return false;
        }
        iovec io{p, size};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 8)];
        msghdr message{};
        message.msg_iov = &io;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof control;
        const ssize_t n = ::recvmsg(socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        for (cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            const std::size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (std::size_t i = 0; i < count; ++i) {
                int fd = -1;
                std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
                if (fds && fdCount && *fdCount < RingFds) (*fds)[(*fdCount)++] = fd;
                else ::close(fd);
            }
        }
        if (message.msg_flags & MSG_CTRUNC) {
            setError(error, "the handshake brought more file descriptors than a ring has");
            return false;
        }
        if (n == 0) {
            setError(error, "the other end closed the handshake socket");
            return false;
        }
        if (n < 0) {
            setError(error, std::string("the handshake socket failed: ") + std::strerror(errno));
            return false;
        }
        p += n;
        size -= std::size_t(n);
        // Fds come only with the first bytes of a message.
        fds = nullptr;
    }
    return true;
}

void answer(int socket, std::uint32_t status)
{
    const std::uint32_t reply[3] = {HandshakeMagic, ProtocolVersion, status};
    sendAll(socket, reply, sizeof reply, nullptr);
}

} // namespace

bool offerRing(int socket, const RingWriter& ring, int timeoutMs, std::string* error)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    const std::uint32_t header[4] = {HandshakeMagic, ProtocolVersion, RingOffer, std::uint32_t(ring.codec().size())};
    std::string message(reinterpret_cast<const char*>(header), sizeof header);
    message += ring.codec();
    const int fds[RingFds] = {ring.memoryFd(), ring.dataFd(), ring.spaceFd()};
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
    do {
        sent = ::sendmsg(socket, &out, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        setError(error, std::string("cannot send the ring: ") + std::strerror(errno));
        return false;
    }
    // The fds went with the first bytes. The rest, if the kernel took less.
    if (std::size_t(sent) < message.size() && !sendAll(socket, message.data() + sent, message.size() - std::size_t(sent), error)) {
        return false;
    }
    std::uint32_t reply[3] = {};
    if (!receiveAll(socket, reply, sizeof reply, deadline, timeoutMs, error)) return false;
    if (reply[0] != HandshakeMagic) {
        setError(error, "the other end answered with something that is not a phosphor-channel handshake");
        return false;
    }
    if (reply[1] != ProtocolVersion) {
        setError(error, versionMismatch(reply[1]));
        return false;
    }
    if (reply[2] != Accepted) {
        setError(error, "the other end refused the ring");
        return false;
    }
    return true;
}

std::unique_ptr<RingReader> acceptRing(int socket, int timeoutMs, std::string* error)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    std::uint32_t header[4] = {};
    std::array<int, RingFds> fds{-1, -1, -1};
    std::size_t fdCount = 0;
    const auto closeFds = [&] {
        for (std::size_t i = 0; i < fdCount; ++i) ::close(fds[i]);
        fdCount = 0;
    };
    if (!receiveAll(socket, header, sizeof header, deadline, timeoutMs, error, &fds, &fdCount)) {
        closeFds();
        return nullptr;
    }
    if (header[0] != HandshakeMagic) {
        closeFds();
        setError(error, "the other end sent something that is not a phosphor-channel handshake");
        return nullptr;
    }
    if (header[1] != ProtocolVersion) {
        closeFds();
        // Answered with this end's version, so the other end names both too.
        answer(socket, Refused);
        setError(error, versionMismatch(header[1]));
        return nullptr;
    }
    if (header[2] != RingOffer || header[3] == 0 || header[3] > MaxCodecBytes || fdCount != RingFds) {
        closeFds();
        answer(socket, Refused);
        setError(error, "the ring offer is malformed");
        return nullptr;
    }
    std::string codec(header[3], '\0');
    if (!receiveAll(socket, codec.data(), codec.size(), deadline, timeoutMs, error)) {
        closeFds();
        return nullptr;
    }
    if (!validCodec(codec)) {
        closeFds();
        answer(socket, Refused);
        setError(error, "the ring offer has a bad codec tag");
        return nullptr;
    }
    auto ring = RingReader::attach(fds[0], fds[1], fds[2], std::move(codec), error);
    answer(socket, ring ? Accepted : Refused);
    return ring;
}

} // namespace PhosphorChannel
