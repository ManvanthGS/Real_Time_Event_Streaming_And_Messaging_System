# 05 — The Thread-Per-Connection Problem

> **TL;DR:** The thread-per-connection model is the most intuitive way to handle concurrent network clients — spawn one OS thread per connection, let the OS schedule them, and write simple blocking code. It works beautifully at tens or hundreds of connections. But every OS thread costs ~8 MB of virtual stack space, ~10 KB of kernel bookkeeping, and ~1–10 µs of context-switch overhead each time the CPU switches away. At 10,000 simultaneous clients (the famous "C10K problem") this naively requires 80 GB of RAM just for stacks, and the scheduler spends more time context-switching than doing real work. The solution is an **event-driven** model — `epoll`/`io_uring` on Linux — that multiplexes thousands of connections over a handful of threads, eliminating nearly all context-switch cost. Thread pools are a practical middle ground for CPU-bound work. C++20 coroutines let you write event-driven code that looks blocking, bridging the impedance mismatch between async I/O and human intuition.

---

## 1. The Thread-Per-Connection Model

### How It Works

The idea is as old as UNIX networking: for every new TCP connection the server accepts, spawn a dedicated OS thread (or process) to handle it. That thread owns the connection for its entire lifetime — reading requests, computing responses, writing back — and then exits.

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <iostream>
#include <vector>
#include <cstring>

// Handles one client connection; runs entirely inside a dedicated thread.
void handle_client(int client_fd) {
    char buf[4096];
    while (true) {
        // recv() BLOCKS here until the kernel has data.
        // While this thread sleeps, the OS can run other threads.
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;          // client disconnected or error

        // Echo the data back (toy protocol)
        send(client_fd, buf, n, 0);
    }
    close(client_fd);               // kernel releases socket resources
    // Thread function returns → OS destroys the thread
}

int main() {
    // Create a TCP listening socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(8080);

    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 128);         // backlog: up to 128 pending connections

    while (true) {
        // accept() blocks until a new client connects
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        // Spawn a new OS thread for every connection.
        // std::thread constructor copies client_fd by value — safe.
        std::thread(handle_client, client_fd).detach(); // <-- problem here
    }
}
```

### Why It Feels Intuitive

- **Sequential mental model**: each connection's logic is a straight-line story — read, process, write. No callbacks, no state machines.
- **Natural isolation**: a crash or slow client in thread A cannot stall thread B.
- **Blocking I/O is simple**: `recv()`, `send()`, `read()` all block naturally. You write code that reads like pseudo-code.

The model is so natural that it dominated server design from the 1970s through the mid-1990s. The problem is that "natural" does not scale.

---

## 2. OS Thread Costs — What the Kernel Actually Allocates

Every time you call `std::thread(...)` or `pthread_create()`, the kernel does a significant amount of work on your behalf.

### 2.1 Stack Memory: 8 MB per Thread (Linux Default)

Each thread needs its own **call stack** — a contiguous region of memory for local variables, return addresses, and saved registers. On Linux, `ulimit -s` defaults to **8192 KB = 8 MB**.

```cpp
#include <pthread.h>
#include <iostream>

