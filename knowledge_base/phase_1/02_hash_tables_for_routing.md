# Hash Tables for Topic Routing

> **TL;DR:** In a high-throughput broker, finding which clients are subscribed to a topic is
> the most critical path. By using `std::unordered_map` (a hash table) instead of `std::map`
> (a binary tree), we achieve O(1) routing lookups, drastically reducing latency as the number
> of topics scales.

---

## 1. std::unordered_map vs std::map

C++ provides two associative containers. They have the same interface but fundamentally
different underlying data structures.

### `std::map` (Red-Black Tree)
- Maintains elements in **sorted order**.
- Lookup time is **O(log N)**.
- Memory layout: Scattered nodes connected by pointers.

### `std::unordered_map` (Hash Table)
- Elements are **unsorted**.
- Lookup time is **O(1)** average case.
- Memory layout: An array of "buckets" containing linked lists of elements.

In our broker, we do not care about alphabetical sorting of topic names. We only care about
answering the question: *"Who is subscribed to `BTC_USD`?"* as fast as possible.

---

## 2. The Mathematics of O(1) vs O(log N)

If a broker has 10,000 active topics, and receives 100,000 messages per second:

- **Using `std::map`:** `log2(10,000)` ≈ 13 comparisons per message.
  100,000 messages × 13 comparisons = 1.3 million string comparisons per second.

- **Using `std::unordered_map`:** 1 hash computation per message.
  100,000 messages × 1 hash = 100,000 hashes per second.

The string hashing algorithm (like MurmurHash or CityHash used by modern standard libraries)
is heavily optimized and vectorizable by the CPU. It is vastly faster than chasing pointers
down a binary tree.

---

## 3. Cache Locality Differences

Modern CPUs are incredibly fast, but RAM is slow. The CPU relies on the L1/L2/L3 cache.

- A tree traversal (`std::map`) chases pointers to random memory locations. Each hop likely
  causes a **cache miss**, stalling the CPU for ~100 nanoseconds while it fetches from RAM.
- A hash table (`std::unordered_map`) computes an array index, which is a single memory
  fetch.

While `std::unordered_map` in C++ still uses linked lists for bucket chaining (which also
ruins cache locality), the chain length is strictly managed by the container's load factor,
so you usually only endure 1 or 2 pointer hops, compared to 13 in the tree.

*(Note: In ultra-low-latency C++ (like HFT), even `std::unordered_map` is too slow due to
bucket chaining. Systems will use open-addressing flat hash maps like `absl::flat_hash_map`
or `folly::F14` which keep all data contiguous in memory.)*

---

## 4. Load Factors, Buckets, and Rehashing

A hash table consists of an array of **buckets**. When you insert an item, it hashes the key,
takes the modulo of the bucket count, and puts the item in that bucket.

```cpp
// How it works under the hood
size_t bucket_index = std::hash<std::string>{}("BTC_USD") % bucket_count;
```

**Load Factor** = (Number of Elements) / (Number of Buckets).

By default, when the load factor exceeds `1.0`, C++ will **rehash**:
1. Allocate a new, larger array of buckets (usually 2x the size).
2. Re-calculate the hash and new bucket index for *every single item*.
3. Deallocate the old array.

**Danger:** Rehashing is an O(N) operation. If a rehash triggers during a latency-critical
trading window, your broker will suddenly stall for milliseconds.

**The Fix:** If you know how many topics you expect, pre-allocate:
```cpp
m_subscriptions.reserve(10000); // Prevents rehashing until 10k topics are reached
```

---

## 5. Our Routing Implementation

```cpp
std::unordered_map<std::string, std::vector<SocketHandle>> m_subscriptions;
```

For every topic string, we store a contiguous `std::vector` of socket handles.

When a message arrives:
```cpp
// O(1) lookup
auto it = m_subscriptions.find(msg.topic);
if (it != m_subscriptions.end()) {
    // Cache-friendly vector iteration
    for (SocketHandle sub : it->second) {
        send_to_client(sub);
    }
}
```

Because `std::vector` stores elements contiguously in memory, the CPU prefetcher will load
the entire list of socket handles into the L1 cache automatically, making the fan-out loop
blisteringly fast.

---

## 6. Thread Safety Considerations

`std::unordered_map` is **not** thread-safe for concurrent read/write.

In our broker:
- Thread A (Client 1) might send a `SUBSCRIBE` (modifies the map).
- Thread B (Client 2) might send a `PUBLISH` (reads the map).

If a `SUBSCRIBE` triggers a rehash while a `PUBLISH` is reading the map, the application
will segfault (Undefined Behavior).

**Our solution:** The `m_routingMutex` protects all access to `m_subscriptions`.

```cpp
// In handleMessage():
std::lock_guard<std::mutex> route_lock(m_routingMutex);
auto it = m_subscriptions.find(msg.topic);
// ... copy subscribers, then release lock
```

We only hold the lock for the O(1) lookup and the `std::vector` copy. We do **not** hold the
lock while doing I/O.

---

## Key Takeaways

- Use `std::unordered_map` for O(1) lookups in the hot path.
- Be aware of rehashing latency spikes; use `reserve()` if possible.
- Use `std::vector` for the value type to maximize CPU cache locality during iteration.
- Protect the map with a mutex, but never hold that mutex during blocking network I/O.

---

## Further Reading

- [cppreference: std::unordered_map](https://en.cppreference.com/w/cpp/container/unordered_map)
- [Matt Kulukundis - Designing a Fast, Efficient, Cache-friendly Hash Table (CppCon 2017)](https://www.youtube.com/watch?v=ncHmEUmJZf4)
