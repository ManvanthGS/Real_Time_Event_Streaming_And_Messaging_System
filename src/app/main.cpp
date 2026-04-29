#include "network/messaging_node.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

void printHelp()
{
    std::cout << "\n--- Messaging Node Commands ---" << std::endl;
    std::cout << "  connect <ip> <port> : Connect to a new peer" << std::endl;
    std::cout << "  send <message>      : Broadcast message to all peers" << std::endl;
    std::cout << "  help                : Show this menu" << std::endl;
    std::cout << "  quit                : Exit application" << std::endl;
    std::cout << "-------------------------------\n" << std::endl;
}

int main()
{
    MessagingNode node;
    int myPort;

    std::cout << "=== Real-Time Messaging Node ===" << std::endl;
    std::cout << "Enter local port to listen on: ";
    if (!(std::cin >> myPort))
    {
        std::cerr << "Invalid port input." << std::endl;
        return 1;
    }

    // Start the internal listener and acceptance loop
    node.start(myPort);
    std::cout << "[System] Node started on port " << myPort << std::endl;

    printHelp();
    std::cin.ignore(); // Clear newline from previous cin

    std::string line;
    while (true)
    {
        std::cout << "> ";
        if (!std::getline(std::cin, line) || line == "quit")
        {
            break;
        }

        std::stringstream ss(line);
        std::string command;
        ss >> command;

        LOG_DEBUG("Received command: " << command);

        if (command == "connect")
        {
            std::string ip;
            int port;
            if (ss >> ip >> port)
            {
                node.connectToPeer(ip, port);
            }
            else
            {
                std::cout << "[Usage] connect <ip> <port>" << std::endl;
            }
        }
        else if (command == "send")
        {
            std::string message;
            std::getline(ss, message);
            LOG_DEBUG("Message to broadcast: " << message);
            if (!message.empty())
            {
                // Remove leading space from getline
                if (message[0] == ' ')
                    message.erase(0, 1);
                node.broadcast(message);
            }
            else
            {
                std::cout << "[Usage] send <message>" << std::endl;
            }
            LOG_DEBUG("Message broadcasted: " << message);
        }
        else if (command == "help")
        {
            printHelp();
        }
        else if (!command.empty())
        {
            std::cout << "[Unknown Command] Type 'help' for options." << std::endl;
        }
    }

    std::cout << "[System] Shutting down node..." << std::endl;
    node.stop();

    return 0;
}
