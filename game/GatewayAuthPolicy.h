#pragma once

#include <string>

namespace gameproto {

/** Login / Logout / Reconnect / Register 由 Gateway 编排 */
bool IsGatewayOwnedAuthPayload(const std::string &payload);

/** 未绑定前允许：Login / Register / Reconnect */
bool IsPreAuthWhitelistPayload(const std::string &payload);

}  // namespace gameproto
