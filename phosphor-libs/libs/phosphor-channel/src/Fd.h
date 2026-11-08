// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cerrno>
#include <chrono>
#include <string>
#include <utility>

#include <fcntl.h>
#include <poll.h>

// The library is Linux-only (memfd, eventfd, SCM_RIGHTS). There EWOULDBLOCK
// is EAGAIN, so the code tests EAGAIN alone.

namespace PhosphorChannel {

/// Whether @p fd became readable within @p timeoutMs (-1: no limit). A
/// hangup or an error counts: the next read reports it.
inline bool waitReadable(int fd, int timeoutMs)
{
    pollfd p{fd, POLLIN, 0};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    while (true) {
        int wait = -1;
        if (timeoutMs >= 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            wait = left.count() > 0 ? int(left.count()) : 0;
        }
        const int ready = ::poll(&p, 1, wait);
        if (ready > 0) return true;
        if (ready == 0) return false;
        if (errno != EINTR) return false;
    }
}

inline void setError(std::string* error, std::string text)
{
    if (error) *error = std::move(text);
}

inline bool setNonBlocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && (flags & O_NONBLOCK || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

} // namespace PhosphorChannel
