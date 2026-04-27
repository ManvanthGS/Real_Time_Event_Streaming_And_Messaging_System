#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

using SocketType = SOCKET;

inline void InitSockets()
{
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

inline void CleanupSockets()
{
    WSACleanup();
}

inline void CloseSocket(SocketType s)
{
    closesocket(s);
}

#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

using SocketType = int;

inline void InitSockets() {}
inline void CleanupSockets() {}

inline void CloseSocket(SocketType s)
{
    close(s);
}
#endif
