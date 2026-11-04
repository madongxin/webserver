#pragma once

#include "ProtoFraming.h"
#include "game.pb.h"

#include <cstdint>
#include <string>
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
