#pragma once
#include "socket.hpp"
#include <atomic>
#include <map>
#include <memory>
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

    // CHANGED: unique_ptr → shared_ptr
    // receiveLoop() and broadcast() need to read sockets safely *outside*
    // the mutex. With unique_ptr this is a data race: another thread could
    // call m_peers.erase() (destroying the socket) at the same moment.
    // shared_ptr solves this: while holding the mutex, threads take a copy
    // (bumping the ref-count), then release the mutex. The Socket object
    // stays alive until the last shared_ptr copy is destroyed — even if the
    // handle has been erased from the map.
    // See: knowledge_base/phase_0/02_data_races_and_mutex_scope.md
    std::map<SocketHandle, std::shared_ptr<Socket>> m_peers;
    std::mutex m_peersMutex;
};
