#pragma once

#include "game.pb.h"

#include <cstdint>

/** Dispatch 路径：用可信 meta.player_id 覆盖 payload 自报身份 */
inline void ApplyTrustedPlayerId(game::GameRequest *req, uint64_t trusted_pid) {
    if (!req || trusted_pid == 0)
        return;
    if (req->has_consume_item())
        req->mutable_consume_item()->set_player_id(trusted_pid);
    if (req->has_grant_item())
        req->mutable_grant_item()->set_player_id(trusted_pid);
    if (req->has_enter_map())
        req->mutable_enter_map()->set_player_id(trusted_pid);
    if (req->has_leave_map())
        req->mutable_leave_map()->set_player_id(trusted_pid);
    if (req->has_logout())
        req->mutable_logout()->set_player_id(trusted_pid);
    if (req->has_flush_bag())
        req->mutable_flush_bag()->set_player_id(trusted_pid);
    if (req->has_push_ack())
        req->mutable_push_ack()->set_player_id(trusted_pid);
}
