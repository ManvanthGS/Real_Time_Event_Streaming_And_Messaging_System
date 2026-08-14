#pragma once
#include "network/socket.hpp"
#include "protocol/framing.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Broker — The central routing engine of the pub/sub system.
//
// Responsibilities:
// 1. Accept client connections.
// 2. Manage a registry of topics to subscribers (m_subscriptions).
// 3. Receive messages and route them:
//    - SUBSCRIBE: Add the client to the topic's subscriber list.
//    - PUBLISH: Find all subscribers for the topic and forward a DATA message.
//
// Threading Model (Phase 1):
// Still using thread-per-connection for now. The key change is the addition
// of m_routingMutex and m_subscriptions to safely route messages between
// independent client threads without race conditions.
// ---------------------------------------------------------------------------
class Broker
{
  public:
    Broker() : m_running(false) {}
    ~Broker()
    {
        stop();
    }

    // Starts the broker listening on the specified port.
    void start(int port);

    // Stops the broker and disconnects all clients.
    void stop();

  private:
    void acceptLoop(int port);
    void clientLoop(SocketHandle handle);

    // Core routing logic
    void handleMessage(SocketHandle sender_handle, const ParsedMessage& msg);
    
    // Helper to clean up when a client disconnects
    void removeClient(SocketHandle handle);

    std::atomic<bool> m_running;

    // Active connections
    std::mutex m_peersMutex;
    std::unordered_map<SocketHandle, std::shared_ptr<Socket>> m_peers;

    // Topic routing registry: Topic Name -> List of Subscriber SocketHandles
    //
    // WHY std::unordered_map?
    //   Topic lookups happen on the hot path for every PUBLISH message.
    //   std::map uses a red-black tree (O(log N) lookup).
    //   std::unordered_map uses a hash table (O(1) average lookup).
    //   At 10,000 topics, O(1) significantly reduces routing latency.
    //   See: knowledge_base/phase_1/02_hash_tables_for_routing.md
    std::mutex m_routingMutex;
    std::unordered_map<std::string, std::vector<SocketHandle>> m_subscriptions;
};
