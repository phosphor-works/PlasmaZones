<!-- SPDX-FileCopyrightText: 2026 fuddlesworth
     SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-channel

> Ordered frames of opaque bytes from one process to another, with
> backpressure. No Qt and no other Phosphor library.

## Responsibility

A channel is unidirectional: one writer feeds one reader. Each channel has a
codec tag, such as `afterglow.state.v1`, that names the format of its
payload. The library stores the tag and never reads the payload. Payload
versioning lives in the consumer.

The first consumer is the mux daemon of Afterglow, which sends terminal state
to a window and input back. The daemon has no Qt and can run on a host
without it, so the library builds alone: `cmake -S phosphor-libs/libs/phosphor-channel`.

It is a separate library from phosphor-ipc. phosphor-ipc carries JSON calls
and events. phosphor-channel carries bytes at a high rate.

## Transports

| Transport | Use | Cost |
| --- | --- | --- |
| `RingWriter` / `RingReader` | Two processes on one computer | One copy per byte |
| `Stream` over a Unix socket | The fallback on one computer | Two copies per byte |
| `Stream` over two pipes | The standard input and output of `ssh` | Two copies per byte, plus ssh |

### The shared-memory ring

- A memfd that both processes map, sealed with `F_SEAL_SHRINK`, `F_SEAL_GROW`
  and `F_SEAL_SEAL`. The reader refuses a memfd without the size seals, so the
  writer can never cut the mapping short.
- The header keeps the write and the read position on separate cache lines.
  Records are 8-byte aligned, and a skip record pads the wrap.
- The largest frame is half the data area less 8 bytes, so a frame fits an
  empty ring wherever the wrap falls.
- An eventfd wakes the reader after a write, and another one wakes the writer
  after a read. An end signals only when the other end said that it sleeps.
- The reader checks each position and length that it reads, and copies each
  frame out. A ring that breaks the layout gives `ReadResult::Corrupt`.
- `offerRing` and `acceptRing` hand the memfd and the two eventfds over a raw
  Unix socket with `SCM_RIGHTS`, with the version and the codec tag.

### The stream

- Several channels over one connected byte stream, or over one fd for input
  and another for output. No file descriptor crosses it.
- Each end begins with a hello that carries the protocol version.
- The reader grants each channel a window of bytes (`Options::window`, 4 MiB
  by default), which is also the largest frame. A writer that has used its
  window gets `WouldBlock`, and `onWritable` says when it may write again. A
  full channel holds back no other channel.
- Frames go out in fragments (`Options::fragment`, 16 KiB), one fragment of
  each channel in turn. A small frame does not wait for a large frame on
  another channel.
- The stream never blocks. Poll `readFd()`, and `writeFd()` while
  `wantsWrite()`. `write()`, `pump()` and `flush()` block with their own poll.
- Values on the wire are little-endian.

## Versioning

`ProtocolVersion` covers the handshakes, the stream's packets and the ring's
layout. Two ends with different versions refuse each other, with a message
that names both versions.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorChannel::WriteResult` | `Written`, `WouldBlock`, `TooLarge` or `Closed` |
| `PhosphorChannel::ReadResult` | `Frame`, `Empty`, `Closed` or `Corrupt` |
| `PhosphorChannel::RingWriter` | Creates a ring and writes frames into it |
| `PhosphorChannel::RingReader` | Maps a ring that a handshake brought, and reads frames |
| `PhosphorChannel::offerRing` / `acceptRing` | The handover of a ring over a Unix socket |
| `PhosphorChannel::Stream` | Several channels over a byte stream |
| `PhosphorChannel::StreamWriter` / `StreamReader` | One channel of a stream |

## Tests

`pch_test_ring`, `pch_test_handshake` and `pch_test_stream` use Qt Test, as
the other libraries do. Without Qt they are not built.
