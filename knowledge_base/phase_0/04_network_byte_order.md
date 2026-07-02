# Network Byte Order & Endianness: A Deep-Dive for Protocol Engineers

> **Phase 0 — Foundations | File 04**
> Audience: C++ systems programmers building networked messaging systems.

---

## TL;DR

When two machines communicate over a network, they must agree not just on *what* data means, but on *how* that data is laid out byte-by-byte in memory. Different CPU architectures store multi-byte integers in different orders — a fact called **endianness**. Because the dominant desktop/server CPUs (x86, x86_64, ARM in LE mode) all happen to be little-endian while network protocols (TCP/IP, RFC 791) mandate big-endian ("network byte order"), every multi-byte integer that crosses a wire must be explicitly converted. Forgetting this conversion silently corrupts message types, lengths, and flags in ways that are maddeningly non-deterministic across platforms. This document explains the *why* and *how* of byte-order conversion, struct packing, fixed-width types, serialization patterns, and how modern formats like Protobuf sidestep the problem entirely.

---

## 1. What Is Endianness?

**Endianness** describes the order in which the individual bytes of a multi-byte value are stored in memory — specifically, whether the *most significant byte (MSB)* comes first (big-endian) or the *least significant byte (LSB)* comes first (little-endian).

### 1.1 The Value 0x01020304 in Memory

Consider the 32-bit integer `0x01020304`. In hex, that's four bytes: `01`, `02`, `03`, `04`, where `01` is the most significant byte.

```
Value: 0x01020304
Bytes: [ 0x01 ][ 0x02 ][ 0x03 ][ 0x04 ]
         MSB                       LSB
```

**Big-Endian (network byte order) — MSB first:**
```
Address:  0x1000   0x1001   0x1002   0x1003
Content:  0x01     0x02     0x03     0x04
          ^^^^MSB                    LSB^^^^
```
Reading left-to-right (low address to high address), the most significant byte appears at the lowest address. This is the "natural" reading order humans use for numbers.

**Little-Endian (x86/x86_64/ARM-LE) — LSB first:**
```
Address:  0x1000   0x1001   0x1002   0x1003
Content:  0x04     0x03     0x02     0x01
          ^^^^LSB                    MSB^^^^
```
The least significant byte sits at the lowest address. This feels "backwards" for humans but is very efficient for CPUs that perform arithmetic starting from the low bits.

### 1.2 Visualising with Binary

```
0x01020304 in 32-bit binary:
  00000001 00000010 00000011 00000100
  ^^^^^^^^                  ^^^^^^^^
  Byte 0 (MSB)             Byte 3 (LSB)

Big-Endian memory layout (addresses increasing →):
  [00000001][00000010][00000011][00000100]
   addr+0    addr+1    addr+2    addr+3

Little-Endian memory layout (addresses increasing →):
  [00000100][00000011][00000010][00000001]
   addr+0    addr+1    addr+2    addr+3
```

---

## 2. Why Little-Endian CPUs and Big-Endian Networks?

### 2.1 Historical Context of x86 Little-Endian

Intel chose little-endian for the 8080 (1974) and 8086 (1978) partly because it simplifies certain arithmetic operations: the CPU can begin processing the LSB (which participates in carries first) without waiting for the full value to be loaded. The x86_64 architecture preserved this for compatibility. ARM is bi-endian (configurable) but virtually all modern ARM deployments (Android, iOS, Linux ARM) run in little-endian mode.

### 2.2 RFC 791 and Network Byte Order (Big-Endian)

In September 1981, **RFC 791** defined the Internet Protocol. Section 2.1 states:

> *"The convention in the documentation of Internet Protocols is to express numbers in decimal and to picture data in 'big-endian' order. That is, bits or octets at the left are more significant."*

This choice made network packet headers human-readable in hex dumps and aligned with IBM mainframes (which were dominant in academia at the time). POSIX later codified the conversion functions `htonl`, `htons`, `ntohl`, `ntohs` to translate between whatever the host uses and this mandated network order.

**Rule of thumb:** Any integer field in a protocol header that travels over the network must be stored in big-endian (network byte order).

---

## 3. What Goes Wrong Without Conversion

This section walks through a concrete bug scenario.

### 3.1 The MessageHeader Struct

```cpp
#include <cstdint>

// A simple protocol message header
struct MessageHeader {
    uint32_t type;    // Message type identifier
    uint32_t length;  // Payload length in bytes
};
```

