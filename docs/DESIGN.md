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

### Target Architecture (Dual-Mode Platform)

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                     │
│               Producer CLI    Consumer CLI               │
├─────────────────────────────────────────────────────────┤
│                Transport Abstraction (ITransport)        │
├──────────────────────────────┬──────────────────────────┤
│    Strategy A: TCP Broker    │ Strategy B: Brokerless   │
│   (Centralized, WAN/Cloud)   │   (UDP Multicast, LAN)   │
├──────────────────────────────┼──────────────────────────┤
│   epoll/IOCP Event Loop      │ Sequence Numbers & NAKs  │
│   Lock-Free Ring Buffers     │ Zero-Copy UDP Sockets    │
├──────────────────────────────┴──────────────────────────┤
│                    Platform Layer                        │
│     Standard OS Network Stack or Kernel Bypass (DPDK)    │
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

**Goal:** Make the existing code correct before adding features. Phase 0 audited the initial skeleton and fixed **6 critical correctness bugs** before any feature work began. The principle: *make it correct first, then make it fast*.

#### Bugs Fixed & Architectural Improvements

| Bug | Severity | Root Cause | Fix Applied |
|---|---|---|---|
| **1. Data race in `receiveLoop`** | 🔴 Critical (UB) | Mutex released before socket access; another thread could free the socket in the gap | Changed peer map from `unique_ptr` → `shared_ptr`. Take a copy (bumping ref-count) while locked, then use it after releasing the mutex. The socket lives as long as the copy exists. |
| **2. No TCP message framing** | 🔴 Critical | TCP is a byte stream — `recv()` returns arbitrary byte counts, splitting or merging messages silently | New `FramedSocket` class + `recv_exact()`. Always reads exactly `sizeof(header)` bytes, parses the `length` field, then reads exactly that many body bytes. |
| **3. Broadcast holds global mutex during I/O** | 🔴 Critical | `send()` is blocking; holding the lock for all sends starves every other thread | Snapshot `shared_ptr` vector under lock (microseconds), release lock, then do all sends outside it. |
| **4. Thread-per-connection model** | 🟡 Scalability | One OS thread per peer — 8 MB stack each, context-switch overhead, no graceful join | **Acknowledged** — will be replaced with an epoll/IOCP event loop in Phase 2. |
| **5. Partial `send()` unhandled** | 🔴 Critical | `send()` can return fewer bytes than requested (kernel buffer full); return value was ignored | New `send_all()` retry loop that keeps sending until all bytes are queued or an error occurs. |
| **6. No endian-safe serialization** | 🔴 Critical | Multi-byte header fields written in host byte order (little-endian on x86) — garbage on cross-platform communication | `MessageHeader::to_bytes()` / `from_bytes()` using `htonl`/`ntohs`. All wire values are now big-endian regardless of host CPU. |

#### Files Changed & Components Added

| File | Change |
|---|---|
| `include/protocol/message.hpp` | Added endian-safe `to_bytes()` / `from_bytes()` with `htonl`/`ntohs` |
| `include/protocol/framing.hpp` | **New** — `FramedSocket` wrapping `Socket` for length-prefix framing, and `ParsedMessage` struct |
| `src/protocol/framing.cpp` | **New** — implementation of framing, including single-buffer `send_all` to minimize syscalls and `recv_exact` for robust reads |
| `include/network/socket.hpp` | Added `send_all()` and `recv_exact()` for robust stream handling |
| `src/network/socket.cpp` | Implemented `send_all()` (retry loop) and `recv_exact()` (accumulation loop) |
| `include/network/messaging_node.hpp` | Changed peer tracking to use `std::shared_ptr` to safely pass ownership to threads |
| `src/network/messaging_node.cpp` | Full rewrite of threading logic: fixed Bugs 1, 2, and 3 with annotated before/after comments |
| `CMakeLists.txt` | Integrated `src/protocol/framing.cpp` into the build |

#### Knowledge Base Entries Created (138 KB total)

- `knowledge_base/phase_0/01_tcp_stream_semantics.md` (16 KB)
- `knowledge_base/phase_0/02_data_races_and_mutex_scope.md` (31 KB)
- `knowledge_base/phase_0/03_partial_io_handling.md` (39 KB)
- `knowledge_base/phase_0/04_network_byte_order.md` (25 KB)
- `knowledge_base/phase_0/05_thread_per_connection_problem.md` (27 KB)

---

### ✅ Phase 1 — Binary Protocol & Broker Core (COMPLETE)

**Goal:** Build the actual pub/sub broker described in the README.

#### Architectural Improvements

