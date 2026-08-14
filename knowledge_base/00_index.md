# Knowledge Base — Real-Time Event Streaming & Messaging System

> This knowledge base is a **living reference** updated after every phase.
> Each document explains the *why* behind a design decision or bug fix,
> not just the *what*. Read these alongside the code and the design document.

---

## How to Use This Knowledge Base

- **Refer to it when reading code** — every fix in the source has a `See: knowledge_base/...` comment.
- **Read it before a phase** — understand the concepts before implementing them.
- **Update it after learning** — if you discover something new or correct an error, update the file.

---

## Phase 0 — Foundation Bug Fixes

> Theme: Making what exists *correct* before making it fast.
> Core concepts: TCP stream semantics, thread safety, I/O correctness, binary protocols.

| File | Topic | Bugs Addressed |
|---|---|---|
| [01_tcp_stream_semantics.md](phase_0/01_tcp_stream_semantics.md) | TCP is a byte stream, not a message stream. Length-prefix framing, `recv_exact()`, `send_all()`. | Bug 2 (no framing), Bug 5 (partial send) |
| [02_data_races_and_mutex_scope.md](phase_0/02_data_races_and_mutex_scope.md) | Data races, undefined behavior, mutex scope, `shared_ptr` for safe concurrent ownership, the snapshot pattern, `std::atomic`. | Bug 1 (data race), Bug 3 (lock during I/O) |
| [03_partial_io_handling.md](phase_0/03_partial_io_handling.md) | Kernel send/receive buffers, partial write/read mechanics, retry loops, blocking vs. non-blocking sockets. | Bug 5 (partial send) |
| [04_network_byte_order.md](phase_0/04_network_byte_order.md) | Endianness (little vs. big), `htonl`/`ntohl` family, safe struct serialization with `memcpy`, fixed-width types. | Bug 6 (endianness) |
| [05_thread_per_connection_problem.md](phase_0/05_thread_per_connection_problem.md) | OS thread costs, context switching, the C10K problem, preview of event loops. | Bug 4 (acknowledged, fixed Phase 2) |

---

## Phase 1 — Broker Core

> Theme: Building the actual pub/sub routing system.
> Core concepts: Hash table internals, O(1) routing, binary protocol design, topic fan-out.

| File | Topic |
|---|---|
| [01_pubsub_model.md](phase_1/01_pubsub_model.md) | Producer/consumer/broker roles, topic fan-out, delivery semantics |
| [02_hash_tables_for_routing.md](phase_1/02_hash_tables_for_routing.md) | `std::unordered_map` internals, load factor, rehashing, O(1) routing |
| [03_binary_protocol_design.md](phase_1/03_binary_protocol_design.md) | Self-describing vs schema-based formats, Protobuf vs hand-rolled binary |

---

## Phase 2 — Abstraction Layer & Baseline Benchmarking *(coming soon)*

> Theme: Preparing the codebase for multiple transport strategies.
> Core concepts: Interfaces, latency percentiles, benchmarking.

| File | Topic |
|---|---|
| `phase_2/01_transport_abstraction.md` | Dependency injection, interfaces vs inheritance |
| `phase_2/02_measuring_latency.md` | p50, p99, coordinated omission, HDR histogram |

---

## Phase 3 — TCP Optimization: Event Loops *(coming soon)*

> Theme: Scaling the centralized TCP Broker connections.
> Core concepts: `epoll`, IOCP, edge vs. level triggering, non-blocking I/O.

| File | Topic |
|---|---|
| `phase_3/01_epoll_fundamentals.md` | `epoll_wait`, O(1) event notification, non-blocking sockets |
| `phase_3/02_event_loop_design.md` | Single-threaded event loops, reactor pattern, callback dispatch |

---

## Phase 4 — TCP Optimization: Lock-Free Queues *(coming soon)*

> Theme: Eliminating mutex contention from the TCP broker hot path.
> Core concepts: CPU cache lines, false sharing, `std::atomic`, ring buffers.

| File | Topic |
|---|---|
| `phase_4/01_cache_lines_and_false_sharing.md` | 64-byte cache lines, MESI coherence, `alignas(64)` |
| `phase_4/02_memory_ordering.md` | `relaxed`, `acquire`, `release`, `seq_cst` |
| `phase_4/03_spsc_ring_buffer.md` | Single-producer single-consumer lock-free queue |

---

## Phase 5 — Brokerless Strategy: UDP Multicast *(coming soon)*

> Theme: Extreme low-latency by dropping the middleman.
> Core concepts: UDP vs TCP, IP Multicast, IGMP.

| File | Topic |
|---|---|
| `phase_5/01_udp_vs_tcp_latency.md` | Head-of-Line blocking, stateless protocols, jitter |
| `phase_5/02_hardware_multicast.md` | How network switches duplicate packets, IGMP joins |

---

## Phase 6 — UDP Reliability Layer *(coming soon)*

> Theme: Building guarantees on top of a lossy protocol.
> Core concepts: Sequence numbers, NAKs, at-least-once delivery.

| File | Topic |
|---|---|
| `phase_6/01_sequence_numbers_and_naks.md` | Negative Acknowledgments, gap detection |
| `phase_6/02_retransmission_buffers.md` | Producer-side ring buffers for replay requests |

---

## Phase 7 — Kernel Bypass *(coming soon)*

> Theme: Bypassing the OS network stack entirely for sub-10μs latency.
> Core concepts: DPDK, XDP, CPU pinning, user-space polling.

| File | Topic |
|---|---|
| `phase_7/01_kernel_bypass_intro.md` | DPDK vs standard sockets, interrupt vs polling |
| `phase_7/02_thread_pinning.md` | CPU affinity, NUMA nodes, isolating cores |

---

## Reference: Glossary

| Term | Definition |
|---|---|
| **Byte stream** | TCP's delivery model — ordered bytes with no message boundaries |
| **Framing** | Adding structure to a byte stream to identify message boundaries |
| **Length-prefix** | A framing scheme where a fixed-size header encodes the payload length |
| **recv_exact()** | A loop over `recv()` that accumulates bytes until exactly N are received |
| **send_all()** | A loop over `send()` that retries until all bytes are sent |
| **Data race** | Two threads access the same memory concurrently, at least one writes — UB in C++ |
| **RAII** | Resource Acquisition Is Initialization — tie resource lifetime to object lifetime |
| **shared_ptr** | Reference-counted smart pointer — object is alive until last copy is destroyed |
| **Network byte order** | Big-endian byte order — the standard convention for multi-byte values on the wire |
| **htonl / ntohl** | host-to-network-long / network-to-host-long — converts between host and network order |
| **epoll** | Linux I/O event notification mechanism — O(1) per event, scales to millions of fds |
| **IOCP** | Windows I/O Completion Ports — kernel-managed async I/O with thread pool dispatch |
| **False sharing** | Two variables on the same cache line written by different cores — causes unnecessary cache invalidation |
| **Memory ordering** | Rules governing when memory writes by one thread become visible to other threads |
| **Coordinated omission** | A benchmarking error where slow responses are not counted, making p99 look better than it is |
