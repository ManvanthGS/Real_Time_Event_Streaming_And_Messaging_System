#include "network/socket_utils.hpp"
#include <cstring>
#include <iostream>

int main()
{
    InitSockets();

    SocketType sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

    connect(sock, (sockaddr*)&server, sizeof(server));

    const char* msg = "Hello from client";
    send(sock, msg, strlen(msg), 0);

    char buffer[1024] = {0};
    recv(sock, buffer, 1024, 0);

    std::cout << "Server says: " << buffer << std::endl;

    CloseSocket(sock);
    CleanupSockets();
    return 0;
}
