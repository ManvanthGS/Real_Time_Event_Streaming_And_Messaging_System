#pragma once
#include "common.hpp"

class Socket
{
  public:
    Socket();                             // Creates a new raw socket
    explicit Socket(SocketHandle handle); // Wraps an existing handle (e.g., from accept)
    ~Socket();                            // RAII Destructor

    // Move only: sockets are unique resources
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool create();
    bool connect(const std::string& ip, int port);
    bool bind(int port);
    bool listen();
    SocketHandle accept(); // Returns the raw handle for the new connection

    int send(const std::vector<uint8_t>& data);
    int receive(std::vector<uint8_t>& buffer, size_t size);

    void close();
    bool is_valid() const
    {
        return IS_VALIDSOCKET(m_handle);
    }
    SocketHandle get_handle() const
    {
        return m_handle;
    }

  private:
    SocketHandle m_handle;
};
