# Real-Time Event Streaming & Messaging System — Design Document

> **Living Document** — updated at the end of every phase.
> Last updated: **Phase 0 — Foundation Bug Fixes**

---

## Table of Contents

1. [Project Vision](#1-project-vision)
2. [Architecture Overview](#2-architecture-overview)
3. [Component Reference](#3-component-reference)
4. [Wire Protocol](#4-wire-protocol)
5. [Phase Status](#5-phase-status)
6. [Known Issues & Future Work](#6-known-issues--future-work)
7. [Performance Goals](#7-performance-goals)
8. [Cross-Platform Strategy](#8-cross-platform-strategy)
9. [Build & Run](#9-build--run)

---

## 1. Project Vision

A **from-scratch C++ pub/sub messaging broker** built for low-latency real-time scenarios
(market data, order routing, strategy event buses). The goal is not to replace Kafka but to
understand and implement every layer — from the BSD socket API to lock-free data structures
— with measurable performance at each step.

**Design philosophy:**
- Learn by building, not by using libraries
- Measure before optimizing (never guess)
- Cross-platform (Linux + Windows) without runtime overhead
- Annotate every design decision inline and in the knowledge base

---

## 2. Architecture Overview

### Target Architecture (End State)

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                     │
│           Producer CLI    Consumer CLI    Broker CLI     │
├─────────────────────────────────────────────────────────┤
│                     Broker Core                          │
│    Topic Registry   Subscriber Map   Routing Engine     │
├─────────────────────────────────────────────────────────┤
│                   Protocol Layer                         │
│    FramedSocket   MessageHeader   Serialization         │
├─────────────────────────────────────────────────────────┤
│                    Network Layer                         │
│    Socket (RAII)   EventLoop (epoll/IOCP)               │
├─────────────────────────────────────────────────────────┤
│                   Platform Layer                         │
│        Linux (epoll, writev)   Windows (IOCP, WSASend)  │
└─────────────────────────────────────────────────────────┘
```

### Current Architecture (Phase 0)

```
┌─────────────────────────────────┐
│        Application Layer        │
│  MessagingNode CLI (main.cpp)   │
├─────────────────────────────────┤
│       Protocol Layer            │
│  FramedSocket + MessageHeader   │  ← Phase 0 addition
├─────────────────────────────────┤
│       Network Layer             │
│  Socket (RAII, blocking TCP)    │
├─────────────────────────────────┤
│       Platform Layer            │
│  common.hpp macro switching     │
└─────────────────────────────────┘
```

### Message Flow (Current)

```
Sender                          MessagingNode
  │                                   │
  │── broadcast("hello") ────────────>│
  │                              [snapshot peers under lock]
  │                              [release lock]
  │                              for each peer:
  │                                FramedSocket::send_message()
  │                                  → build frame [header|topic|payload]
  │                                  → send_all() retry loop
  │                                  → kernel TCP buffer → wire
  │
  │                         Receiver Thread (receiveLoop)
  │                                   │
  │                              [shared_ptr copy under lock]
  │                              [release lock]
  │                              FramedSocket::recv_message()
  │                                  → recv_exact(8 bytes) = header
  │                                  → parse + validate header
  │                                  → recv_exact(body bytes)
  │                                  → split into topic + payload
```

---

## 3. Component Reference

### 3.1 Platform Layer — `include/network/common.hpp`

**Purpose:** Zero-cost cross-platform socket type and error abstraction.

| Abstraction | Linux | Windows |
|---|---|---|
| `SocketHandle` | `int` | `SOCKET` (UINT_PTR) |
| `INVALID_SOCKET` | `-1` | `INVALID_SOCKET` (defined by WinSock) |
| `IS_VALIDSOCKET(s)` | `s >= 0` | `s != INVALID_SOCKET` |
| `CLOSE_SOCKET(s)` | `::close(s)` | `closesocket(s)` |
| `GET_SOCKET_ERR()` | `errno` | `WSAGetLastError()` |

**Design decision:** Macro-based switching (not virtual dispatch) — zero runtime cost.
See: `knowledge_base/phase_0/06_cross_platform_strategy.md`

---

### 3.2 Network Layer — `include/network/socket.hpp`

**Purpose:** RAII-managed BSD socket with move semantics.

| Method | Description |
|---|---|
| `Socket()` | Creates an invalid (closed) socket |
| `Socket(SocketHandle)` | Wraps an existing handle (e.g., from `accept()`) |
| `create()` | Allocates a new `SOCK_STREAM` socket |
| `bind(port)` | Binds to `INADDR_ANY:port` with `SO_REUSEADDR` |
| `listen()` | Marks socket as passive (backlog = `SOMAXCONN`) |
| `accept()` | Blocks until a client connects; returns raw handle |
| `connect(ip, port)` | Initiates TCP connection using `inet_pton` |
| `send(data)` | Raw send (may be partial) |
| `send_all(data)` | **Phase 0** — Guaranteed full write, retries on partial |
| `receive(buf, size)` | Raw recv (may be partial) |
| `recv_exact(buf, size)` | **Phase 0** — Guaranteed full read, loops until done |
| `close()` | Closes the handle and invalidates it |

**Move semantics:** Sockets are unique resources. Copy is `= delete`.
Move transfers ownership, setting the source handle to `INVALID_SOCKET`.

---

### 3.3 Protocol Layer — `include/protocol/message.hpp`

**Purpose:** Defines the on-wire binary message format.

```
Wire Frame Layout:
┌────────────────────────────────────────────────────────┐
│  length (4 bytes, uint32, big-endian)                  │ ← total frame size
│  type   (2 bytes, uint16, big-endian)                  │ ← MessageType enum
│  topic_length (2 bytes, uint16, big-endian)            │ ← topic byte count
├────────────────────────────────────────────────────────┤
│  topic  (topic_length bytes, UTF-8)                    │
├────────────────────────────────────────────────────────┤
│  payload (remaining bytes)                             │
└────────────────────────────────────────────────────────┘
```

**Serialization:** `MessageHeader::to_bytes()` / `from_bytes()` use
`htonl`/`ntohl` to ensure correct byte order on all platforms.
**Never** memcpy the struct directly to the wire (compiler padding + endianness).

---

### 3.4 Protocol Layer — `include/protocol/framing.hpp`

**Purpose:** Wraps a `Socket` with framing-aware send/recv that handles TCP
stream fragmentation transparently.

| Method | Description |
|---|---|
| `send_message(type, topic, payload)` | Builds a complete frame + calls `send_all()` |
| `recv_message(out)` | Loops via `recv_exact()` to reassemble a complete frame |

**Validation:** `recv_message()` validates `length` and `topic_length` fields
before allocating memory (DoS prevention, 64 MB cap).

---

### 3.5 Application Layer — `MessagingNode`

**Purpose:** P2P messaging node that can accept inbound connections, connect
to peers, and broadcast messages.

**Threading model (Phase 0 — will change in Phase 2):**

| Thread | Role |
|---|---|
| Main thread | User command loop |
| `acceptLoop` thread | Blocks on `accept()`, spawns receive threads |
| Per-peer `receiveLoop` thread | Blocks on `recv_message()` for that peer |

**Peer storage:** `std::map<SocketHandle, std::shared_ptr<Socket>>`

The `shared_ptr` is critical: `receiveLoop` and `broadcast` take a copy of the
`shared_ptr` while holding `m_peersMutex`, then release the mutex before doing I/O.
The ref-count keeps the `Socket` alive even if another thread erases it from the map.

---

## 4. Wire Protocol

### Message Types

| Value | Name | Direction | Meaning |
|---|---|---|---|
| `1` | `SUBSCRIBE` | Client → Broker | Subscribe to a topic |
| `2` | `PUBLISH` | Client → Broker | Publish to a topic |
| `3` | `DATA` | Broker → Client | Deliver a message |

### Serialization Rules

1. All integers are **network byte order** (big-endian): use `htonl`/`htons` on send, `ntohl`/`ntohs` on recv.
2. The `length` field includes the 8-byte header itself.
3. `topic` is raw UTF-8 bytes, no null terminator.
4. `payload` is raw bytes — interpretation is application-defined.

---

## 5. Phase Status

### ✅ Phase 0 — Foundation Bug Fixes (COMPLETE)

**Goal:** Make the existing code correct before adding features.

#### Bugs Fixed

| Bug | Severity | Root Cause | Fix Applied |
|---|---|---|---|
| **Bug 1** — Data race in `receiveLoop` | 🔴 Critical | Mutex released before socket access | `shared_ptr` copy pattern — take copy under lock, use outside |
| **Bug 2** — No TCP framing | 🔴 Critical | `recv()` returns arbitrary byte counts | New `FramedSocket` + `recv_exact()` / `send_all()` |
| **Bug 3** — Broadcast holds lock during I/O | 🔴 Critical | Blocking `send()` called inside `lock_guard` scope | Snapshot `shared_ptr` vector under lock, send outside |
| **Bug 4** — Thread-per-connection | 🟡 Scalability | One OS thread per peer | **Acknowledged** — will fix in Phase 2 (epoll/IOCP) |
| **Bug 5** — Partial `send()` unhandled | 🔴 Critical | `send()` return value ignored | New `send_all()` retry loop |
| **Bug 6** — No endian handling | 🔴 Critical | Multi-byte fields in host byte order | `MessageHeader::to_bytes()` / `from_bytes()` with `htonl`/`ntohs` |

#### Files Changed

| File | Change |
|---|---|
| `include/protocol/message.hpp` | Added endian-safe `to_bytes()` / `from_bytes()` |
| `include/protocol/framing.hpp` | **New** — `FramedSocket`, `ParsedMessage` |
| `src/protocol/framing.cpp` | **New** — framing implementation |
| `include/network/socket.hpp` | Added `send_all()`, `recv_exact()` |
| `src/network/socket.cpp` | Implemented `send_all()`, `recv_exact()` |
| `include/network/messaging_node.hpp` | `unique_ptr` → `shared_ptr` for peers |
| `src/network/messaging_node.cpp` | Fixed Bugs 1, 2, 3 — full rewrite with comments |
| `CMakeLists.txt` | Added `src/protocol/framing.cpp` |

#### Knowledge Base Entries Created

- `knowledge_base/phase_0/01_tcp_stream_semantics.md`
- `knowledge_base/phase_0/02_data_races_and_mutex_scope.md`
- `knowledge_base/phase_0/03_partial_io_handling.md`
- `knowledge_base/phase_0/04_network_byte_order.md`
- `knowledge_base/phase_0/05_thread_per_connection_problem.md`

---

### 🔲 Phase 1 — Binary Protocol & Broker Core (NEXT)

**Goal:** Build the actual pub/sub broker described in the README.

**Planned additions:**
- `Broker` class with `std::unordered_map<string, vector<SocketHandle>>` topic registry
- Separate `broker_main.cpp`, `producer_main.cpp`, `consumer_main.cpp` binaries
- Full SUBSCRIBE / PUBLISH / DATA message routing
- Topic-based fan-out (not broadcast-to-all)

---

### 🔲 Phase 2 — Non-Blocking I/O Event Loop

**Goal:** Replace thread-per-connection with epoll (Linux) / IOCP (Windows).

**Planned additions:**
- `IOPoller` abstract interface
- `EpollPoller` (Linux) and `SelectPoller` (cross-platform fallback)
- Event-driven `receiveLoop` on a single thread

---

### 🔲 Phase 3 — Lock-Free Ring Buffer

**Goal:** Eliminate mutex contention on the broker hot path.

**Planned additions:**
- SPSC (Single-Producer Single-Consumer) lock-free ring buffer
- Cache-line alignment (`alignas(64)`) to prevent false sharing
- `std::atomic` with `acquire`/`release` memory ordering

---

### 🔲 Phase 4 — Zero-Copy I/O

**Goal:** Reduce memory copies and syscall overhead.

**Planned additions:**
- `writev()` / `WSASend` for scatter-gather (header + body in one syscall)
- `TCP_NODELAY` for latency-critical paths
- Profiling to validate improvement

---

### 🔲 Phase 5 — Benchmarking

**Goal:** Measure everything. Establish p50/p99 baseline.

**Planned additions:**
- Latency histogram in `benchmarks/perf_overhead.cpp`
- Round-trip time measurement (not one-way throughput)
- Coordinated-omission-aware statistics

---

### 🔲 Phase 6 — Persistence (Write-Ahead Log)

**Goal:** Messages survive broker restarts.

---

### 🔲 Phase 7 — Consumer Groups & Partitioning

**Goal:** Scale to multiple consumers with delivery ordering guarantees.

---

## 6. Known Issues & Future Work

| Issue | Phase | Notes |
|---|---|---|
| Thread-per-connection model | Phase 2 | Unscalable past ~100 connections |
| Detached threads — no graceful join | Phase 2 | `stop()` closes sockets; threads exit on next recv |
| No actual pub/sub routing | Phase 1 | `broadcast()` sends to ALL peers, not topic subscribers |
| No backpressure | Phase 3 | Fast producer can overwhelm slow consumer |
| No reconnection logic | Phase 1+ | Disconnected peer is erased, never retried |
| No authentication | Out of scope | No TLS, no handshake |

---

## 7. Performance Goals

| Metric | Target (loopback) | Target (LAN, 1Gbps) |
|---|---|---|
| p50 latency | < 10 μs | < 100 μs |
| p99 latency | < 50 μs | < 500 μs |
| Throughput | > 500K msg/s | > 100K msg/s |
| Max message size | 64 MB (capped) | 64 MB |
| Max connections | 10,000+ (Phase 2+) | 10,000+ |

---

## 8. Cross-Platform Strategy

### Compile-Time Switching (No Runtime Cost)

All platform differences are resolved at **compile time** via `#ifdef _WIN32`.
No virtual dispatch, no function pointers, no runtime branching.

| Feature | Linux | Windows | Header |
|---|---|---|---|
| Socket type | `int` | `SOCKET` | `common.hpp` |
| Socket close | `::close()` | `closesocket()` | `socket.cpp` |
| Error code | `errno` | `WSAGetLastError()` | `common.hpp` |
| WinSock init | N/A | `WSAStartup` via `WinsockContext` RAII | `socket.cpp` |
| Non-blocking (Phase 2) | `fcntl(O_NONBLOCK)` | `ioctlsocket(FIONBIO)` | TBD |
| I/O multiplexing (Phase 2) | `epoll` | IOCP | TBD |
| High-res timer (Phase 5) | `clock_gettime(CLOCK_MONOTONIC)` | `QueryPerformanceCounter` | TBD |
| Network byte order | `arpa/inet.h` | `winsock2.h` | `message.hpp` |

### Planned Phase 2 Abstraction

```cpp
// A compile-time-selected I/O poller (no vtable overhead in release)
// Linux:   EpollPoller
// Windows: IOCPPoller
// Both:    SelectPoller (fallback)
```

---

## 9. Build & Run

### Prerequisites

- CMake 3.20+
- C++20 capable compiler (GCC 11+, Clang 13+, MSVC 19.29+)
- Linux: no extra deps
- Windows: WinSock2 (included in Windows SDK, linked via `ws2_32`)

### Build

```bash
# Configure (Debug)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build just the main app
cmake --build build --target messaging_app

# Build everything (includes tests, benchmarks)
cmake --build build
```

### Run (Two-Node Example)

```bash
# Terminal 1 — Node A on port 9001
./build/messaging_app 9001

# Terminal 2 — Node B on port 9002
./build/messaging_app 9002

# In Node B: connect to Node A
> connect 127.0.0.1 9001

# In Node A: send to all peers (including B)
> send hello world
```

### Commands

| Command | Description |
|---|---|
| `connect <ip> <port>` | Connect to a peer |
| `send <message>` | Broadcast framed message to all peers |
| `help` | Show command list |
| `quit` | Graceful shutdown |
