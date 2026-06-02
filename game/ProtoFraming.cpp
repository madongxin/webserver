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

bool TryDecodeOneFrame(std::string *buffer, std::string *payload) {
    if (!buffer || !payload || buffer->size() < 4)
        return false;

    // 读 4 字节长度（大端）
    uint32_t be = 0;
    std::memcpy(&be, buffer->data(), 4);
    const uint32_t len = ntohl(be);

    if (len == 0 || len > kMaxFrameSize || buffer->size() < 4u + len)
        return false;  // 半包：等下次 OnMessage 再 append 后重试

    payload->assign(buffer->data() + 4, len);
    buffer->erase(0, 4 + len);
    return true;
}

}  // namespace gameproto