int main() {
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    size_t stack_size;
    pthread_attr_getstacksize(&attr, &stack_size);
    // Prints: Default stack size: 8388608 bytes (8 MB)
    std::cout << "Default stack size: " << stack_size << " bytes\n";

    // You CAN reduce it, but many libraries assume ≥1 MB.
    // Reducing below 512 KB risks stack overflow in deep call chains.
    pthread_attr_setstacksize(&attr, 512 * 1024); // 512 KB — risky
    pthread_attr_destroy(&attr);
}
```

> **Important distinction**: 8 MB is *virtual* address space, not necessarily physical RAM. Linux uses demand paging — pages are only faulted into physical memory when touched. But virtual address space is still a finite resource (128 TB on x86-64), and the kernel's page tables and VMAs (virtual memory areas) consume real kernel memory per mapping.

### 2.2 Kernel Data Structures: `task_struct` ≈ 10 KB

Every thread is represented in the kernel by a `task_struct` — a C struct containing scheduling state, signal masks, file descriptor tables, memory maps, CPU register snapshots, and dozens of other fields. On Linux 6.x, `sizeof(struct task_struct)` is roughly **9–11 KB** depending on kernel config.

```
10,000 threads × 10 KB task_struct = ~100 MB of kernel memory
```

This memory lives in the kernel's non-swappable memory region. It cannot be reclaimed under pressure.

### 2.3 Scheduling Overhead

The kernel's job is to share CPU cores across all runnable threads. With N threads and K cores, the scheduler must:

1. Pick the highest-priority runnable thread.
2. Save the current thread's CPU state (registers, FPU, SSE/AVX state — up to ~2 KB).
3. Load the new thread's CPU state.
4. Flush or update the TLB if the address space changed.
5. Return to user space inside the new thread.

This is a **context switch**, and it is never free.

---

## 3. Context Switch Cost — Why Microseconds Add Up

### Measured Latency

A **voluntary context switch** (thread calls `recv()` and the kernel deschedules it) costs roughly:

| Hardware generation | Measured context switch cost |
|---|---|
| Intel Haswell (2013) | ~3–5 µs |
| Intel Skylake (2015) | ~2–4 µs |
| AMD Zen 3 (2020) | ~1–3 µs |
| ARM Cortex-A72 | ~5–8 µs |

Even at the optimistic end (1 µs), 10,000 threads waking up every 100 ms means:

```
10,000 × 2 switches × 1 µs = 20 ms of pure overhead per 100 ms window = 20% CPU wasted
```

### Why Caches Make It Worse: TLB and L1/L2 Eviction

Modern CPUs use the **Translation Lookaside Buffer (TLB)** to cache virtual→physical address translations. It typically holds 64–1536 entries on modern Intel processors. When the OS switches threads:

- The new thread touches different memory → TLB misses → each miss causes a page-table walk (~100 ns on a warm cache, ~1 µs if the page table itself isn't cached).
- The new thread's working set evicts the old thread's data from L1/L2 caches.
- L1 cache miss: ~4 cycles. L2 miss: ~12 cycles. L3 miss: ~40 cycles. DRAM: ~200 cycles.

```
Thread A's cache footprint: 128 KB (L2 cache on a typical core)
Thread B replaces it: 128 KB evicted
Thread A resumes: 128 KB cold misses = ~128 KB / 64 bytes per line × 12 cycles = ~24,000 cycles ≈ 8 µs at 3 GHz
```

This "cache thrashing" means the true cost of a context switch is often **5–20 µs** in practice, not the theoretical 1 µs.

---

## 4. The C10K Problem

In 1999, engineer **Dan Kegel** published a landmark paper titled *"The C10K Problem"* asking: how do you engineer a server to handle **10,000 simultaneous clients** on a single machine?

### The Math That Breaks Thread-Per-Connection

```
10,000 connections × 1 thread each
= 10,000 OS threads

Stack memory alone:
10,000 × 8 MB = 80,000 MB = ~78 GB of virtual address space

Kernel task_structs:
10,000 × 10 KB = ~100 MB of non-swappable kernel memory

