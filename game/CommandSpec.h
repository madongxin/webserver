#pragma once

#include "game.pb.h"

namespace gameproto {

/** 一条公网命令的路由和鉴权。新增 GameRequest 字段必须在这张表里补一行。 */
struct CommandSpec {
    game::GameRequest::BodyCase body;
    bool global_service;       // Gateway 转到 GlobalService（含邮件）
    bool mail;                 // 邮件帧，Formal 下可转 GameDB
    bool preauth_allow;        // 未登录连接可以发
    bool authenticated_allow;  // 已登录连接可以发
    bool dangerous;            // 已登录也要 GAMEMESH_ALLOW_UNSAFE_DEBUG_COMMANDS
    bool needs_session_token;  // GameLogic::Handle 入口要校验 token
};

const CommandSpec *FindCommandSpec(game::GameRequest::BodyCase body);
const CommandSpec *CommandSpecTable(int *count);

}  // namespace gameproto
