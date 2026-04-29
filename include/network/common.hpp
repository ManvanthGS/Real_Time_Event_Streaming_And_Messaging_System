#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
#define IS_VALIDSOCKET(s) ((s) != INVALID_SOCKET)
#define CLOSE_SOCKET(s) closesocket(s)
#define GET_SOCKET_ERR() WSAGetLastError()
#else
#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
#define INVALID_SOCKET -1
#define IS_VALIDSOCKET(s) ((s) >= 0)
#define CLOSE_SOCKET(s) close(s)
#define GET_SOCKET_ERR() (errno)
#endif

#include <stdexcept>
#include <string>
#include <vector>

// Log messages only in debug mode, no-op otherwise
#ifndef NDEBUG
#define LOG_DEBUG(msg) std::cout << "[Debug] " << msg << std::endl << std::flush;
#else
#define LOG_DEBUG(msg)
#endif

class NetworkException : public std::runtime_error
{
  public:
    NetworkException(const std::string& msg)
        : std::runtime_error(msg + " (Error: " + std::to_string(GET_SOCKET_ERR()) + ")")
    {
    }
};
