// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorChannel/Ring.h>
#include <PhosphorChannel/phosphorchannel_export.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// The handover of a ring from its writer to its reader over a connected Unix
// stream socket: the memfd and the reader's wake socket go across with
// SCM_RIGHTS, with the protocol version and the codec tag. A raw socket, not
// one of phosphor-ipc: QLocalSocket does not pass file descriptors cleanly.
//
// The writer offers, the reader accepts, and the reader answers whether it
// took the ring. Either end refuses a version that is not its own, and the
// message names both versions. Both calls block up to their timeout, -1 for
// no limit.
//
// A failed handshake can leave bytes of the other end in the socket, and a
// ring that failed after its offer went out may still have a reader. After
// a failure, close the socket and destroy the ring.

namespace PhosphorChannel {

/// What a reader takes.
struct AcceptOptions
{
    /// The largest data area, in bytes.
    std::size_t maxCapacity = RingWriter::MaxCapacity;
    /// The codecs that this end reads. Empty: any.
    std::vector<std::string> codecs;
};

/// Sends @p ring over @p socket and waits for the answer. A ring is offered
/// once, whatever the answer. False with @p error when the reader refused
/// it, the socket failed or the time ran out.
PHOSPHORCHANNEL_EXPORT bool offerRing(int socket, RingWriter& ring, int timeoutMs, std::string* error);

/// Receives a ring over @p socket, maps it, and answers whether it took it.
/// Nullptr with @p error when it was refused or failed.
PHOSPHORCHANNEL_EXPORT std::unique_ptr<RingReader> acceptRing(int socket, const AcceptOptions& options, int timeoutMs,
                                                              std::string* error);

} // namespace PhosphorChannel
