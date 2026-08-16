#include "GatewayConnRegistry.h"

#include <vector>

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

bool GatewayConnRegistry::CloseIfMatch(uint64_t player_id, const std::string &session_id,
                                       uint64_t generation) {
    if (player_id == 0)
        return false;
    std::vector<std::function<void()>> closers;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto &kv : by_conn_) {
            const Bind &b = kv.second;
            if (b.player_id != player_id)
                continue;
            if (!session_id.empty() && b.session_id != session_id)
                continue;
            if (generation != 0 && b.generation != generation)
                continue;
            if (b.close_conn)
                closers.push_back(b.close_conn);
        }
    }
    for (auto &fn : closers) {
        if (fn)
            fn();
    }
    return !closers.empty();
}

bool GatewayConnRegistry::NotifyAndCloseIfMatch(uint64_t player_id, const std::string &session_id,
                                                uint64_t generation, const std::string &frame,
                                                double grace_sec) {
    if (player_id == 0)
        return false;
    struct Job {
        std::function<void(const std::string &)> send_frame;
        std::function<void()> close_conn;
        std::function<void(double, std::function<void()>)> run_after;
        std::function<void(std::function<void()>)> queue_on_loop;
        uint64_t connection_id = 0;
        std::string session_id;
        uint64_t player_id = 0;
    };
    std::vector<Job> jobs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto &kv : by_conn_) {
            const Bind &b = kv.second;
            if (b.player_id != player_id)
                continue;
            if (!session_id.empty() && b.session_id != session_id)
                continue;
            if (generation != 0 && b.generation != generation)
                continue;
            Job j;
            j.send_frame = b.send_frame;
            j.close_conn = b.close_conn;
            j.run_after = b.run_after;
            j.queue_on_loop = b.queue_on_loop;
            j.connection_id = kv.first;
            j.session_id = b.session_id;
            j.player_id = b.player_id;
            jobs.push_back(std::move(j));
        }
    }
    for (auto &j : jobs) {
        auto closer = j.close_conn;
        auto conn_id = j.connection_id;
        auto sid = j.session_id;
        auto pid = j.player_id;
        auto send = j.send_frame;
        auto run_after = j.run_after;
        auto guarded_close = [this, closer, conn_id, sid, pid]() {
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = by_conn_.find(conn_id);
                if (it == by_conn_.end() || it->second.player_id != pid)
                    return;
                if (!sid.empty() && it->second.session_id != sid)
                    return;
                // session/player 索引可能已指向新连接；仍关闭本 TCP。
                // Forget 只会在索引仍指向本 connection 时删除。
            }
            if (closer)
                closer();
        };
        auto work = [send, frame, grace_sec, run_after, guarded_close]() {
            if (!frame.empty() && send)
                send(frame);
            if (grace_sec > 0.0 && run_after)
                run_after(grace_sec, guarded_close);
            else
                guarded_close();
        };
        if (j.queue_on_loop)
            j.queue_on_loop(std::move(work));
        else
            work();
    }
    return !jobs.empty();
}
