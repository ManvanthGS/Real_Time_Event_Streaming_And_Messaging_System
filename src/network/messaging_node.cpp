#include "network/messaging_node.hpp"
#include "protocol/framing.hpp"
#include <iostream>

void MessagingNode::start(int port)
{
    m_running = true;
    // NOTE (Phase 0 known issue \u2014 Thread-per-connection):
    // Each peer spawns a detached OS thread. This approach is correct and
    // simple but does not scale past a few hundred connections because:
    //   1. Each thread consumes ~8MB of stack by default
    //   2. The OS scheduler incurs context-switch overhead for every thread
    //   3. Detached threads cannot be gracefully joined on shutdown
    // This will be replaced in Phase 2 with an epoll/IOCP event loop.
    // See: knowledge_base/phase_0/05_thread_per_connection_problem.md
    std::thread(&MessagingNode::acceptLoop, this, port).detach();
}

void MessagingNode::acceptLoop(int port)
{
    Socket listener;
    if (!listener.create() || !listener.bind(port) || !listener.listen())
    {
        std::cerr << "Failed to start listener on port " << port << std::endl;
        return;
    }

    while (m_running)
    {
        SocketHandle raw_handle = listener.accept();
        if (raw_handle != INVALID_SOCKET)
        {
            {
                std::lock_guard<std::mutex> lock(m_peersMutex);
                // make_shared: socket object is now reference-counted so that
                // receiveLoop and broadcast can share ownership safely.
                m_peers[raw_handle] = std::make_shared<Socket>(raw_handle);
            }
            std::thread(&MessagingNode::receiveLoop, this, raw_handle).detach();
            LOG_DEBUG("New peer connected: " << raw_handle);
        }
    }
}

void MessagingNode::receiveLoop(SocketHandle handle)
{
    LOG_DEBUG("Started receive loop with handle: " << handle);

    while (m_running)
    {
        // ------------------------------------------------------------------
        // FIX (Bug 1 \u2014 Data Race):
        //
        // BEFORE (broken):
        //   { lock; check m_peers[handle] exists; } // lock released here
        //   m_peers[handle]->receive(...)           // RACE: handle may be
        //                                           // erased by another thread
        //
        // AFTER (fixed):
        //   Take a shared_ptr copy while holding the lock. The copy bumps
        //   the reference count, keeping the Socket object alive even if
        //   another thread calls stop() and erases the entry from m_peers.
        //   Then release the lock before the blocking I/O call.
        //
        // Key insight: locking only protects the *map* (m_peers). The Socket
        // object itself is protected by shared ownership (ref-count > 0).
        // See: knowledge_base/phase_0/02_data_races_and_mutex_scope.md
        // ------------------------------------------------------------------
        std::shared_ptr<Socket> sock;
        {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            auto it = m_peers.find(handle);
            if (it == m_peers.end())
                break; // Already removed (e.g., by stop())
            sock = it->second; // Bump ref-count: socket stays alive after unlock
        }
        // Mutex released here. `sock` keeps the Socket object alive.

        // FIX (Bug 2 \u2014 TCP Framing):
        // Use FramedSocket which calls recv_exact() internally to accumulate
        // a complete header + body before returning. This correctly handles
        // TCP partial reads that would otherwise split a message mid-frame.
        ParsedMessage msg;
        FramedSocket framed(*sock);
        if (!framed.recv_message(msg))
        {
            LOG_DEBUG("Peer disconnected or framing error on handle: " << handle);
            std::lock_guard<std::mutex> lock(m_peersMutex);
            m_peers.erase(handle);
            break;
        }

        std::string text(msg.payload.begin(), msg.payload.end());
        LOG_DEBUG("Received [topic=" << msg.topic << "] \"" << text
                                     << "\" from peer " << handle);
    }

    LOG_DEBUG("Receive loop exited for handle: " << handle);
}

void MessagingNode::broadcast(const std::string& text)
{
    LOG_DEBUG("Broadcasting: " << text);

    // ------------------------------------------------------------------
    // FIX (Bug 3 \u2014 Broadcast holds global lock during I/O):
    //
    // BEFORE (broken):
    //   lock(m_peersMutex);
    //   for each peer: socket->send(data);   // blocking I/O under lock!
    //   unlock();                             // lock held for ALL sends
    //
    // AFTER (fixed):
    //   1. Hold lock only long enough to snapshot the shared_ptrs.
    //   2. Release lock before any I/O.
    //
    // Why shared_ptr copies here?
    //   Without bumping ref-counts, a peer could disconnect between
    //   snapshotting and sending, causing stop() to destroy the Socket
    //   while we hold a raw pointer to it \u2014 use-after-free.
    // See: knowledge_base/phase_0/02_data_races_and_mutex_scope.md
    // ------------------------------------------------------------------
    std::vector<std::shared_ptr<Socket>> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_peersMutex);
        snapshot.reserve(m_peers.size());
        for (auto const& [handle, socket] : m_peers)
            snapshot.push_back(socket); // Bump ref-count for each
    }
    // Mutex fully released. All Socket objects are kept alive by snapshot.

    for (auto& sock : snapshot)
    {
        if (sock && sock->is_valid())
        {
            // FIX (Bug 2 + Bug 5): FramedSocket::send_message:
            //   \u2022 Wraps the text in a proper binary frame (length prefix + header)
            //   \u2022 Internally calls send_all() which retries on partial writes
            FramedSocket framed(*sock);
            framed.send_message(MessageType::DATA, "", text);
        }
    }
}

void MessagingNode::connectToPeer(const std::string& ip, int port)
{
    auto client = std::make_shared<Socket>();
    if (client->create() && client->connect(ip, port))
    {
        SocketHandle h = client->get_handle();
        {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            m_peers[h] = client;
        }
        std::thread(&MessagingNode::receiveLoop, this, h).detach();
        LOG_DEBUG("Connected to " << ip << ":" << port);
    }
    else
    {
        std::cerr << "[Error] Failed to connect to " << ip << ":" << port << std::endl;
    }
}

void MessagingNode::stop()
{
    m_running = false;
    std::lock_guard<std::mutex> lock(m_peersMutex);
    for (auto const& [handle, socket] : m_peers)
        socket->close();
    m_peers.clear();
    // Note: detached receiveLoop threads will exit on their next recv_message()
    // call which will fail because the socket is now closed. A proper shutdown
    // sequence (joining threads) will be implemented in Phase 2 with the event loop.
}
