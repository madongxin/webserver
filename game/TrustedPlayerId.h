#pragma once

#include "game.pb.h"

#include <cstdint>

/**
 * Gateway/GameLogic 可信身份：覆盖客户端自报 player_id。
 * 含 player_id 的 oneof 必须全部在此登记；新增字段时 trusted_player_id_test 会失败。
 */
enum class TrustedPlayerIdResult {
    NoPlayerField = 0,  // 该 body 无 player_id（或空 body）
    Applied = 1,        // 已覆盖为可信 id
    Mismatch = 2,       // 客户端自报与可信 id 不一致（已覆盖，调用方应拒绝）
};

/** 当前 GameRequest oneof 中含 player_id 的分支数（测试锁定；含 login/reconnect；不含 mail_deliver） */
inline constexpr int kTrustedPlayerIdBodyCaseCount = 27;

inline bool GameRequestBodyHasPlayerId(game::GameRequest::BodyCase c) {
    switch (c) {
    case game::GameRequest::kConsumeItem:
    case game::GameRequest::kReleaseSkill:
    case game::GameRequest::kGrantItem:
    case game::GameRequest::kLogin:
    case game::GameRequest::kValidateSession:
    case game::GameRequest::kCheckOnline:
    case game::GameRequest::kLogout:
    case game::GameRequest::kReconnect:
    case game::GameRequest::kFlushBag:
    case game::GameRequest::kMailboxSummary:
    case game::GameRequest::kMailList:
    case game::GameRequest::kMailGet:
    case game::GameRequest::kMailRead:
    case game::GameRequest::kMailClaim:
    case game::GameRequest::kMailBatchClaim:
    case game::GameRequest::kMailFavorite:
    case game::GameRequest::kMailBatchRead:
    case game::GameRequest::kMailBatchDelete:
    case game::GameRequest::kEnterMap:
    case game::GameRequest::kLeaveMap:
    case game::GameRequest::kMapPing:
    case game::GameRequest::kChatSend:
    case game::GameRequest::kFriendList:
    case game::GameRequest::kPushAck:
    case game::GameRequest::kGetSelfProfile:
    case game::GameRequest::kMove:
    case game::GameRequest::kPlayerMailSend:
        return true;
    case game::GameRequest::kRegister:
    case game::GameRequest::BODY_NOT_SET:
    default:
        return false;
    }
}

inline uint64_t ReportedPlayerId(const game::GameRequest &req) {
    switch (req.body_case()) {
    case game::GameRequest::kConsumeItem:
        return req.consume_item().player_id();
    case game::GameRequest::kReleaseSkill:
        return req.release_skill().player_id();
    case game::GameRequest::kGrantItem:
        return req.grant_item().player_id();
    case game::GameRequest::kLogin:
        return req.login().player_id();
    case game::GameRequest::kValidateSession:
        return req.validate_session().player_id();
    case game::GameRequest::kCheckOnline:
        return req.check_online().player_id();
    case game::GameRequest::kLogout:
        return req.logout().player_id();
    case game::GameRequest::kReconnect:
        return req.reconnect().player_id();
    case game::GameRequest::kFlushBag:
        return req.flush_bag().player_id();
    case game::GameRequest::kMailboxSummary:
        return req.mailbox_summary().player_id();
    case game::GameRequest::kMailList:
        return req.mail_list().player_id();
    case game::GameRequest::kMailGet:
        return req.mail_get().player_id();
    case game::GameRequest::kMailRead:
        return req.mail_read().player_id();
    case game::GameRequest::kMailClaim:
        return req.mail_claim().player_id();
    case game::GameRequest::kMailBatchClaim:
        return req.mail_batch_claim().player_id();
    case game::GameRequest::kMailFavorite:
        return req.mail_favorite().player_id();
    case game::GameRequest::kMailBatchRead:
        return req.mail_batch_read().player_id();
    case game::GameRequest::kMailBatchDelete:
        return req.mail_batch_delete().player_id();
    case game::GameRequest::kEnterMap:
        return req.enter_map().player_id();
    case game::GameRequest::kLeaveMap:
        return req.leave_map().player_id();
    case game::GameRequest::kMapPing:
        return req.map_ping().player_id();
    case game::GameRequest::kChatSend:
        return req.chat_send().player_id();
    case game::GameRequest::kFriendList:
        return req.friend_list().player_id();
    case game::GameRequest::kPushAck:
        return req.push_ack().player_id();
    case game::GameRequest::kGetSelfProfile:
        return req.get_self_profile().player_id();
    case game::GameRequest::kMove:
        return req.move().player_id();
    case game::GameRequest::kPlayerMailSend:
        return req.player_mail_send().sender_player_id();
    default:
        return 0;
    }
}

