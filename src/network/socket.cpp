#include "network/socket.hpp"
#include <iostream>

// --- Hidden Global Initializer for Windows ---
#ifdef _WIN32
class WinsockContext
{
  public:
    WinsockContext()
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        {
            std::cerr << "Winsock init failed!" << std::endl;
        }
    }
    ~WinsockContext()
    {
        WSACleanup();
    }
};
static WinsockContext g_winsock_context; // Automatically initializes Winsock
#endif

// --- Platform Specific Macros ---
#ifdef _WIN32
#define CLOSE_FUNC(s) closesocket(s)
#else
// uses global ::close from unistd.h to avoid potential name clashes with Socket::close()
// :: tells the compiler to use the global namespace version of close, which is the system
// call for closing file descriptors/sockets on Unix-like systems.
#define CLOSE_FUNC(s) ::close(s)
#endif

Socket::Socket() : m_handle(INVALID_SOCKET) {}

Socket::Socket(SocketHandle handle) : m_handle(handle) {}

Socket::~Socket()
{
    close();
}

// Move Constructor
Socket::Socket(Socket&& other) noexcept : m_handle(other.m_handle)
{
    other.m_handle = INVALID_SOCKET;
}

// Move Assignment
Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other)
    {
        close();
        m_handle = other.m_handle;
        other.m_handle = INVALID_SOCKET;
    }
    return *this;
}

bool Socket::create()
{
    close();
    m_handle = socket(AF_INET, SOCK_STREAM, 0);
    return is_valid();
}

bool Socket::connect(const std::string& ip, int port)
{
    if (!is_valid() && !create())
        return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // inet_pton works on Windows Vista+ and all modern Linux
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0)
        return false;

    return ::connect(m_handle, (struct sockaddr*)&addr, sizeof(addr)) == 0;
}

bool Socket::bind(int port)
{
    if (!is_valid() && !create())
        return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    // Solve "Address already in use" errors on restart
    int opt = 1;
    setsockopt(m_handle, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    return ::bind(m_handle, (struct sockaddr*)&addr, sizeof(addr)) == 0;
}

bool Socket::listen()
{
    return ::listen(m_handle, SOMAXCONN) == 0;
}

SocketHandle Socket::accept()
{
    return ::accept(m_handle, nullptr, nullptr);
}

void Socket::close()
{
    if (is_valid())
    {
        CLOSE_FUNC(m_handle);
        m_handle = INVALID_SOCKET;
    }
}

int Socket::send(const std::vector<uint8_t>& data)
{
    // Cast to char* is required for Windows, works for Linux void*
    LOG_DEBUG("Sending " << data.size() << " bytes on handle: " << m_handle);
    return ::send(m_handle, reinterpret_cast<const char*>(data.data()), (int)data.size(),
                  0);
}

bool Socket::send_all(const std::vector<uint8_t>& data)
{
    // FIX (Bug 5): The kernel send buffer may be full, causing send() to
    // return fewer bytes than requested. We retry until all bytes are sent
    // or a genuine error/disconnect occurs.
    //
    // Why this happens:
    //   The kernel has a finite send buffer (SO_SNDBUF, default ~128KB on Linux).
    //   When it's full, send() returns the number of bytes actually queued
    //   (which may be less than data.size()). Ignoring this means silently
    //   dropping the rest of the message with no error reported.
    size_t total_sent = 0;
    while (total_sent < data.size())
    {
        int bytes = ::send(m_handle,
                           reinterpret_cast<const char*>(data.data() + total_sent),
                           static_cast<int>(data.size() - total_sent),
                           0);
        if (bytes <= 0)
            return false; // Genuine error or peer disconnected
        total_sent += static_cast<size_t>(bytes);
    }
    LOG_DEBUG("send_all: " << total_sent << " bytes sent on handle: " << m_handle);
    return true;
}

int Socket::receive(std::vector<uint8_t>& buffer, size_t size)
{
    buffer.assign(size, 0);
    int bytes = ::recv(m_handle, reinterpret_cast<char*>(buffer.data()), (int)size, 0);

    if (bytes > 0)
    {
        buffer.resize(bytes);
    }
    else
    {
        buffer.clear();
    }
    return bytes;
}

bool Socket::recv_exact(std::vector<uint8_t>& buffer, size_t size)
{
    // FIX (Bug 2 — partial reads):
    // TCP is a byte stream. A single recv() call may return anywhere from
    // 1 byte up to `size` bytes. recv_exact() loops until the buffer is
    // completely filled, making partial reads invisible to the caller.
    //
    // This is the building block for FramedSocket::recv_message(), which
    // calls recv_exact(HEADER_SIZE) and then recv_exact(body_size) to
    // atomically reassemble a complete message regardless of how TCP
    // fragments it.
    buffer.resize(size);
    size_t total_received = 0;
    while (total_received < size)
    {
        int bytes = ::recv(m_handle,
                           reinterpret_cast<char*>(buffer.data() + total_received),
                           static_cast<int>(size - total_received),
                           0);
        if (bytes <= 0)
        {
            buffer.clear();
            return false; // Connection closed or error
        }
        total_received += static_cast<size_t>(bytes);
    }
    return true;
}
