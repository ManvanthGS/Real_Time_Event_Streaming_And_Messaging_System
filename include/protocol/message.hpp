#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

// Platform includes for htonl / ntohl (network byte order conversion)
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

// ---------------------------------------------------------------------------
// Wire format for every message on the network:
//
//   [ MessageHeader (8 bytes) ][ topic (topic_length bytes) ][ payload ]
//
// WHY 8 BYTES?
//   uint32_t (4) + uint16_t (2) + uint16_t (2) = 8 bytes.
//   No struct padding surprises because fields are naturally aligned and
//   we serialize field-by-field (not via raw memcpy of the struct).
//
// WHY NETWORK BYTE ORDER?
//   x86/x64 CPUs are little-endian (LSB first). Network convention is
//   big-endian (MSB first). Using htonl/ntohl on every multi-byte field
//   ensures producer and consumer agree on integer layout regardless of
//   the host platform.
//   See: knowledge_base/phase_0/04_network_byte_order.md
// ---------------------------------------------------------------------------

static constexpr uint32_t HEADER_SIZE = 8; // Fixed wire size of MessageHeader

enum class MessageType : uint16_t
{
    SUBSCRIBE = 1, // Client → Broker: subscribe to a topic
    PUBLISH   = 2, // Client → Broker: publish a message to a topic
    DATA      = 3, // Broker → Client: deliver a message to a subscriber
};

struct MessageHeader
{
    uint32_t length;       // Total frame byte length: HEADER_SIZE + topic + payload
    uint16_t type;         // MessageType enum value
    uint16_t topic_length; // Byte length of the topic string

    // ------------------------------------------------------------------
    // Serialize to network byte order and append exactly HEADER_SIZE
    // bytes to `buf`.
    //
    // IMPORTANT: Never cast this struct directly to uint8_t* and put it
    // on the wire. The compiler may insert padding between fields, and
    // the byte order would be host-dependent. This function is the only
    // correct way to serialize a header.
    // ------------------------------------------------------------------
    void to_bytes(std::vector<uint8_t>& buf) const
    {
        uint32_t net_len  = htonl(length);
        uint16_t net_type = htons(type);
        uint16_t net_tlen = htons(topic_length);

        const uint8_t* p;
        p = reinterpret_cast<const uint8_t*>(&net_len);
        buf.insert(buf.end(), p, p + 4);
        p = reinterpret_cast<const uint8_t*>(&net_type);
        buf.insert(buf.end(), p, p + 2);
        p = reinterpret_cast<const uint8_t*>(&net_tlen);
        buf.insert(buf.end(), p, p + 2);
    }

    // ------------------------------------------------------------------
    // Deserialize from `data` (must point to at least HEADER_SIZE bytes
    // in network byte order). Returns a host-byte-order MessageHeader.
    // ------------------------------------------------------------------
    static MessageHeader from_bytes(const uint8_t* data)
    {
        MessageHeader hdr{};
        uint32_t net_len{};
        uint16_t net_type{}, net_tlen{};
        std::memcpy(&net_len,  data,     4);
        std::memcpy(&net_type, data + 4, 2);
        std::memcpy(&net_tlen, data + 6, 2);
        hdr.length       = ntohl(net_len);
        hdr.type         = ntohs(net_type);
        hdr.topic_length = ntohs(net_tlen);
        return hdr;
    }
};