inline void SetBodyPlayerId(game::GameRequest *req, uint64_t pid) {
    switch (req->body_case()) {
    case game::GameRequest::kConsumeItem:
        req->mutable_consume_item()->set_player_id(pid);
        break;
    case game::GameRequest::kReleaseSkill:
        req->mutable_release_skill()->set_player_id(pid);
        break;
    case game::GameRequest::kGrantItem:
        req->mutable_grant_item()->set_player_id(pid);
        break;
    case game::GameRequest::kLogin:
        req->mutable_login()->set_player_id(pid);
        break;
    case game::GameRequest::kValidateSession:
        req->mutable_validate_session()->set_player_id(pid);
        break;
    case game::GameRequest::kCheckOnline:
        req->mutable_check_online()->set_player_id(pid);
        break;
    case game::GameRequest::kLogout:
        req->mutable_logout()->set_player_id(pid);
        break;
    case game::GameRequest::kReconnect:
        req->mutable_reconnect()->set_player_id(pid);
        break;
    case game::GameRequest::kFlushBag:
        req->mutable_flush_bag()->set_player_id(pid);
        break;
    case game::GameRequest::kMailboxSummary:
        req->mutable_mailbox_summary()->set_player_id(pid);
        break;
    case game::GameRequest::kMailList:
        req->mutable_mail_list()->set_player_id(pid);
        break;
    case game::GameRequest::kMailGet:
        req->mutable_mail_get()->set_player_id(pid);
        break;
    case game::GameRequest::kMailRead:
        req->mutable_mail_read()->set_player_id(pid);
        break;
    case game::GameRequest::kMailClaim:
        req->mutable_mail_claim()->set_player_id(pid);
        break;
    case game::GameRequest::kMailBatchClaim:
        req->mutable_mail_batch_claim()->set_player_id(pid);
        break;
    case game::GameRequest::kMailFavorite:
        req->mutable_mail_favorite()->set_player_id(pid);
        break;
    case game::GameRequest::kMailBatchRead:
        req->mutable_mail_batch_read()->set_player_id(pid);
        break;
    case game::GameRequest::kMailBatchDelete:
        req->mutable_mail_batch_delete()->set_player_id(pid);
        break;
    case game::GameRequest::kEnterMap:
        req->mutable_enter_map()->set_player_id(pid);
        break;
    case game::GameRequest::kLeaveMap:
        req->mutable_leave_map()->set_player_id(pid);
        break;
    case game::GameRequest::kMapPing:
        req->mutable_map_ping()->set_player_id(pid);
        break;
    case game::GameRequest::kChatSend:
        req->mutable_chat_send()->set_player_id(pid);
        break;
    case game::GameRequest::kFriendList:
        req->mutable_friend_list()->set_player_id(pid);
        break;
    case game::GameRequest::kPushAck:
        req->mutable_push_ack()->set_player_id(pid);
        break;
    case game::GameRequest::kGetSelfProfile:
        req->mutable_get_self_profile()->set_player_id(pid);
        break;
    case game::GameRequest::kMove:
        req->mutable_move()->set_player_id(pid);
        break;
    case game::GameRequest::kPlayerMailSend:
        req->mutable_player_mail_send()->set_sender_player_id(pid);
        break;
    default:
        break;
    }
}

/**
 * 用可信 player_id 覆盖 payload。
 * 登录前白名单（login/reconnect）通常不走本函数；若调用且自报不一致则 Mismatch。
 */
inline TrustedPlayerIdResult ApplyTrustedPlayerId(game::GameRequest *req, uint64_t trusted_pid) {
    if (!req || trusted_pid == 0)
        return TrustedPlayerIdResult::NoPlayerField;
    if (!GameRequestBodyHasPlayerId(req->body_case()))
        return TrustedPlayerIdResult::NoPlayerField;
    const uint64_t reported = ReportedPlayerId(*req);
    SetBodyPlayerId(req, trusted_pid);
    if (reported != 0 && reported != trusted_pid)
        return TrustedPlayerIdResult::Mismatch;
    return TrustedPlayerIdResult::Applied;
}

/** Dispatch 路径业务命令：登录/注册/重连不应经此覆盖策略做“已绑定业务” */
inline bool IsPostAuthGameBody(game::GameRequest::BodyCase c) {
    switch (c) {
    case game::GameRequest::kLogin:
    case game::GameRequest::kReconnect:
    case game::GameRequest::kRegister:
        return false;
    default:
        return GameRequestBodyHasPlayerId(c) || c == game::GameRequest::BODY_NOT_SET;
    }
}