| Component | Purpose | Implementation Details |
|---|---|---|
| **Broker Core** | Centralized message routing | `Broker` class in `broker.cpp` replaces the generic `MessagingNode`. Handles client connections and routing. |
| **Topic Registry** | Fast topic-based fan-out | `std::unordered_map<string, vector<SocketHandle>>` provides O(1) topic lookup for lightning-fast routing of `PUBLISH` messages to `SUBSCRIBE`d clients. |
| **Client Separation** | Decoupling producers/consumers | Code split into three distinct binaries (`broker`, `producer`, `consumer`) to accurately model a real-world pub/sub environment. |
| **Routing Safety** | Thread-safe fan-out | Uses a snapshot pattern: subscribers are copied into a local `std::vector<shared_ptr<Socket>>` under a lock, and the lock is released before `send_message` blocks. |
| **Decoupled Lifetimes** | Safe Client Tracking | The topic registry stores `SocketHandle` (integer ID) rather than `shared_ptr<Socket>`. This acts as a zero-cost weak reference. If a client disconnects, they are removed from `m_peers`, and routing will safely ignore their orphaned ID during fan-out, preventing "zombie sockets". |
| **Protocol Agnosticism** | Adapter Pattern | The broker tracks raw `Socket` objects instead of `FramedSocket`. `FramedSocket` is instantiated dynamically on the stack at zero cost. This keeps the core connection registry completely independent of the wire protocol. |

#### Files Changed & Components Added

| File | Change |
|---|---|
| `include/broker/broker.hpp` | **New** — `Broker` header defining the topic registry and routing mechanics. |
| `src/broker/broker.cpp` | **New** — `Broker` implementation including `SUBSCRIBE` / `PUBLISH` handling. |
| `src/app/broker_main.cpp` | **New** — Broker executable entry point. |
| `src/app/producer_main.cpp` | **New** — Producer executable entry point. |
| `src/app/consumer_main.cpp` | **New** — Consumer executable entry point. |
| `CMakeLists.txt` | Refactored to build `broker`, `producer`, and `consumer` binaries linking to a shared `messaging_core` static library. |
| `src/app/main.cpp` | *Deleted* — replaced by the three specialized binaries. |
| `src/network/messaging_node.*` | *Deleted* — obsolete p2p node logic removed. |

#### Knowledge Base Entries Created

- `knowledge_base/phase_1/01_pubsub_model.md`
- `knowledge_base/phase_1/02_hash_tables_for_routing.md`
- `knowledge_base/phase_1/03_binary_protocol_design.md`

---

### 🔲 Phase 2 — Abstraction Layer & Baseline Benchmarking (NEXT)

**Goal:** Prepare the codebase for multiple transport strategies and establish a TCP performance baseline.

**Planned additions:**
- `ITransport` interface to decouple Application logic from Network logic.
- Refactor existing TCP client/broker behind `TcpTransport`.
- Build `benchmarks/` to measure p50/p99 latency and throughput for the basic TCP mode.

---

### 🔲 Phase 3 — TCP Optimization: Event Loops (epoll/IOCP)

**Goal:** Push the centralized TCP Broker strategy to its maximum potential.

**Planned additions:**
- Replace thread-per-connection with `epoll` (Linux) / `IOCP` (Windows).
- Non-blocking socket I/O.
- Benchmark validation (comparing against Phase 2 baseline to prove no regressions).

---

### 🔲 Phase 4 — TCP Optimization: Lock-Free Data Structures

**Goal:** Eliminate mutex contention in the TCP Broker hot path.

**Planned additions:**
- SPSC/MPMC lock-free ring buffers (LMAX Disruptor style).
- Background worker threads for async I/O dispatch.
- Benchmark validation to observe latency jitter reduction.

---

### 🔲 Phase 5 — Brokerless Strategy: UDP Multicast

**Goal:** Introduce the second transport strategy for extreme low-latency environments.

**Planned additions:**
- `UdpMulticastTransport` implementing the `ITransport` interface.
- CLI flags to toggle between `--mode tcp` and `--mode udp`.
- Hardware-level fan-out (no broker executable required).
- Benchmark validation (TCP vs. UDP Multicast latency comparison).

---

### 🔲 Phase 6 — UDP Reliability Layer (NAKs)

**Goal:** Add configurable delivery guarantees to the lossy UDP strategy.

**Planned additions:**
- Sequence numbers in packet headers.
- Negative Acknowledgment (NAK) logic for consumers to request missed packets.
- Retransmission ring buffers on the producer side.

---

### 🔲 Phase 7 — Kernel Bypass (DPDK/XDP)

**Goal:** Achieve the absolute minimum hardware latency for the UDP strategy.

**Planned additions:**
- OS networking stack bypass (user-space NIC polling).
- Thread pinning and CPU isolation.
- Final benchmark validation (targeting sub-10μs latency).

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