Context switches (assuming each thread wakes 10×/sec):
10,000 × 10 × 2 × 3 µs = 600 ms/sec = 60% of a single core, wasted
```

**80 GB of address space** was a physical impossibility on 1999 hardware (32-bit, max 4 GB virtual). Even on modern 64-bit systems with 128 GB RAM, you would spend most of it on *empty thread stacks*.

### What Kegel Found

Kegel's paper surveyed all known approaches and concluded:

1. **Thread-per-connection**: Hits OS limits around 1,000–5,000 threads. Stack memory and scheduler overhead dominate.
2. **Process-per-connection** (Apache MPM prefork): Even worse — processes don't share address space so page tables are duplicated.
3. **Non-blocking I/O with `select()`**: O(n) scan of file descriptors on every call — degrades at large n.
4. **Non-blocking I/O with `poll()`**: Same O(n) problem as `select()`, different API.
5. **`epoll` / `kqueue` / IOCP**: O(1) per-event notification — the correct answer.

The C10K paper directly motivated the development of `epoll` in Linux 2.5.44 (2002).

---

## 5. Detached Threads and Why They're Dangerous

Notice the `.detach()` call in the example above. This is the natural choice when you don't want to `join()` every thread manually — but it creates serious problems.

### The Problem with `detach()`

```cpp
// DANGEROUS pattern — illustrates what goes wrong
void bad_server() {
    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);

        // detach() means: "I give up all ability to track this thread"
        std::thread([client_fd]() {
            handle_client(client_fd);
        }).detach();

        // Problems:
        // 1. You cannot join() this thread → cannot wait for clean shutdown
        // 2. If the thread crashes, you may not know
        // 3. No back-pressure: if clients arrive faster than they finish,
        //    threads accumulate without bound → OOM
        // 4. Graceful shutdown requires knowing when ALL threads are done,
        //    but detach() makes this impossible without extra coordination
    }
}

// BETTER: Track threads explicitly (still not great at scale)
std::vector<std::thread> threads;
// ... in accept loop:
threads.emplace_back(handle_client, client_fd);
// ... at shutdown:
for (auto& t : threads) {
    if (t.joinable()) t.join(); // wait for each one
}
```

### Graceful Shutdown Is Nearly Impossible

Imagine SIGTERM arrives — your process must shut down cleanly. With detached threads:

- You cannot enumerate running threads (the OS doesn't expose this per-thread).
- You cannot signal each thread to finish (no handle).
- You cannot wait for them (no joinable handle).
- Clients in the middle of a request get their connection dropped hard.

The only escape is global shared state (an atomic `bool shutdown_requested` that every thread polls) — which is error-prone, adds latency, and doesn't compose cleanly.

---

## 6. What the OS Scheduler Actually Does

Understanding the scheduler demystifies why threads are expensive at scale.

### 6.1 Time Slicing and Preemption

The Linux scheduler gives each thread a **time quantum** — a maximum CPU time before preemption. Under the **Completely Fair Scheduler (CFS)**, introduced in Linux 2.6.23, this is not a fixed quantum but rather a dynamic slice based on the number of runnable tasks and their "nice" (priority) values.

CFS tracks **virtual runtime** (`vruntime`) for each task: a monotonically increasing counter of how much CPU time the task has consumed, weighted by its priority. The scheduler always picks the task with the **smallest vruntime** — the one that has received the least CPU time relative to its peers.

```
vruntime += (actual_delta_ns × NICE_0_LOAD) / task_weight
```

This achieves fairness: no thread starves indefinitely, high-priority threads get larger time slices.

### 6.2 The Runqueue

CFS maintains a **red-black tree** of runnable tasks keyed by `vruntime`. Insertion and removal are O(log N). The leftmost node (smallest vruntime) is the next task to run, cached in a pointer for O(1) access.

With 10,000 runnable threads:
- O(log 10,000) ≈ 13 operations per scheduling decision.
- Each operation touches cache lines in the rb-tree.
- At 1,000 context switches/sec → 13,000 rb-tree operations/sec just for scheduling.

### 6.3 I/O Wait and Sleep

When a thread calls `recv()` on a socket with no data:

1. The kernel moves the thread from **TASK_RUNNING** to **TASK_INTERRUPTIBLE** (sleeping).
2. The thread is removed from the runqueue (no longer competing for CPU).
3. The socket's wait queue gets an entry pointing to this thread.
4. When data arrives (NIC interrupt → driver → TCP stack), the kernel finds the wait queue entry and moves the thread back to **TASK_RUNNING**.

This wake-up path — interrupt → softirq → TCP processing → waitqueue wakeup → scheduler enqueue — takes roughly **10–50 µs** from packet arrival to thread getting CPU time.

---

## 7. The Event Loop Model: epoll

`epoll` is the Linux kernel mechanism that lets a single thread efficiently monitor thousands of file descriptors simultaneously.

### 7.1 The epoll API

```cpp
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <cstring>
#include <iostream>
#include <unordered_map>

