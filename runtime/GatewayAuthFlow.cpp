#include "GatewayAuthFlow.h"

GatewayAuthFlow &GatewayAuthFlow::Instance() {
    static GatewayAuthFlow g;
    return g;
}

void GatewayAuthFlow::OnConnected(uint64_t connection_id) {
    std::lock_guard<std::mutex> lk(mu_);
    Slot &s = slots_[connection_id];
    s.alive = true;
    s.in_flight = false;
    // 保留 flow_gen 递增空间；新连接从 0 起
    s.flow_gen = 0;
}

void GatewayAuthFlow::OnDisconnected(uint64_t connection_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(connection_id);
    if (it == slots_.end())
        return;
    it->second.alive = false;
    it->second.in_flight = false;
    // 保留 slot 直至回调 End，避免 Accept 误判；也可 erase
}

bool GatewayAuthFlow::TryBegin(uint64_t connection_id, uint64_t *flow_gen_out) {
    std::lock_guard<std::mutex> lk(mu_);
    Slot &s = slots_[connection_id];
    if (!s.alive)
        return false;
    if (s.in_flight)
        return false;
    s.in_flight = true;
    ++s.flow_gen;
    if (flow_gen_out)
        *flow_gen_out = s.flow_gen;
    return true;
}

void GatewayAuthFlow::End(uint64_t connection_id, uint64_t flow_gen) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(connection_id);
    if (it == slots_.end())
        return;
    if (it->second.flow_gen != flow_gen)
        return;
    it->second.in_flight = false;
    if (!it->second.alive)
        slots_.erase(it);
}

bool GatewayAuthFlow::AcceptCallback(uint64_t connection_id, uint64_t flow_gen) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(connection_id);
    if (it == slots_.end())
        return false;
    return it->second.alive && it->second.flow_gen == flow_gen;
}

bool GatewayAuthFlow::Alive(uint64_t connection_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(connection_id);
    return it != slots_.end() && it->second.alive;
}
