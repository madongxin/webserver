#include "BrpcGameDbRepository.h"

#include "BrpcSslUtil.h"
#include "GameDbRepository.h"
#include "GameMeshPaths.h"
#include "GatewayConfigPath.h"
#include "Logging.h"
#include "gamedb.pb.h"

#include <brpc/channel.h>
#include <brpc/controller.h>

BrpcGameDbRepository &BrpcGameDbRepository::Instance() {
    static BrpcGameDbRepository g;
    return g;
}

bool BrpcGameDbRepository::Init(const std::string &addr, int timeout_ms) {
    if (addr.empty())
        return false;
    return Init(std::vector<std::string>{addr}, timeout_ms);
}

bool BrpcGameDbRepository::Init(const std::vector<std::string> &addrs, int timeout_ms) {
    if (addrs.empty())
        return false;
    timeout_ms_ = timeout_ms > 0 ? timeout_ms : 3000;
    if (!ApplySnapshot(addrs))
        return false;
    GameDbRepository::Set(this);
    started_ = true;
    LOG_INFO << "BrpcGameDbRepository ready channels=" << channel_count();
    return true;
}

bool BrpcGameDbRepository::ApplySnapshot(const std::vector<std::string> &addrs) {
    if (addrs.empty()) {
        LOG_WARN << "BrpcGameDbRepository: ignore empty ApplySnapshot";
        return started_;
    }
    BrpcSslUtil::SslFiles ssl;
    BrpcSslUtil::LoadFromCnf(GatewayConfigPath::Cnf(), &ssl);
    if (!ssl.enable) {
        std::string gamedb_cnf = "../config/gamedb.cnf";
        std::string resolved;
        if (GameMeshPaths::ResolveProjectSubdir("config/gamedb.cnf", &resolved))
            gamedb_cnf = resolved;
        BrpcSslUtil::LoadFromCnf(gamedb_cnf, &ssl);
    }
    std::vector<std::shared_ptr<brpc::Channel>> next;
    next.reserve(addrs.size());
    for (const auto &addr : addrs) {
        if (addr.empty())
            continue;
        auto ch = std::make_shared<brpc::Channel>();
        brpc::ChannelOptions opt;
        opt.protocol = "baidu_std";
        opt.timeout_ms = timeout_ms_;
        opt.max_retry = 0;  // 非幂等写禁止框架重试
        if (BrpcSslUtil::ApplyChannel(&opt, ssl))
            LOG_INFO << "BrpcGameDbRepository SSL enabled";
        if (ch->Init(addr.c_str(), &opt) != 0) {
            LOG_ERROR << "BrpcGameDbRepository: Channel Init failed " << addr;
            return false;
        }
        LOG_INFO << "BrpcGameDbRepository channel ready addr=" << addr;
        next.push_back(std::move(ch));
    }
    if (next.empty())
        return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        channels_ = std::move(next);
        started_ = true;
    }
    return true;
}

size_t BrpcGameDbRepository::channel_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return channels_.size();
}

std::shared_ptr<brpc::Channel> BrpcGameDbRepository::ChannelForPlayer(uint64_t player_id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (channels_.empty())
        return nullptr;
    const size_t idx = static_cast<size_t>(player_id % channels_.size());
    return channels_[idx];
}

std::shared_ptr<brpc::Channel> BrpcGameDbRepository::ChannelAt(size_t idx) {
    std::lock_guard<std::mutex> lk(mu_);
    if (idx >= channels_.size())
        return nullptr;
    return channels_[idx];
}

void BrpcGameDbRepository::Start(int) {}

void BrpcGameDbRepository::Stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        channels_.clear();
        started_ = false;
    }
    if (GameDbRepository::Get() == this)
        GameDbRepository::Set(nullptr);
}

void BrpcGameDbRepository::ClaimMailAttachmentsAsync(GameDbMailClaimRequest req, MailClaimDone done) {
    auto r = ClaimMailAttachments(std::move(req));
    if (done)
        done(std::move(r));
}

