#pragma once

namespace gameproto {

/** Logout 三类结果：编码成功 ≠ Session 离线 ≠ Logic/AOI 清理成功。 */
struct GatewayLogoutResult {
    bool encoded = false;
    bool session_ok = false;
    bool logic_ok = false;
    bool clear_bind = false;
};

inline bool LogoutAuthoritativeOk(const GatewayLogoutResult &r) {
    return r.session_ok && r.logic_ok;
}

inline bool LogoutShouldClearBind(const GatewayLogoutResult &r) {
    return LogoutAuthoritativeOk(r);
}

}  // namespace gameproto
