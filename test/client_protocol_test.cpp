/**
 * 锁定外网协议字段编号与 Push message_type；未改 proto 时 hash 由脚本测。
 */
#include "game.pb.h"

#include <cstdio>

namespace {

int fails = 0;
void Expect(bool c, const char *m) {
    if (!c) {
        std::printf("FAIL: %s\n", m);
        ++fails;
    }
}

}  // namespace

int main() {
    Expect(game::LoginRsp::kProfileFieldNumber == 9, "LoginRsp.profile=9");
    Expect(game::EnterMapReq::kMapDataVersionFieldNumber == 5, "EnterMapReq.version=5");
    Expect(game::EnterMapReq::kMapDataSha256FieldNumber == 6, "EnterMapReq.sha256=6");
    Expect(game::EnterMapReq::kOperationIdFieldNumber == 7, "EnterMapReq.operation_id=7");
    Expect(game::EnterMapRsp::kSpawnPositionFieldNumber == 8, "EnterMapRsp.spawn=8");
    Expect(game::EnterMapRsp::kAoiSnapshotFieldNumber == 13, "EnterMapRsp.aoi_snapshot=13");
    Expect(game::GameRequest::kGetSelfProfileFieldNumber == 60, "req get_self_profile=60");
    Expect(game::GameRequest::kMoveFieldNumber == 61, "req move=61");
    Expect(game::GameRequest::kPlayerMailSendFieldNumber == 63, "req player_mail_send=63");
    Expect(game::GameRequest::kClientHelloFieldNumber == 70, "req client_hello=70");
    Expect(game::GameRequest::kHeartbeatFieldNumber == 71, "req heartbeat=71");
    Expect(game::GameRequest::kWorldSnapshotFieldNumber == 72, "req world_snapshot=72");
    Expect(game::GameRequest::kRespawnFieldNumber == 73, "req respawn=73");
    Expect(game::GameRequest::kGetPlayerBriefFieldNumber == 74, "req get_player_brief=74");
    Expect(game::GameRequest::kQueryOnlineStateFieldNumber == 75, "req query_online_state=75");
    Expect(game::GameResponse::kErrorCodeFieldNumber == 4, "rsp error_code=4");
    Expect(game::GameResponse::kRetryableFieldNumber == 5, "rsp retryable=5");
    Expect(game::GameResponse::kServerTimeMsFieldNumber == 6, "rsp server_time_ms=6");
    Expect(game::GameResponse::kTraceIdFieldNumber == 7, "rsp trace_id=7");
    Expect(game::GameResponse::kGetSelfProfileFieldNumber == 60, "rsp get_self_profile=60");
    Expect(game::GameResponse::kMoveFieldNumber == 61, "rsp move=61");
    Expect(game::GameResponse::kAoiDeltaFieldNumber == 62, "rsp aoi_delta=62");
    Expect(game::GameResponse::kPlayerMailSendFieldNumber == 63, "rsp player_mail_send=63");
    Expect(game::GameResponse::kMailboxChangedFieldNumber == 64, "rsp mailbox_changed=64");
    Expect(game::GameResponse::kServerHelloFieldNumber == 70, "rsp server_hello=70");
    Expect(game::GameResponse::kHeartbeatFieldNumber == 71, "rsp heartbeat=71");
    Expect(game::GameResponse::kRespawnFieldNumber == 72, "rsp respawn=72");
    Expect(game::GameResponse::kChatNotifyFieldNumber == 73, "rsp chat_notify=73");
    Expect(game::GameResponse::kGetPlayerBriefFieldNumber == 74, "rsp get_player_brief=74");
    Expect(game::GameResponse::kQueryOnlineStateFieldNumber == 75, "rsp query_online_state=75");
    Expect(game::FullStateSnapshotRsp::kProfileFieldNumber == 8, "snapshot profile=8");
    Expect(game::FullStateSnapshotRsp::kAoiEntitiesFieldNumber == 16, "snapshot aoi=16");
    Expect(game::FullStateSnapshotRsp::kBaselineServerSeqFieldNumber == 7, "snapshot baseline=7");
    Expect(game::PlayerAttributes::kLifeStateFieldNumber == 16, "life_state=16");
    Expect(game::GameResponse::kServerPushFieldNumber == 53, "rsp server_push=53");
    Expect(game::PlayerAttributes::kMoveSpeedFieldNumber == 13, "move_speed=13");
    Expect(game::PlayerAttributes::kStatsVersionFieldNumber == 15, "stats_version=15");

    game::GameRequest req;
    req.mutable_player_mail_send()->set_sender_player_id(1);
    Expect(req.body_case() == game::GameRequest::kPlayerMailSend, "oneof player_mail_send");

    if (fails) {
        std::printf("client_protocol_test FAIL count=%d\n", fails);
        return 1;
    }
    std::printf("OK client_protocol_test\n");
    return 0;
}
