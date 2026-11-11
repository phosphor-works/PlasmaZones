// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorChannel/Channel.h>

#include <cerrno>
#include <chrono>
#include <string>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>

// The library's private helpers. The library is Linux-only (memfd,
// SCM_RIGHTS, MSG_NOSIGNAL). There EWOULDBLOCK is EAGAIN, so the code tests
// EAGAIN alone.

namespace PhosphorChannel {

/// A point in time that a blocking call must not pass. A negative timeout
/// means no limit.
class Deadline
{
public:
    explicit Deadline(int timeoutMs)
        : m_unlimited(timeoutMs < 0)
        , m_end(std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs))
    {
    }

    /// What is left in milliseconds for poll(): -1 without a limit, 0 once
    /// the time ran out.
    int remainingMs() const
    {
        if (m_unlimited)
            return -1;
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(m_end - std::chrono::steady_clock::now());
        return left.count() > 0 ? int(left.count()) : 0;
    }

    bool expired() const
    {
        return remainingMs() == 0;
    }

private:
    bool m_unlimited;
    std::chrono::steady_clock::time_point m_end;
};

enum class Wait {
    Ready,
    TimedOut,
    /// poll failed, or @p fd is not an open file descriptor. errno says why.
    Failed,
};

/// Waits until @p fd has one of @p events, up to @p deadline. A hangup or an
/// error counts as ready: the next read or write reports it.
inline Wait waitFor(int fd, short events, const Deadline& deadline)
{
    if (fd < 0) {
        errno = EBADF;
        return Wait::Failed;
    }
    while (true) {
        pollfd p{fd, events, 0};
        const int ready = ::poll(&p, 1, deadline.remainingMs());
        if (ready > 0) {
            if (p.revents & POLLNVAL) {
                errno = EBADF;
                return Wait::Failed;
            }
            return Wait::Ready;
        }
        if (ready == 0)
            return Wait::TimedOut;
        if (errno != EINTR)
            return Wait::Failed;
    }
}

inline void setError(std::string* error, std::string text)
{
    if (error)
        *error = std::move(text);
}

inline bool setNonBlocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ((flags & O_NONBLOCK) || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

/// Whether @p fd is an AF_UNIX stream socket, the only kind that
/// keeps the byte order of a stream and carries file descriptors.
inline bool isUnixStreamSocket(int fd)
{
    int type = 0;
    int domain = 0;
    socklen_t length = sizeof type;
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 || type != SOCK_STREAM)
        return false;
    length = sizeof domain;
    return ::getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &length) == 0 && domain == AF_UNIX;
}

/// The rule that validCodec() checks, for error messages.
inline std::string codecRule()
{
    return "the codec tag must be 1 to " + std::to_string(MaxCodecBytes) + " printable ASCII characters without spaces";
}

} // namespace PhosphorChannel
