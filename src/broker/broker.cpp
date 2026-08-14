#include "broker/broker.hpp"
#include <algorithm>
#include <iostream>

void Broker::start(int port)
{
    m_running = true;
    std::thread(&Broker::acceptLoop, this, port).detach();
}

void Broker::acceptLoop(int port)
{
    Socket listener;
    if (!listener.create() || !listener.bind(port) || !listener.listen())
    {
        std::cerr << "[Broker] Failed to start listener on port " << port << std::endl;
        return;
    }

    std::cout << "[Broker] Listening on port " << port << "..." << std::endl;

    while (m_running)
    {
        SocketHandle raw_handle = listener.accept();
        if (raw_handle != INVALID_SOCKET)
        {
            {
                std::lock_guard<std::mutex> lock(m_peersMutex);
                m_peers[raw_handle] = std::make_shared<Socket>(raw_handle);
            }
            std::thread(&Broker::clientLoop, this, raw_handle).detach();
            LOG_DEBUG("New client connected: " << raw_handle);
        }
    }
}

void Broker::clientLoop(SocketHandle handle)
{
    LOG_DEBUG("Started client loop for: " << handle);

    while (m_running)
    {
        // 1. Safely acquire the socket shared_ptr
        std::shared_ptr<Socket> sock;
        {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            auto it = m_peers.find(handle);
            if (it == m_peers.end())
                break;
            sock = it->second;
        }

        // 2. Read framed message (blocks until full frame arrives)
        ParsedMessage msg;
        FramedSocket framed(*sock);
        if (!framed.recv_message(msg))
        {
            LOG_DEBUG("Client disconnected or framing error: " << handle);
            removeClient(handle);
            break;
        }

        // 3. Route the message
        handleMessage(handle, msg);
    }
}

void Broker::handleMessage(SocketHandle sender_handle, const ParsedMessage& msg)
{
    if (msg.type == MessageType::SUBSCRIBE)
    {
        std::lock_guard<std::mutex> lock(m_routingMutex);
        auto& subs = m_subscriptions[msg.topic];
        
        // Add only if not already subscribed
        if (std::find(subs.begin(), subs.end(), sender_handle) == subs.end())
        {
            subs.push_back(sender_handle);
            std::cout << "[Broker] Client " << sender_handle 
                      << " subscribed to [" << msg.topic << "]" << std::endl;
        }
    }
    else if (msg.type == MessageType::PUBLISH)
    {
        LOG_DEBUG("Received PUBLISH on topic [" << msg.topic << "] from " << sender_handle);

        // Snapshot subscribers to avoid holding lock during I/O
        std::vector<std::shared_ptr<Socket>> targets;
        {
            std::lock_guard<std::mutex> route_lock(m_routingMutex);
            auto it = m_subscriptions.find(msg.topic);
            if (it != m_subscriptions.end())
            {
                std::lock_guard<std::mutex> peers_lock(m_peersMutex);
                for (SocketHandle sub_handle : it->second)
                {
                    auto peer_it = m_peers.find(sub_handle);
                    if (peer_it != m_peers.end())
                    {
                        targets.push_back(peer_it->second);
                    }
                }
            }
        } // Mutexes released

        // Forward to all subscribers as a DATA message
        for (auto& sock : targets)
        {
            if (sock && sock->is_valid())
            {
                FramedSocket framed(*sock);
                framed.send_message(MessageType::DATA, msg.topic, msg.payload);
            }
        }
    }
}

void Broker::removeClient(SocketHandle handle)
{
    // Remove from active peers
    {
        std::lock_guard<std::mutex> lock(m_peersMutex);
        m_peers.erase(handle);
    }

    // Remove from all topics
    {
        std::lock_guard<std::mutex> lock(m_routingMutex);
        for (auto& [topic, subs] : m_subscriptions)
        {
            subs.erase(std::remove(subs.begin(), subs.end(), handle), subs.end());
        }
    }
    std::cout << "[Broker] Cleaned up client " << handle << std::endl;
}

void Broker::stop()
{
    m_running = false;
    std::lock_guard<std::mutex> lock(m_peersMutex);
    for (auto const& [handle, socket] : m_peers)
    {
        socket->close();
    }
    m_peers.clear();
}
