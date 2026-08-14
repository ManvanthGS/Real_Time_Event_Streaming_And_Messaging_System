#include "broker/broker.hpp"
#include <iostream>
#include <string>

int main(int argc, char* argv[])
{
    int port = 9000;
    if (argc > 1)
    {
        try
        {
            port = std::stoi(argv[1]);
        }
        catch (...)
        {
            std::cerr << "Invalid port argument\n";
            return 1;
        }
    }

    std::cout << "=== Real-Time Event Broker ===\n";
    
    Broker broker;
    broker.start(port);

    std::cout << "Type 'quit' to shut down the broker.\n> ";
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line == "quit")
            break;
    }

    std::cout << "Shutting down broker...\n";
    broker.stop();

    return 0;
}
