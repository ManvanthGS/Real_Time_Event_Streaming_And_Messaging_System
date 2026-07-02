# Data Races, Mutex Scope, and Safe Concurrent Access in C++

> **Phase 0 — Knowledge Base | Document 02**
> Prerequisites: Basic C++ (classes, pointers), Phase 0 Doc 01 (threads & RAII)

---

## TL;DR

A **data race** occurs when two or more threads access the same memory location concurrently, at least one access is a write, and no synchronisation mechanism orders those accesses — the result is **undefined behaviour** under the C++11 memory model, meaning the compiler and CPU are free to do literally anything. Mutexes prevent data races by enforcing mutual exclusion, but **what** you protect matters as much as **whether** you protect it: a mutex guards an *invariant*, not an object. The most dangerous pattern is "take a raw pointer or reference under a lock, release the lock, then dereference" — even one microsecond of gap is enough for another thread to destroy the object. The solution is `std::shared_ptr`, which keeps objects alive through reference counting regardless of map erasure, combined with the **snapshot pattern** (copy handles under lock, do I/O outside lock) to avoid holding a mutex during blocking operations.

---

## 1. What Is a Data Race? (C++11 Definition)

The C++11 standard (§1.10) defines a **data race** precisely:

> Two actions are *conflicting* if they access the same memory location and at least one is a write. A program execution has a data race if it contains two conflicting actions in different threads, neither of which happens-before the other.

### 1.1 Why "Undefined Behaviour"?

"Undefined behaviour" (UB) is not a polite way of saying "sometimes wrong result." It means the *entire program* loses all guarantees. The optimiser is legally allowed to:

- Eliminate the racy read entirely (it "proved" the value never changes).
- Reorder stores across the race window.
- Duplicate or hoist memory accesses.
- Assume the branch containing UB is never taken, deleting surrounding code.

In practice, data-race UB manifests as:

| Symptom | Root Cause |
|---------|-----------|
| Segfault in release, works in debug | Debug disables optimisations that expose the race |
| Intermittent wrong values | CPU store buffers not yet flushed |
| Hang / deadlock | Torn read of a boolean used as spin flag |
| Silent data corruption | Two threads half-write a struct |

### 1.2 The Hardware Reality

Modern CPUs are *out-of-order* and *superscalar*. Each core has its own store buffer and L1 cache. Without a **memory fence**, a write on core A may not be visible on core B for hundreds of nanoseconds — even if the C++ source looks sequential. Mutexes and atomics implicitly emit the correct fence instructions (`MFENCE` on x86, `DMB` on ARM).

```cpp
// BROKEN — two threads, no synchronisation
int counter = 0; // shared

// Thread A                   // Thread B
counter++;                    counter++;
// x86 compiles to:           // same
// MOV eax, [counter]         // MOV eax, [counter]
// INC eax                    // INC eax
// MOV [counter], eax         // MOV [counter], eax

// Both threads read 0, both write 1. Final value: 1, not 2.
// Classic "lost update" — a data race.
```

---

## 2. The Exact Bug in `receiveLoop` — Race Window Analysis

### 2.1 The Broken `before` Version

```cpp
// BROKEN receiveLoop — DO NOT USE
void Server::receiveLoop() {
    while (m_running) {                              // (a) read m_running
        auto [clientId, message] = m_queue.pop();   // (b) blocking dequeue

        std::lock_guard<std::mutex> lock(m_mutex);  // (c) acquire lock
        auto it = m_clients.find(clientId);         // (d) look up map
        if (it == m_clients.end()) continue;

        Connection* conn = it->second.get();        // (e) raw pointer — DANGER
        // Lock is released HERE when lock_guard destructs
        // at the end of this block... wait, actually we release it
        // manually by scoping — but the original bug is worse:
    }
}

// The real broken pattern (explicit unlock before use):
void Server::receiveLoopBroken() {
    while (m_running) {
        auto [clientId, message] = m_queue.pop();

        Connection* conn = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex); // lock acquired
            auto it = m_clients.find(clientId);
            if (it == m_clients.end()) continue;
            conn = it->second.get();                   // (A) raw pointer copied
        }                                              // (B) lock RELEASED here

        // (C) RACE WINDOW: between (B) and here, another thread
        //     can call removeClient(clientId), which erases the
        //     unique_ptr from the map → destructor runs → conn is DANGLING
        conn->send(message);                           // (D) USE-AFTER-FREE 💥
    }
}
```

### 2.2 Timeline of the Race