### 3.2 Sender (Little-Endian Machine — x86_64)

```cpp
#include <cstring>
#include <cstdio>

void send_without_conversion() {
    MessageHeader hdr;
    hdr.type   = 0x00000001;  // Type = 1 (e.g., "CONNECT")
    hdr.length = 0x00000064;  // Length = 100 bytes

    // In little-endian memory, hdr.type looks like:
    // [0x01][0x00][0x00][0x00]  ← bytes at increasing addresses
    // hdr.length looks like:
    // [0x64][0x00][0x00][0x00]

    // Sender ships the raw struct bytes over TCP... BUG: no byte-order conversion!
    uint8_t buffer[sizeof(MessageHeader)];
    std::memcpy(buffer, &hdr, sizeof(hdr));

    // buffer now contains: 01 00 00 00 | 64 00 00 00
    // (little-endian layout)
}
```

### 3.3 Receiver (Big-Endian Machine — SPARC, or same machine misinterpreting)

```cpp
void receive_without_conversion(const uint8_t* buffer) {
    MessageHeader hdr;
    std::memcpy(&hdr, buffer, sizeof(hdr));

    // The receiver reads the bytes in big-endian order:
    // buffer = [0x01][0x00][0x00][0x00] → interpreted as 0x01000000 = 16777216!
    // buffer = [0x64][0x00][0x00][0x00] → interpreted as 0x64000000 = 1677721600!

    printf("type   = %u\n", hdr.type);    // Prints: 16777216  (WRONG! Should be 1)
    printf("length = %u\n", hdr.length);  // Prints: 1677721600 (WRONG! Should be 100)

    // The receiver now tries to allocate 1.6 GB for a "100-byte" message.
    // This is a crash, a DoS vector, or worse.
}
```

The bug is **silent** on same-endian machines (tests pass locally!) and only manifests in cross-platform deployments — making it one of the nastiest classes of network bugs.

---

## 4. The htonl / htons / ntohl / ntohs Family

These POSIX functions are the canonical solution for converting between host byte order and network (big-endian) byte order.

| Function | Stands For            | Input     | Output          | Use Case                         |
|----------|-----------------------|-----------|-----------------|----------------------------------|
| `htons`  | host-to-network short | `uint16_t`| `uint16_t` (BE) | Port numbers, 16-bit fields      |
| `htonl`  | host-to-network long  | `uint32_t`| `uint32_t` (BE) | IPv4 addresses, 32-bit fields    |
| `ntohs`  | network-to-host short | `uint16_t`| `uint16_t` (HE) | Read 16-bit field from wire      |
| `ntohl`  | network-to-host long  | `uint32_t`| `uint32_t` (HE) | Read 32-bit field from wire      |

> **Note:** On big-endian machines, all four functions are **no-ops** (the compiler optimises them away). On little-endian machines, they perform a byte-swap. Your code is correct on both.

### 4.1 Correct Send and Receive

```cpp
#include <arpa/inet.h>   // htonl, htons, ntohl, ntohs on POSIX
#include <cstring>
#include <cstdio>

// ── SENDER ──────────────────────────────────────────────────────────
void send_with_conversion(int socket_fd) {
    MessageHeader hdr;
    // Always convert to network byte order before writing to the wire
    hdr.type   = htonl(1);    // Host value 1   → big-endian on wire
    hdr.length = htonl(100);  // Host value 100 → big-endian on wire

    // Safe to send raw bytes now; receiver will call ntohl() to recover values
    ::send(socket_fd, &hdr, sizeof(hdr), 0);
}

// ── RECEIVER ────────────────────────────────────────────────────────
void receive_with_conversion(const uint8_t* buffer) {
    MessageHeader wire_hdr;
    std::memcpy(&wire_hdr, buffer, sizeof(wire_hdr));

    // Convert from network byte order back to host byte order
    uint32_t msg_type   = ntohl(wire_hdr.type);    // Big-endian → host
    uint32_t msg_length = ntohl(wire_hdr.length);  // Big-endian → host

    printf("type   = %u\n", msg_type);    // Prints: 1
    printf("length = %u\n", msg_length);  // Prints: 100
}
```

### 4.2 64-bit Values

There is no standard `htonll` in POSIX (though some systems provide it). Roll your own:

