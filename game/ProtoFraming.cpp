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
    uint32_t be = 0;
    std::memcpy(&be, buffer->data(), 4);
    const uint32_t len = ntohl(be);
    if (len == 0 || len > kMaxFrameSize || buffer->size() < 4u + len)
        return false;
    payload->assign(buffer->data() + 4, len);
    buffer->erase(0, 4 + len);
    return true;
}

}  // namespace gameproto
