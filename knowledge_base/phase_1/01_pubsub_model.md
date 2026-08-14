# The Publish-Subscribe (Pub/Sub) Model

> **TL;DR:** Pub/Sub is an architectural pattern that decouples the creators of data (producers)
> from the consumers of data. Instead of sending messages directly to a specific receiver,
> producers send messages to a **topic**, and a centralized **broker** fans out the message to
> all interested subscribers.

---

## 1. Producer, Consumer, and Broker Roles

### The Decoupling Benefit

In a Point-to-Point (P2P) system, if Node A wants to send data to Node B and Node C, it must:
1. Know the IP and Port of Node B and Node C.
2. Maintain TCP connections to both.
3. Serialize and transmit the message twice.

**Pub/Sub** introduces a middleman (the **Broker**):
1. **Producer:** Only knows about the Broker and the Topic (e.g., `BTC_USD`). Doesn't care who is listening.
2. **Consumer:** Only knows about the Broker and the Topic. Doesn't care where the data came from.
3. **Broker:** Maintains the network topology. Knows which consumers want which topics.

This decoupling allows systems to scale organically. You can add 50 new consumers to `BTC_USD` without changing a single line of code in the producer.

---

## 2. Topic Fan-Out Mechanics

"Fan-out" is the process of taking one incoming message and replicating it to N outbound connections.

```
Producer (Publishes 1 message)
     │
     ▼
   Broker (Topic: BTC_USD)
     ├──► Consumer 1 (TCP socket A)
     ├──► Consumer 2 (TCP socket B)
     └──► Consumer 3 (TCP socket C)
```

In our C++ implementation, this happens in `Broker::handleMessage`:

```cpp
// 1. Look up the topic in O(1) time
auto it = m_subscriptions.find(msg.topic);

// 2. Iterate over all subscribers for that topic
for (SocketHandle sub_handle : it->second) {
    // 3. Dispatch the exact same payload to each socket
    framed.send_message(MessageType::DATA, msg.topic, msg.payload);
}
```

The challenge with fan-out is **backpressure**. If Consumer 3 is on a slow network connection, its kernel TCP send buffer will fill up. If the Broker blocks on `send_all()` to Consumer 3, it starves Consumers 1 and 2. We will solve this in Phase 3 using asynchronous I/O and lock-free queues.

---

## 3. Delivery Semantics

Messaging systems are defined by their delivery guarantees during failures (crashes, network drops).

| Semantic | Meaning | Cost | Use Case |
|---|---|---|---|
| **At-most-once** | Fire and forget. Messages may be lost, but never duplicated. | Lowest | High-frequency trading (ticks), telemetry. |
| **At-least-once** | Guaranteed delivery, but receiver might get duplicates (retries). | Medium | Order processing (if idempotent). |
| **Exactly-once** | Flawless delivery. Mathematically impossible over TCP without two-phase commit or idempotency keys. | Highest | Financial ledgers, billing. |

### Our Target: At-most-once

Currently, if the Broker crashes, all messages in flight are lost. If a consumer disconnects, it misses messages published while it was offline.

This is intentional for our target domain. In real-time market data, **stale data is worse than no data**. If a consumer reconnects after 5 seconds, sending them 500,000 queued, obsolete price ticks will crash their strategy. They just want the *current* price.

---

## 4. Push vs. Pull Models

### The Push Model (Our Approach)
The broker actively writes data to the consumer's socket the millisecond it arrives.
- **Pros:** Lowest possible latency.
- **Cons:** A fast producer can overwhelm a slow consumer.
- **Examples:** Redis Pub/Sub, ZeroMQ, Our System.

### The Pull Model
The consumer asks the broker for data: `fetch(topic, offset)`.
- **Pros:** Consumer controls the pace. Easy to replay old messages.
- **Cons:** Higher latency (requires a request-response cycle or long-polling).
- **Examples:** Apache Kafka.

---

## 5. Pub/Sub vs. Point-to-Point Queues

Do not confuse Pub/Sub with a Work Queue (like RabbitMQ or Celery).

- **Pub/Sub (Topics):** 1 message is broadcast to ALL subscribers. (Broadcast pattern).
- **Work Queue:** 1 message is delivered to EXACTLY ONE worker. (Load balancing pattern).

Our system implements pure Pub/Sub.

---

## Key Takeaways

- The Broker's sole job is to maintain the topology and execute the fan-out loop efficiently.
- We target **at-most-once** delivery because latency matters more than reliability for real-time streams.
- We use a **push model** to minimize latency.

---

## Further Reading

- [Enterprise Integration Patterns: Publish-Subscribe Channel](https://www.enterpriseintegrationpatterns.com/patterns/messaging/PublishSubscribeChannel.html)
- [Kafka vs RabbitMQ Architecture](https://www.confluent.io/blog/kafka-vs-rabbitmq/)
