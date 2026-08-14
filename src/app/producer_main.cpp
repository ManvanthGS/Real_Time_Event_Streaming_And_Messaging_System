#include "network/socket.hpp"
#include "protocol/framing.hpp"
#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    if (argc < 4)
    {
        std::cerr << "Usage: producer <ip> <port> <topic>\n";
        return 1;
    }

    std::string ip = argv[1];
    int port = std::stoi(argv[2]);
    std::string topic = argv[3];

    Socket sock;
    if (!sock.create() || !sock.connect(ip, port))
    {
        std::cerr << "Failed to connect to broker at " << ip << ":" << port << "\n";
        return 1;
    }

    FramedSocket framed(sock);
    std::cout << "Connected. Type messages to publish to [" << topic << "]. Type 'quit' to exit.\n";

    std::string line;
    while (true)
    {
        std::cout << "> ";
        if (!std::getline(std::cin, line) || line == "quit")
        {
            break;
        }

        if (line.empty()) continue;

        if (!framed.send_message(MessageType::PUBLISH, topic, line))
        {
            std::cerr << "Connection to broker lost.\n";
            break;
        }
    }

    return 0;
}
