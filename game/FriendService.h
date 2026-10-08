#pragma once

#include "game.pb.h"

#include <cstdint>

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

private:
    FriendService() = default;
};
