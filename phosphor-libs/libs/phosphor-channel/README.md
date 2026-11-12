<!-- SPDX-FileCopyrightText: 2026 fuddlesworth
     SPDX-License-Identifier: LGPL-2.1-or-later -->

# phosphor-channel

> Ordered frames of opaque bytes from one process to another, with
> backpressure. No Qt and no other Phosphor library. Linux only.

## Responsibility

A channel is unidirectional: one writer feeds one reader. Each channel has a
codec tag, such as `afterglow.state.v1`, that names the format of its
payload. The library stores the tag and never reads the payload. Payload
versioning lives in the consumer.

The first consumer is the mux daemon of Afterglow, which sends terminal state
to a window and input back. The daemon has no Qt and can run on a host
without it, so the library builds alone, from the repository root:

```bash
cmake -S phosphor-libs/libs/phosphor-channel -B build-channel -DCMAKE_BUILD_TYPE=Release
cmake --build build-channel
```

It needs Linux: memfd, SCM_RIGHTS and MSG_NOSIGNAL have no portable stand-in,
and the configure stops on any other system.

It is a separate library from phosphor-ipc. phosphor-ipc carries JSON calls
and events. phosphor-channel carries bytes at a high rate.

## Transports

| Transport | Use | Cost |
| --- | --- | --- |
| `RingWriter` / `RingReader` | Two processes on one computer | A copy in and a copy out; no system call per frame while the reader keeps up |
| `Stream` over a Unix socket | The fallback on one computer | Through the kernel, with a buffer copy at each end |
| `Stream` over two pipes | The standard input and output of `ssh` | The same, plus ssh |

### The shared-memory ring

- A memfd that both processes map, sealed with `F_SEAL_SHRINK`, `F_SEAL_GROW`
  and `F_SEAL_SEAL`. The reader refuses a memfd that is not plain shared
  memory, lacks the size seals, or is larger than `AcceptOptions::maxCapacity`,
  so the writer can never cut the mapping short.
- The write position, the read position and each end's wake request sit on
  cache lines of their own, and the closed flags share one. Records are
  8-byte aligned, and a skip record pads the wrap.
- The largest frame is half the data area less 8 bytes, so a frame fits an
  empty ring wherever the wrap falls.
- A connected pair of Unix sockets wakes the ends, one socket in each
  process. The writer wakes the reader only after the reader's `read()`
  returned `Empty`, so a reader reads until `Empty` before it waits. The
  reader wakes the writer only once the space that its `WouldBlock` asked
  for is free. When the other process exits, its socket closes and the wake
  fd becomes readable. Once an end drains it, or fails to signal, its writes
  say `Closed`, and its reads say `Closed` after the last frame.
- The reader checks each position and length that it reads, and copies each
  frame out. A ring that breaks the layout gives `ReadResult::Corrupt`.
- `offerRing` and `acceptRing` hand the memfd and the reader's wake socket
  over a raw Unix socket with `SCM_RIGHTS`, with the version and the codec
  tag. A ring is offered once. The reader may limit the ring's size and the
  codecs that it takes, and the writer learns why it was refused. After a
  failed handshake, close the socket and destroy the ring.

### The stream

- Several channels over one connected byte stream, or over one fd for input
  and another for output. No file descriptor crosses it. ssh must not
  allocate a terminal (no `-t`), since a terminal rewrites the bytes.
- Each end begins with a hello that carries the protocol version, the
  number of channels that it reads at a time (`Options::maxChannels`), and
  the limit of the channel ids that it reads (`Options::maxChannelId`, 4096 by
  default). Owners reuse ids below the limit.
- The reader grants each channel a window of bytes (`Options::window`, 4 MiB
  by default). Each frame costs its size plus `Stream::FrameCost` against it,
  so the largest frame is the window less that. A writer that has used its
  window gets `WouldBlock`, and `onWritable` says when it may write again. A
  full channel holds back no other channel.
- A channel ends when its writer closes it and its reader has read every
  frame. Then the writer may open its id again.
- Frames go out in fragments (`Options::fragment`, 16 KiB), one fragment of
  each channel in turn. A small frame waits for a few fragments of a large
  frame on another channel, plus what the kernel and ssh already hold.
- `tryWrite()`, `readable()` and `writable()` never block. `write()`,
  `pump()`, `flush()` and `shutdown()` block with their own poll. Poll
  `readFd()`, and `writeFd()` while `wantsWrite()`, and read every reader
  until `read()` returns something other than `Frame` after each call, or in
  `onReadable`.
- An end that fails tells the other end why before it stops writing.
  `shutdown()` ends a stream cleanly. Destroying a stream closes it at once.
- Values on the wire are little-endian.

## Versioning

`ProtocolVersion` covers the handshakes, the stream's packets and the ring's
layout. Two ends with different versions refuse each other, with a message
that names both versions. The soname carries the minor version while the
version is 0.x, so a consumer built against 0.1 does not load 0.2.

## Key types

| Type | Purpose |
|------|---------|
| `PhosphorChannel::WriteResult` | `Written`, `WouldBlock`, `TooLarge` or `Closed` |
| `PhosphorChannel::ReadResult` | `Frame`, `Empty`, `Closed` or `Corrupt` |
| `PhosphorChannel::RingWriter` | Creates a ring and writes frames into it |
| `PhosphorChannel::RingReader` | Maps a ring that a handshake brought, and reads frames |
| `PhosphorChannel::offerRing` / `acceptRing` | The handover of a ring over a Unix socket |
| `PhosphorChannel::AcceptOptions` | The largest ring and the codecs that a reader takes |
| `PhosphorChannel::Stream` | Several channels over a byte stream |
| `PhosphorChannel::StreamWriter` / `StreamReader` | One channel of a stream |

## Tests

`phosphorchannel_test_ring`, `phosphorchannel_test_handshake` and
`phosphorchannel_test_stream` use Qt Test, as the other libraries do. A
standalone configure builds them unless `-DBUILD_TESTING=OFF`, and leaves them
out when Qt is not found. A host without Qt therefore builds the library but
cannot run its tests; CI runs them in a standalone build that has Qt, and
builds once more without Qt.
