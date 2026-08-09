#include "BrpcChannelManager.h"

#include "Logging.h"

#include <brpc/channel.h>

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

bool BrpcChannelManager::Init(const std::vector<std::string> &addrs,
                              const std::vector<std::string> &instance_ids, int timeout_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    return InitUnlocked(addrs, instance_ids, timeout_ms);
}

bool BrpcChannelManager::InitUnlocked(const std::vector<std::string> &addrs,
                                      const std::vector<std::string> &instance_ids,
                                      int timeout_ms) {
    channels_.clear();
    instance_ids_.clear();
    addrs_.clear();
    id_to_index_.clear();
    if (addrs.empty()) {
        LOG_ERROR << "BrpcChannelManager: empty addrs";
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
        LOG_ERROR << "BrpcChannelManager: instance_ids size mismatch addrs=" << addrs.size()
                  << " ids=" << ids.size();
        return false;
    }
    timeout_ms_ = timeout_ms > 0 ? timeout_ms : 3000;
    channels_.reserve(addrs.size());
    for (size_t i = 0; i < addrs.size(); ++i) {
        auto ch = MakeChannel(addrs[i], timeout_ms_);
        if (!ch) {
            LOG_ERROR << "BrpcChannelManager: Channel::Init failed addr=" << addrs[i];
            channels_.clear();
            id_to_index_.clear();
            instance_ids_.clear();
            addrs_.clear();
            return false;
        }
        LOG_INFO << "BrpcChannelManager: channel ready id=" << ids[i] << " addr=" << addrs[i]
                 << " timeout_ms=" << timeout_ms_;
        id_to_index_[ids[i]] = i;
        channels_.push_back(std::move(ch));
    }
    instance_ids_ = std::move(ids);
    addrs_ = addrs;
    return true;
}

bool BrpcChannelManager::ApplySnapshot(const std::vector<std::string> &addrs,
                                       const std::vector<std::string> &instance_ids) {
    std::lock_guard<std::mutex> lk(mu_);
    if (addrs.empty()) {
        ++empty_snapshot_ignored_;
        LOG_WARN << "BrpcChannelManager: ignore empty ApplySnapshot (keep "
                 << channels_.size() << " channels) ignored_total=" << empty_snapshot_ignored_;
        return false;
    }
    if (channels_.empty())
        return InitUnlocked(addrs, instance_ids, timeout_ms_);

    std::vector<std::string> ids = instance_ids;
    if (ids.empty()) {
        for (size_t i = 0; i < addrs.size(); ++i) {
            std::ostringstream os;
            os << "gl-" << i;
            ids.push_back(os.str());
        }
    } else if (ids.size() != addrs.size()) {
        LOG_ERROR << "BrpcChannelManager: ApplySnapshot size mismatch";
        return false;
    }

    // 旧 id → (addr, channel)；未纳入新快照的 shared_ptr 在函数结束时释放，
    // 若仍有 RPC 持有副本则 Channel 延迟析构。
    std::unordered_map<std::string, std::pair<std::string, std::shared_ptr<brpc::Channel>>> old;
    for (size_t i = 0; i < instance_ids_.size(); ++i)
        old[instance_ids_[i]] = {addrs_[i], channels_[i]};

    channels_.clear();
    instance_ids_.clear();
    addrs_.clear();
    id_to_index_.clear();

    for (size_t i = 0; i < addrs.size(); ++i) {
        std::shared_ptr<brpc::Channel> ch;
        auto it = old.find(ids[i]);
        if (it != old.end() && it->second.first == addrs[i] && it->second.second) {
            ch = it->second.second;
            old.erase(it);
        } else {
            ch = MakeChannel(addrs[i], timeout_ms_);
            if (!ch) {
                LOG_ERROR << "BrpcChannelManager: ApplySnapshot Init failed " << ids[i] << " "
                          << addrs[i];
                continue;
            }
            LOG_INFO << "BrpcChannelManager: hot-add id=" << ids[i] << " addr=" << addrs[i];
        }
        id_to_index_[ids[i]] = channels_.size();
        channels_.push_back(std::move(ch));
        instance_ids_.push_back(ids[i]);
        addrs_.push_back(addrs[i]);
    }
    if (channels_.empty()) {
        LOG_ERROR << "BrpcChannelManager: ApplySnapshot produced empty pool";
        return false;
    }
    return true;
}

void BrpcChannelManager::Shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    channels_.clear();
    instance_ids_.clear();
    addrs_.clear();
    id_to_index_.clear();
}

bool BrpcChannelManager::ready() const {
    std::lock_guard<std::mutex> lk(mu_);
    return !channels_.empty();
}

size_t BrpcChannelManager::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return channels_.size();
}

std::vector<std::string> BrpcChannelManager::instance_ids() const {
    std::lock_guard<std::mutex> lk(mu_);
    return instance_ids_;
}

uint64_t BrpcChannelManager::empty_snapshot_ignored() const {
    std::lock_guard<std::mutex> lk(mu_);
    return empty_snapshot_ignored_;
}

std::shared_ptr<brpc::Channel> BrpcChannelManager::SharedChannelForPlayer(uint64_t player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (channels_.empty())
        return nullptr;
    const size_t idx = static_cast<size_t>(player_id % channels_.size());
    return channels_[idx];
}

std::shared_ptr<brpc::Channel> BrpcChannelManager::SharedChannelForInstance(
    const std::string &gamelogic_instance_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (channels_.empty() || gamelogic_instance_id.empty())
        return nullptr;
    auto it = id_to_index_.find(gamelogic_instance_id);
    if (it == id_to_index_.end())
        return nullptr;
    return channels_[it->second];
}

brpc::Channel *BrpcChannelManager::ChannelForPlayer(uint64_t player_id) {
    return SharedChannelForPlayer(player_id).get();
}

brpc::Channel *BrpcChannelManager::ChannelForInstance(const std::string &gamelogic_instance_id) {
    return SharedChannelForInstance(gamelogic_instance_id).get();
}