```
Thread A (receiveLoop)          Thread B (removeClient)
────────────────────────────    ────────────────────────────
lock(m_mutex)
conn = it->second.get()  ← raw ptr to Connection object
unlock(m_mutex)
                                lock(m_mutex)
                                m_clients.erase(clientId)
                                  └─ unique_ptr destructor runs
                                     └─ delete Connection object
                                unlock(m_mutex)
conn->send(message)      ← DANGLING POINTER — undefined behaviour
```

The **race window** is the gap between releasing the mutex (step B) and using `conn` (step D). On a multi-core machine this window is real and exploitable — not theoretical.

### 2.3 The Fixed `after` Version

```cpp
// FIXED receiveLoop using shared_ptr snapshot pattern
void Server::receiveLoopFixed() {
    while (m_running.load(std::memory_order_acquire)) { // atomic read

        auto [clientId, message] = m_queue.pop();       // blocking — outside lock

        std::shared_ptr<Connection> connSnapshot;       // will hold a ref-count
        {
            std::lock_guard<std::mutex> lock(m_mutex);  // (1) acquire lock
            auto it = m_clients.find(clientId);
            if (it == m_clients.end()) continue;        // (2) not found — skip
            connSnapshot = it->second;                  // (3) copy shared_ptr
                                                        //     ref-count now ≥ 2
        }                                               // (4) lock released
                                                        //     ref-count still ≥ 1

        // Even if removeClient() runs here and erases the map entry,
        // the Connection object stays alive because connSnapshot
        // holds a reference. No dangling pointer possible.
        connSnapshot->send(message);                    // (5) safe send
    }                                                   // (6) connSnapshot destructs
                                                        //     ref-count may drop to 0
}
```

---

## 3. Mutex Fundamentals: Protecting Invariants, Not Objects

### 3.1 The Wrong Mental Model

Many beginners think: "I lock `m_mutex` before touching `m_clients`, so `m_clients` is protected."

This is **incomplete**. A mutex protects a *logical invariant* — a property that must remain true between operations. For example:

> *Every `clientId` in `m_clients` maps to a valid, live `Connection` object that can safely receive messages.*

If you violate this invariant — even briefly — any thread that observes the intermediate state sees corruption.

### 3.2 RAII `lock_guard` — Never Forget to Unlock

```cpp
#include <mutex>

std::mutex m_mutex;

void badFunction() {
    m_mutex.lock();
    // ... if this throws, unlock() is never called → deadlock
    doSomethingRisky();
    m_mutex.unlock(); // might never reach here
}

void goodFunction() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // lock_guard calls lock() in its constructor
    // and unlock() in its destructor — even if an exception is thrown
    doSomethingRisky(); // safe: mutex always released
}   // ← destructor of lock_guard fires here
```

`std::unique_lock` provides the same guarantee with added flexibility (deferred locking, manual unlock, use with condition variables):

```cpp
void flexibleFunction() {
    std::unique_lock<std::mutex> lock(m_mutex);   // locked immediately
    if (earlyExit()) return;                       // still unlocked on exit
    lock.unlock();                                 // manual early release OK
    doBigWork();                                   // work without holding lock
    lock.lock();                                   // re-acquire if needed
}
```

### 3.3 What the Mutex Does NOT Do

- It does **not** prevent another thread from reading the protected data — unless that thread also acquires the same mutex.
- It does **not** protect against UB caused by holding a raw pointer after releasing the lock.
- It does **not** make individual operations atomic (e.g., `map.find()` then `map.erase()` are two separate critical sections unless bracketed by a single lock).

---

## 4. The Lock-Then-Release-Then-Access Anti-Pattern

This is one of the most common concurrency bugs in production C++ code. It has several names: **time-of-check-time-of-use (TOCTOU)**, **use-after-unlock**, or simply the **stale reference bug**.

```cpp
// Anti-pattern: extract raw pointer/reference under lock, use after unlock
class SessionManager {
    std::mutex m_mutex;
    std::unordered_map<int, std::unique_ptr<Session>> m_sessions;

public:
    void processSession(int id) {
        Session* rawPtr = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_sessions.find(id);
            if (it != m_sessions.end())
                rawPtr = it->second.get(); // (A) raw pointer — WRONG
        } // (B) lock released

        if (rawPtr)
            rawPtr->process(); // (C) concurrent erase → dangling! WRONG
    }

    void removeSession(int id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions.erase(id); // unique_ptr destructor → delete Session
    }
};
```

### 4.1 Why This Feels Safe but Isn't

The `find()` call was safe. The raw pointer *was* valid at point (A). But validity is not preserved across a lock boundary. The moment the lock is released, the invariant ("the map entry exists") is no longer guaranteed — another thread can invalidate it before you dereference the pointer.