GameDbMailClaimResult BrpcGameDbRepository::ClaimMailAttachments(GameDbMailClaimRequest req) {
    GameDbMailClaimResult out;
    auto ch = ChannelForPlayer(req.player_id);
    if (!ch) {
        out.error_code = "INTERNAL_ERROR";
        out.message = "gamedb channel missing";
        return out;
    }
    gdb::ClaimMailReq rpc_req;
    gdb::ClaimMailRsp rpc_rsp;
    brpc::Controller cntl;
    rpc_req.set_player_id(req.player_id);
    rpc_req.set_mail_id(req.mail_id);
    rpc_req.set_idempotency_key(req.idempotency_key);
    rpc_req.set_trace_id(req.trace_id);
    rpc_req.set_inventory_soft_cap(req.inventory_soft_cap);
    for (const auto &kv : req.bag_snapshot) {
        auto *e = rpc_req.add_bag_snapshot();
        e->set_item_id(kv.first);
        e->set_count(kv.second);
    }
    gdb::GameDbService_Stub stub(ch.get());
    stub.ClaimMailAttachments(&cntl, &rpc_req, &rpc_rsp, nullptr);
    if (cntl.Failed()) {
        out.error_code = "INTERNAL_ERROR";
        out.message = std::string("rpc_failed: ") + cntl.ErrorText();
        return out;
    }
    out.ok = rpc_rsp.ok();
    out.idempotent_hit = rpc_rsp.idempotent_hit();
    out.should_apply_memory = rpc_rsp.should_apply_memory();
    out.error_code = rpc_rsp.error_code();
    out.message = rpc_rsp.message();
    out.attachment_state = rpc_rsp.attachment_state();
    out.mail_row_version = rpc_rsp.mail_row_version();
    for (int i = 0; i < rpc_rsp.grants_size(); ++i) {
        GameDbGrantedItem g;
        g.asset_id = rpc_rsp.grants(i).asset_id();
        g.count = rpc_rsp.grants(i).count();
        out.grants.push_back(g);
    }
    return out;
}

bool BrpcGameDbRepository::LoadPlayer(uint64_t player_id, uint64_t *asset_version, bool *exists,
                                      std::string *err) {
    const size_t n = channel_count();
    for (size_t i = 0; i < n; ++i) {
        auto ch = ChannelAt((static_cast<size_t>(player_id) + i) % n);
        if (!ch)
            continue;
        gdb::LoadPlayerReq req;
        gdb::LoadPlayerRsp rsp;
        brpc::Controller cntl;
        req.set_player_id(player_id);
        gdb::GameDbService_Stub stub(ch.get());
        stub.LoadPlayer(&cntl, &req, &rsp, nullptr);
        if (cntl.Failed()) {
            if (err)
                *err = cntl.ErrorText();
            continue;
        }
        if (!rsp.ok()) {
            if (err)
                *err = rsp.message();
            return false;
        }
        if (asset_version)
            *asset_version = rsp.asset_version();
        if (exists)
            *exists = rsp.exists();
        return true;
    }
    if (err && err->empty())
        *err = "gamedb unavailable";
    return false;
}

bool BrpcGameDbRepository::LoadInventory(uint64_t player_id, std::map<uint32_t, uint32_t> *bag,
                                         uint64_t *version, std::string *err) {
    if (!bag)
        return false;
    const size_t n = channel_count();
    for (size_t i = 0; i < n; ++i) {
        auto ch = ChannelAt((static_cast<size_t>(player_id) + i) % n);
        if (!ch)
            continue;
        gdb::LoadInventoryReq req;
        gdb::LoadInventoryRsp rsp;
        brpc::Controller cntl;
        req.set_player_id(player_id);
        gdb::GameDbService_Stub stub(ch.get());
        stub.LoadInventory(&cntl, &req, &rsp, nullptr);
        if (cntl.Failed()) {
            if (err)
                *err = cntl.ErrorText();
            continue;
        }
        if (!rsp.ok()) {
            if (err)
                *err = rsp.message();
            return false;
        }
        bag->clear();
        for (int j = 0; j < rsp.bag_size(); ++j)
            (*bag)[rsp.bag(j).item_id()] = rsp.bag(j).count();
        if (version)
            *version = rsp.asset_version();
        return true;
    }
    if (err && err->empty())
        *err = "gamedb unavailable";
    return false;
}

