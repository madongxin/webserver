/**
 * 量一条 AoiDelta MOVE 帧的真实字节：4 字节长度头 + GameResponse + protobuf。
 * 字段和网关 FlushMoveSlot 一样，只用移动快照（没有名字和血量）。
 */
#include "ProtoFraming.h"
#include "game.pb.h"

#include <cstdio>
#include <string>

int main() {
    game::GameResponse notify;
    notify.set_ok(true);
    notify.set_message("aoi");
    auto *delta = notify.mutable_aoi_delta();
    delta->set_map_instance_id(10001);
    constexpr int kN = 40;
    for (int i = 0; i < kN; ++i) {
        auto *ev = delta->add_events();
        ev->set_op(2);
        ev->set_server_time_ms(1760000000000LL + i);
        auto *ent = ev->mutable_entity();
        ent->set_player_id(100000ULL + static_cast<unsigned>(i));
        ent->mutable_position()->set_x(12.5f + static_cast<float>(i));
        ent->mutable_position()->set_y(0.f);
        ent->mutable_position()->set_z(-8.25f);
        ent->set_yaw(1.25f);
        ent->set_state_seq(1000ULL + static_cast<unsigned>(i));
    }
    std::string body;
    std::string frame;
    if (!notify.SerializeToString(&body) || !gameproto::EncodeFrame(body, &frame)) {
        std::printf("FAIL encode\n");
        return 1;
    }
    const int per = static_cast<int>((frame.size() + kN - 1) / kN);
    std::printf("frame_bytes=%zu body_bytes=%zu events=%d bytes_per_event_ceil=%d\n", frame.size(),
                body.size(), kN, per);

    game::GameResponse one;
    one.set_ok(true);
    one.set_message("aoi");
    auto *d1 = one.mutable_aoi_delta();
    d1->set_map_instance_id(10001);
    auto *ev = d1->add_events();
    ev->set_op(2);
    ev->set_server_time_ms(1760000000000LL);
    auto *ent = ev->mutable_entity();
    ent->set_player_id(100000);
    ent->mutable_position()->set_x(12.5f);
    ent->mutable_position()->set_y(0.f);
    ent->mutable_position()->set_z(-8.25f);
    ent->set_yaw(1.25f);
    ent->set_state_seq(1000);
    std::string body1;
    std::string frame1;
    if (!one.SerializeToString(&body1) || !gameproto::EncodeFrame(body1, &frame1)) {
        std::printf("FAIL encode one\n");
        return 1;
    }
    std::printf("one_event_frame_bytes=%zu\n", frame1.size());
    if (per != 38 || frame1.size() != 54 || frame.size() != 1498) {
        std::printf("FAIL measured size drifted; update kAoiMoveEventBytes and docs/perf_audit/aoi_move_egress.md\n");
        return 1;
    }
    std::printf("PASS aoi_move_frame_size_test\n");
    return 0;
}
