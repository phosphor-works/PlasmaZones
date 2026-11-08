// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <PhosphorChannel/phosphorchannel_export.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// PhosphorChannel moves frames of opaque bytes from one process to another,
// in order and with backpressure. A channel is unidirectional: one writer
// feeds one reader. Each channel has a codec tag that names the format of its
// payload; the library stores the tag and never reads the payload.
//
// Two transports:
//  - the shared-memory ring (Ring.h, handed over by Handshake.h): one copy
//    per byte, between two processes on one computer;
//  - the stream (Stream.h): several channels over any connected byte stream,
//    such as a Unix socket or the standard input and output of an ssh
//    process, each channel with its own flow control.
//
// No Qt and no other Phosphor library: a headless daemon links it alone.

namespace PhosphorChannel {

/// Covers the handshakes, the stream's packets and the ring's layout. Two
/// ends with different versions refuse each other, with a message that names
/// both. The codec tag, not this, versions a payload.
inline constexpr std::uint32_t ProtocolVersion = 1;

/// The longest codec tag, in bytes.
inline constexpr std::size_t MaxCodecBytes = 64;

/// 1 to MaxCodecBytes of printable ASCII without spaces, such as
/// "afterglow.state.v1".
PHOSPHORCHANNEL_EXPORT bool validCodec(std::string_view codec);

enum class WriteResult {
    Written,
    /// The channel is full. Nothing was written: the writer keeps the frame,
    /// or merges it with the next one, and waits for the writer's fd.
    WouldBlock,
    /// The frame can never fit this channel.
    TooLarge,
    /// The reader closed the channel, or the transport failed.
    Closed,
};

enum class ReadResult {
    Frame,
    /// No frame waits now: wait for the reader's fd.
    Empty,
    /// The writer closed the channel and every frame was read.
    Closed,
    /// The other process broke the protocol. Close the channel.
    Corrupt,
};

/// The message for two protocol versions that do not match.
PHOSPHORCHANNEL_EXPORT std::string versionMismatch(std::uint32_t theirs);

} // namespace PhosphorChannel
