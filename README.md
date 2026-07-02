# Real-Time Event Streaming and Messaging System

> A from-scratch C++ pub/sub messaging broker built for low-latency real-time scenarios.
> Built with the dual goal of **learning every layer** and **achieving measurable performance**.

---

## Overview

This project implements a TCP-based pub/sub messaging system in C++ with no third-party
networking or serialization libraries. Every layer — from BSD sockets to the binary wire
protocol — is built and understood from first principles.

**Target use cases:** Market data streaming, order routing, strategy event buses, real-time analytics.

**Design philosophy:** *Learn by building. Measure before optimizing. Never guess.*

---

## Project Status

| Phase | Goal | Status |
|---|---|---|
| **Phase 0** | Foundation — fix all correctness bugs | ✅ **Complete** |
| **Phase 1** | Broker core — topic routing (SUBSCRIBE / PUBLISH / DATA) | 🔲 Next |
| **Phase 2** | Non-blocking I/O — epoll (Linux) / IOCP (Windows) | 🔲 Planned |
| **Phase 3** | Lock-free ring buffer — eliminate mutex on hot path | 🔲 Planned |
| **Phase 4** | Zero-copy I/O — `writev`, `TCP_NODELAY`, scatter-gather | 🔲 Planned |
| **Phase 5** | Benchmarking — p50/p99 latency histograms | 🔲 Planned |
| **Phase 6** | Persistence — Write-Ahead Log | 🔲 Planned |
| **Phase 7** | Consumer groups & topic partitioning | 🔲 Planned |

---

## ✅ Phase 0 — What Was Built & Fixed

Phase 0 audited the initial skeleton and fixed **6 critical correctness bugs** before any
feature work began. The principle: *make it correct first, then make it fast*.

### Bugs Fixed

| # | Bug | Severity | Root Cause | Fix |
|---|---|---|---|---|
| 1 | **Data race in `receiveLoop`** | 🔴 Critical (UB) | Mutex released before socket access; another thread could free the socket in the gap | Changed peer map from `unique_ptr` → `shared_ptr`. Take a copy (bumping ref-count) while locked, then use it after releasing the mutex. The socket lives as long as the copy exists. |
| 2 | **No TCP message framing** | 🔴 Critical | TCP is a byte stream — `recv()` returns arbitrary byte counts, splitting or merging messages silently | New `FramedSocket` class + `recv_exact()`. Always reads exactly `sizeof(header)` bytes, parses the `length` field, then reads exactly that many body bytes. |
| 3 | **Broadcast holds global mutex during I/O** | 🔴 Critical | `send()` is blocking; holding the lock for all sends starves every other thread | Snapshot `shared_ptr` vector under lock (microseconds), release lock, then do all sends outside it. |
| 4 | **Thread-per-connection model** | 🟡 Scalability | One OS thread per peer — 8 MB stack each, context-switch overhead, no graceful join | **Acknowledged** — will be replaced with an epoll/IOCP event loop in Phase 2. |
| 5 | **Partial `send()` unhandled** | 🔴 Critical | `send()` can return fewer bytes than requested (kernel buffer full); return value was ignored | New `send_all()` retry loop that keeps sending until all bytes are queued or an error occurs. |
| 6 | **No endian-safe serialization** | 🔴 Critical | Multi-byte header fields written in host byte order (little-endian on x86) — garbage on cross-platform communication | `MessageHeader::to_bytes()` / `from_bytes()` using `htonl`/`ntohs`. All wire values are now big-endian regardless of host CPU. |

### New Components Added

#### `FramedSocket` (`include/protocol/framing.hpp`)

Wraps a `Socket` with the project's length-prefix framing protocol.

```
Wire Frame:
┌──────────────────────────────────────────────────────────┐
│  length       (uint32, 4 bytes, network byte order)      │  ← total frame size
│  type         (uint16, 2 bytes, network byte order)      │  ← MessageType enum
│  topic_length (uint16, 2 bytes, network byte order)      │  ← topic byte count
├──────────────────────────────────────────────────────────┤
│  topic        (topic_length bytes, UTF-8)                │
├──────────────────────────────────────────────────────────┤
│  payload      (remaining bytes)                          │
└──────────────────────────────────────────────────────────┘
```

- `send_message()` — builds a single contiguous frame buffer and calls `send_all()` (one syscall)
- `recv_message()` — calls `recv_exact()` twice: once for the 8-byte header, once for the body
- Header validation before allocation: rejects frames with `length > 64 MB` or inconsistent `topic_length`

#### `Socket::send_all()` / `Socket::recv_exact()`

```cpp
// send_all: retries until all N bytes are sent or an error occurs
bool Socket::send_all(const std::vector<uint8_t>& data);

// recv_exact: loops until exactly `size` bytes are accumulated
bool Socket::recv_exact(std::vector<uint8_t>& buffer, size_t size);
```

Both are the correct low-level primitives for working with a TCP byte stream.

#### Endian-Safe `MessageHeader`

```cpp
// Serialize to network byte order — the only correct way to put a header on the wire
void MessageHeader::to_bytes(std::vector<uint8_t>& buf) const;

// Deserialize from network byte order back to host integers
static MessageHeader MessageHeader::from_bytes(const uint8_t* data);
```

### Files Changed in Phase 0

| File | Change |
|---|---|
| `include/protocol/message.hpp` | Added `to_bytes()` / `from_bytes()` with `htonl`/`ntohs` |
| `include/protocol/framing.hpp` | **New** — `FramedSocket` and `ParsedMessage` |
| `src/protocol/framing.cpp` | **New** — framing implementation |
| `include/network/socket.hpp` | Added `send_all()`, `recv_exact()` |
| `src/network/socket.cpp` | Implemented `send_all()`, `recv_exact()` |
| `include/network/messaging_node.hpp` | `unique_ptr` → `shared_ptr` for peer map |
| `src/network/messaging_node.cpp` | Fixed Bugs 1, 2, 3 with annotated before/after comments |
| `CMakeLists.txt` | Added `src/protocol/framing.cpp` to build |
| `docs/DESIGN.md` | **New** — living design document |
| `knowledge_base/` | **New** — 138 KB of reference documentation |

