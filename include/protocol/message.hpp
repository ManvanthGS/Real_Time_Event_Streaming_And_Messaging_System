#pragma once
#include <cstdint>

struct MessageHeader
{
    uint32_t length; // total message size
    uint16_t type;   // SUBSCRIBE / PUBLISH / DATA
    uint16_t topic_length;
};

enum class MessageType : uint16_t
{
    SUBSCRIBE = 1,
    PUBLISH = 2,
    DATA = 3
};