### 4.2 Rules to Avoid This Pattern

1. **Never store raw pointers/references to locked objects and use them after the lock scope.**
2. If you need to use a resource outside a lock, use `shared_ptr` to co-own it.
3. If the resource truly cannot be shared, do all work *inside* the lock scope.
4. Use thread sanitiser (`-fsanitize=thread`) to catch these bugs automatically.

---

## 5. The `shared_ptr` Solution: Reference Counting as a Lifetime Anchor

### 5.1 How `shared_ptr` Works

`std::shared_ptr<T>` maintains a **control block** on the heap containing:
- A **strong reference count** (number of `shared_ptr` owners).
- A **weak reference count** (number of `weak_ptr` observers).
- A deleter function.

The object is destroyed only when the strong count drops to **zero**.

```cpp
// Anatomy of shared_ptr ref counting
auto sp1 = std::make_shared<Connection>(id); // strong count = 1
{
    auto sp2 = sp1;  // copy → strong count = 2
    auto sp3 = sp1;  // copy → strong count = 3
    // sp3 destructs at end of block → count = 2
    // sp2 destructs at end of block → count = 1
}
// sp1 destructs → count = 0 → Connection deleted
```

### 5.2 Applying It to the Map

```cpp
// Change the map value type from unique_ptr to shared_ptr
class Server {
    std::mutex m_mutex;
    // Before: std::unordered_map<int, std::unique_ptr<Connection>> m_clients;
    std::unordered_map<int, std::shared_ptr<Connection>> m_clients; // FIXED
};

void Server::removeClient(int clientId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_clients.erase(clientId);
    // The shared_ptr inside the map is destroyed → strong count decremented
    // BUT if receiveLoop holds a copy (connSnapshot), count stays ≥ 1
    // The Connection object is NOT deleted yet — it lives until all
    // shared_ptr copies are destroyed.
}
```

### 5.3 Thread Safety of `shared_ptr` Itself

The C++ standard guarantees that **ref-count operations** (copy, destroy) on `shared_ptr` are atomic. However, the **pointed-to object** is not automatically thread-safe — you still need to synchronise access to `Connection::send()` if multiple threads can call it concurrently.

```cpp
// shared_ptr thread safety summary:
// ✅ Copying the same shared_ptr from multiple threads — safe
// ✅ Destroying shared_ptr copies in multiple threads — safe (atomic ref-count)
// ❌ Writing to *shared_ptr from multiple threads — NOT safe (need mutex or atomic on the object)
// ❌ Assigning the same shared_ptr instance from multiple threads — NOT safe
```

---

## 6. `unique_ptr` vs `shared_ptr` Ownership Semantics in Concurrent Code

### 6.1 `unique_ptr` — Exclusive Ownership

```cpp
std::unique_ptr<Foo> up = std::make_unique<Foo>();
// Only ONE owner at a time. Transfer via std::move().
// Cannot be copied → cannot be shared across threads safely.
// Perfect for: factory functions, containers where you OWN the object.
// NOT suitable for: handing out references to concurrent threads.
```

### 6.2 `shared_ptr` — Shared Ownership

```cpp
std::shared_ptr<Foo> sp = std::make_shared<Foo>();
// Many owners. Ref count maintained atomically.
// Safe to copy across threads.
// Suitable for: objects with non-deterministic lifetime,
//              objects accessed by multiple concurrent threads.
// Overhead: heap allocation for control block, atomic ref-count ops.
```

### 6.3 Decision Guide for Concurrent Code

| Scenario | Use |
|----------|-----|
| Single-threaded ownership, short lifetime | `unique_ptr` |
| Object passed to exactly one other thread (move semantics) | `unique_ptr` + `std::move` |
| Object accessed by multiple threads with uncertain lifetime | `shared_ptr` |
| Observer that should NOT prevent destruction | `weak_ptr` + `lock()` |
| Ultra-hot path, no shared lifetime | Raw pointer with external lifetime guarantee |

```cpp
// weak_ptr for observers (e.g., a timer that might outlive the connection)
void Timer::onExpiry(std::weak_ptr<Connection> weakConn) {
    if (auto conn = weakConn.lock()) { // atomically checks count > 0 and bumps it
        conn->send("KEEPALIVE");        // safe: conn is now a shared_ptr
    } else {
        // Connection already destroyed — do nothing
    }
}
```

### 6.4 Design Decision: `shared_ptr` for Sockets — Unique Resource, Shared Lifetime