// Make a file descriptor non-blocking
void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    // ... bind, listen as before ...

    // Create the epoll instance — returns a file descriptor
    // that represents a kernel-maintained interest list
    int epfd = epoll_create1(0);

    // Register server_fd: notify me when it's readable (new connection ready)
    epoll_event ev{};
    ev.events  = EPOLLIN;           // interested in read events
    ev.data.fd = server_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

    // Event loop — one thread, thousands of connections
    epoll_event events[64];         // batch up to 64 events per iteration
    std::unordered_map<int, /* connection state */ int> connections;

    while (true) {
        // epoll_wait BLOCKS until at least one fd is ready.
        // Crucially: it sleeps without consuming CPU.
        // When N fds become ready, it returns all N at once — O(1) per event.
        int n = epoll_wait(epfd, events, 64, -1 /* timeout: infinite */);

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                // New connection — accept it and add to epoll
                int client_fd = accept(server_fd, nullptr, nullptr);
                set_nonblocking(client_fd);

                epoll_event cev{};
                cev.events   = EPOLLIN | EPOLLET; // Edge-Triggered mode
                cev.data.fd  = client_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev);
                connections[client_fd] = 0; // initial state

            } else if (events[i].events & EPOLLIN) {
                // Existing client has data — read without blocking
                char buf[4096];
                ssize_t n_read = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n_read <= 0) {
                    // Connection closed or error
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    connections.erase(fd);
                } else {
                    send(fd, buf, n_read, 0); // echo back
                }
            }
        }
    }
}
```

### 7.2 Edge-Triggered vs Level-Triggered

| Mode | Trigger | When to use |
|---|---|---|
| **Level-Triggered (LT)** | epoll_wait returns as long as the fd has data | Simpler to implement; default mode |
| **Edge-Triggered (ET)** | epoll_wait returns *only once* when state changes (e.g., new data arrives) | Higher performance; requires draining the fd in a loop; used by Nginx, Node.js |

```
LT: epoll fires repeatedly until buffer is empty → safe but extra wakeups
ET: epoll fires once per new data arrival → must read until EAGAIN, or miss data
```

### 7.3 Why epoll Eliminates Context Switch Overhead

With epoll:
- **1 thread** waits inside `epoll_wait()` — no context switches while idle.
- When 500 clients send data simultaneously, `epoll_wait` returns *all 500 events* in one call.
- We process each one in a tight loop — all data stays hot in L1/L2 cache.
- Zero threads are created or destroyed.

Compare to thread-per-connection with 500 active clients:
- 500 threads wake up (500 context switches, each ~3 µs → 1.5 ms overhead).
- Each thread's working set competes for L2 cache → thrashing.
- epoll's single thread: **0 context switches**, full cache locality.

### 7.4 Latency and Throughput Under High Connection Count

```
Thread-per-connection at 10K connections:
  - Throughput: degrades due to scheduling overhead
  - Latency: p99 latency spikes as threads wait for CPU time

epoll with 1 event-loop thread at 10K connections:
  - Throughput: limited only by network bandwidth and compute
  - Latency: consistently low; no scheduler queue to wait in
```

Real-world data (Nginx vs Apache on static file serving):
- Apache (thread-per-request): ~4,000 req/sec at 10K concurrent
- Nginx (event-loop): ~50,000 req/sec at 10K concurrent — a 12× difference

---

## 8. When Thread-Per-Connection IS Appropriate

Despite its problems at scale, thread-per-connection is the right choice in several scenarios:

### 8.1 CPU-Bound, Short-Lived Work

```cpp
// A server that performs image resizing on upload.
// Each request is CPU-heavy, ~200ms of pure computation.
// With 8 cores, only 8 threads run at once anyway.
// Event loop gives zero benefit here — you're CPU-bound, not I/O-bound.

