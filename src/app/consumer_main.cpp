#include "network/socket.hpp"
#include "protocol/framing.hpp"
#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    if (argc < 4)
    {
        std::cerr << "Usage: consumer <ip> <port> <topic>\n";
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

    // Send SUBSCRIBE message
    if (!framed.send_message(MessageType::SUBSCRIBE, topic, ""))
    {
        std::cerr << "Failed to send subscribe request.\n";
        return 1;
    }

    std::cout << "Subscribed to [" << topic << "]. Waiting for messages...\n";

    ParsedMessage msg;
    while (framed.recv_message(msg))
    {
        if (msg.type == MessageType::DATA)
        {
            std::string payload(msg.payload.begin(), msg.payload.end());
            std::cout << "[" << msg.topic << "] " << payload << "\n";
        }
        else
        {
            std::cout << "[Warning] Received unexpected message type: " 
                      << static_cast<int>(msg.type) << "\n";
        }
    }

    std::cout << "Disconnected from broker.\n";
    return 0;
}