```cpp
#include <cstdint>
#include <bit>           // C++20 std::endian

// Portable 64-bit host-to-network conversion
inline uint64_t htonll(uint64_t value) {
    // C++20 way: check endianness at compile time
    if constexpr (std::endian::native == std::endian::big) {
        return value;  // Already network order
    } else {
        // Byte-swap all 8 bytes
        return ((value & 0xFF00000000000000ULL) >> 56) |
               ((value & 0x00FF000000000000ULL) >> 40) |
               ((value & 0x0000FF0000000000ULL) >> 24) |
               ((value & 0x000000FF00000000ULL) >>  8) |
               ((value & 0x00000000FF000000ULL) <<  8) |
               ((value & 0x0000000000FF0000ULL) << 24) |
               ((value & 0x000000000000FF00ULL) << 40) |
               ((value & 0x00000000000000FFULL) << 56);
    }
}

inline uint64_t ntohll(uint64_t value) { return htonll(value); }  // Self-inverse
```

---

## 5. The from_bytes / to_bytes Pattern with memcpy

Naïve code often tries to cast a `struct*` directly to `uint8_t*` to read individual bytes. This is **undefined behaviour** in C++ due to **strict aliasing rules**.

### 5.1 Why Pointer Casting is Dangerous

#### The Strict Aliasing Rule — Why It Exists

The C++ standard grants the compiler a fundamental optimisation licence called the **strict aliasing rule**:

> A compiler is permitted to assume that pointers of **different types** do not point to the same memory location.

This allows the compiler to keep values in registers across writes through a different-typed pointer, reorder loads/stores, and avoid redundant memory reloads — producing faster code. If the assumption were violated by your pointer cast, the compiler's model of memory state would be wrong, and it would silently generate incorrect output.

```cpp
void add(float* f, int* i) {
    *f += 1.0f;
    *i += 1;
    *f += 1.0f;  // Compiler ASSUMES *i and *f can't point to the same memory.
                 // It may compute *f += 2.0f in a register, skipping a reload.
                 // If f and i DO alias the same address, the result is wrong —
                 // but that is defined as YOUR fault, not the compiler's.
}
```

#### Case 1: `reinterpret_cast<uint8_t*>` to inspect a `uint32_t`'s bytes

```cpp
// ⚠️  TECHNICALLY PERMITTED via the unsigned char* exemption,
//     but fragile — best replaced with memcpy for clarity and portability.
uint32_t value = 0x01020304;
uint8_t* p = reinterpret_cast<uint8_t*>(&value);
uint8_t first_byte = p[0];
```

The C++ standard has a **specific one-way exemption**: `char*`, `unsigned char*`, and `std::byte*` are explicitly allowed to alias *any* object type. This exception exists precisely so programmers can inspect and copy the raw bytes of objects. Since `uint8_t` is almost universally a `typedef` for `unsigned char`, reading `value`'s bytes through `uint8_t*` is technically permitted by this rule.

**So why does this still appear in the "dangerous" category?**

Two reasons:

1. **It is not guaranteed to be `unsigned char` on all platforms.** The standard requires `uint8_t` to be exactly 8 bits, but does not require it to be `unsigned char`. On a hypothetical platform where `uint8_t` is a distinct non-character type, the aliasing exemption would not apply and this would be UB. Writing `memcpy` costs nothing and eliminates all doubt.

2. **The exemption is one-directional.** You can legally read any object through `unsigned char*`. You cannot legally read a `uint8_t[]` buffer through a `uint32_t*`. The moment code does the reverse — and it almost always does, because you need to reconstruct integers from byte buffers — you fall into genuine UB. Establishing `memcpy` as the only pattern prevents the dangerous reversal from ever appearing.

#### Case 2: `reinterpret_cast<uint32_t*>` from a `uint8_t` buffer — genuinely dangerous

```cpp
// ❌ UNDEFINED BEHAVIOUR — two independent grounds
uint8_t raw[4] = {0x01, 0x02, 0x03, 0x04};
uint32_t* vp = reinterpret_cast<uint32_t*>(raw);  // Alignment + aliasing UB
uint32_t v = *vp;  // Could trap on SPARC, older ARM; silently wrong on optimised x86
```

This is UB on **two independent grounds**, either of which alone is sufficient to break your program:

**Ground 1 — Strict Aliasing Violation:**

`raw` is declared as `uint8_t[4]`. Accessing those bytes through a `uint32_t*` is a strict aliasing violation — the aliasing exemption is one-directional (`unsigned char*` can alias anything; `uint32_t*` cannot alias a `char[]`). The compiler is permitted to assume no `uint32_t` object exists at that address, and may:
- Hoist the read of `v` above assignments to `raw`, since it can prove no `uint32_t`-typed store happened.
- Eliminate the load of `v` entirely if it deems the value "provably unwritten".
- Produce correct output in debug builds (`-O0` disables alias-based reordering) and silently wrong output in release builds (`-O2`/`-O3`).

