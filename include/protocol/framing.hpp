#pragma once
#include "network/socket.hpp"
#include "protocol/message.hpp"
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ParsedMessage — a fully-reassembled, in-memory message extracted from wire.
// ---------------------------------------------------------------------------
struct ParsedMessage
{
    MessageType          type;
    std::string          topic;
    std::vector<uint8_t> payload;
};

// ---------------------------------------------------------------------------
// FramedSocket — wraps a Socket with the project's length-prefix framing protocol.
//
// THE CORE PROBLEM THIS SOLVES (TCP framing):
//   TCP is a *byte stream* protocol. There are no message boundaries.
//   A single call to send(header + topic + payload) on the sender can
//   arrive as:
//     - One recv() call with all bytes at once (lucky)
//     - Two recv() calls split at any byte position (common)
//     - Many tiny recv() calls (kernel busy, Nagle off)
//
//   Without framing, the receiver cannot tell where one message ends and
//   the next begins. FramedSocket fixes this with a length-prefix scheme:
//
//   Wire Layout:
//     [ MessageHeader (8 bytes) ][ topic ][ payload ]
//     ^--- fixed size ---------^ ^--- variable, length given by header ---^
//
//   Send protocol:
//     1. Build a single contiguous buffer: header + topic + payload
//     2. Call send_all() which loops until ALL bytes are sent (Bug 5 fix)
//
//   Receive protocol:
//     1. recv_exact(HEADER_SIZE)  → always get exactly 8 header bytes
//     2. Parse header.length, header.topic_length
//     3. recv_exact(header.length - HEADER_SIZE) → get exact body bytes
//     4. Slice body into topic[0..topic_length] and payload[topic_length..]
//
// See: knowledge_base/phase_0/01_tcp_stream_semantics.md
// ---------------------------------------------------------------------------
class FramedSocket
{
  public:
    explicit FramedSocket(Socket& socket) : m_socket(socket) {}

    // Build and send a complete framed message (vector payload).
    // Returns false on send failure.
    bool send_message(MessageType type, const std::string& topic,
                      const std::vector<uint8_t>& payload);

    // Convenience overload for plain-text payloads.
    bool send_message(MessageType type, const std::string& topic,
                      const std::string& text);

    // Block until a complete framed message is received and parsed.
    // Returns false if the connection closes or a framing error occurs.
    bool recv_message(ParsedMessage& out);

  private:
    Socket& m_socket;
};
