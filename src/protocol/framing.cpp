#include "protocol/framing.hpp"

bool FramedSocket::send_message(MessageType type, const std::string& topic,
                                 const std::vector<uint8_t>& payload)
{
    // Build the full wire frame in a single contiguous buffer.
    // Benefits of a single buffer vs. three separate send() calls:
    //   1. One send_all() call instead of three → fewer kernel syscalls
    //   2. The kernel sees the complete frame at once → may coalesce into
    //      one TCP segment (better latency vs. Nagle fragmentation)

    MessageHeader hdr{};
    hdr.topic_length = static_cast<uint16_t>(topic.size());
    hdr.type         = static_cast<uint16_t>(type);
    hdr.length       = HEADER_SIZE
                       + static_cast<uint32_t>(topic.size())
                       + static_cast<uint32_t>(payload.size());

    std::vector<uint8_t> frame;
    frame.reserve(hdr.length); // Pre-allocate exact size to avoid reallocations

    // Serialize header fields individually into network byte order.
    // (Never memcpy the raw struct — see message.hpp for the reason.)
    hdr.to_bytes(frame);

    // Append topic bytes
    frame.insert(frame.end(), topic.begin(), topic.end());

    // Append payload bytes
    frame.insert(frame.end(), payload.begin(), payload.end());

    // send_all() retries internally until every byte is sent (Bug 5 fix).
    return m_socket.send_all(frame);
}

bool FramedSocket::send_message(MessageType type, const std::string& topic,
                                 const std::string& text)
{
    std::vector<uint8_t> payload(text.begin(), text.end());
    return send_message(type, topic, payload);
}

bool FramedSocket::recv_message(ParsedMessage& out)
{
    // ------------------------------------------------------------------
    // Step 1: Read exactly HEADER_SIZE bytes.
    //
    // WHY RECV_EXACT?
    //   TCP may deliver fewer bytes than requested in a single recv() call.
    //   recv_exact() loops internally until the buffer is full, making
    //   partial reads transparent to this function.
    // ------------------------------------------------------------------
    std::vector<uint8_t> hdr_buf;
    if (!m_socket.recv_exact(hdr_buf, HEADER_SIZE))
        return false; // Peer disconnected or error

    // ------------------------------------------------------------------
    // Step 2: Deserialize the header from network byte order.
    // ------------------------------------------------------------------
    MessageHeader hdr = MessageHeader::from_bytes(hdr_buf.data());

    // ------------------------------------------------------------------
    // Step 3: Validate header fields before allocating memory.
    //
    // WHY VALIDATE?
    //   A malformed or malicious client could send length=0xFFFFFFFF,
    //   causing us to allocate 4 GB before reading. Validation prevents
    //   DoS attacks and catches protocol errors early.
    //
    // Constraints:
    //   - hdr.length must be >= HEADER_SIZE (frame includes its own header)
    //   - hdr.topic_length must fit within the body  (topic ≤ body_size)
    //   - Apply a hard cap (e.g., 64 MB) to prevent malicious allocation
    // ------------------------------------------------------------------
    static constexpr uint32_t MAX_MESSAGE_SIZE = 64u * 1024u * 1024u; // 64 MB cap
    if (hdr.length < HEADER_SIZE || hdr.length > MAX_MESSAGE_SIZE)
        return false;
    if (hdr.topic_length > hdr.length - HEADER_SIZE)
        return false;

    // ------------------------------------------------------------------
    // Step 4: Read the remaining body bytes (topic + payload).
    // ------------------------------------------------------------------
    uint32_t body_size = hdr.length - HEADER_SIZE;
    std::vector<uint8_t> body;
    if (body_size > 0 && !m_socket.recv_exact(body, body_size))
        return false;

    // ------------------------------------------------------------------
    // Step 5: Slice the body into topic and payload.
    //
    // body layout: [topic (topic_length bytes)][payload (remaining bytes)]
    // ------------------------------------------------------------------
    out.type    = static_cast<MessageType>(hdr.type);
    out.topic   = std::string(body.begin(),
                              body.begin() + hdr.topic_length);
    out.payload = std::vector<uint8_t>(body.begin() + hdr.topic_length,
                                       body.end());

    return true;
}