A question that naturally arises when reading this codebase: **the `Socket` class is already move-only** (copy constructor and copy-assignment are `= delete`), which is the right design — an OS socket file descriptor is a unique resource. So why store it in a `shared_ptr` inside `m_peers`? Shouldn't `unique_ptr` express ownership more accurately?

The distinction to internalise is:

> **The socket resource is unique. The C++ *object wrapping* that resource needs shared lifetime.**

These are two separate concerns. The `Socket` class correctly models uniqueness at the *value* level (no copying, RAII destructor closes the fd). But the *object itself* must be kept alive across a mutex boundary by multiple concurrent threads — and `unique_ptr` provides no mechanism for that.

#### Why `unique_ptr` breaks at the mutex boundary

With `std::map<SocketHandle, std::unique_ptr<Socket>> m_peers`, to use a socket outside the lock you can only obtain a raw pointer:

```cpp
Socket* sock_ptr = nullptr;
{
    std::lock_guard<std::mutex> lock(m_peersMutex);
    auto it = m_peers.find(handle);
    if (it != m_peers.end())
        sock_ptr = it->second.get();  // (A) raw pointer borrowed from unique_ptr
}                                     // (B) lock released — unique_ptr still owns the object

// RACE WINDOW: stop() can run HERE, calling m_peers.erase(handle)
// → unique_ptr destructor fires → Socket object deleted
// → sock_ptr is now dangling
framed_socket(*sock_ptr).recv_message(msg);  // (C) USE-AFTER-FREE 💥
```

A raw pointer borrowed from a `unique_ptr` has **no ability to prevent the owner from destroying the object**. The `unique_ptr` by definition answers to exactly one owner (the map), and when that owner decides to erase, the object is gone.

#### What `shared_ptr` actually represents here

The `shared_ptr` is **not** claiming that the socket is semantically shared. It is a **lifetime-extension tool**. The map remains the *canonical semantic owner* — it created the socket, it decides when to disconnect. But while a thread is blocked in `recv_exact()` or `send_all()`, it holds a local `shared_ptr` copy that keeps the C++ object alive until the I/O call returns:

```
m_peers map (canonical owner)  ────→  [shared_ptr → Socket object]
                                              ↑
receiveLoop's local `sock`  ──────────────────┘  (ref bumped before I/O)
broadcast's snapshot vector ──────────────────┘  (ref bumped before I/O)
```

When `stop()` calls `m_peers.erase(handle)`, the map's `shared_ptr` is destroyed (ref-count drops by 1), but the Socket object itself is not deleted until all threads finish their current I/O and release their local copies. At that point, the ref-count hits zero and the destructor runs — cleanly, with no dangling pointers.

#### The three alternatives and their tradeoffs

| Approach | Correct? | Concurrent I/O? | Shutdown behaviour |
|---|---|---|---|
| `unique_ptr` + raw ptr borrow (current broken pattern) | ❌ UB — dangling ptr | ✅ | N/A |
| `unique_ptr` + hold mutex during entire I/O | ✅ Safe | ❌ Fully serialised (Bug 3) | Immediate |
| `unique_ptr` + `shared_mutex` (shared read, exclusive write) | ✅ Safe | ✅ | `stop()` **blocks** until all in-flight I/O completes |
| **`shared_ptr` + mutex snapshot** *(chosen)* | ✅ **Safe** | ✅ | `stop()` closes fd immediately; error propagates naturally |

**`unique_ptr` + `shared_mutex` explained:** You could achieve the same safety with a `std::shared_mutex`. `receiveLoop` and `broadcast` take a `std::shared_lock` (reader lock) for the *duration* of their I/O. `stop()` takes a `std::unique_lock` (writer lock) before erasing — it blocks until all in-flight I/O finishes. This is semantically clean but has a significant drawback: **`stop()` can stall indefinitely** if a peer is slow, unresponsive, or intentionally stalling the connection (a denial-of-service vector).

```cpp
// Conceptual: unique_ptr + shared_mutex approach
std::shared_mutex m_peersMutex;
std::map<SocketHandle, std::unique_ptr<Socket>> m_peers;

void receiveLoop(SocketHandle handle) {
    while (m_running) {
        std::shared_lock<std::shared_mutex> lock(m_peersMutex); // shared
        auto it = m_peers.find(handle);
        if (it == m_peers.end()) break;
        // Blocking recv under a SHARED lock — stop() cannot proceed until this returns
        framed.recv_message(msg); // if peer is unresponsive, stop() waits forever
    }
}

void stop() {
    m_running = false;
    std::unique_lock<std::shared_mutex> lock(m_peersMutex); // EXCLUSIVE — waits for all readers
    m_peers.clear(); // only runs when no thread holds a shared_lock
}
```

