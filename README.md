# Real-Time Event Streaming and Messaging System

## Overview
This is a C++ project for a real-time event streaming and messaging system. It's built from scratch with a focus on low-latency, like what you'd need for trading systems.

The system uses a TCP-based pub/sub model:
- Producers send messages to topics
- Consumers listen to topics
- A broker handles the routing

It's all about keeping things fast and predictable, not just high throughput.

## Why This Project?
Most messaging systems like Kafka are great for big data, but this one is tuned for speed and reliability in real-time scenarios. I wanted to understand the internals by building it myself.

## Features
- Custom binary protocol (no JSON bloat)
- TCP networking
- Pub/Sub with topic routing
- Supports multiple producers and consumers
- Single-threaded event loop for now
- Handles network issues like partial reads and disconnections
- Modular design for easy expansion

## Architecture
```
Producers -> TCP Layer -> Broker -> Subscribers
```

### Components
- **Network Layer**: TCP server/client, connection handling, buffers
- **Protocol Layer**: Binary message format, framing, serialization
- **Broker Core**: Topic management, subscriber lists, message routing

## Message Protocol
### Header
```cpp
struct Message_Header {
    uint32_t Length;
    uint16_t Type;
    uint16_t Topic_Length;
};
```

### Format
[Header][Topic][Payload]

### Types
- SUBSCRIBE: Join a topic
- PUBLISH: Send a message
- DATA: Message delivery

## Getting Started
### Build
```
mkdir build && cd build
cmake ..
make
```

---

### ▶️ Run

#### Start Broker
```
./broker
```

#### Start Consumer
```
./consumer BTC_USD
```

#### Send Message
```
./producer BTC_USD "price=101"
```

#### Output
```
[BTC_USD] price=101
```

---

## 🧪 Engineering Highlights

### 🔸 Message Framing
Handles TCP stream behavior correctly using buffering and parsing only complete messages.

### 🔸 Clean Architecture
Strict separation between:
- Networking
- Protocol
- Broker logic

### 🔸 Deterministic Flow
Single-threaded event loop ensures predictable behavior and simplifies debugging.

---

## 📈 Future Work

- epoll-based non-blocking I/O
- Lock-free queues (ring buffers)
- Message batching
- Persistent storage (WAL)
- Topic partitioning
- Consumer groups
- Latency benchmarking (p50, p99)

---

## 🎯 Use Cases

- Market data streaming
- Order routing systems
- Strategy event buses
- Real-time analytics pipelines