This is the classic **"works in debug, breaks in production"** failure mode.

**Ground 2 — Alignment Violation:**

`uint8_t[]` has alignment of 1 — the compiler may place `raw` at any byte address (e.g., `0x1001`). But `uint32_t` requires **4-byte alignment** on most architectures — it must reside at an address divisible by 4. Dereferencing a misaligned `uint32_t*`:

| Architecture | Effect of misaligned `uint32_t*` dereference |
|---|---|
| x86 / x86_64 | Works by accident — hardware silently handles unaligned loads (with a performance penalty) |
| SPARC | **Hardware bus error → SIGBUS → process terminated** |
| Older ARM (ARMv4, ARMv5) | **SIGBUS or silently reads wrong bytes** |
| Modern ARM (ARMv7+, AArch64) | Usually works, but can be disabled by OS alignment fault settings |

Code that passes CI on x86 and kills the process on the customer's SPARC or embedded ARM board is a real-world failure pattern for this exact bug.

### 5.2 The Safe Pattern: Always Use memcpy

`memcpy` is the **explicitly blessed** mechanism for type-punning in C++. Compilers recognise it and emit exactly the same machine code as the pointer-cast version — zero overhead.

```cpp
#include <cstring>
#include <cstdint>

// ✅ SAFE: serialize a uint32_t into a byte buffer (big-endian / network order)
void to_bytes_be(uint32_t host_value, uint8_t* out) {
    uint32_t net_value = htonl(host_value);  // Convert to network order first
    std::memcpy(out, &net_value, sizeof(net_value));  // Then copy bytes
}

// ✅ SAFE: deserialize a uint32_t from a byte buffer (big-endian / network order)
uint32_t from_bytes_be(const uint8_t* in) {
    uint32_t net_value;
    std::memcpy(&net_value, in, sizeof(net_value));  // Copy bytes first
    return ntohl(net_value);  // Then convert from network to host order
}

// Usage example
void serialize_header(const MessageHeader& hdr, uint8_t* buf) {
    to_bytes_be(hdr.type,   buf);      // Writes bytes 0-3
    to_bytes_be(hdr.length, buf + 4);  // Writes bytes 4-7
}

MessageHeader deserialize_header(const uint8_t* buf) {
    MessageHeader hdr;
    hdr.type   = from_bytes_be(buf);      // Reads bytes 0-3
    hdr.length = from_bytes_be(buf + 4);  // Reads bytes 4-7
    return hdr;
}
```

**Why memcpy fixes both problems:**

- **No aliasing violation.** The C++ standard explicitly blesses `memcpy` for copying object representations between any two addresses, regardless of declared types. The compiler treats it as a full memory barrier for alias analysis — it cannot assume `memcpy`'s arguments don't overlap.
- **No alignment requirement.** `memcpy` operates byte-by-byte internally (or uses unaligned vector instructions where available). It makes no assumption about source or destination alignment.
- **Zero runtime overhead.** A modern compiler recognises `memcpy` of exactly 4 bytes and compiles it to a single `MOV` instruction — identical machine code to the pointer-cast version, but with no UB:

```asm
; What the compiler emits for memcpy(&value, raw, 4) on x86_64:
; (same machine code as the cast, but fully defined behaviour)
mov eax, dword ptr [rdi]
mov dword ptr [rsp-4], eax
```

> **Rule of thumb:** If you need to reinterpret the bytes of one type as another, always use `memcpy`. Never use `reinterpret_cast` on pointers of unrelated types. The compiler generates identical machine code and you have no UB, no alignment hazard, and no platform-dependent surprises.

---

## 6. Struct Padding and Alignment

C++ compilers insert **padding bytes** between struct fields to satisfy alignment requirements. This means `sizeof(struct)` is often larger than the sum of its fields. Sending a raw struct over the wire without accounting for padding will lead to garbled data.

### 6.1 Demonstrating Padding

