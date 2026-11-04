#pragma once

#include "ProtoFraming.h"
#include "game.pb.h"

#include <cstddef>
#include <string>
#include <vector>

struct InboundFrame {
    std::string payload;
    game::GameRequest req;
    int rank = 2;
    bool ok = false;
};

inline int InboundRank(const game::GameRequest &req) {
    if (req.has_heartbeat() || req.has_client_hello())
        return 0;
    if (req.has_reconnect() || req.has_login() || req.has_register_() || req.has_logout())
        return 1;
    return 2;
}

/**
 * 每个完整帧只 ParseFromString 一次。
 * 返回 false 表示帧头非法，调用方应关连接。*parses 与 frames 一一对应。
 */
inline bool DecodeInboundFrames(const std::string &raw, size_t scan_cap, std::vector<InboundFrame> *frames,
                                size_t *consumed, size_t *parses) {
    frames->clear();
    *consumed = 0;
    *parses = 0;
    size_t offset = 0;
    while (frames->size() < scan_cap) {
        size_t n = 0;
        std::string payload;
        const auto dr = gameproto::DecodeOneFrameFrom(raw, offset, &payload, &n);
        if (dr == gameproto::FrameDecodeResult::Incomplete)
            break;
        if (dr == gameproto::FrameDecodeResult::Invalid)
            return false;
        InboundFrame item;
        item.payload = std::move(payload);
        item.ok = item.req.ParseFromString(item.payload);
        ++(*parses);
        if (item.ok)
            item.rank = InboundRank(item.req);
        frames->push_back(std::move(item));
        offset += n;
    }
    *consumed = offset;
    return true;
}