void handle_image_resize(int client_fd) {
    auto image = read_image(client_fd);   // fast, small upload
    auto resized = resize_image(image);   // 200ms of CPU work
    send_image(client_fd, resized);       // fast send
    close(client_fd);
}
// spawn_thread(handle_image_resize, client_fd) — perfectly fine here
```

### 8.2 Low Connection Count, High Code Simplicity

Internal services (microservices talking to each other) often have < 50 simultaneous connections. Thread-per-connection's simplicity — no callbacks, no state machines, no async complexity — is a meaningful engineering advantage.

### 8.3 Debugging and Observability

A stack trace in a threaded server shows you exactly where a connection is: `handle_client → read_request → parse_headers → ...`. In an event loop, a connection's state is spread across callbacks and a state machine — harder to debug.

---

## 9. Thread Pools: The Practical Middle Ground

A **thread pool** gives you the simplicity of blocking code with *bounded* concurrency — you pre-create N threads and reuse them for all connections.

```cpp
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <atomic>

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads) : running_(true) {
        for (size_t i = 0; i < num_threads; ++i) {
            // Pre-create all threads once — amortize creation cost
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    // Submit a task to the pool. Returns immediately.
    // The task will run in one of the pre-existing threads.
    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(task));
        }
        // Wake one sleeping worker
        cv_.notify_one();
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        cv_.notify_all();  // wake all workers so they can exit
        for (auto& t : workers_) t.join(); // clean shutdown!
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // Sleep until there's a task OR we're shutting down
                cv_.wait(lock, [this] {
                    return !queue_.empty() || !running_;
                });
                if (!running_ && queue_.empty()) return; // clean exit
                task = std::move(queue_.front());
                queue_.pop();
            }
            task(); // execute outside the lock
        }
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex                        mutex_;
    std::condition_variable           cv_;
    std::atomic<bool>                 running_;
};

// Usage: fixed-size pool of 16 threads handles all connections
ThreadPool pool(16); // bounded: never more than 16 threads, regardless of connections

