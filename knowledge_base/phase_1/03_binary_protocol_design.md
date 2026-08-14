# Binary Protocol Design

> **TL;DR:** Text-based protocols (like JSON) are bloated and computationally expensive to parse.
> By designing a custom, fixed-layout binary protocol, we eliminate parsing overhead, reduce
> network bandwidth, and achieve deterministic parsing times critical for real-time systems.

---

## 1. Comparing Format Paradigms

When transmitting data over TCP, you must choose a serialization format.

### 1. Self-Describing Text (JSON, XML)
```json
{"type": "PUBLISH", "topic": "BTC_USD", "payload": "price=101"}
```
- **Pros:** Human-readable, schema-less, easy to debug.
- **Cons:** High bandwidth overhead (quotes, braces, keys repeated every time). Requires CPU-intensive string parsing, memory allocations, and branching.

### 2. Schema-Based Binary (Protobuf, FlatBuffers)
Requires an IDL (Interface Definition Language) file compiled into C++ classes.
- **Pros:** Extremely compact, backward-compatible, auto-generates client code.
- **Cons:** Requires a heavy third-party dependency (Google Protobuf). Protobuf still requires memory allocation during deserialization.

### 3. Hand-Rolled Binary (Our Approach)
We define the exact byte layout manually in C++.
- **Pros:** Zero dependencies. Zero-allocation parsing (we cast bytes directly to structs). Absolute maximum performance.
- **Cons:** Hard to maintain across versions. Endianness must be handled manually (see Phase 0).

---

## 2. The Overhead of Text Protocols

Why not just use JSON for a messaging broker?

**1. Parse Time:** Parsing JSON requires scanning every character, escaping strings, and building a DOM tree in memory. A fast JSON parser takes ~1-2 microseconds per message. Our binary header parse takes **~2 nanoseconds** (just reading integers).
**2. Size:** The JSON string above is 63 bytes. Our binary equivalent is 24 bytes.
**3. Throughput:** If a 10Gbps link can push ~1.2 GB/s, saving 40 bytes per message allows you to push millions of additional messages per second.

---

## 3. Our Wire Layout

We use a **Length-Prefix Framing** approach with a fixed 8-byte header.

```
Byte Offset:
0        4        6        8                     length
+--------+--------+--------+---------------------+-------------------+
| length |  type  | topic_ | topic string        | payload bytes     |
| (u32)  | (u16)  | len    | (UTF-8, no \0)      | (raw, arbitrary)  |
|        |        | (u16)  |                     |                   |
+--------+--------+--------+---------------------+-------------------+
```

### Field Breakdown:
1. `length` (4 bytes): The total size of the entire frame, *including the header itself*.
   - Max size: `4,294,967,295` bytes (4GB). We artificially cap this at 64MB to prevent memory exhaustion DoS attacks.
2. `type` (2 bytes): Identifies the action.
3. `topic_len` (2 bytes): How many bytes belong to the topic string.
4. `topic`: The exact number of bytes specified by `topic_len`.
5. `payload`: The remaining bytes. `Payload Size = length - 8 - topic_len`.

---

## 4. MessageTypes

The `type` field maps to a `uint16_t` enum:

```cpp
enum class MessageType : uint16_t {
    UNKNOWN = 0,
    SUBSCRIBE = 1,  // Client tells broker it wants a topic
    PUBLISH = 2,    // Client sends data to broker
    DATA = 3        // Broker delivers data to client
};
```

Why separate `PUBLISH` and `DATA`?
- Semantic clarity. `PUBLISH` is an ingress command. `DATA` is an egress event.
- It allows future expansion (e.g., `PUBLISH_ACK` to acknowledge receipt).

---

## 5. Why No Null Terminators?

In C, strings end with a `\0` (null terminator).

```c
char topic[] = {'B','T','C','\0'}; // 4 bytes
```

**We do NOT send the `\0` over the wire.**

Why?
1. **Security/Robustness:** If you rely on a null terminator to find the end of a string in a TCP stream, a malicious client can send a 1GB stream without a `\0`, blowing up your memory.
2. **Speed:** Finding a `\0` requires an O(N) scan (`strlen()`).
3. **Binary safety:** If the payload happens to contain a `0x00` byte, parsing breaks.

Instead, we use explicit length fields (`topic_len`).
```cpp
std::string topic(buffer.begin(), buffer.begin() + hdr.topic_length);
```
This is O(1), safe, and explicitly bounded.

---

## 6. Future Considerations: Versioning

The biggest flaw in our hand-rolled protocol is **lack of versioning**.

If we want to add a `timestamp` field to the header in the future, we have to change the layout to 16 bytes.
Old clients connecting to the new broker will send 8-byte headers. The new broker will misinterpret the payload as part of the new header.

**How production systems handle this:**
1. **Magic Bytes + Version:** The first 2 bytes of the header are always a magic identifier (e.g., `0xBB` `0xBB`) followed by a 1-byte version number.
2. The broker reads the version, and delegates to a specific parser (`ParserV1`, `ParserV2`).

For this educational project, we will stick to V1 for simplicity, but in a real system, versioning is mandatory on Day 1.

---

## Key Takeaways

- Binary protocols offer unmatched latency and throughput by eliminating parse time.
- Fixed-width fields must use specific sizes (`uint32_t`, not `int`).
- Never rely on null terminators for network strings; always send explicit lengths.
- Length fields provide O(1) parsing and protect against buffer overflows.

---

## Further Reading

- [Protocol Buffers Documentation](https://protobuf.dev/)
- [FlatBuffers: Memory Efficient Serialization Library](https://google.github.io/flatbuffers/)
- [RFC 854: Telnet Protocol Specification](https://datatracker.ietf.org/doc/html/rfc854) (An example of an old, in-band text protocol)
