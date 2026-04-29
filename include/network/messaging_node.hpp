#pragma once
#include "socket.hpp"
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

class MessagingNode
{
  public:
    MessagingNode() : m_running(false) {}
    ~MessagingNode()
    {
        stop();
    }

    void start(int port);
    void connectToPeer(const std::string& ip, int port);
    void broadcast(const std::string& text);
    void stop();

  private:
    void acceptLoop(int port);
    void receiveLoop(SocketHandle handle);

    std::atomic<bool> m_running;
    std::map<SocketHandle, std::unique_ptr<Socket>> m_peers;
    std::mutex m_peersMutex;
};
