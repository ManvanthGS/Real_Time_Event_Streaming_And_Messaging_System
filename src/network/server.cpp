#include "network/socket_utils.hpp"
#include <cstring>
#include <iostream>

int main()
{
    InitSockets();

    SocketType server_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(8080);
    address.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (sockaddr*)&address, sizeof(address));
    listen(server_fd, 5);

    std::cout << "Server listening on port 8080\n";

    SocketType client_fd = accept(server_fd, nullptr, nullptr);

    char buffer[1024] = {0};
    recv(client_fd, buffer, 1024, 0);

    std::cout << "Received: " << buffer << std::endl;

    const char* reply = "Message received";
    send(client_fd, reply, strlen(reply), 0);

    CloseSocket(client_fd);
    CloseSocket(server_fd);

    CleanupSockets();
    return 0;
}