**`shared_ptr` + close-signals-error (chosen approach):** `stop()` closes the OS file descriptor immediately via `socket->close()`. Any thread currently blocked in `recv_exact()` or `send_all()` will get an error return (EBADF / ECONNRESET / 0 bytes) on its next syscall. It checks the return value, sees failure, and exits on its own schedule — without `stop()` having to wait. The `Socket` C++ object is then destroyed once the last `shared_ptr` copy drops out of scope. This makes shutdown maximally responsive.

```cpp
void stop() {
    m_running = false;
    std::lock_guard<std::mutex> lock(m_peersMutex);
    for (auto const& [handle, socket] : m_peers)
        socket->close(); // close fd NOW — unblocks any thread stuck in recv/send
    m_peers.clear();     // drops map's shared_ptr copies
    // receiveLoop threads will see recv failure and self-exit — no blocking wait here
}
```

#### Summary rule

> Use `unique_ptr` when one owner controls both *object lifetime* and *object access*. Switch to `shared_ptr` when multiple concurrent threads need to *access* the object across a mutex boundary, even if only one entity is the *semantic* owner. The `Socket` class's `= delete` copy semantics are the right model for the **resource**; `shared_ptr` is the right model for **C++ object lifetime** in a multithreaded context.

---

## 7. Lock Granularity: Coarse vs. Fine-Grained Locking

### 7.1 Coarse-Grained Locking (One Big Lock)

```cpp
class CoarseBroker {
    std::mutex m_bigLock;                                    // single lock
    std::unordered_map<std::string, Topic> m_topics;
    std::unordered_map<int, ClientInfo>    m_clients;
    std::queue<Message>                    m_pendingMsgs;

public:
    void publish(const std::string& topic, Message msg) {
        std::lock_guard<std::mutex> lock(m_bigLock); // locks EVERYTHING
        m_topics[topic].messages.push_back(msg);
        for (auto& sub : m_topics[topic].subscribers)
            m_pendingMsgs.push({sub, msg});
    }
    // All methods contend on ONE mutex → serialised → poor scalability
};
```

**Pros:** Simple, easy to reason about, impossible to deadlock (no lock ordering required).  
**Cons:** Maximum contention — all threads queue behind one lock even for unrelated data.

### 7.2 Fine-Grained Locking (Per-Resource Locks)

```cpp
class FineBroker {
    std::mutex m_topicsMutex;
    std::unordered_map<std::string, std::shared_ptr<Topic>> m_topics;

    std::mutex m_clientsMutex;
    std::unordered_map<int, std::shared_ptr<ClientInfo>> m_clients;

public:
    void publish(const std::string& topic, Message msg) {
        std::shared_ptr<Topic> t;
        {
            std::lock_guard<std::mutex> lock(m_topicsMutex); // only lock topics
            t = m_topics[topic]; // copy shared_ptr — snapshot pattern
        }
        if (t) t->distribute(msg); // work without holding any lock
    }
    // Separate locks → publish and client operations can proceed in parallel
};
```

**Pros:** Higher throughput, better CPU utilisation.  
**Cons:** Risk of **deadlock** if two locks are acquired in inconsistent order.

### 7.3 Deadlock Prevention with Lock Ordering

```cpp
// DEADLOCK: Thread A locks L1 then L2; Thread B locks L2 then L1
void threadA() {
    std::lock_guard<std::mutex> a(L1);
    std::lock_guard<std::mutex> b(L2); // waits for Thread B to release L2
}
void threadB() {
    std::lock_guard<std::mutex> a(L2);
    std::lock_guard<std::mutex> b(L1); // waits for Thread A to release L1
    // → circular wait → deadlock
}

// FIX: always acquire locks in consistent global order, or use std::scoped_lock
void safeFunction() {
    // std::scoped_lock uses deadlock-avoidance algorithm internally
    std::scoped_lock lock(L1, L2); // C++17 — locks both without deadlock
    // ...
}
```

---

## 8. Bug 3 Fix: Never Hold a Mutex During Blocking I/O

### 8.1 Why Blocking Under a Lock Is Catastrophic

Consider:

```cpp
// CATASTROPHIC — mutex held during blocking network send
void Server::broadcastBroken(const Message& msg) {
    std::lock_guard<std::mutex> lock(m_mutex);       // lock acquired
    for (auto& [id, conn] : m_clients) {
        conn->send(msg);  // send() may block on TCP backpressure
                          // if one client's buffer is full, ALL threads
                          // trying to acquire m_mutex are now BLOCKED
                          // waiting for this one slow client!
    }
}   // lock finally released — maybe after seconds
```