### Knowledge Base Created

All concepts learned in Phase 0 are documented in `knowledge_base/phase_0/`:

| Document | Topic | Size |
|---|---|---|
| `01_tcp_stream_semantics.md` | TCP byte stream, 3 framing strategies, `recv_exact`, Nagle's algorithm | 16 KB |
| `02_data_races_and_mutex_scope.md` | Data races, UB, mutex scope, `shared_ptr` ownership, snapshot pattern | 31 KB |
| `03_partial_io_handling.md` | Kernel send/recv buffers, `SO_SNDBUF`, retry loops, blocking vs. non-blocking | 39 KB |
| `04_network_byte_order.md` | Endianness, `htonl` family, struct padding, aliasing, Protobuf comparison | 25 KB |
| `05_thread_per_connection_problem.md` | OS thread costs, C10K problem, context switching, epoll preview | 27 KB |

---

## Architecture

### Current (Phase 0)

```
┌─────────────────────────────────────────┐
│          Application Layer              │
│   MessagingNode CLI  (src/app/main.cpp) │
├─────────────────────────────────────────┤
│          Protocol Layer                 │
│  FramedSocket   MessageHeader           │
│  (framing.hpp)  (message.hpp)           │
├─────────────────────────────────────────┤
│          Network Layer                  │
│  Socket — RAII, move-only, blocking TCP │
│  send_all()  recv_exact()               │
├─────────────────────────────────────────┤
│          Platform Layer                 │
│  common.hpp — compile-time #ifdef       │
│  Linux: int fd    Windows: SOCKET       │
└─────────────────────────────────────────┘
```

### Target (End State)

```
┌──────────────────────────────────────────────────────────┐
│  Producer CLI    Consumer CLI    Broker CLI               │
├──────────────────────────────────────────────────────────┤
│  Broker Core — Topic Registry, Subscriber Map, Routing   │
├──────────────────────────────────────────────────────────┤
│  Protocol Layer — FramedSocket, MessageHeader, WAL       │
├──────────────────────────────────────────────────────────┤
│  Network Layer — EventLoop (epoll/IOCP), Lock-Free Queue │
├──────────────────────────────────────────────────────────┤
│  Platform Layer — Linux / Windows compile-time selection │
└──────────────────────────────────────────────────────────┘
```

---

## Wire Protocol

```
Message Types:
  SUBSCRIBE (1)  Client → Broker  Subscribe to a topic
  PUBLISH   (2)  Client → Broker  Publish a message to a topic
  DATA      (3)  Broker → Client  Deliver a message to subscribers
```

All multi-byte integer fields are transmitted in **network byte order** (big-endian).
The `length` field always includes the 8-byte header itself.

---

## Build

### Requirements

- CMake 3.20+
- C++20 compiler: GCC 11+, Clang 13+, or MSVC 19.29+
- Linux: no additional dependencies
- Windows: WinSock2 (Windows SDK — linked automatically via `ws2_32`)

### Build & Run

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build --target messaging_app

# Run — Node A (port 9001)
./build/messaging_app 9001

# Run — Node B (port 9002), then connect and send:
./build/messaging_app 9002
> connect 127.0.0.1 9001
> send hello from node B
```

### Available Commands

| Command | Description |
|---|---|
| `connect <ip> <port>` | Connect to a peer node |
| `send <message>` | Broadcast a framed message to all connected peers |
| `help` | Show command reference |
| `quit` | Graceful shutdown |

---

## Performance Goals

| Metric | Target (loopback) | Target (LAN, 1 Gbps) |
|---|---|---|
| p50 latency | < 10 μs | < 100 μs |
| p99 latency | < 50 μs | < 500 μs |
| Throughput | > 500K msg/s | > 100K msg/s |
| Max connections | 10,000+ (Phase 2+) | 10,000+ |

---

## Cross-Platform Design

All platform differences are resolved at **compile time** (`#ifdef _WIN32`) — zero runtime cost:

| Feature | Linux | Windows |
|---|---|---|
| Socket type | `int` | `SOCKET` |
| Socket close | `::close()` | `closesocket()` |
| Error code | `errno` | `WSAGetLastError()` |
| WinSock init | N/A | `WinsockContext` RAII auto-init |
| I/O multiplexing (Phase 2) | `epoll` | IOCP |
| High-res timer (Phase 5) | `clock_gettime(CLOCK_MONOTONIC)` | `QueryPerformanceCounter` |

---

## Repository Structure

```
├── include/
│   ├── network/
│   │   ├── common.hpp          # Cross-platform socket types & macros
│   │   ├── socket.hpp          # RAII Socket class
│   │   └── messaging_node.hpp  # P2P node (accept, connect, broadcast)
│   └── protocol/
│       ├── message.hpp         # MessageHeader + endian-safe serialization
│       └── framing.hpp         # FramedSocket + ParsedMessage [Phase 0]
├── src/
│   ├── app/main.cpp            # CLI entry point
│   ├── network/
│   │   ├── socket.cpp
│   │   └── messaging_node.cpp
│   └── protocol/
│       └── framing.cpp         # [Phase 0]
├── docs/
│   └── DESIGN.md               # Living design document
├── knowledge_base/
│   ├── 00_index.md             # Master index + glossary
│   └── phase_0/                # 138 KB of Phase 0 reference docs
├── benchmarks/
├── tests/
└── CMakeLists.txt
```
