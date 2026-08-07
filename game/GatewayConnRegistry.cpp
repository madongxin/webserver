#include "GatewayConnRegistry.h"

GatewayConnRegistry &GatewayConnRegistry::Instance() {
    static GatewayConnRegistry g;
    return g;
}

void GatewayConnRegistry::Remember(int connection_id, Bind bind) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_conn_.find(connection_id);
    if (it != by_conn_.end()) {
        if (!it->second.session_id.empty())
            session_to_conn_.erase(it->second.session_id);
        if (it->second.player_id != 0)
            player_to_conn_.erase(it->second.player_id);
    }
    bind.connection_id = connection_id;
    if (!bind.session_id.empty())
        session_to_conn_[bind.session_id] = connection_id;
    if (bind.player_id != 0)
        player_to_conn_[bind.player_id] = connection_id;
    by_conn_[connection_id] = std::move(bind);
}

void GatewayConnRegistry::Forget(int connection_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_conn_.find(connection_id);
    if (it == by_conn_.end())
        return;
    if (!it->second.session_id.empty())
        session_to_conn_.erase(it->second.session_id);
    if (it->second.player_id != 0)
        player_to_conn_.erase(it->second.player_id);
    by_conn_.erase(it);
}

bool GatewayConnRegistry::FindBySession(const std::string &session_id, Bind *out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto sit = session_to_conn_.find(session_id);
    if (sit == session_to_conn_.end())
        return false;
    auto it = by_conn_.find(sit->second);
    if (it == by_conn_.end())
        return false;
    if (out)
        *out = it->second;
    return true;
}

bool GatewayConnRegistry::FindByConnection(int connection_id, Bind *out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_conn_.find(connection_id);
    if (it == by_conn_.end())
        return false;
    if (out)
        *out = it->second;
    return true;
}

bool GatewayConnRegistry::FindByPlayer(uint64_t player_id, Bind *out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto pit = player_to_conn_.find(player_id);
    if (pit == player_to_conn_.end())
        return false;
    auto it = by_conn_.find(pit->second);
    if (it == by_conn_.end())
        return false;
    if (out)
        *out = it->second;
    return true;
}

bool GatewayConnRegistry::SendBySession(const std::string &session_id, const std::string &frame) {
    Bind b;
    if (!FindBySession(session_id, &b) || !b.send_frame)
        return false;
    b.send_frame(frame);
    return true;
}