```cpp
#include <cstdint>
#include <cstdio>

struct Unpadded {
    uint8_t  flag;    // 1 byte
    uint32_t value;   // 4 bytes — but needs 4-byte alignment!
    uint16_t count;   // 2 bytes
};
// Actual memory layout (on most 64-bit compilers):
//   [flag:1][PAD:3][value:4][count:2][PAD:2]  = 12 bytes total, not 7!

struct __attribute__((packed)) Packed {
    uint8_t  flag;    // 1 byte, no padding
    uint32_t value;   // 4 bytes, unaligned (may be slow or fault on some CPUs)
    uint16_t count;   // 2 bytes
};
// Layout: [flag:1][value:4][count:2] = 7 bytes, but accessing value is unaligned UB on SPARC

int main() {
    printf("sizeof(Unpadded) = %zu\n", sizeof(Unpadded));  // Likely 12
    printf("sizeof(Packed)   = %zu\n", sizeof(Packed));    // Likely 7
    return 0;
}
```

### 6.2 The Right Solution: Explicit Field-by-Field Serialization

Instead of fighting the compiler with `__attribute__((packed))` (which causes unaligned access UB on strict architectures), serialize field-by-field:

```cpp
// Portable, safe serialization — no struct packing games
void serialize(const Unpadded& s, uint8_t* buf) {
    buf[0] = s.flag;                         // 1 byte, no conversion needed
    to_bytes_be(s.value, buf + 1);           // 4 bytes, big-endian
    uint16_t net_count = htons(s.count);     // 2 bytes, big-endian
    std::memcpy(buf + 5, &net_count, 2);
    // Wire format is exactly 7 bytes, no padding, fully portable
}
```

> **Rule:** Never send a raw `struct` over the network. Always serialize field-by-field. Use `__attribute__((packed))` only as a last resort, and never dereference unaligned pointers to packed struct members.

---

## 7. Fixed-Width Types: Why uint32_t Instead of int or long

The C/C++ standard only guarantees *minimum* widths for primitive types, not exact widths:

| Type        | Minimum Width | Actual Width (LP64 Linux) | Actual Width (LLP64 Windows) | Actual Width (16-bit embedded) |
|-------------|---------------|---------------------------|------------------------------|-------------------------------|
| `char`      | 8 bits        | 8 bits                    | 8 bits                       | 8 bits                        |
| `short`     | 16 bits       | 16 bits                   | 16 bits                      | 16 bits                       |
| `int`       | 16 bits       | 32 bits                   | 32 bits                      | **16 bits**                   |
| `long`      | 32 bits       | **64 bits**               | **32 bits**                  | 32 bits                       |
| `long long` | 64 bits       | 64 bits                   | 64 bits                      | 64 bits                       |

This means `long` is 64-bit on Linux but 32-bit on Windows — a protocol using `long` for a field will break silently when cross-compiled.

```cpp
// ❌ BAD: platform-dependent width
struct BadHeader {
    unsigned int  type;    // 32-bit on x86_64 Linux, but 16-bit on some embedded
    unsigned long length;  // 64-bit on Linux, 32-bit on Windows!
};

// ✅ GOOD: guaranteed exact width via <cstdint>
#include <cstdint>
struct GoodHeader {
    uint32_t type;    // Always exactly 32 bits, on every platform
    uint32_t length;  // Always exactly 32 bits, on every platform
};
```

> **Rule:** Every field in a protocol struct must use `uint8_t`, `uint16_t`, `uint32_t`, or `uint64_t` from `<cstdint>`. Never use `int`, `long`, `short`, or `unsigned` bare.

---

## 8. Mixed-Endian Machines (The Exotic Cases)

Most engineers will never encounter these, but they are real and documented:

### 8.1 PDP-Endian (Middle-Endian)

The PDP-11 minicomputer (1970s) stored 32-bit integers as two 16-bit words in big-endian order, but the bytes *within each 16-bit word* were in little-endian order. For `0x01020304`:

```
PDP-11 layout:
  Word 0 (high word, big-endian):  [0x02][0x01]  ← bytes swapped within word
  Word 1 (low word, big-endian):   [0x04][0x03]  ← bytes swapped within word
Full memory: [0x02][0x01][0x04][0x03]  — neither BE nor LE!
```

### 8.2 SPARC, PowerPC

SPARC is natively big-endian. PowerPC is bi-endian (configurable per access). These are increasingly rare in commodity computing but common in networking hardware, FPGAs, and DSPs.

### 8.3 Practical Advice

The `std::endian` enum (C++20) lets you detect endianness at compile time:

```cpp
#include <bit>
#include <cstdio>

void print_endianness() {
    if constexpr (std::endian::native == std::endian::big) {
        printf("This machine is big-endian\n");
    } else if constexpr (std::endian::native == std::endian::little) {
        printf("This machine is little-endian\n");
    } else {
        printf("This machine is mixed-endian — extra care needed!\n");
    }
}
```

---

## 9. Testing Serialization: Write-Then-Read Round-Trip Test

A round-trip test is the gold standard for validating your serialization logic. It catches byte-order bugs, padding bugs, and truncation bugs.

```cpp
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdio>

// ── Structs ──────────────────────────────────────────────────────────────
struct MessageHeader {
    uint32_t type;
    uint32_t length;
};

static constexpr size_t WIRE_SIZE = 8;  // 2 × uint32_t, no padding on wire

// ── Serialization ────────────────────────────────────────────────────────
void serialize_header(const MessageHeader& hdr, uint8_t* buf) {
    uint32_t net_type   = htonl(hdr.type);
    uint32_t net_length = htonl(hdr.length);
    std::memcpy(buf,     &net_type,   4);  // Bytes 0–3
    std::memcpy(buf + 4, &net_length, 4);  // Bytes 4–7
}

// ── Deserialization ──────────────────────────────────────────────────────
MessageHeader deserialize_header(const uint8_t* buf) {
    uint32_t net_type, net_length;
    std::memcpy(&net_type,   buf,     4);  // Read bytes 0–3
    std::memcpy(&net_length, buf + 4, 4);  // Read bytes 4–7
    return MessageHeader{ ntohl(net_type), ntohl(net_length) };
}

// ── Round-Trip Test ──────────────────────────────────────────────────────
void test_roundtrip() {
    MessageHeader original = { 42, 1024 };  // type=42, length=1024

    uint8_t wire_buffer[WIRE_SIZE] = {};
    serialize_header(original, wire_buffer);

    // Inspect wire bytes: should be big-endian
    printf("Wire bytes: ");
    for (size_t i = 0; i < WIRE_SIZE; ++i) printf("%02X ", wire_buffer[i]);
    printf("\n");
    // Expected: 00 00 00 2A  00 00 04 00
    //           ↑ type=42    ↑ length=1024 in big-endian

    MessageHeader recovered = deserialize_header(wire_buffer);

    // Verify round-trip fidelity
    assert(recovered.type   == original.type);
    assert(recovered.length == original.length);

    printf("Round-trip test PASSED: type=%u, length=%u\n",
           recovered.type, recovered.length);
}

int main() {
    test_roundtrip();
    return 0;
}
```

> **Tip:** Run this test on both little-endian and big-endian machines (or use QEMU to emulate a big-endian target) to validate portability.

---

## 10. Relation to Protocol Design: Self-Describing vs. Schema-Based Formats

The endianness problem is a special case of the broader question: how do two parties agree on data layout?

### 10.1 Self-Describing Formats

Formats like **JSON** and **XML** embed field names alongside values as text. Since ASCII/UTF-8 has no multi-byte integer representation, there is no endianness to worry about:

```json
{ "type": 42, "length": 1024 }
```

Numbers are transmitted as decimal strings. The trade-off: verbose, slow to parse, large on the wire.

### 10.2 Schema-Based Binary Formats

**Protocol Buffers (Protobuf)**, **FlatBuffers**, **MessagePack**, and **Apache Avro** define an external schema (`.proto` file, schema registry, etc.) and handle byte order internally in their generated code. Engineers never manually call `htonl`.

**Protobuf** uses **varint encoding** (little-endian base-128) for integers and explicitly defines field encoding in the spec — endianness is handled by the generated serializer. Fixed-size types (`fixed32`, `fixed64`) are always stored little-endian in the wire format.

**MessagePack** always uses **big-endian** for multi-byte integers (network byte order), matching the spirit of RFC 791.

**FlatBuffers** uses **little-endian** throughout, optimising for zero-copy on x86 machines.

---

## 11. What JSON / Protobuf / MessagePack Do Differently

| Format       | Endianness Handling      | How                                                            | Trade-off                             |
|--------------|--------------------------|----------------------------------------------------------------|---------------------------------------|
| JSON         | None needed              | All numbers are text strings in ASCII; no binary integers      | Very verbose, slow                    |
| XML          | None needed              | Same as JSON                                                   | Very verbose, slow                    |
| Protobuf     | Handled by codegen       | varints = LE base-128; `fixed32`/`fixed64` = explicit LE       | Requires `.proto` schema + codegen    |
| MessagePack  | Always big-endian        | All multi-byte integers are big-endian per spec                | Compact, schema-less, but still typed |
| FlatBuffers  | Always little-endian     | Zero-copy on LE machines; explicit byte-swap on BE machines    | Very fast, requires schema            |
| Apache Avro  | Always big-endian        | Specified in Avro spec; schema resolved at read time           | Requires schema registry              |
| Raw C struct | Depends on platform      | Whatever the CPU's native order is — you must handle it        | Maximum performance, maximum danger   |