This causes two classical problems:

**Priority Inversion:** A high-priority thread needs the mutex but is blocked behind a low-priority thread that is blocked on slow I/O. The high-priority thread effectively runs at low priority.

**Convoy Effect:** Many threads line up behind one stalled operation. When the lock is released, they all rush the mutex. Throughput collapses as threads serialise on a bottleneck that should be parallelisable.

### 8.2 The Fix: Snapshot Under Lock, Work Outside Lock

```cpp
// FIXED — snapshot pattern eliminates lock during I/O
void Server::broadcastFixed(const Message& msg) {
    // Step 1: Collect all connection handles quickly under lock
    std::vector<std::shared_ptr<Connection>> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);       // held briefly
        snapshot.reserve(m_clients.size());
        for (auto& [id, conn] : m_clients)
            snapshot.push_back(conn);                    // copy shared_ptrs
    }   // lock released — entire I/O happens outside the lock

    // Step 2: Perform I/O without any lock held
    for (auto& conn : snapshot) {
        conn->send(msg); // may block — only this thread is affected
                         // other threads can acquire m_mutex freely
    }
    // shared_ptrs in snapshot destructs here — ref counts decremented
}
```

### 8.3 Quantified Impact

| Metric | Lock-During-I/O | Snapshot Pattern |
|--------|----------------|------------------|
| Lock hold time | O(N × RTT) | O(N) pointer copies |
| Throughput under slow client | Collapses | Unaffected |
| CPU utilisation | Threads spin/block | Threads run in parallel |
| Deadlock risk | High | Low |

---

## 9. The Snapshot Pattern in Depth

The snapshot pattern is the canonical solution for "iterate over a shared collection and do work per element."

```cpp
// Generic snapshot pattern template
template<typename Key, typename Value>
std::vector<std::shared_ptr<Value>> snapshot(
    const std::unordered_map<Key, std::shared_ptr<Value>>& map,
    std::mutex& mtx)
{
    std::vector<std::shared_ptr<Value>> result;
    std::lock_guard<std::mutex> lock(mtx);
    result.reserve(map.size());
    for (auto& [k, v] : map)
        result.push_back(v); // cheap: only copies shared_ptr (atomic ref increment)
    return result;           // copy elision (NRVO) — no extra allocation
}

// Usage
void Server::notifyAll(Event event) {
    auto clients = snapshot(m_clients, m_mutex);   // O(n) under lock
    for (auto& c : clients)
        c->notify(event);                           // O(n × latency) without lock
}
```

### 9.1 When the Snapshot Pattern Is Not Enough

If you need **transactional consistency** (e.g., "all clients see the same sequence number"), a snapshot is insufficient — by the time you send to the last client, new clients may have joined. In that case, you need either:
- A single lock for the entire send (accept the convoy risk), or
- A **sequence number + versioning** scheme (optimistic concurrency).

---

## 10. Memory Model Basics: Why `volatile` Is NOT a Substitute

### 10.1 What `volatile` Actually Does

`volatile` tells the compiler: "do not cache this variable in a register; reload it from memory on every access." It was designed for **memory-mapped hardware registers**, not thread communication.

```cpp
// WRONG — volatile does NOT prevent data races
volatile bool g_running = true; // volatile ≠ thread-safe

void workerThread() {
    while (g_running) { // read may be reordered past writes
        doWork();
    }
}

void stopThread() {
    g_running = false; // write has no memory fence — other cores
                       // may not see this for an arbitrary time
}
```

`volatile` does **not**:
- Emit memory fences (no `MFENCE`/`DMB`).
- Prevent the CPU from reordering this access with others.
- Make read-modify-write operations atomic.
- Prevent the compiler from reordering surrounding code.

### 10.2 The C++11 Memory Model

C++11 defines a formal **happens-before** relationship. An operation A *happens-before* B if:
1. A and B are in the same thread and A is sequenced before B, **or**
2. A *synchronises-with* B (e.g., mutex unlock synchronises-with subsequent lock, atomic release-store synchronises-with acquire-load).

Without a happens-before relationship, concurrent conflicting accesses are a data race → UB.

```cpp
// Correct use of memory_order for a flag
std::atomic<bool> g_ready{false};
int g_data = 0; // protected by the happens-before chain

void producer() {
    g_data = 42;                                          // (A) write data
    g_ready.store(true, std::memory_order_release);       // (B) release fence
    // All writes before (B) are visible to threads that see (B)'s effect
}

void consumer() {
    while (!g_ready.load(std::memory_order_acquire)) {}   // (C) acquire fence
    // (C) synchronises-with (B) → (A) happens-before (C)
    assert(g_data == 42); // guaranteed to see 42
}
```

