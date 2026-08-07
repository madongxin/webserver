#include "GameRequestPlayerId.h"

#include "game.pb.h"

namespace gameproto {

uint64_t ExtractPlayerIdFromRequestPayload(const std::string &request_payload) {
    game::GameRequest req;
    if (!req.ParseFromString(request_payload))
        return 0;
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
    case game::GameRequest::kRegister:
        return 0;  // 注册尚未有 player_id，串行键用 0 池
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
    case game::GameRequest::kMailDeliver:
        return req.mail_deliver().receiver_id();
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
    default:
        return 0;
    }
}

}  // namespace gameproto