The lesson: in a custom binary protocol (like the one you are building), you *are* the serialization format author. You must make an explicit choice — big-endian is conventional for network protocols; little-endian is an optimisation for LE-only deployments — and enforce it consistently.

---

## 12. Do You Need to Convert the Payload? Header Fields vs. Payload Bytes

A question that naturally arises when working with framed protocols: **the header fields get `htonl`/`htons` treatment — but what about the payload?**

The answer hinges on a single principle:

> **Endianness conversion is required only for values that the receiver will interpret as a multi-byte integer.** The framing layer converts its own header fields. The payload is an opaque byte blob — the application layer is responsible for the byte order of whatever it puts inside.

### 12.1 Why the Framing Layer Converts Header Fields

Our [`MessageHeader`](../include/protocol/message.hpp) contains three fields the transport layer itself must read and act on:

```
uint32_t length        — "read this many bytes next" — used in arithmetic
uint16_t type          — "which message type" — compared against enum constants
uint16_t topic_length  — "where the topic ends in the body" — used as an array index
```

If `length = 1000` is stored as little-endian bytes (`E8 03 00 00`) and a big-endian receiver reads those four bytes as a `uint32_t`, it gets `0xE8030000 = 3,893,346,304`. It would then try to allocate ~3.8 GB of memory and block waiting for 3.8 GB of data that will never arrive. The framing layer *must* agree on numeric values with the other side — hence `htonl`/`htons` on send, `ntohl`/`ntohs` on receive.

### 12.2 Why the Framing Layer Does NOT Convert the Payload

Look at `framing.cpp`:

```cpp
// Sender — payload bytes inserted verbatim, no conversion:
frame.insert(frame.end(), payload.begin(), payload.end());

// Receiver — payload bytes handed back verbatim, no conversion:
out.payload = std::vector<uint8_t>(body.begin() + hdr.topic_length, body.end());
```

The framing layer's contract is: *"I will deliver these exact bytes to the other side."* It does not interpret the payload — it is just a carrier. Byte-order conversion would require knowing the payload's structure, which the transport layer deliberately does not know. **The application layer owns that knowledge.**

### 12.3 The Decision Rule for Payload Content

| What is inside the payload | Needs conversion? | Reason |
|---|---|---|
| UTF-8 / ASCII text | ❌ No | Each character is one byte. There is no multi-byte integer to reorder. `"Hello"` is `48 65 6C 6C 6F` on every machine. |
| JSON string e.g. `{"x": 42}` | ❌ No | Numbers are represented as decimal character sequences (`'4'`, `'2'`), not as binary integers. |
| A raw `uint32_t` value (timestamp, price, ID) | ✅ Yes — application must convert | Sender must `htonl` it before writing into the payload buffer; receiver must `ntohl` after extracting it. |
| A raw `double` or `float` | ✅ Yes — carefully | IEEE 754 floating-point also has byte-order sensitivity. Use `memcpy` to/from `uint32_t`/`uint64_t`, then `htonl`/`htonll`. |
| Protobuf / MessagePack / FlatBuffers blob | ✅ Handled internally by the serializer | These libraries encode multi-byte integers in a fixed byte order defined by their spec — you don't call `htonl` yourself. |

### 12.4 Concrete Example: Text vs. Binary Payload

```cpp
// ── TEXT PAYLOAD (our current usage in broadcast()) ──────────────────────────
// The payload is a UTF-8 string. No conversion needed.
std::string text = "hello world";
std::vector<uint8_t> payload(text.begin(), text.end());
// Bytes: 68 65 6C 6C 6F 20 77 6F 72 6C 64
// Receiver reads these same bytes back and constructs a std::string → correct.
framed.send_message(MessageType::DATA, "topic", payload);

// ── BINARY PAYLOAD (application-layer responsibility) ────────────────────────
// Suppose you want to embed a uint32_t timestamp in the payload.
// YOU must byte-order-convert it — the framing layer will not.
uint32_t timestamp_ms = 1'700'000'000;
uint32_t net_ts = htonl(timestamp_ms);      // Convert before embedding

std::vector<uint8_t> bin_payload(4);
std::memcpy(bin_payload.data(), &net_ts, 4); // memcpy — never pointer cast

framed.send_message(MessageType::DATA, "sensor/temp", bin_payload);

// ── Receiver reconstructs it ─────────────────────────────────────────────────
ParsedMessage msg;
framed.recv_message(msg);
uint32_t net_ts_received;
std::memcpy(&net_ts_received, msg.payload.data(), 4);
uint32_t timestamp_ms_received = ntohl(net_ts_received); // Convert after extracting
```

