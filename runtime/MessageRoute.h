#pragma once

#include "game.pb.h"

namespace gameproto {

/** 阶段 6：应路由到 World 中控的请求（邮件 + 聊天/好友骨架） */
bool IsWorldBoundRequest(const game::GameRequest &req);

/** 邮件读写：Formal 下 World 再转 GameDB（MailStore 只在 GameDB） */
bool IsMailBoundRequest(const game::GameRequest &req);

/** 场景/战斗/进图等仍走 GameLogic */
bool IsLogicBoundRequest(const game::GameRequest &req);

}  // namespace gameproto