// In accept loop:
// pool.submit([client_fd]() { handle_client(client_fd); });
```

### Thread Pool Tradeoffs

| Aspect | Unlimited threads | Thread pool | epoll event loop |
|---|---|---|---|
| Concurrency | Unbounded (dangerous) | Bounded (safe) | Single-threaded (or few) |
| Code style | Simple blocking | Simple blocking | Callbacks / coroutines |
| Idle cost | High (stacks, task_structs) | Moderate (fixed N threads) | Very low |
| I/O-bound perf | Poor at scale | Good if pool ≫ I/O waits | Excellent |
| CPU-bound perf | Excellent | Excellent | Poor (blocks event loop) |
| Shutdown | Hard (detach problem) | Clean (join workers) | Easy |

---

## 10. The Impedance Mismatch: Async I/O Meets Human Brains

The fundamental tension is: **blocking I/O is easy to reason about; non-blocking I/O is efficient but hard to write**.

### 10.1 Callback Hell

The naïve event-loop approach leads to deeply nested callbacks:

```cpp
// Callback-based async — hard to follow, hard to debug
socket.async_read(buf, [buf](error_code ec, size_t n) {
    if (ec) return handle_error(ec);
    process(buf, n, [](Result r) {
        socket.async_write(r, [](error_code ec2) {
            if (ec2) return handle_error(ec2);
            // What was the original request? Context is lost.
        });
    });
});
// This is called "callback hell" or "pyramid of doom"
```

### 10.2 C++20 Coroutines: Best of Both Worlds

C++20 coroutines let you write *apparently sequential* code that is actually event-driven under the hood. The compiler transforms `co_await` expressions into a state machine that integrates with an event loop — you get the readability of blocking code with the performance of `epoll`.

```cpp
// C++20 coroutine style (using Asio or a similar library)
// This looks blocking but is fully non-blocking and epoll-backed.
Task<void> handle_client(AsyncSocket socket) {
    char buf[4096];

    // co_await suspends this coroutine; the event loop runs other coroutines.
    // When data arrives, the event loop resumes THIS coroutine exactly here.
    size_t n = co_await socket.async_read(buf, sizeof(buf));

    auto response = compute_response(buf, n);

    // Suspends again during write; resumes when write completes.
    co_await socket.async_write(response);

    // Coroutine frame is on the heap (~hundreds of bytes), not an 8MB stack.
    // 10,000 coroutines ≈ a few MB total — vs 80 GB for 10,000 threads.
}
```

### 10.3 Preview: io_uring (Linux 5.1+)

`io_uring` is the next generation after `epoll` — it allows **true async I/O submission and completion** via shared memory ring buffers, eliminating even the `epoll_wait` syscall overhead. A single `io_uring_enter()` can submit multiple I/O operations and harvest multiple completions in one kernel crossing. This is what modern high-performance servers (liburing, Tokio, Monoio) use.

---

## Key Takeaways

- **Thread-per-connection is intuitive but has hard OS limits**: stack memory (~8 MB), kernel task_struct (~10 KB), and scheduler overhead make it impractical beyond ~1,000–5,000 concurrent connections.
- **Context switches cost 1–20 µs** including cache warm-up effects. At scale, this overhead dominates useful work.
- **The C10K problem (Dan Kegel, 1999)** crystallized these limits: 10,000 threads × 8 MB = ~80 GB RAM, plus 60%+ CPU wasted on scheduling — killed servers of the era.
- **`detach()`ed threads are a liability**: you lose the ability to join, shutdown gracefully, or bound resource growth.
- **Linux CFS uses a red-black tree** ordered by `vruntime` for fair scheduling — but this O(log N) per-switch cost multiplies with thread count.
- **`epoll` solves the C10K problem** by monitoring thousands of fds with a single thread: O(1) per event, zero context switches while idle, full cache locality.
- **Edge-triggered epoll** is more efficient than level-triggered but requires careful draining of read buffers to avoid missing events.
- **Thread pools** provide a practical middle ground: bounded concurrency, clean shutdown, simple blocking code — ideal for CPU-bound or mixed workloads.
- **Callback-based async code** is hard to write and maintain; **C++20 coroutines** restore the sequential mental model while retaining non-blocking performance.
- **The right model depends on the workload**: event loop for I/O-bound (web servers, proxies, message brokers); thread pool for CPU-bound (image processing, ML inference); coroutines for complex async flows.

---

## Further Reading

- **Dan Kegel, "The C10K Problem" (1999)** — http://www.kegel.com/c10k.html — the original paper that defined the problem and surveyed solutions.
- **Linux `epoll(7)` man page** — `man 7 epoll` — canonical reference for edge vs level triggering, `EPOLLONESHOT`, `EPOLLEXCLUSIVE`.
- **"Linux Kernel Development" by Robert Love (3rd ed.)** — Chapter 4 covers the CFS scheduler in depth.
- **Jens Axboe, "Efficient IO with io_uring" (2019)** — https://kernel.dk/io_uring.pdf — the design paper for io_uring.
- **cppreference: C++20 Coroutines** — https://en.cppreference.com/w/cpp/language/coroutines — the standard specification.
- **Asio C++ Library documentation** — https://think-async.com/Asio/ — production-grade async I/O in C++ with coroutine support.
- **"The Art of Multiprocessor Programming" by Herlihy & Shavit** — Foundational text on concurrent data structures used in thread pools and schedulers.
- **Brendan Gregg, "Systems Performance" (2nd ed.)** — Chapter 5 (CPUs) and Chapter 6 (Memory) for profiling context switch and cache costs in practice.
- **nginx architecture overview** — https://nginx.org/en/docs/dev/development_guide.html — how a real production server applies the event-loop model.
