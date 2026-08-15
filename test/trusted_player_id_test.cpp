/**
 * 工作包一 §2：穷举 GameRequest oneof 中含 player_id 的分支；覆盖与不一致拒绝。
 */
#include "TrustedPlayerId.h"

#include <cstdio>

namespace {

int fails = 0;

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++fails;
    }
}

void CheckCase(game::GameRequest::BodyCase /*c*/, game::GameRequest *req, const char *name) {
    Expect(GameRequestBodyHasPlayerId(req->body_case()), name);
    req->set_seq(1);
    // 先写入伪造 id
    SetBodyPlayerId(req, 999);
    Expect(ReportedPlayerId(*req) == 999, name);
    const auto r = ApplyTrustedPlayerId(req, 42);
    Expect(r == TrustedPlayerIdResult::Mismatch, name);
    Expect(ReportedPlayerId(*req) == 42, name);
    // 一致
    SetBodyPlayerId(req, 42);
    Expect(ApplyTrustedPlayerId(req, 42) == TrustedPlayerIdResult::Applied, name);
}

}  // namespace

int main() {
    int covered = 0;

#define CHECK_ONE(setter, field)                                                                   \
    do {                                                                                           \
        game::GameRequest req;                                                                     \
        req.mutable_##field()->set_player_id(1);                                                   \
        CheckCase(req.body_case(), &req, #field);                                                  \
        ++covered;                                                                                 \
    } while (0)

    CHECK_ONE(mutable_consume_item, consume_item);
    CHECK_ONE(mutable_release_skill, release_skill);
    CHECK_ONE(mutable_grant_item, grant_item);
    CHECK_ONE(mutable_login, login);
    CHECK_ONE(mutable_validate_session, validate_session);
    CHECK_ONE(mutable_check_online, check_online);
    CHECK_ONE(mutable_logout, logout);
    CHECK_ONE(mutable_reconnect, reconnect);
    CHECK_ONE(mutable_flush_bag, flush_bag);
    CHECK_ONE(mutable_mailbox_summary, mailbox_summary);
    CHECK_ONE(mutable_mail_list, mail_list);
    CHECK_ONE(mutable_mail_get, mail_get);
    CHECK_ONE(mutable_mail_read, mail_read);
    CHECK_ONE(mutable_mail_claim, mail_claim);
    CHECK_ONE(mutable_mail_batch_claim, mail_batch_claim);
    CHECK_ONE(mutable_mail_favorite, mail_favorite);
    CHECK_ONE(mutable_mail_batch_read, mail_batch_read);
    CHECK_ONE(mutable_mail_batch_delete, mail_batch_delete);
    CHECK_ONE(mutable_enter_map, enter_map);
    CHECK_ONE(mutable_leave_map, leave_map);
    CHECK_ONE(mutable_map_ping, map_ping);
    CHECK_ONE(mutable_chat_send, chat_send);
    CHECK_ONE(mutable_friend_list, friend_list);
    CHECK_ONE(mutable_push_ack, push_ack);
    CHECK_ONE(mutable_get_self_profile, get_self_profile);
    CHECK_ONE(mutable_move, move);
    CHECK_ONE(mutable_world_snapshot, world_snapshot);
    CHECK_ONE(mutable_respawn, respawn);
    CHECK_ONE(mutable_get_player_brief, get_player_brief);
    CHECK_ONE(mutable_query_online_state, query_online_state);
    {
        game::GameRequest req;
        req.mutable_player_mail_send()->set_sender_player_id(1);
        CheckCase(req.body_case(), &req, "player_mail_send");
        ++covered;
    }

#undef CHECK_ONE

    Expect(covered == kTrustedPlayerIdBodyCaseCount, "body case count lock");
    Expect(!GameRequestBodyHasPlayerId(game::GameRequest::kRegister), "register has no player_id");
    Expect(!GameRequestBodyHasPlayerId(game::GameRequest::BODY_NOT_SET), "empty body");

    game::GameRequest empty;
    Expect(ApplyTrustedPlayerId(&empty, 1) == TrustedPlayerIdResult::NoPlayerField, "empty");

    {
        game::GameRequest req;
        req.mutable_get_player_brief()->set_player_id(1);
        req.mutable_get_player_brief()->set_target_player_id(99);
        Expect(ApplyTrustedPlayerId(&req, 42) == TrustedPlayerIdResult::Mismatch, "brief overlay");
        Expect(req.get_player_brief().player_id() == 42, "brief querier overlay");
        Expect(req.get_player_brief().target_player_id() == 99, "brief target preserved");
    }

    if (fails) {
        std::printf("trusted_player_id_test FAIL count=%d covered=%d\n", fails, covered);
        return 1;
    }
    std::printf("OK trusted_player_id_test covered=%d\n", covered);
    return 0;
}
