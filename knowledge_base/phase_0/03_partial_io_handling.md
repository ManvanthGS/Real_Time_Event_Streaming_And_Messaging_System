# Partial I/O Handling in TCP Networking

> **Phase 0 — Foundation Concepts | Document 03**

---

## TL;DR

TCP is a **stream protocol**, not a message protocol. When you call `send()` or `recv()`, the kernel does not guarantee that all the bytes you asked for will be transferred in a single call. Instead, it moves as many bytes as fit into its internal buffers and returns how many it actually moved. If you ignore this return value and assume the whole message was sent or received, you will silently corrupt or truncate data — a class of bug that is notoriously hard to reproduce and nearly impossible to debug in production. This document explains *why* partial I/O happens at every layer from the kernel send buffer to TCP flow control, shows concrete data-corruption scenarios, and walks through the canonical `send_all()` / `recv_exact()` loop patterns that every production TCP application must implement.

---

## Table of Contents

1. [Why send() and recv() Return Partial Results](#1-why-send-and-recv-return-partial-results)
2. [SO_SNDBUF / SO_RCVBUF Socket Options](#2-so_sndbuf--so_rcvbuf-socket-options)
3. [The Broken send() — A Concrete Corruption Example](#3-the-broken-send--a-concrete-corruption-example)
4. [The send_all() Retry Loop Pattern](#4-the-send_all-retry-loop-pattern)
5. [The recv_exact() Accumulation Loop Pattern](#5-the-recv_exact-accumulation-loop-pattern)
6. [Interpreting send()/recv() Return Values](#6-interpreting-sendrecv-return-values)
7. [Blocking vs. Non-Blocking Sockets and Partial I/O](#7-blocking-vs-non-blocking-sockets-and-partial-io)
8. [Why MSG_WAITALL Is Not a Complete Solution](#8-why-msg_waitall-is-not-a-complete-solution)
9. [Windows Differences: WSAEWOULDBLOCK vs EAGAIN](#9-windows-differences-wsaewouldblock-vs-eagain)
10. [Partial I/O and TCP Framing — The Two-Fix Rule](#10-partial-io-and-tcp-framing--the-two-fix-rule)
11. [Exercises](#11-exercises)
12. [Key Takeaways](#key-takeaways)
13. [Further Reading](#further-reading)

---

## 1. Why send() and recv() Return Partial Results

### The Kernel Buffer Model

Every connected TCP socket has **two kernel-managed ring buffers** sitting between your process and the network:

```
 Your process (write/send)
        │
        ▼
 ┌─────────────────────────┐
 │  Kernel Send Buffer     │  ← SO_SNDBUF controls this size
 │  (sk_wmem_queued)       │
 └────────────┬────────────┘
              │  TCP stack drains this into segments
              ▼
         [network wire / NIC]
              │
              ▼
 ┌─────────────────────────┐
 │  Kernel Receive Buffer  │  ← SO_RCVBUF controls this size
 │  (sk_rmem_alloc)        │
 └────────────┬────────────┘
              │
              ▼
 Your process (read/recv)
```

When you call `send(fd, buf, 4096, 0)`, you are **not** writing to the wire. You are writing into the kernel's send buffer. The kernel then independently drains that buffer using TCP segments sized by MSS (typically 1460 bytes on Ethernet) and governed by TCP flow control (the receiver's advertised window) and congestion control (cwnd).

### Why a Single send() Call May Not Accept All Bytes

`send()` copies data from your user-space buffer into the kernel send buffer. If the kernel send buffer is **full or nearly full** (because the TCP stack hasn't drained it yet — perhaps the receiver's window is closed, or the network is congested), the kernel can only accept however many bytes fit in the remaining space. It does NOT block waiting for more space (unless the socket is in blocking mode AND there is some space — it will copy what fits and return immediately with that count).

On a **blocking socket**, `send()` will sleep until *at least one byte* can be accepted — but it still returns as soon as it has copied some bytes, not necessarily all of them.

### Why a Single recv() Call May Not Deliver All Bytes

Similarly, `recv()` copies bytes from the kernel receive buffer into your user-space buffer. If only 512 bytes have arrived from the network so far (even if you asked for 4096), `recv()` returns 512. TCP doesn't know that your application thinks of those 4096 bytes as a single "message" — it is just a stream of bytes.

### The Three Root Causes of Partial I/O

| Cause | send() affected? | recv() affected? |
|---|---|---|
| Kernel buffer partially full / partially filled | ✅ Yes | ✅ Yes |
| TCP flow control (receiver window = 0 or small) | ✅ Yes | No |
| Signal interruption (EINTR) on blocking call | ✅ Yes | ✅ Yes |
| MSS segmentation (segments arrive out of order) | No | ✅ Yes |
| Nagle algorithm delaying small segments | No | ✅ Yes |

Understanding these causes makes it clear: **you cannot eliminate partial I/O by tuning buffers alone**. You must handle it in code.

### 1.4 POSIX Signals and Blocking I/O — What Actually Interrupts Your send()/recv()

The table above lists "signal interruption (EINTR)" as a cause of partial I/O, but gives no detail on what a signal actually is. This section fills that gap.

#### What is a POSIX Signal?

A **signal** is a software interrupt delivered asynchronously to a process by the operating system (or another process). Think of it as the OS tapping your thread on the shoulder mid-execution and forcing it to run a handler function — possibly in the middle of a blocking `send()` or `recv()` call. After the handler runs, control returns to wherever the thread was interrupted.

Signals exist only on POSIX systems (Linux, macOS, BSD). **Windows has no equivalent mechanism** — Winsock calls are never interrupted by signals, which is why `EINTR` is listed as N/A in the Windows column of the error table.

#### The Signals That Can Interrupt Blocking I/O

Any signal that has a handler installed (or the default action is not "ignore") can interrupt a blocking syscall. The most common ones in networked server code:

| Signal | Default action | What triggers it | Networking relevance |
|---|---|---|---|
| `SIGCHLD` | Ignore (but handlers common) | A child process exited or stopped | Very common interruptor — servers often `fork()` workers; when a worker exits, the parent's blocking `recv()` gets interrupted |
| `SIGALRM` | Terminate | `alarm(N)` timer expired | Used for socket timeouts: `alarm(5)` before `recv()` makes it return after 5s with `EINTR` |
| `SIGUSR1` / `SIGUSR2` | Terminate | User-defined; sent via `kill(pid, SIGUSR1)` | Used for live config reload, log rotation — can arrive at any time |
| `SIGTERM` | Terminate | `kill(pid)` — graceful shutdown request | Sent when you `systemctl stop` your service; your shutdown handler may run mid-I/O |
| `SIGHUP` | Terminate | Controlling terminal closed | Sent when an SSH session closes; daemon processes ignore this or use it for reload |
| `SIGINT` | Terminate | Ctrl+C in terminal | During interactive development; your `recv()` will return `EINTR` when user presses Ctrl+C |
| `SIGPIPE` | **Terminate process** | `send()` to a socket whose peer has closed | The most dangerous signal for networking — see below |

#### The EINTR Mechanism

When a signal arrives while a thread is blocked inside a syscall (`send()`, `recv()`, `accept()`, etc.), the kernel:

1. **Interrupts the syscall** — wakes the thread before the I/O completes.
2. **Runs the signal handler** (if one is installed).
3. **Returns the syscall with `−1`** and sets `errno = EINTR`.

Crucially: **EINTR means zero bytes were transferred**. The syscall made no progress. This is entirely different from a partial transfer (where `n > 0` bytes moved). The correct response is to retry the call from exactly the same position:

```cpp
// In send_all() / recv_exact() loops — EINTR is always safe to retry
if (errno == EINTR) {
    continue; // Retry from the same buffer position — nothing was transferred
}
```

```
Thread blocked in recv(fd, buf, 1024)
         │
         │  ...waiting for data...
         │
         ◄── SIGCHLD arrives (child process died)
         │
         │  Kernel suspends recv(), runs SIGCHLD handler
         │
         recv() returns -1, errno = EINTR
         │
         │  Loop retries recv(fd, buf, 1024) ← same position, nothing was consumed
         │
         recv() returns 512  ← data finally arrived
```

#### SA_RESTART: Automatic EINTR Retry

When you install a signal handler with `sigaction()`, you can set the `SA_RESTART` flag. This tells the kernel to automatically restart interrupted syscalls on your behalf — the `recv()` call never surfaces `EINTR` to your code at all:

```cpp
struct sigaction sa{};
sa.sa_handler = my_sigchld_handler;
sa.sa_flags   = SA_RESTART;  // ← kernel auto-restarts interrupted syscalls
sigemptyset(&sa.sa_mask);
sigaction(SIGCHLD, &sa, nullptr);

// Now recv() will never return EINTR due to SIGCHLD.
// The kernel silently restarts it after the handler runs.
```

**SA_RESTART does not work for all syscalls.** Notably, `select()`, `poll()`, and `namedpipe read()` are NOT auto-restarted even with `SA_RESTART`. Always handle `EINTR` explicitly in your loops as a defence-in-depth measure even when SA_RESTART is set, since some signals (like `SIGALRM`) intentionally bypass it.

#### SIGPIPE — The Silent Killer for Networking Code

`SIGPIPE` deserves special attention. It is delivered when you call `send()` on a socket whose **peer has already closed the connection**. The OS-level socket layer detects that writing to a closed pipe (or socket) makes no sense and raises `SIGPIPE`.

The default action for `SIGPIPE` is **to terminate the entire process** — not just return an error. Your server process can be killed silently by a misbehaving or crashed client with no log message and no core dump, unless you explicitly suppress it:

```cpp
// Option 1: Ignore SIGPIPE globally (most common in server code)
// send() will then return -1 with errno=EPIPE instead of killing the process.
signal(SIGPIPE, SIG_IGN);

// Option 2: Per-send suppression using MSG_NOSIGNAL flag (Linux only)
ssize_t n = send(fd, buf, size, MSG_NOSIGNAL);
// On macOS, use SO_NOSIGPIPE socket option instead:
int opt = 1;
setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));

// Option 3: Block SIGPIPE for just this thread
sigset_t mask;
sigemptyset(&mask);
sigaddset(&mask, SIGPIPE);
pthread_sigmask(SIG_BLOCK, &mask, nullptr);
```

In our codebase, `send_all()` checks `bytes <= 0` and returns `false` on any error — but only if `SIGPIPE` is suppressed and converted to `errno = EPIPE`. Without suppression, the process terminates before `send_all()` even gets a chance to check the return value.

#### Why SIGCHLD Is the Most Common I/O Interruptor in Practice

Most server processes `fork()` child processes for tasks (CGI, worker pools, external commands via `popen()`). Every time any of those children exit, the OS delivers `SIGCHLD` to the parent. If the parent is blocked in `recv()` at that moment, the syscall returns `EINTR`. This is by far the most frequent real-world source of EINTR — not exotic timer-based signals. A production server handling hundreds of requests per second may see dozens of EINTR returns per second due to SIGCHLD alone.

#### Summary

| Signal | Can interrupt send()/recv()? | Common in server code? | Needs special handling? |
|---|---|---|---|
| `SIGCHLD` | ✅ Yes | ✅ Very common | Retry with `continue` or use `SA_RESTART` |
| `SIGALRM` | ✅ Yes | Common (timeouts) | Intentional; detect by checking `errno=EINTR` after timeout |
| `SIGUSR1/2` | ✅ Yes | Moderate (config reload) | Retry with `continue` |
| `SIGTERM` | ✅ Yes | Common (shutdown) | Set `m_running = false` in handler; loop exits on next check |
| `SIGINT` | ✅ Yes | Development only | Same as SIGTERM |
| `SIGHUP` | ✅ Yes | Common (daemons) | Usually: reload config, then retry |
| `SIGPIPE` | Terminates process instead | ✅ Very dangerous | **Must suppress with `SIG_IGN` or `MSG_NOSIGNAL`** |

---

## 2. SO_SNDBUF / SO_RCVBUF Socket Options

These socket options let you **request** a specific kernel buffer size. They are *hints*, not guarantees — the kernel imposes a hard ceiling and applies internal accounting overhead.

### Two Critical Realities Before You Call setsockopt

#### Reality 1: Linux halves your usable space for bookkeeping overhead

When Linux allocates a socket send/receive buffer, it uses **twice** the requested size internally — the extra half is reserved for kernel metadata (`sk_buff` linked-list overhead, socket control blocks, etc.). `getsockopt` reports this doubled figure, which can be misleading:

```
You request:            128 KB
Linux allocates:        256 KB  ← getsockopt reports this
Your usable data space: ~128 KB ← only half is for your bytes
```

This means you should request **twice** what you actually want if you are tuning explicitly. More importantly: **do not interpret the `getsockopt` return value as available data capacity** — it's the total allocated region including kernel overhead.

#### Reality 2: Linux clamps all requests to wmem_max (default ~208 KB)

Even if you call `setsockopt(SO_SNDBUF, 256 * 1024)`, Linux will silently clamp your request to `/proc/sys/net/core/wmem_max` (default: **212,992 bytes ≈ 208 KB**) unless the process has `CAP_NET_ADMIN` (i.e., is running as root). On a stock Linux system:

| You request | Kernel clamps to (wmem_max) | getsockopt reports (2×) | Your usable data space |
|---|---|---|---|
| 256 KB (262,144) | 212,992 (hard cap) | 425,984 | **~208 KB** — not 256 KB! |
| 128 KB (131,072) | 131,072 (under cap) | 262,144 | ~128 KB |
| 4 KB (4,096) | 4,096 (under cap) | 8,192 | ~4 KB |

> **Summary:** On a default Linux system, you cannot get more than ~208 KB of usable send/receive buffer space per socket without root access to raise `wmem_max`. Requesting 256 KB silently gives you 208 KB.

### Setting Buffer Sizes

```cpp
#include <sys/socket.h>

// Request a 256KB send buffer.
// WARNING: On a stock Linux system without root, this is silently clamped
// to wmem_max (~208 KB). The getsockopt result will appear doubled but
// only half is usable data space (the rest is kernel bookkeeping overhead).
int snd_buf = 256 * 1024;
if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd_buf, sizeof(snd_buf)) < 0) {
    perror("setsockopt SO_SNDBUF");
}

// Verify what the kernel actually allocated (will be 2× the effective size)
int actual;
socklen_t len = sizeof(actual);
getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actual, &len);
// actual / 2 = your true usable send buffer capacity
printf("Kernel-reported buffer: %d bytes (usable: ~%d bytes)\n", actual, actual / 2);
```

### Platform Defaults

| Platform | Default SO_SNDBUF (send) | Default SO_RCVBUF (receive) | Hard maximum | Notes |
|---|---|---|---|---|
| Linux (kernel ≥ 3.x) | 87,040 bytes (~85 KB) | 212,992 bytes (~208 KB) | 212,992 bytes | `/proc/sys/net/core/{w,r}mem_default`; raisable with root via `wmem_max` |
| macOS | 131,072 bytes (128 KB) | 131,072 bytes (128 KB) | `kern.ipc.maxsockbuf` | |
| Windows | 8,192 bytes (8 KB) | 8,192 bytes (8 KB) | No fixed cap; tunable per-socket | Default is ~26× smaller than Linux! |

> **Key Insight (Windows):** Windows defaults are tiny (8 KB). A 200 KB message will *always* require many `send()` calls on an untuned Windows socket. This is a common shock when developing on Linux (larger buffers → less visible partial I/O) and shipping on Windows.

> **Key Insight (Linux send vs. receive asymmetry):** Note that the Linux *send* default (~85 KB) is substantially smaller than the *receive* default (~208 KB). This is intentional — the receive buffer must be large enough to hold in-flight data while the application is busy; the send buffer only needs to hold data while TCP drains it. This asymmetry means **send-side partial I/O is more common than receive-side on Linux** for large messages.

### Our Current Implementation's Buffer Sizes

`Socket::create()` in this codebase calls only:
```cpp
m_handle = socket(AF_INET, SOCK_STREAM, 0);
```

**No `setsockopt` for buffer sizes is called anywhere.** We use 100% OS defaults:

| Platform | Our SO_SNDBUF | Our SO_RCVBUF |
|---|---|---|
| Linux | ~85 KB (87,040 bytes) | ~208 KB (212,992 bytes) |
| Windows | **8 KB** | **8 KB** |

This is the right choice for Phase 0. The entire purpose of `send_all()` and `recv_exact()` is to be **buffer-size agnostic** — they loop until every byte is transferred regardless of how many partial I/O iterations the kernel requires. Buffer size only affects *how many loop iterations occur*, not *correctness*. Artificially inflating the buffer with `setsockopt` would hide partial I/O bugs during development rather than making the code more correct.

### The Linux Auto-Tuning Caveat

Linux has TCP auto-tuning (`tcp_moderate_rcvbuf`) that can grow the receive buffer dynamically based on bandwidth-delay product. However, the *send* buffer is **not** auto-tuned by default. This asymmetry means send-side partial I/O is more common than receive-side on Linux for large messages.

To check and set system-wide limits (requires root):
```bash
# View current defaults
cat /proc/sys/net/core/wmem_default   # send default, e.g., 87040
cat /proc/sys/net/core/rmem_default   # recv default, e.g., 212992
cat /proc/sys/net/core/wmem_max       # send hard cap, e.g., 212992
cat /proc/sys/net/core/rmem_max       # recv hard cap, e.g., 212992

# Increase limits (requires root / CAP_NET_ADMIN)
sysctl -w net.core.wmem_max=16777216
sysctl -w net.core.wmem_default=4194304
```

---

## 3. The Broken send() — A Concrete Corruption Example

Let's trace exactly what happens when you naively send a 200 KB message to a socket whose send buffer happens to have only 128 KB of free space.

### The Broken Code

```cpp
#include <sys/socket.h>
#include <cstring>
#include <cstdio>

// BROKEN: Does not handle partial send
bool broken_send(int fd, const void* data, size_t size) {
    // We call send() once and assume ALL bytes were accepted.
    ssize_t sent = send(fd, data, size, 0);

    if (sent < 0) {
        perror("send failed");
        return false;
    }

    // BUG: sent may be less than size!
    // If size = 204800 (200KB) and the buffer only had 131072 (128KB) free,
    // sent = 131072 — and the remaining 73728 bytes are silently dropped.
    printf("Sent %zd of %zu bytes\n", sent, size);
    return true;
}
```

### The Concrete Scenario

```
Timeline of a 200 KB message (200,000 bytes) with 128 KB free in send buffer:

  Caller:  broken_send(fd, message, 200000)
                │
                ▼
  Kernel:  "I have 131072 bytes free in send buffer"
           Copy min(200000, 131072) = 131072 bytes from user → kernel
           Return 131072 to caller
                │
                ▼
  Caller:  Thinks: "send() returned > 0, so success!"
           Returns true.
           The remaining 68928 bytes (bytes 131073..200000) are NEVER sent.
                │
                ▼
  Receiver: Receives exactly 131072 bytes.
            Tries to deserialize a 200,000-byte message from 131,072 bytes.
            Result: CORRUPTED MESSAGE, deserialization error, or crash.

  What the receiver sees (conceptually):
  ┌────────────────────────────┬────────────────────┐
  │  Received 131,072 bytes   │  MISSING 68,928 B  │
  │  (valid prefix of message) │  (silently dropped) │
  └────────────────────────────┴────────────────────┘
```

### Why This Is Hard to Debug

- On a fast loopback connection (`localhost`), the send buffer drains almost instantly. `send()` almost always returns the full count. The bug is invisible in local testing.
- In production, under load (many concurrent connections competing for send buffer space), partial sends start occurring — often only at 3 AM under peak traffic.
- If you're using length-prefixed framing (Document 02), the receiver reads the 4-byte length header (e.g., 200,000), then calls `recv()` expecting 200,000 bytes — but only 131,072 arrive before the sender moves on. The receiver blocks forever waiting for bytes that will never come.

---

## 4. The send_all() Retry Loop Pattern

The fix is straightforward once you understand the problem: keep calling `send()` until all bytes have been accepted by the kernel, advancing a pointer through your buffer on each iteration.

### Full Implementation with Commentary

```cpp
#include <sys/socket.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>

/**
 * send_all() — Reliably send exactly 'size' bytes over a blocking TCP socket.
 *
 * TCP's send() call may accept fewer bytes than requested when the kernel send
 * buffer is full. This function loops until all bytes are delivered to the
 * kernel, or an unrecoverable error occurs.
 *
 * @param fd    Connected TCP socket (must be in blocking mode)
 * @param buf   Pointer to the data to send
 * @param size  Number of bytes to send
 * @return      true if all bytes were sent, false on error or disconnect
 */
bool send_all(int fd, const void* buf, size_t size) {
    // Cast to byte pointer so we can do pointer arithmetic
    const char* ptr = static_cast<const char*>(buf);

    // Track how many bytes remain to be sent
    size_t remaining = size;

    while (remaining > 0) {
        // Attempt to send as many bytes as possible from current position.
        // On a blocking socket, this will sleep until at least 1 byte can
        // be accepted by the kernel send buffer, then return immediately.
        ssize_t sent = send(fd, ptr, remaining, 0);

        if (sent < 0) {
            // A negative return means a real error occurred.
            if (errno == EINTR) {
                // EINTR: a signal interrupted our blocking send() before any
                // bytes were transferred. This is NOT a fatal error — simply
                // retry the send from the same position.
                // Example: SIGCHLD arriving while blocked in send().
                continue;
            }
            // Any other error (EPIPE, ECONNRESET, ETIMEDOUT, etc.) is fatal.
            // EPIPE means the peer closed the connection while we were writing.
            perror("send_all: send() failed");
            return false;
        }

        if (sent == 0) {
            // send() returning 0 is unusual but possible in some
            // implementations when size == 0 was requested. Since remaining > 0
            // here, treat this as an unexpected disconnect.
            fprintf(stderr, "send_all: send() returned 0 unexpectedly\n");
            return false;
        }

        // Advance the buffer pointer past the bytes we just sent.
        // If sent == remaining, the loop condition (remaining > 0) will be
        // false and we exit cleanly.
        ptr       += sent;
        remaining -= static_cast<size_t>(sent);

        // Optional: log progress for debugging
        // printf("send_all: sent %zd bytes, %zu remaining\n", sent, remaining);
    }

    // All bytes have been accepted by the kernel send buffer.
    // TCP will deliver them to the peer reliably (or report an error later).
    return true;
}

// ── Usage Example ────────────────────────────────────────────────────────────

struct MessageHeader {
    uint32_t length; // Network byte order (big-endian)
};

bool send_message(int fd, const char* payload, size_t payload_len) {
    // Step 1: Send the 4-byte length prefix (also needs send_all!)
    MessageHeader hdr;
    hdr.length = htonl(static_cast<uint32_t>(payload_len));

    if (!send_all(fd, &hdr, sizeof(hdr))) {
        fprintf(stderr, "send_message: failed to send header\n");
        return false;
    }

    // Step 2: Send the payload bytes
    if (!send_all(fd, payload, payload_len)) {
        fprintf(stderr, "send_message: failed to send payload\n");
        return false;
    }

    return true;
}
```

### What the Loop Does Visually

```
send_all(fd, buf, 200000 bytes)

Iteration 1:  ptr=buf+0,      remaining=200000
              send() → 131072 bytes accepted
              ptr=buf+131072, remaining=68928

Iteration 2:  ptr=buf+131072, remaining=68928
              Buffer has drained a bit; send() → 68928 bytes accepted
              ptr=buf+200000, remaining=0

Loop exits. All 200,000 bytes delivered to kernel. ✅
```

### Performance Considerations

- The retry loop does **not** busy-spin on a blocking socket. Each blocked `send()` call yields the CPU to the scheduler. The loop is IO-bound, not CPU-bound.
- On non-blocking sockets, you **must** integrate with an event loop (epoll/kqueue) between retries instead of looping tightly. See Section 7.

---

## 5. The recv_exact() Accumulation Loop Pattern

The receive side has the same problem: `recv()` returns however many bytes are currently in the kernel receive buffer, which may be far fewer than you need. You must accumulate bytes until you have exactly what you need.

### Full Implementation with Commentary

```cpp
#include <sys/socket.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>

/**
 * recv_exact() — Receive exactly 'size' bytes from a blocking TCP socket.
 *
 * Loops until the full requested number of bytes has been read, or the
 * connection is closed / an error occurs.
 *
 * @param fd    Connected TCP socket (must be in blocking mode for simplicity)
 * @param buf   Buffer to receive bytes into (must be at least 'size' bytes)
 * @param size  Exact number of bytes to receive
 * @return      true if exactly 'size' bytes were received
 *              false if connection closed (peer sent FIN) or error occurred
 */
bool recv_exact(int fd, void* buf, size_t size) {
    char* ptr = static_cast<char*>(buf);
    size_t remaining = size;

    while (remaining > 0) {
        // Attempt to receive up to 'remaining' bytes.
        // On a blocking socket, this will sleep until at least 1 byte
        // is available in the kernel receive buffer.
        ssize_t recvd = recv(fd, ptr, remaining, 0);

        if (recvd < 0) {
            if (errno == EINTR) {
                // Signal interrupted us before any data arrived.
                // Safe to retry — no bytes were consumed.
                continue;
            }
            // Real error: ECONNRESET (peer reset), ETIMEDOUT, etc.
            perror("recv_exact: recv() failed");
            return false;
        }

        if (recvd == 0) {
            // Peer sent FIN — the connection is half-closed.
            // If we haven't received all expected bytes, this is an error
            // (truncated message / premature disconnect).
            if (remaining < size) {
                fprintf(stderr, "recv_exact: connection closed mid-message "
                        "(got %zu of %zu bytes)\n",
                        size - remaining, size);
            } else {
                fprintf(stderr, "recv_exact: connection closed before any data\n");
            }
            return false;
        }

        // Advance our buffer pointer and reduce the remaining count
        ptr       += recvd;
        remaining -= static_cast<size_t>(recvd);
    }

    return true;
}

// ── Combining with Length-Prefixed Framing ───────────────────────────────────

/**
 * recv_message() — Receive a length-prefixed message.
 *
 * Protocol: [4-byte big-endian length][<length> bytes of payload]
 *
 * @param fd          Connected TCP socket
 * @param out_buf     Output buffer (must be pre-allocated or dynamically sized)
 * @param max_size    Maximum acceptable message size (DoS protection)
 * @param out_len     Set to actual payload length on success
 * @return            true on success
 */
bool recv_message(int fd, char* out_buf, size_t max_size, size_t& out_len) {
    // Step 1: Receive the 4-byte header
    uint32_t net_length = 0;
    if (!recv_exact(fd, &net_length, sizeof(net_length))) {
        fprintf(stderr, "recv_message: failed to read header\n");
        return false;
    }

    // Step 2: Convert from network byte order to host byte order
    uint32_t payload_len = ntohl(net_length);

    // Step 3: Validate length to prevent DoS (malicious or corrupt header)
    if (payload_len == 0 || payload_len > max_size) {
        fprintf(stderr, "recv_message: invalid payload length %u "
                "(max %zu)\n", payload_len, max_size);
        return false;
    }

    // Step 4: Receive exactly payload_len bytes of payload
    if (!recv_exact(fd, out_buf, payload_len)) {
        fprintf(stderr, "recv_message: failed to read payload\n");
        return false;
    }

    out_len = payload_len;
    return true;
}
```

---

## 6. Interpreting send()/recv() Return Values

Understanding the three possible return value categories is critical. Getting this wrong leads to either ignoring errors or mishandling partial I/O.

### The Three Cases

```cpp
ssize_t n = recv(fd, buf, size, 0);  // same logic applies to send()

if (n > 0) {
    // ── CASE 1: Data transferred ──────────────────────────────────────────
    // n bytes were moved. This could be anywhere from 1 to size.
    // If n == size: full transfer — but don't rely on this always happening!
    // If n < size:  partial transfer — you MUST loop.

} else if (n == 0) {
    // ── CASE 2: Peer closed the connection ────────────────────────────────
    // For recv(): The remote peer called close() or shutdown(SHUT_WR).
    //             TCP received a FIN segment. No more data will arrive.
    //             The socket is still open for writing (half-duplex close).
    // For send(): Returning 0 is uncommon; usually means you passed size=0.
    //             A send() to a closed peer typically returns -1 (EPIPE/ECONNRESET)
    //             or triggers SIGPIPE signal on Linux.
    close(fd);

} else {  // n == -1
    // ── CASE 3: Error ─────────────────────────────────────────────────────
    int err = errno;  // On Windows: WSAGetLastError()

    switch (err) {
        case EINTR:
            // Not a real error. A signal arrived before any bytes transferred.
            // Retry the call. (Your send_all/recv_exact loops handle this.)
            break;

        case EAGAIN:      // Same value as EWOULDBLOCK on Linux
        case EWOULDBLOCK:
            // Non-blocking socket: no data available (recv) or
            // send buffer full (send). Wait for the fd to become
            // readable/writable (use select/poll/epoll) then retry.
            // On blocking sockets, this should NOT happen normally.
            break;

        case EPIPE:
            // send() only: Peer closed the connection (received RST or FIN).
            // Also triggers SIGPIPE by default — install a SIG_IGN handler
            // to prevent your process from being killed silently.
            fprintf(stderr, "Broken pipe — peer disconnected\n");
            break;

        case ECONNRESET:
            // Peer sent a TCP RST (abrupt disconnect, e.g., crashed process).
            fprintf(stderr, "Connection reset by peer\n");
            break;

        case ETIMEDOUT:
            // TCP keepalive or retransmit timer expired.
            fprintf(stderr, "Connection timed out\n");
            break;

        default:
            fprintf(stderr, "Unexpected error: %s\n", strerror(err));
            break;
    }
}
```

### Quick Reference Table

| Return Value | Meaning | Action |
|---|---|---|
| `> 0` and `== size` | Full transfer (lucky!) | Continue |
| `> 0` and `< size` | Partial transfer | Advance pointer, loop |
| `== 0` (recv) | Peer sent FIN (graceful close) | Close socket, clean up |
| `== 0` (send) | Rare; usually size=0 was passed | Check your call |
| `== -1`, `errno=EINTR` | Signal interrupted syscall | Retry immediately |
| `== -1`, `errno=EAGAIN` | Non-blocking, not ready | Poll with epoll/select |
| `== -1`, `errno=EPIPE` | Peer closed before all data sent | Error, disconnect |
| `== -1`, other errno | Real error | Log and close |

---

## 7. Blocking vs. Non-Blocking Sockets and Partial I/O

### Blocking Sockets (Default)

On a blocking socket, `send()` and `recv()` will **sleep** until at least 1 byte can be transferred. They return immediately once any bytes are moved — not when *all* bytes are moved.

```
Blocking recv(fd, buf, 1000):
  If 0 bytes available → sleep until ≥1 byte arrives → return N (1 ≤ N ≤ 1000)
  Never returns EAGAIN.
  May return -1 with EINTR if a signal arrives while sleeping.
```

This is why `recv_exact()` can loop without an event loop — each iteration blocks until progress is made.

### Non-Blocking Sockets

Set with `fcntl(fd, F_SETFL, O_NONBLOCK)` on Linux. On a non-blocking socket:

- `send()` returns immediately. If the send buffer is full, returns `-1` with `EAGAIN`.
- `recv()` returns immediately. If no data is available, returns `-1` with `EAGAIN`.

```cpp
// Non-blocking send_all requires integration with an event loop (epoll)
bool nb_send_all(int epfd, int fd, const char* buf, size_t size) {
    size_t remaining = size;
    const char* ptr = buf;

    while (remaining > 0) {
        ssize_t sent = send(fd, ptr, remaining, 0);

        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Send buffer is full. Register fd for EPOLLOUT and
                // yield control back to the event loop. When the buffer
                // drains, epoll will wake us up.
                struct epoll_event ev;
                ev.events  = EPOLLOUT | EPOLLET;
                ev.data.fd = fd;
                epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
                // In a coroutine or state machine, you would suspend here.
                // This simplified example just returns false to signal "need retry".
                return false;
            }
            if (errno == EINTR) { continue; }
            perror("nb_send_all: send failed");
            return false;
        }

        ptr       += sent;
        remaining -= sent;
    }
    return true;
}
```

> **Design Rule:** For high-performance servers, use non-blocking sockets + epoll + an explicit send-buffer state machine per connection. For simpler servers with blocking sockets + threads, `send_all()` is sufficient.

---

## 8. Why MSG_WAITALL Is Not a Complete Solution

`MSG_WAITALL` is a flag you can pass to `recv()` that asks the kernel to wait until the full requested amount has been received:

```cpp
// Attempt to receive exactly 'size' bytes using MSG_WAITALL
ssize_t n = recv(fd, buf, size, MSG_WAITALL);
```

This seems to solve the partial recv() problem without a loop. But it has critical limitations:

### Limitation 1: Signal Interruption (Linux)

On Linux, `recv()` with `MSG_WAITALL` can still return fewer bytes than requested if a signal is delivered while waiting:

```
recv(fd, buf, 4096, MSG_WAITALL):
  Received 2048 bytes...
  Signal SIGCHLD delivered ← recv() interrupted!
  Returns 2048, not 4096.
  errno = EINTR
```

You **still** need a loop to handle `EINTR`.

### Limitation 2: EOF Mid-Message

If the peer closes the connection after sending only part of the expected data, `MSG_WAITALL` returns however many bytes arrived — not the full count. You must check the return value.

### Limitation 3: Not Available for send()

There is no `MSG_WAITALL` equivalent for `send()`. The `send()` side always requires an explicit loop.

### Limitation 4: Not Available on Non-Blocking Sockets

`MSG_WAITALL` is silently ignored on non-blocking sockets — it behaves as a regular `recv()`.

### Conclusion

`MSG_WAITALL` can simplify `recv_exact()` slightly (fewer EINTR retries in practice), but it does not eliminate the need for a loop. The fully correct approach is always an explicit accumulation loop. Rely on `MSG_WAITALL` only as an optimization hint, not as a correctness guarantee.

---

## 9. Windows Differences: WSAEWOULDBLOCK vs EAGAIN

Windows uses the Winsock API, which has different error codes and semantics.

### Error Code Mapping

| Linux errno | Windows WSA Error | Meaning |
|---|---|---|
| `EAGAIN` / `EWOULDBLOCK` | `WSAEWOULDBLOCK` | Non-blocking socket not ready |
| `EINTR` | N/A (no signal model) | Signals don't interrupt Winsock calls |
| `EPIPE` | `WSAECONNRESET` | Peer closed / reset |
| `ECONNRESET` | `WSAECONNRESET` | TCP RST received |
| `ETIMEDOUT` | `WSAETIMEDOUT` | Connection timed out |

### Portable Error Checking

```cpp
// Cross-platform partial I/O helper

#ifdef _WIN32
  #include <winsock2.h>
  #define SOCKET_ERR   SOCKET_ERROR       // send/recv return SOCKET_ERROR (-1) on error
  #define GET_ERR()    WSAGetLastError()
  #define IS_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK)
  #define IS_INTERRUPTED(e) (false)       // Windows Winsock is not interrupted by signals
  typedef SOCKET socket_t;
#else
  #include <sys/socket.h>
  #include <cerrno>
  #define SOCKET_ERR   (-1)
  #define GET_ERR()    errno
  #define IS_WOULDBLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
  #define IS_INTERRUPTED(e) ((e) == EINTR)
  typedef int socket_t;
#endif

bool portable_send_all(socket_t fd, const void* buf, size_t size) {
    const char* ptr = static_cast<const char*>(buf);
    size_t remaining = size;

    while (remaining > 0) {
        int n = (int)send(fd, ptr, (int)remaining, 0);

        if (n == SOCKET_ERR) {
            int err = GET_ERR();
            if (IS_INTERRUPTED(err)) { continue; }
            if (IS_WOULDBLOCK(err))  { /* handle non-blocking */ return false; }
            fprintf(stderr, "send error: %d\n", err);
            return false;
        }

        ptr       += n;
        remaining -= (size_t)n;
    }
    return true;
}
```

### Windows-Specific Note: Default Buffer Sizes

Windows defaults to **8 KB** send/receive buffers — roughly **26× smaller** than the Linux receive default. This means partial I/O occurs far more frequently on Windows for any message larger than ~8 KB. Always set `SO_SNDBUF`/`SO_RCVBUF` explicitly in production Windows networking code.

```cpp
// Windows: request larger buffers.
// Unlike Linux, Windows does not have the same wmem_max hard cap — buffer
// sizes can generally be set to the requested value (subject to available
// kernel memory). However, always verify with getsockopt.
// Note: Windows does NOT double the value for bookkeeping the way Linux does,
// so getsockopt returns approximately what you requested.
int bufsize = 256 * 1024; // 256 KB — achievable on Windows, clamped to ~208 KB on stock Linux
setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&bufsize, sizeof(bufsize));
setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&bufsize, sizeof(bufsize));

// Always verify:
int actual; int len = sizeof(actual);
getsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&actual, &len);
printf("Windows actual SO_SNDBUF: %d bytes\n", actual); // ~262144 on Windows
```

---

## 10. Partial I/O and TCP Framing — The Two-Fix Rule

Document 02 covered length-prefixed framing to solve the "message boundary" problem in TCP streams. This document covers `send_all()`/`recv_exact()` to solve the partial I/O problem. **Both fixes are required.** They solve different but complementary problems.

### Why Length Prefixing Alone Is Insufficient

```
Scenario: You implement length-prefix framing but use broken send().

Sender sends [HDR=200000][...200000 bytes of payload...]
  ↓ broken_send() sends only 131072 bytes to kernel
  ↓ TCP delivers 131072 bytes to receiver

Receiver reads HDR → length = 200000
Receiver calls recv_exact(fd, buf, 200000)
  → Receives 131072 bytes then blocks forever.
  → The remaining 68928 bytes were NEVER sent by the sender!
  → Connection hangs indefinitely (deadlock).
```

### Why send_all() Alone Is Insufficient

```
Scenario: You implement send_all() but no framing.

Sender A calls send_all(fd, msg1, 100) → sends 100 bytes
Sender A calls send_all(fd, msg2, 50)  → sends 50 bytes

TCP stream: [100 bytes of msg1][50 bytes of msg2] = 150 contiguous bytes

Receiver calls recv(fd, buf, 512)
  → May receive all 150 bytes in one call.
  → May receive 100 bytes, then 50 bytes.
  → May receive 73 bytes, then 77 bytes — splitting msg1!
  → Receiver cannot tell where msg1 ends and msg2 begins. Data corruption.
```

### The Complete Solution Requires Both

```
┌───────────────────────────────────────────────────────────────────┐
│                    CORRECT MESSAGE PROTOCOL                        │
│                                                                   │
│  Sender:                                                          │
│    1. Construct: [4-byte length][payload]                         │
│    2. send_all(fd, &header, 4)   ← reliably deliver header        │
│    3. send_all(fd, payload, len) ← reliably deliver payload       │
│                                                                   │
│  Receiver:                                                        │
│    1. recv_exact(fd, &header, 4)         ← get complete header   │
│    2. length = ntohl(header)                                      │
│    3. recv_exact(fd, buf, length)        ← get complete payload  │
│                                                                   │
│  Result: Atomic, reliable, correctly framed message transfer      │
└───────────────────────────────────────────────────────────────────┘
```

---

## 11. Exercises

### Exercise 1: Observe Partial Writes via SO_SNDBUF

This test artificially reduces the send buffer to 4 KB to force partial `send()` calls on a localhost connection.

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdio>
#include <thread>

// ── Receiver thread ──────────────────────────────────────────────────────────
void receiver_thread(int server_fd) {
    int client_fd = accept(server_fd, nullptr, nullptr);

    // Set a tiny receive buffer to force partial reads on the receive side too
    int rcv_buf = 4096;  // 4 KB
    setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &rcv_buf, sizeof(rcv_buf));

    char buf[65536];
    size_t total = 0;
    int call_count = 0;

    while (total < 65536) {
        ssize_t n = recv(client_fd, buf + total, 65536 - total, 0);
        if (n <= 0) break;
        total += n;
        call_count++;
        printf("[Receiver] recv() call #%d returned %zd bytes (total: %zu)\n",
               call_count, n, total);
    }

    printf("[Receiver] Done: received %zu bytes in %d recv() calls\n",
           total, call_count);
    close(client_fd);
    close(server_fd);
}

// ── Main / Sender ─────────────────────────────────────────────────────────────
int main() {
    // Create server socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(9988);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 1);

    // Launch receiver in background thread
    std::thread t(receiver_thread, server_fd);

    // Create client socket
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);

    // *** KEY: Shrink the send buffer to 4 KB to force partial sends ***
    int snd_buf = 4096;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &snd_buf, sizeof(snd_buf));

    // Verify actual size (kernel may double it)
    socklen_t len = sizeof(snd_buf);
    getsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &snd_buf, &len);
    printf("[Sender] Actual send buffer size: %d bytes\n", snd_buf);

    sockaddr_in srv_addr{};
    srv_addr.sin_family      = AF_INET;
    srv_addr.sin_port        = htons(9988);
    inet_pton(AF_INET, "127.0.0.1", &srv_addr.sin_addr);
    connect(client_fd, (sockaddr*)&srv_addr, sizeof(srv_addr));

    // Try to send 64 KB in one shot
    char data[65536];
    memset(data, 'A', sizeof(data));
    int call_count = 0;
    size_t remaining = 65536;
    char* ptr = data;

    while (remaining > 0) {
        ssize_t sent = send(client_fd, ptr, remaining, 0);
        if (sent <= 0) break;
        call_count++;
        printf("[Sender]   send() call #%d returned %zd bytes (remaining: %zu)\n",
               call_count, sent, remaining - sent);
        ptr       += sent;
        remaining -= sent;
    }

    printf("[Sender] Done: sent 65536 bytes in %d send() calls\n", call_count);
    close(client_fd);
    t.join();
    return 0;
}

// Expected output (approximate):
// [Sender] Actual send buffer size: 8192 bytes  ← kernel doubled our 4KB request
// [Sender]   send() call #1 returned 8192 bytes (remaining: 57344)
// [Sender]   send() call #2 returned 8192 bytes (remaining: 49152)
// ...
// [Receiver] recv() call #1 returned 4096 bytes (total: 4096)
// [Receiver] recv() call #2 returned 4096 bytes (total: 8192)
// ...
// Observe: multiple send() calls were needed — partial I/O in action!
```

### Exercise 2: Write a Test That Detects the Broken send() Bug

```cpp
// Modify Exercise 1 to use broken_send() and observe data loss.
// 1. Have the receiver count total bytes received.
// 2. Use broken_send() (single call, ignore partial) on the sender.
// 3. Compare sender's "bytes sent" vs receiver's "bytes received".
// 4. They will differ — the delta is silently dropped data.
// 5. Then switch to send_all() and verify they match.
```

### Exercise 3: Implement Timeout-Aware recv_exact()

```cpp
// Challenge: Modify recv_exact() to accept a timeout parameter.
// If the full 'size' bytes don't arrive within 'timeout_ms' milliseconds,
// return false with a timeout error. (Hint: use select() or SO_RCVTIMEO.)
```

---

## Key Takeaways

- **TCP is a stream protocol.** It has no concept of message boundaries. A single `send()` call does not guarantee all bytes arrive as one unit at the receiver.
- **`send()` and `recv()` can return partial results** whenever the kernel buffer is full (send) or partially filled (recv). This is expected, documented behavior — not a bug.
- **Always check the return value.** The return value of `send()`/`recv()` is the *actual* byte count transferred, not a success/failure boolean.
- **Use `send_all()` and `recv_exact()` loops.** These are the canonical, correct patterns for reliable message I/O over TCP.
- **EINTR is not an error.** Retry the syscall. On Windows, Winsock is not interrupted by signals, so this case doesn't exist.
- **0 means EOF, not error.** A `recv()` returning 0 means the peer sent FIN (closed their write side). Handle it explicitly.
- **Non-blocking sockets require event-loop integration** between retry iterations. The simple while-loop pattern only works on blocking sockets.
- **`MSG_WAITALL` is not a silver bullet.** It can still be interrupted by signals and doesn't help on non-blocking sockets or for `send()`.
- **Windows buffers are tiny (8 KB default).** Partial I/O is far more frequent on Windows. Always configure `SO_SNDBUF`/`SO_RCVBUF` explicitly.
- **Both fixes are required:** length-prefixed framing (Document 02) solves message boundary ambiguity; `send_all()`/`recv_exact()` (this document) solves partial I/O. Neither alone is sufficient.
- **Small `SO_SNDBUF` is a great test tool.** Shrinking the send buffer to 4–8 KB reliably exercises partial I/O paths in unit tests that would otherwise only trigger under production load.

---

## Further Reading

- **Stevens, Wright — Unix Network Programming Vol. 1** (Chapter 3, Sockets Introduction; Chapter 16, Nonblocking I/O): The definitive reference for all `send()`/`recv()` semantics and partial I/O handling.
- **Linux `man 2 send`** and **`man 2 recv`**: The authoritative specification including all error codes and flags.
- **RFC 793 — Transmission Control Protocol**: The original TCP specification explaining stream semantics, flow control, and segment delivery.
- **Linux Kernel Source — `net/ipv4/tcp.c`**: `tcp_sendmsg()` shows exactly how kernel send buffers work internally.
- **Beej's Guide to Network Programming** (https://beej.us/guide/bgnet/): Free, practical guide with clear explanations of blocking/non-blocking behavior.
- **`/proc/sys/net/core/` and `/proc/sys/net/ipv4/tcp_*`**: Linux kernel tunables for buffer sizes, auto-tuning, and TCP behavior.
- **Windows Winsock Documentation** — `SO_SNDBUF`/`SO_RCVBUF` on MSDN: Explains Windows-specific defaults and tuning parameters.
- **Ceph Messenger source code** (GitHub): A production-grade example of `send_all()`/`recv_exact()` patterns in a high-performance distributed storage system.
- **libevent / libuv source**: Reference implementations of non-blocking send queues with automatic retry logic on EAGAIN/EWOULDBLOCK.

---

*Document 03 of Phase 0 — Next: [04_epoll_and_nonblocking_io.md](04_epoll_and_nonblocking_io.md)*
