#pragma once

#include "game.pb.h"

#include <string>

namespace gameproto {

enum class CommandTrustLevel {
    PreAuthClient = 0,
    AuthenticatedClient = 1,
    InternalService = 2,
    AdminService = 3,
};

enum class CommandDecision {
    Allow = 0,
    Forbid = 1,
};

/** Formal 或未显式开启时禁止公网 GrantItem/MailDeliver 等危险命令。 */
bool AllowUnsafeDebugCommands();

/**
 * 公网 TCP 命令策略：未登记 body 默认拒绝（AuthenticatedClient）。
 * Internal/Admin 允许危险命令；Formal 下即使 allow_unsafe 也拒绝客户端危险命令。
 */
CommandDecision ValidateCommandPolicy(game::GameRequest::BodyCase body, CommandTrustLevel trust,
                                      bool formal_mode, std::string *err_code = nullptr);

/** 解析 payload 后校验；失败时 err_code 一般为 ERR_COMMAND_FORBIDDEN。 */
bool AllowClientTcpPayload(const std::string &payload, bool bound, bool formal_mode,
                           std::string *err_code = nullptr);

}  // namespace gameproto