### 12.5 The Layered Responsibility Model

```
Wire frame:
  [ HEADER (8 bytes) ][ topic (N bytes) ][ payload (M bytes) ]
        │                    │                   │
        │                    │                   │
  FramedSocket           FramedSocket        Application layer
  converts with          copies verbatim     decides:
  htonl/htons             (raw UTF-8)        • text → no conversion
  on send                                    • binary int → htonl/ntohl
  ntohl/ntohs                                • serialized format →
  on recv                                      format handles it
```

### 12.6 The Topic String — Same Rule as Text Payload

The topic (e.g., `"sensor/temperature"`) is a UTF-8 string, so it also needs no conversion — raw bytes go on the wire, raw bytes come off. The *length* of the topic (`topic_length` in the header) is a `uint16_t` and **does** get `htons`/`ntohs` treatment because the receiver uses it as a numeric index to slice the body.

---

## Key Takeaways

- **Endianness is about byte order within a multi-byte integer.** Big-endian stores MSB first; little-endian stores LSB first.
- **x86, x86_64, and ARM (in default mode) are little-endian.** Network protocols (RFC 791) mandate big-endian ("network byte order").
- **Always convert with htonl/htons before writing to the wire; always convert with ntohl/ntohs after reading from the wire.** On big-endian machines these are no-ops; on little-endian machines they byte-swap.
- **Never send a raw struct over the network.** Padding, alignment, and endianness make it non-portable. Serialize field-by-field with explicit byte-order conversion.
- **Use memcpy for type-punning, not pointer casts.** Casting a `struct*` to `uint8_t*` violates strict aliasing rules and is undefined behaviour.
- **Use `uint32_t`, `uint16_t`, etc. from `<cstdint>`.** `int` and `long` have platform-dependent widths.
- **Use `__attribute__((packed))` only as a last resort** and never access unaligned fields via pointers on strict-alignment architectures.
- **Write round-trip tests.** Serialize → inspect wire bytes → deserialize → assert equality. Run on multiple platforms if possible.
- **Modern formats (Protobuf, MessagePack, FlatBuffers) handle endianness in their serializers**, freeing you from manual conversion at the cost of schema overhead.
- **In a custom binary protocol, pick one byte order and document it** — big-endian is the conventional choice for interoperability with existing network tooling.
- **Header fields vs. payload:** The framing layer converts its own integer fields (lengths, types). The payload is an opaque byte blob — conversion is the application layer's responsibility. Text/JSON payloads need none; raw binary integers and floats inside the payload must be converted by the application before embedding and after extracting.

---

## Further Reading

- **RFC 791** — *Internet Protocol* (1981): Defines network byte order. https://www.rfc-editor.org/rfc/rfc791
- **RFC 1700** — *Assigned Numbers*: Context on network data representations.
- **POSIX `arpa/inet.h`** — `htonl`, `htons`, `ntohl`, `ntohs` documentation.
- **cppreference — `std::endian`** (C++20): https://en.cppreference.com/w/cpp/types/endian
- **C++ Standard §6.8** — Object representation and `memcpy`-based type punning.
- **Protocol Buffers Encoding Guide**: https://protobuf.dev/programming-guides/encoding/
- **MessagePack Specification**: https://github.com/msgpack/msgpack/blob/master/spec.md
- **FlatBuffers White Paper**: https://google.github.io/flatbuffers/
- **"Computer Organization and Design" — Patterson & Hennessy**: Chapter 2 covers endianness in the context of MIPS (big-endian) and x86 (little-endian).
- **"TCP/IP Illustrated, Volume 1" — W. Richard Stevens**: Deep dive into how byte order manifests in real protocol headers (IP, TCP, UDP).
- **Beej's Guide to Network Programming**: https://beej.us/guide/bgnet/ — Practical C examples of `htonl`/`ntohl` in socket programming.
