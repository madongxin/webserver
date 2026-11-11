#pragma once

#include "ProtoFraming.h"
#include "game.pb.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

/** ENTER/LEAVE 里的实体。位置帧在写出第一个字节前要按这些 id 整帧丢掉。 */
inline std::vector<uint64_t> AoiStripEntityIds(const std::string &frame) {
    std::vector<uint64_t> ids;
    std::string buf = frame;
    std::string payload;
    if (gameproto::DecodeOneFrame(&buf, &payload) != gameproto::FrameDecodeResult::Complete)
        return ids;
    game::GameResponse gr;
    if (!gr.ParseFromString(payload) || !gr.has_aoi_delta())
        return ids;
    for (const auto &ev : gr.aoi_delta().events()) {
        if (ev.op() == 2 || ev.entity().player_id() == 0)
            continue;
        ids.push_back(ev.entity().player_id());
    }
    return ids;
}

/**
 * 从一帧 AOI 里去掉这些实体的事件并重新编码。
 * 其它实体留下。一个事件都不剩时 *empty 为真，调用方丢掉整帧。
 * 不是 AOI 帧时返回 false，调用方保持原字节。
 * 已经写出的半帧不该交给这个函数。
 */
inline bool RewriteAoiFrameDroppingEntities(const std::string &frame,
                                            const std::vector<uint64_t> &drop_ids, std::string *out,
                                            bool *empty, int *removed) {
    if (empty)
        *empty = false;
    if (removed)
        *removed = 0;
    if (!out || drop_ids.empty())
        return false;
    std::string payload;
    size_t consumed = 0;
    if (gameproto::DecodeOneFrameFrom(frame, 0, &payload, &consumed) !=
            gameproto::FrameDecodeResult::Complete ||
        consumed != frame.size())
        return false;
    game::GameResponse gr;
    if (!gr.ParseFromString(payload) || !gr.has_aoi_delta())
        return false;
    std::unordered_set<uint64_t> drop(drop_ids.begin(), drop_ids.end());
    game::AoiDelta kept;
    kept.set_map_instance_id(gr.aoi_delta().map_instance_id());
    int n = 0;
    for (const auto &ev : gr.aoi_delta().events()) {
        if (ev.entity().player_id() != 0 && drop.count(ev.entity().player_id()) != 0) {
            ++n;
            continue;
        }
        *kept.add_events() = ev;
    }
    if (n == 0) {
        *out = frame;
        return true;
    }
    if (removed)
        *removed = n;
    if (kept.events_size() == 0) {
        if (empty)
            *empty = true;
        out->clear();
        return true;
    }
    *gr.mutable_aoi_delta() = kept;
    std::string body;
    if (!gr.SerializeToString(&body) || !gameproto::EncodeFrame(body, out))
        return false;
    return true;
}
