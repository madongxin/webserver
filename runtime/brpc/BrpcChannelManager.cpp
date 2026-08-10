#include "BrpcChannelManager.h"

#include "Logging.h"

#include <brpc/channel.h>

#include <atomic>
#include <sstream>

BrpcChannelManager &BrpcChannelManager::Instance() {
    static BrpcChannelManager g;
    return g;
}

BrpcChannelManager::~BrpcChannelManager() {
    Shutdown();
}

std::shared_ptr<brpc::Channel> BrpcChannelManager::MakeChannel(const std::string &addr,
                                                              int timeout_ms) {
    auto ch = std::make_shared<brpc::Channel>();
    brpc::ChannelOptions opt;
    opt.protocol = "baidu_std";
    opt.timeout_ms = timeout_ms;
    opt.max_retry = 0;
    if (ch->Init(addr.c_str(), &opt) != 0)
        return nullptr;
    return ch;
}

std::shared_ptr<const LogicChannelPoolSnapshot> BrpcChannelManager::Current() const {
    return std::atomic_load_explicit(&snap_, std::memory_order_acquire);
}

void BrpcChannelManager::Publish(std::shared_ptr<LogicChannelPoolSnapshot> next) {
    if (!next)
        return;
    next->version = version_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::atomic_store_explicit(&snap_,
                               std::shared_ptr<const LogicChannelPoolSnapshot>(std::move(next)),
                               std::memory_order_release);
}

bool BrpcChannelManager::Init(const std::vector<std::string> &addrs,
                              const std::vector<std::string> &instance_ids, int timeout_ms) {
    timeout_ms_ = timeout_ms > 0 ? timeout_ms : 3000;
    return ApplySnapshot(addrs, instance_ids);
}

bool BrpcChannelManager::ApplySnapshot(const std::vector<std::string> &addrs,
                                       const std::vector<std::string> &instance_ids) {
    if (addrs.empty()) {
        empty_snapshot_ignored_.fetch_add(1, std::memory_order_relaxed);
        auto cur = Current();
        LOG_WARN << "BrpcChannelManager: ignore empty ApplySnapshot (keep "
                 << (cur ? cur->channels.size() : 0)
                 << " channels) ignored_total=" << empty_snapshot_ignored_.load();
        return false;
    }

    std::vector<std::string> ids = instance_ids;
    if (ids.empty()) {
        for (size_t i = 0; i < addrs.size(); ++i) {
            std::ostringstream os;
            os << "gl-" << i;
            ids.push_back(os.str());
        }
    } else if (ids.size() != addrs.size()) {
        LOG_ERROR << "BrpcChannelManager: ApplySnapshot size mismatch (keep last snapshot)";
        return false;
    }

    auto prev = Current();
    auto next = std::make_shared<LogicChannelPoolSnapshot>();
    next->timeout_ms = timeout_ms_;
    next->channels.reserve(addrs.size());
    next->instance_ids.reserve(addrs.size());
    next->addrs.reserve(addrs.size());

    for (size_t i = 0; i < addrs.size(); ++i) {
        std::shared_ptr<brpc::Channel> ch;
        if (prev) {
            auto it = prev->id_to_index.find(ids[i]);
            if (it != prev->id_to_index.end() && it->second < prev->addrs.size() &&
                prev->addrs[it->second] == addrs[i] && it->second < prev->channels.size()) {
                ch = prev->channels[it->second];
            }
        }
        if (!ch) {
            ch = MakeChannel(addrs[i], timeout_ms_);
            if (!ch) {
                LOG_ERROR << "BrpcChannelManager: Channel::Init failed id=" << ids[i] << " "
                          << addrs[i] << " (skip)";
                continue;
            }
            LOG_INFO << "BrpcChannelManager: channel ready id=" << ids[i] << " addr=" << addrs[i]
                     << " timeout_ms=" << timeout_ms_;
        }
        next->id_to_index[ids[i]] = next->channels.size();
        next->channels.push_back(std::move(ch));
        next->instance_ids.push_back(ids[i]);
        next->addrs.push_back(addrs[i]);
    }
    if (next->channels.empty()) {
        LOG_ERROR << "BrpcChannelManager: ApplySnapshot produced empty pool (keep last)";
        return false;
    }
    Publish(std::move(next));
    return true;
}

void BrpcChannelManager::Shutdown() {
    Publish(std::make_shared<LogicChannelPoolSnapshot>());
}

bool BrpcChannelManager::ready() const {
    auto s = Current();
    return s && !s->channels.empty();
}

size_t BrpcChannelManager::size() const {
    auto s = Current();
    return s ? s->channels.size() : 0;
}

std::vector<std::string> BrpcChannelManager::instance_ids() const {
    auto s = Current();
    return s ? s->instance_ids : std::vector<std::string>{};
}

uint64_t BrpcChannelManager::empty_snapshot_ignored() const {
    return empty_snapshot_ignored_.load(std::memory_order_relaxed);
}

uint64_t BrpcChannelManager::snapshot_version() const {
    auto s = Current();
    return s ? s->version : 0;
}

std::shared_ptr<brpc::Channel> BrpcChannelManager::SharedChannelForPlayer(uint64_t player_id) {
    auto s = Current();
    if (!s || s->channels.empty())
        return nullptr;
    const size_t idx = static_cast<size_t>(player_id % s->channels.size());
    return s->channels[idx];
}

std::shared_ptr<brpc::Channel> BrpcChannelManager::SharedChannelForInstance(
    const std::string &gamelogic_instance_id) {
    auto s = Current();
    if (!s || s->channels.empty() || gamelogic_instance_id.empty())
        return nullptr;
    auto it = s->id_to_index.find(gamelogic_instance_id);
    if (it == s->id_to_index.end())
        return nullptr;
    return s->channels[it->second];
}

brpc::Channel *BrpcChannelManager::ChannelForPlayer(uint64_t player_id) {
    return SharedChannelForPlayer(player_id).get();
}

brpc::Channel *BrpcChannelManager::ChannelForInstance(const std::string &gamelogic_instance_id) {
    return SharedChannelForInstance(gamelogic_instance_id).get();
}
