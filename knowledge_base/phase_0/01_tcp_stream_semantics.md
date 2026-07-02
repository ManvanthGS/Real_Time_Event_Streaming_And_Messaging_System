# TCP Stream Semantics & Message Framing

> **TL;DR:** TCP is a *byte stream*, not a *message stream*. A single `send()` call on the sender
> can arrive as 1, 2, or 100 `recv()` calls on the receiver — split at arbitrary byte positions.
> Without explicit framing, you cannot know where one message ends and the next begins.
> This file explains why, shows the bug it caused in this project, and details the fix.

---

## Table of Contents

1. [What TCP Actually Guarantees](#1-what-tcp-actually-guarantees)
2. [The Stream vs. Message Confusion](#2-the-stream-vs-message-confusion)
3. [How Partial Reads Happen](#3-how-partial-reads-happen)
4. [The Bug in the Original Code](#4-the-bug-in-the-original-code)
5. [Message Framing Strategies](#5-message-framing-strategies)
6. [Our Solution: Length-Prefix Framing](#6-our-solution-length-prefix-framing)
7. [The recv_exact() Pattern](#7-the-recv_exact-pattern)
8. [TCP Coalescing and Nagle's Algorithm](#8-tcp-coalescing-and-nagles-algorithm)
9. [Testing Framing Correctness](#9-testing-framing-correctness)
10. [Key Takeaways](#10-key-takeaways)
11. [Further Reading](#11-further-reading)

---

## 1. What TCP Actually Guarantees

TCP provides exactly **three guarantees** about the byte stream:

1. **Ordered delivery** — bytes arrive in the same order they were sent.
2. **Reliable delivery** — lost packets are retransmitted automatically.
3. **Error detection** — corrupted bytes are detected and rejected (16-bit checksum + TCP optional MD5).

**TCP does NOT guarantee:**
- That a single `send(N bytes)` results in a single `recv(N bytes)` on the other end.
- That consecutive `send()` calls result in separate `recv()` calls.
- Any timing relationship between sends and receives.

This is fundamental and cannot be changed. It is the correct design for a byte stream — it gives the OS freedom to batch, split, and re-order segments for efficiency.

---

## 2. The Stream vs. Message Confusion

Newcomers often think of a TCP connection as a "packet pipe":

```
Sender:                           Receiver:
send("hello")    ─────────────>  recv() returns "hello"   ← WRONG mental model
send("world")    ─────────────>  recv() returns "world"   ← WRONG mental model
```

The correct mental model is a **garden hose**:

```
Sender:                              Receiver (TCP stream):
send("hello")   ──┐                  recv() might return "hel"
send("world")   ──┤ TCP stream ───>  recv() might return "loworld"
                  └───────────────>  recv() might return "" (buffered, not ready yet)
```

All three are equally valid outcomes. The bytes will all arrive eventually, in order,
but the *framing* (grouping into messages) is entirely your responsibility.

---

## 3. How Partial Reads Happen

### Scenario 1: Sender side fragmentation

You call `send(2048 bytes)`. The kernel's TCP send buffer is only 1500 bytes free.
It queues 1500 bytes into one TCP segment and holds the rest. The next `recv()` on
the other side returns 1500 bytes. Later, a second `recv()` returns 548 bytes.

```
send(2048)
        ↓
[kernel send buffer: 128KB]
        ↓ MTU = 1500 bytes
[IP packet 1: 1500 bytes]  ─────────────────────> recv() returns 1500
[IP packet 2: 548 bytes]   ─────────────────────> recv() returns 548
```

### Scenario 2: Nagle's Algorithm coalescing

You call `send("header")` and then immediately `send("payload")`. Nagle's algorithm
(enabled by default) may hold `"header"` in the kernel buffer for up to 200ms,
waiting to see if more data arrives to fill a full TCP segment. If `"payload"` arrives
before the timer fires, both are sent in **one segment**. Now the receiver's single
`recv()` returns `"headerpayload"` — both messages merged.

### Scenario 3: CPU scheduler preemption

The receiver's thread is preempted between two `recv()` calls. During that window,
two full messages arrive and fill the kernel receive buffer. The next `recv()` returns
**both messages concatenated**.

---

## 4. The Bug in the Original Code

```cpp
// src/network/messaging_node.cpp — ORIGINAL (BROKEN)
void MessagingNode::receiveLoop(SocketHandle handle)
{
    std::vector<uint8_t> buffer;
    while (m_running)
    {
        if (m_peers[handle]->receive(buffer, 1024) <= 0)  // ← reads up to 1024 bytes
        {
            // ...disconnect handling...
            break;
        }
        // BUG: buffer may contain:
        //   - Half a message (partial read)
        //   - 1.5 messages (coalesced)
        //   - 3 full messages concatenated
        std::string msg(buffer.begin(), buffer.end());
        // "Processing" this as-if it were a complete message is WRONG.
        LOG_DEBUG("Received message: " << msg);
    }
}
```

### What goes wrong under load

Sender sends two messages rapidly:
```
send("SUBSCRIBE:BTC_USD")   → 18 bytes
send("SUBSCRIBE:ETH_USD")   → 18 bytes
```

Receiver might get:
```
recv() → "SUBSCRIBE:BTC_USDSUBSCRIBE:ETH_USD"   (36 bytes, both merged)
recv() → "SUBSCRIBE:BTC_US"                      (16 bytes, mid-message split)
```

Both cases are **undetectable without framing**. The receiver has no idea how many
complete messages are in the buffer.

---

## 5. Message Framing Strategies

There are three standard approaches:

### 5.1 Fixed-Length Messages

Every message is exactly N bytes. No framing field needed.

```
Message 1: [16 bytes][16 bytes][16 bytes]...
```

**Pros:** Simplest possible; zero framing overhead.
**Cons:** Inflexible — can't send variable-length data without padding.
**Use when:** High-frequency trading tick data with fixed schemas.

### 5.2 Delimiter-Based Framing

Messages are separated by a special byte sequence (e.g., `\r\n` in HTTP/1.1).

```
"GET / HTTP/1.1\r\nHost: example.com\r\n\r\n"
```

**Pros:** Human-readable; easy to debug.
**Cons:** Delimiter must be escaped in payload; scanning for delimiter is O(n).
**Use when:** Text protocols (HTTP, SMTP, Redis RESP).

### 5.3 Length-Prefix Framing (Our Choice)

A fixed-size header at the start of each message contains the total length.
Receiver reads the header first, then reads exactly `length` more bytes.

```
[4-byte length][2-byte type][2-byte topic_len][topic][payload]
     8 bytes    ←────────────── variable ──────────────────────>
```

**Pros:**
- O(1) to know the message boundary
- No scanning required
- Works for binary data (no delimiter escaping)
- Efficient: two `recv_exact()` calls per message regardless of size

**Cons:** Must know message size before sending (not suitable for infinite streams).
**Use when:** Binary protocols (this project, Kafka wire protocol, gRPC).

---

## 6. Our Solution: Length-Prefix Framing

### Wire Format

```
┌─────────────────────────────────────────────────────────────┐
│ length       (uint32, 4 bytes, big-endian)                  │
│ type         (uint16, 2 bytes, big-endian)                  │
│ topic_length (uint16, 2 bytes, big-endian)                  │
├─────────────────────────────────────────────────────────────┤
│ topic        (topic_length bytes, UTF-8)                    │
├─────────────────────────────────────────────────────────────┤
│ payload      (length - 8 - topic_length bytes)              │
└─────────────────────────────────────────────────────────────┘
```

The `length` field = HEADER_SIZE + topic_length + payload_size.
It always includes the header itself so the receiver always knows the total frame size
after reading just the first 8 bytes.

### Send Protocol (FramedSocket::send_message)

```cpp
bool FramedSocket::send_message(MessageType type, const std::string& topic,
                                 const std::vector<uint8_t>& payload)
{
    MessageHeader hdr{};
    hdr.topic_length = static_cast<uint16_t>(topic.size());
    hdr.type         = static_cast<uint16_t>(type);
    hdr.length       = HEADER_SIZE + topic.size() + payload.size();

    std::vector<uint8_t> frame;
    frame.reserve(hdr.length);  // ← allocate exact size upfront

    hdr.to_bytes(frame);        // ← serialize to network byte order
    frame.insert(frame.end(), topic.begin(), topic.end());
    frame.insert(frame.end(), payload.begin(), payload.end());

    return m_socket.send_all(frame);  // ← guaranteed full write
}
```

**Why one buffer?** Building the full frame in a single `std::vector` before calling
`send_all()` means one syscall instead of three. Every syscall costs ~200ns on modern
hardware (user↔kernel context switch). At 500K messages/sec, 3× syscalls per message =
300K extra syscalls/sec = ~60ms/sec wasted in syscall overhead alone.

### Receive Protocol (FramedSocket::recv_message)

```cpp
bool FramedSocket::recv_message(ParsedMessage& out)
{
    // Step 1: Always read exactly 8 bytes for the header.
    std::vector<uint8_t> hdr_buf;
    if (!m_socket.recv_exact(hdr_buf, HEADER_SIZE))
        return false;

    // Step 2: Decode from network byte order.
    MessageHeader hdr = MessageHeader::from_bytes(hdr_buf.data());

    // Step 3: Validate before allocating (DoS prevention).
    if (hdr.length < HEADER_SIZE || hdr.length > 64*1024*1024)
        return false;
    if (hdr.topic_length > hdr.length - HEADER_SIZE)
        return false;

    // Step 4: Read exactly the body bytes.
    uint32_t body_size = hdr.length - HEADER_SIZE;
    std::vector<uint8_t> body;
    if (body_size > 0 && !m_socket.recv_exact(body, body_size))
        return false;

    // Step 5: Slice topic and payload.
    out.type    = static_cast<MessageType>(hdr.type);
    out.topic   = std::string(body.begin(), body.begin() + hdr.topic_length);
    out.payload = std::vector<uint8_t>(body.begin() + hdr.topic_length, body.end());
    return true;
}
```

---

## 7. The recv_exact() Pattern

This is the cornerstone of correct framing:

```cpp
bool Socket::recv_exact(std::vector<uint8_t>& buffer, size_t size)
{
    buffer.resize(size);
    size_t total_received = 0;

    while (total_received < size)
    {
        int bytes = ::recv(
            m_handle,
            reinterpret_cast<char*>(buffer.data() + total_received),
            static_cast<int>(size - total_received),  // ← only request remaining bytes
            0
        );

        if (bytes <= 0)
        {
            buffer.clear();
            return false;  // 0 = peer closed (FIN), -1 = error
        }

        total_received += static_cast<size_t>(bytes);
    }
    // total_received == size — guaranteed
    return true;
}
```

### Key points

1. **Pointer arithmetic** (`buffer.data() + total_received`): Each iteration starts
   writing at the *next unfilled position* in the buffer.

2. **Remaining bytes** (`size - total_received`): We only request what's still missing,
   so if TCP delivers 1 byte at a time (extreme fragmentation), we make progress and
   never overwrite already-received data.

3. **The break conditions:**
   - `bytes == 0`: Peer sent TCP FIN (graceful close). Return `false`.
   - `bytes < 0`: Error. On Linux check `errno`; on Windows check `WSAGetLastError()`.
   - `bytes > 0`: Normal partial read. Loop continues.

4. **Why not `MSG_WAITALL`?** On Linux, `MSG_WAITALL` tells `recv()` to block until all
   bytes arrive. But it can still return early if a signal is received (EINTR). The explicit
   loop handles this correctly. On Windows, `MSG_WAITALL` has different semantics and
   may not be supported on all socket types. The loop is portable and explicit.

---

## 8. TCP Coalescing and Nagle's Algorithm

### Nagle's Algorithm (RFC 896)

Nagle's algorithm is enabled by default on all TCP sockets. Its rule:

> If there is unacknowledged data in flight, hold small outgoing segments in the buffer
> until either: (a) a full MSS-sized segment accumulates, or (b) all outstanding data
> is acknowledged.

This is excellent for throughput (fewer small packets = less header overhead).
It is bad for latency when you send small messages sequentially:

```
t=0ms:  send(header, 8 bytes)   ← held in buffer (< MSS, ack pending)
t=0ms:  send(payload, 100 bytes) ← merged with header, sent as one segment
t=200ms: ← if no more data, Nagle timer fires and flushes
```

### TCP_NODELAY

For latency-critical paths (e.g., order acknowledgments), disable Nagle:

```cpp
int flag = 1;
setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
           reinterpret_cast<const char*>(&flag), sizeof(flag));
```

This will be done in Phase 4 as part of I/O optimization.

### TCP_CORK (Linux only)

`TCP_CORK` is the opposite of `TCP_NODELAY`: it holds all data until the cork is
removed. Useful for our single-buffer framing approach:

```cpp
// Cork: accumulate header + topic + payload in kernel
int cork = 1;
setsockopt(fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
send(header); send(topic); send(payload);
// Uncork: flush the whole frame as one TCP segment
cork = 0;
setsockopt(fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
```

We avoid needing `TCP_CORK` by building a single `std::vector` frame before sending.

---

## 9. Testing Framing Correctness

### Experiment 1: Simulate partial reads using `socketpair()`

```cpp
// On Linux: socketpair() creates two connected sockets in one process.
// Use send() with small chunks to simulate fragmentation.
int sv[2];
socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

// Producer: send message in two halves
std::vector<uint8_t> frame = build_frame(MessageType::DATA, "topic", "hello");
send(sv[0], frame.data(), 4, 0);                  // send first 4 bytes (partial header)
send(sv[0], frame.data() + 4, frame.size() - 4, 0); // send rest

// Consumer: recv_message should still reconstruct the full message
Socket consumer_sock(sv[1]);
FramedSocket framed(consumer_sock);
ParsedMessage msg;
assert(framed.recv_message(msg));
assert(msg.topic == "topic");
assert(std::string(msg.payload.begin(), msg.payload.end()) == "hello");
```

### Experiment 2: Send two messages, verify they're received separately

```cpp
send_message(sv[0], MessageType::DATA, "a", "first");
send_message(sv[0], MessageType::DATA, "b", "second");
// Both messages sent without waiting for a recv() in between.
// TCP may coalesce them. recv_message() must still return them separately.

ParsedMessage m1, m2;
assert(framed.recv_message(m1));  // must return "first"
assert(framed.recv_message(m2));  // must return "second"
assert(m1.topic == "a");
assert(m2.topic == "b");
```

---

## 10. Key Takeaways

| Concept | Rule |
|---|---|
| TCP is a byte stream | Never assume one `send()` = one `recv()` |
| Framing is your job | Use length-prefix, delimiter, or fixed-size |
| Length-prefix is best | O(1) boundary detection, works for binary |
| `recv_exact()` is the primitive | Always loop until the buffer is full |
| Build one frame buffer | One `send_all()` = one syscall = lower latency |
| Validate before allocating | `length` field can be malicious; cap at 64MB |

---

## 11. Further Reading

- **Stevens, W. Richard — "Unix Network Programming Vol. 1"** — Chapter 3 (Sockets) and
  Chapter 8 (TCP Sockets). The definitive reference for all socket programming.
- **RFC 793** — The original TCP specification. Section 1.5 defines the stream model.
- **RFC 896** — Nagle's algorithm ("Congestion Control in IP/TCP Internetworks").
- **Beej's Guide to Network Programming** — https://beej.us/guide/bgnet/ — free, excellent
  intro to BSD socket framing patterns.
- **Kafka Wire Protocol** — https://kafka.apache.org/protocol — a production example of
  length-prefix framing at scale (used by a system handling trillions of messages/day).
