// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorChannel/Ring.h>
#include <PhosphorChannel/phosphorchannel_export.h>

#include <memory>
#include <string>

// The handover of a ring from its writer to its reader over a connected Unix
// stream socket: the memfd and the two eventfds go across with SCM_RIGHTS,
// with the protocol version and the codec tag. A raw socket, not one of
// phosphor-ipc: QLocalSocket does not pass file descriptors cleanly.
//
// The writer offers, the reader accepts, and the reader answers whether it
// took the ring. Either end refuses a version that is not its own, and the
// message names both versions. Both calls block up to their timeout.

namespace PhosphorChannel {

/// Sends @p ring over @p socket and waits for the answer. False with
/// @p error when the reader refused it, the socket failed or the time ran out.
PHOSPHORCHANNEL_EXPORT bool offerRing(int socket, const RingWriter& ring, int timeoutMs, std::string* error);

/// Receives a ring over @p socket, answers, and maps it. Nullptr with
/// @p error when it was refused or failed.
PHOSPHORCHANNEL_EXPORT std::unique_ptr<RingReader> acceptRing(int socket, int timeoutMs, std::string* error);

} // namespace PhosphorChannel