bool BrpcGameDbRepository::ApplyAssetMutation(uint64_t player_id, const std::string &idempotency_key,
                                              uint64_t expected_version,
                                              const std::string &mutation_type, uint32_t item_id,
                                              uint32_t count, const std::string &trace_id,
                                              AssetMutationResult *out) {
    if (!out)
        return false;
    *out = AssetMutationResult{};
    auto ch = ChannelForPlayer(player_id);
    if (!ch) {
        out->error_code = "GAMEDB_UNAVAILABLE";
        out->message = "gamedb channel missing";
        return false;
    }
    gdb::AssetMutationReq req;
    gdb::AssetMutationRsp rsp;
    brpc::Controller cntl;
    req.set_player_id(player_id);
    req.set_idempotency_key(idempotency_key);
    req.set_expected_version(expected_version);
    req.set_mutation_type(mutation_type);
    req.set_item_id(item_id);
    req.set_count(count);
    req.set_trace_id(trace_id);
    gdb::GameDbService_Stub stub(ch.get());
    stub.ApplyAssetMutation(&cntl, &req, &rsp, nullptr);
    if (cntl.Failed()) {
        out->error_code = "RPC_FAILED";
        out->message = cntl.ErrorText();
        return false;
    }
    out->ok = rsp.ok();
    out->idempotent_hit = rsp.idempotent_hit();
    out->error_code = rsp.error_code();
    out->message = rsp.message();
    out->asset_version = rsp.asset_version();
    out->remain_count = rsp.remain_count();
    return out->ok;
}

bool BrpcGameDbRepository::SavePlayerSnapshot(uint64_t player_id, uint64_t expected_version,
                                              const std::map<uint32_t, uint32_t> &bag,
                                              const std::string &idempotency_key,
                                              uint64_t *new_version, std::string *err) {
    auto ch = ChannelForPlayer(player_id);
    if (!ch) {
        if (err)
            *err = "gamedb channel missing";
        return false;
    }
    gdb::SavePlayerSnapshotReq req;
    gdb::SavePlayerSnapshotRsp rsp;
    brpc::Controller cntl;
    req.set_player_id(player_id);
    req.set_expected_version(expected_version);
    req.set_idempotency_key(idempotency_key);
    for (const auto &kv : bag) {
        auto *e = req.add_bag();
        e->set_item_id(kv.first);
        e->set_count(kv.second);
    }
    gdb::GameDbService_Stub stub(ch.get());
    stub.SavePlayerSnapshot(&cntl, &req, &rsp, nullptr);
    if (cntl.Failed()) {
        if (err)
            *err = cntl.ErrorText();
        return false;
    }
    if (!rsp.ok()) {
        if (err)
            *err = rsp.error_code().empty() ? rsp.message() : rsp.error_code();
        return false;
    }
    if (new_version)
        *new_version = rsp.asset_version();
    return true;
}

bool BrpcGameDbRepository::FlushPlayer(uint64_t player_id, const std::string &reason,
                                       uint64_t *version, std::string *err) {
    auto ch = ChannelForPlayer(player_id);
    if (!ch) {
        if (err)
            *err = "gamedb channel missing";
        return false;
    }
    gdb::FlushPlayerReq req;
    gdb::FlushPlayerRsp rsp;
    brpc::Controller cntl;
    req.set_player_id(player_id);
    req.set_reason(reason);
    gdb::GameDbService_Stub stub(ch.get());
    stub.FlushPlayer(&cntl, &req, &rsp, nullptr);
    if (cntl.Failed()) {
        if (err)
            *err = cntl.ErrorText();
        return false;
    }
    if (!rsp.ok()) {
        if (err)
            *err = rsp.message();
        return false;
    }
    if (version)
        *version = rsp.asset_version();
    return true;
}
