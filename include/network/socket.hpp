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

    // Low-level send — may return fewer bytes than requested (partial write).
    // Prefer send_all() for correctness.
    int send(const std::vector<uint8_t>& data);

    // Guaranteed full write: loops until all bytes in `data` are sent or an
    // error/disconnect occurs. Returns false on failure.
    // FIX (Bug 5): The original send() could silently truncate messages when
    // the kernel send buffer was full. send_all() retries until done.
    bool send_all(const std::vector<uint8_t>& data);

    // Low-level receive — may return fewer bytes than `size` (partial read).
    // Prefer recv_exact() when you need a precise number of bytes.
    int receive(std::vector<uint8_t>& buffer, size_t size);

    // Guaranteed full read: loops until exactly `size` bytes are received or
    // a disconnect/error occurs. Returns false on failure, clears buffer.
    // Used by FramedSocket to read headers and bodies without partial-read bugs.
    bool recv_exact(std::vector<uint8_t>& buffer, size_t size);

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
