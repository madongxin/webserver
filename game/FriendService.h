#pragma once

#include "game.pb.h"

#include <cstdint>
#include <string>

/** 私聊是否可以投递。Hide 表示对方拉黑了自己：对发送方返回成功，但不把消息送出去。 */
enum class FriendWhisperGate { Deliver, Reject, Hide };

class FriendService {
public:
    static FriendService &Instance();

    bool HandleList(const game::FriendListReq &req, game::GameResponse *rsp);
    bool HandleSearch(const game::FriendSearchReq &req, game::GameResponse *rsp);
    bool HandleApply(const game::FriendApplyReq &req, game::GameResponse *rsp);
    bool HandleRequestList(const game::FriendRequestListReq &req, game::GameResponse *rsp);
    bool HandleAccept(const game::FriendAcceptReq &req, game::GameResponse *rsp);
    bool HandleReject(const game::FriendRejectReq &req, game::GameResponse *rsp);
    bool HandleDelete(const game::FriendDeleteReq &req, game::GameResponse *rsp);
    bool HandleBlock(const game::FriendBlockReq &req, game::GameResponse *rsp);
    bool HandleUnblock(const game::FriendUnblockReq &req, game::GameResponse *rsp);
    bool HandleBlockList(const game::FriendBlockListReq &req, game::GameResponse *rsp);

    void FanoutPresence(uint64_t player_id, bool online);
    FriendWhisperGate GateWhisper(uint64_t actor_player_id, uint64_t target_player_id,
                                  std::string *error_code);

private:
    FriendService() = default;
};
