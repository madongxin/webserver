#include "GatewayConnRegistry.h"

GatewayConnRegistry &GatewayConnRegistry::Instance() {
    static GatewayConnRegistry g;
    return g;
}

void GatewayConnRegistry::Remember(uint64_t connection_id, Bind bind) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_conn_.find(connection_id);
    if (it != by_conn_.end()) {
        // 仅当索引仍指向本连接时才清理（避免误删已被新连接接管的索引）
        if (!it->second.session_id.empty()) {
            auto sit = session_to_conn_.find(it->second.session_id);
            if (sit != session_to_conn_.end() && sit->second == connection_id)
                session_to_conn_.erase(sit);
        }
        if (it->second.player_id != 0) {
            auto pit = player_to_conn_.find(it->second.player_id);
            if (pit != player_to_conn_.end() && pit->second == connection_id)
                player_to_conn_.erase(pit);
        }
    }
    bind.connection_id = connection_id;
    if (!bind.session_id.empty())
        session_to_conn_[bind.session_id] = connection_id;
    if (bind.player_id != 0)
        player_to_conn_[bind.player_id] = connection_id;
    by_conn_[connection_id] = std::move(bind);
}

void GatewayConnRegistry::Forget(uint64_t connection_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_conn_.find(connection_id);
    if (it == by_conn_.end())
        return;
    if (!it->second.session_id.empty()) {
        auto sit = session_to_conn_.find(it->second.session_id);
        if (sit != session_to_conn_.end() && sit->second == connection_id)
            session_to_conn_.erase(sit);
    }
    if (it->second.player_id != 0) {
        auto pit = player_to_conn_.find(it->second.player_id);
        if (pit != player_to_conn_.end() && pit->second == connection_id)
            player_to_conn_.erase(pit);
    }
    by_conn_.erase(it);
}

bool GatewayConnRegistry::ApplyRoute(uint64_t connection_id, const std::string &gamelogic_instance_id,
                                     uint64_t map_instance_id, uint64_t map_owner_epoch,
                                     uint64_t route_version) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = by_conn_.find(connection_id);
    if (it == by_conn_.end())
        return false;
    if (route_version != 0 && it->second.route_version != 0 &&
        route_version < it->second.route_version)
        return false;
    it->second.gamelogic_instance_id = gamelogic_instance_id;
    it->second.map_instance_id = map_instance_id;
    it->second.map_owner_epoch = map_owner_epoch;
    if (route_version != 0)
        it->second.route_version = route_version;
    return true;
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

bool GatewayConnRegistry::FindByConnection(uint64_t connection_id, Bind *out) {
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