---

## 11. `std::atomic<bool>` for `m_running`: Why It's Sufficient

### 11.1 The Pattern

```cpp
class Server {
    std::atomic<bool> m_running{false}; // single writer (main thread), multiple readers
    // ...
};

// Main/control thread — single writer
void Server::start() {
    m_running.store(true, std::memory_order_release);
    m_receiveThread = std::thread(&Server::receiveLoop, this);
}

void Server::stop() {
    m_running.store(false, std::memory_order_release); // signal all readers
    m_queue.push(POISON_PILL);   // unblock any waiting pop()
    if (m_receiveThread.joinable())
        m_receiveThread.join();
}

// Worker thread — multiple readers
void Server::receiveLoop() {
    while (m_running.load(std::memory_order_acquire)) { // acquire: see all prior writes
        // process messages
    }
}
```

### 11.2 Why `atomic<bool>` Is Sufficient Here

| Property | Why It Holds |
|----------|-------------|
| Single writer | Only one thread calls `stop()` — no write-write race |
| Multiple readers | `load()` is always safe with multiple concurrent readers |
| Visibility | `memory_order_release`/`acquire` pair ensures happens-before |
| No tearing | `bool` is typically 1 byte; x86 guarantees aligned byte writes are atomic anyway — but `std::atomic` makes it *portable and guaranteed* |
| No ABA problem | Boolean has only two states; ABA is irrelevant |

### 11.3 Why a Plain `bool` Would Be Wrong

```cpp
bool m_running = false; // plain bool — data race!

// Thread A writes: m_running = false;
// Thread B reads:  while (m_running) { ... }
// These are conflicting accesses with no synchronisation → data race → UB
// Sanitiser output: "data race on m_running at server.cpp:42"
```

### 11.4 `memory_order_relaxed` vs. `acquire`/`release`

```cpp
// relaxed: no ordering guarantees relative to other operations
// Use only when you don't need happens-before (e.g., counters)
m_running.store(false, std::memory_order_relaxed); // NOT safe for flags

// seq_cst (default): full sequential consistency — safest, slowest
m_running.store(false); // equivalent to memory_order_seq_cst

// release/acquire pair: efficient and correct for producer-consumer flags
m_running.store(false, std::memory_order_release); // writer
m_running.load(std::memory_order_acquire);          // reader — RECOMMENDED
```

---

## 12. Practical Exercises to Verify the Fixes

### Exercise 1: Reproduce the Race with Thread Sanitiser

```bash
# Compile with ThreadSanitiser (Clang or GCC)
clang++ -std=c++20 -fsanitize=thread -g -O1 server.cpp -o server_tsan
./server_tsan

# Expected TSan output for the broken version:
# WARNING: ThreadSanitizer: data race
#   Write of size 8 at 0x... by thread T2 (removeClient):
#     #0 std::unique_ptr<Connection>::~unique_ptr() ...
#   Read of size 8 at 0x... by thread T1 (receiveLoop):
#     #0 Connection::send() ...
```

### Exercise 2: Verify Fix with Valgrind Helgrind

```bash
g++ -std=c++20 -g -O0 server.cpp -lpthread -o server_helgrind
valgrind --tool=helgrind ./server_helgrind

# After fix: "ERROR SUMMARY: 0 errors from 0 contexts"
```

### Exercise 3: Stress Test — Connect/Disconnect Flood

```cpp
// stress_test.cpp — rapidly connect and disconnect clients
// while the server broadcasts, verifying no crash/corruption
#include <thread>
#include <vector>

void stressTest(Server& server) {
    constexpr int N = 1000;
    std::vector<std::thread> threads;

    // Writer threads: add and remove clients rapidly
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&server, i, N]() {
            for (int j = 0; j < N; ++j) {
                int id = i * N + j;
                server.addClient(id, makeConnection(id));
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                server.removeClient(id);
            }
        });
    }

    // Reader threads: broadcast continuously
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&server]() {
            for (int j = 0; j < 5000; ++j)
                server.broadcast({"test_message"});
        });
    }

    for (auto& t : threads) t.join();
    // If no crash and TSan reports no races → fix is correct
}
```

### Exercise 4: Benchmark Lock Hold Time

