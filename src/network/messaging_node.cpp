#include "network/messaging_node.hpp"
#include <iostream>

void MessagingNode::start(int port)
{
    m_running = true;
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
            std::lock_guard<std::mutex> lock(m_peersMutex);
            m_peers[raw_handle] = std::make_unique<Socket>(raw_handle);
            std::thread(&MessagingNode::receiveLoop, this, raw_handle).detach();
            LOG_DEBUG("New peer connected: " << raw_handle);
        }
    }
}

void MessagingNode::receiveLoop(SocketHandle handle)
{
    std::vector<uint8_t> buffer;
    LOG_DEBUG("Started receive loop with handle: " << handle);
    while (m_running)
    {
        {
            std::lock_guard<std::mutex> lock(m_peersMutex);
            if (m_peers.find(handle) == m_peers.end())
                break;

            if (m_peers[handle]->receive(buffer, 1024) <= 0)
            {
                LOG_DEBUG("Peer disconnected: " << handle);
                m_peers.erase(handle);
                break;
            }
        }
        std::string msg(buffer.begin(), buffer.end());
        LOG_DEBUG("Received message from peer " << handle << ": " << msg);
    }
}

void MessagingNode::broadcast(const std::string& text)
{
    LOG_DEBUG("Broadcasting message: " << text);
    std::vector<uint8_t> data(text.begin(), text.end());
    LOG_DEBUG("Prepared data for broadcast, size: " << data.size() << " bytes");
    // std::lock_guard<std::mutex> lock(m_peersMutex);
    LOG_DEBUG("Broadcasting message to " << m_peers.size() << " peers");
    for (auto const& [handle, socket] : m_peers)
    {
        LOG_DEBUG("Sending to peer " << handle);
        socket->send(data);
    }
}

void MessagingNode::connectToPeer(const std::string& ip, int port)
{
    auto client = std::make_unique<Socket>();
    if (client->create() && client->connect(ip, port))
    {
        SocketHandle h = client->get_handle();
        std::lock_guard<std::mutex> lock(m_peersMutex);
        m_peers[h] = std::move(client);
        std::thread(&MessagingNode::receiveLoop, this, h).detach();
        LOG_DEBUG("Connected to " << ip << ":" << port);
    }
}

void MessagingNode::stop()
{
    m_running = false;
    // std::lock_guard<std::mutex> lock(m_peersMutex);
    for (auto const& [handle, socket] : m_peers)
    {
        socket->close();
    }
    m_peers.clear();
}
