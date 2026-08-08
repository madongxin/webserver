/**
 * @file ProtoFraming.cpp
 * @brief length-prefix 编解码，与客户端 game_client 帧格式一致
 */

#include "ProtoFraming.h"

#include <arpa/inet.h>
#include <cstring>

namespace gameproto {

bool EncodeFrame(const std::string &payload, std::string *out) {
    if (!out || payload.size() > kMaxFrameSize)
        return false;
    uint32_t be = htonl(static_cast<uint32_t>(payload.size()));
    out->assign(reinterpret_cast<const char *>(&be), sizeof(be));
    out->append(payload);
    return true;
}

FrameDecodeResult DecodeOneFrame(std::string *buffer, std::string *payload) {
    if (!buffer || !payload)
        return FrameDecodeResult::Invalid;
    if (buffer->size() < 4)
        return FrameDecodeResult::Incomplete;

    uint32_t be = 0;
    std::memcpy(&be, buffer->data(), 4);
    const uint32_t len = ntohl(be);

    if (len == 0 || len > kMaxFrameSize)
        return FrameDecodeResult::Invalid;

    if (buffer->size() < 4u + len)
        return FrameDecodeResult::Incomplete;

    payload->assign(buffer->data() + 4, len);
    buffer->erase(0, 4 + len);
    return FrameDecodeResult::Complete;
}

bool TryDecodeOneFrame(std::string *buffer, std::string *payload) {
    return DecodeOneFrame(buffer, payload) == FrameDecodeResult::Complete;
}

}  // namespace gameproto