```cpp
#include <chrono>

void benchmarkLockHoldTime(Server& server) {
    using Clock = std::chrono::high_resolution_clock;
    auto start = Clock::now();
    constexpr int ITERS = 100'000;

    for (int i = 0; i < ITERS; ++i) {
        // Measure: snapshot pattern
        auto clients = server.snapshotClients(); // returns vector<shared_ptr>
    }

    auto elapsed = Clock::now() - start;
    double nsPerOp = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
                     / static_cast<double>(ITERS);
    printf("Snapshot lock hold: %.1f ns/op\n", nsPerOp);
    // Typical result: 50–200 ns vs 1000–100000 ns when sending under lock
}
```

### Exercise 5: Confirm `atomic<bool>` Ordering

```cpp
// Minimal test: ensure worker sees updated data when m_running becomes false
#include <atomic>
#include <cassert>
#include <thread>

std::atomic<bool> ready{false};
int shared_data = 0;

void producer() {
    shared_data = 99;                                   // write data
    ready.store(true, std::memory_order_release);       // signal
}

void consumer() {
    while (!ready.load(std::memory_order_acquire)) {}   // spin
    assert(shared_data == 99);                          // must hold
    printf("Consumer sees: %d\n", shared_data);
}

int main() {
    std::thread t1(producer), t2(consumer);
    t1.join(); t2.join();
    // Run under TSan — must report zero races
}
```

---

## Key Takeaways

- **A data race is always undefined behaviour** — no "mostly works" — even if it passes testing, the next compiler version or CPU model may break it.
- **Mutexes protect invariants, not objects.** Ask: "What logical property must remain true?" — that's what the mutex scope must enforce.
- **Raw pointers escape lock scope** — the moment you extract a raw pointer and release the lock, you have a dangling-pointer time bomb.
- **`shared_ptr` is a lifetime anchor** — its ref-count is atomic; as long as any thread holds a copy, the object lives.
- **`unique_ptr` is for exclusive ownership** — do not share raw `get()` pointers across threads.
- **Unique resource ≠ unique lifetime.** A socket is a unique OS resource (correctly modelled by a move-only `Socket` class), but the C++ *object* wrapping it may need shared lifetime across threads — use `shared_ptr` for the wrapper, `= delete` copy semantics for the resource.
- **`shared_ptr` does not mean "semantically shared ownership"** — it can be used purely as a lifetime-extension mechanism when a mutex boundary separates access from ownership.
- **`unique_ptr` + `shared_mutex` is a valid alternative** to `shared_ptr` for concurrent access, but forces `stop()` to block until all in-flight I/O completes — a potential stall under slow/adversarial peers.
- **Close-signals-error is the preferred shutdown pattern** — close the fd immediately (unblocking all in-flight syscalls), let threads observe the error and self-exit, then let ref-counts clean up the C++ object.
- **Never hold a mutex during blocking I/O** — use the snapshot pattern to copy refs under lock, then do work outside.
- **`volatile` is not thread-safe** — it prevents compiler caching but emits no memory fences.
- **`std::atomic<bool>` with `release`/`acquire`** is sufficient for single-writer, multiple-reader flags; it's lighter than a full mutex.
- **ThreadSanitiser (`-fsanitize=thread`) is your best friend** — run it in CI on every concurrent code change.
- **Deadlock requires consistent lock ordering** — use `std::scoped_lock` (C++17) when acquiring multiple mutexes simultaneously.
- **The snapshot pattern** (copy shared handles under lock, do work outside) is the gold standard for iterating shared collections in concurrent code.

---

## Further Reading

| Resource | Focus |
|----------|-------|
| **C++ Concurrency in Action** — Anthony Williams (2nd ed.) | Comprehensive mutex, atomic, memory model |
| **cppreference.com/atomic** | `std::atomic` interface, memory orders |
| **"Herb Sutter: atomic<> Weapons"** (CppCon 2012, YouTube) | Memory model deep dive, x86 vs. ARM |
| **N4860 — C++20 Standard §6.9.2** | Formal memory model, happens-before definition |
| **Google SRE Book — Chapter 9** | Lock contention, latency tails in production |
| **`man 7 pthreads`** | POSIX thread semantics underlying `std::thread` |
| **Valgrind Helgrind manual** | Race detection in multi-threaded programs |
| **"Is Parallel Programming Hard?" — Paul McKenney** | RCU, memory barriers, Linux kernel patterns |
| **CppCon 2017: Fedor Pikus "Read, Lock-Free, or Atomic?"** | Choosing between mutex, atomic, lock-free |
| **LLVM ThreadSanitizer documentation** | TSan internals, suppression files, CI integration |

---

*Document maintained by the Real-Time Messaging System project. Part of Phase 0: Foundations.*  
*Next: `03_condition_variables_and_blocking_queues.md`*
