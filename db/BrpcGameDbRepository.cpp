#include "BrpcGameDbRepository.h"

#include "GameDbRepository.h"
#include "GatewayConfigPath.h"
#include "Logging.h"
#include "BrpcSslUtil.h"
#include "GameMeshPaths.h"
#include "gamedb.pb.h"

#include <brpc/channel.h>

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
    Stop();
    timeout_ms_ = timeout_ms > 0 ? timeout_ms : 3000;
    BrpcSslUtil::SslFiles ssl;
    BrpcSslUtil::LoadFromCnf(GatewayConfigPath::Cnf(), &ssl);
    if (!ssl.enable) {
        std::string gamedb_cnf = "../config/gamedb.cnf";
        std::string resolved;
        if (GameMeshPaths::ResolveProjectSubdir("config/gamedb.cnf", &resolved))
            gamedb_cnf = resolved;
        BrpcSslUtil::LoadFromCnf(gamedb_cnf, &ssl);
    }
    channels_.reserve(addrs.size());
    for (const auto &addr : addrs) {
        if (addr.empty())
            continue;
        auto ch = std::make_unique<brpc::Channel>();
        brpc::ChannelOptions opt;
        opt.protocol = "baidu_std";
        opt.timeout_ms = timeout_ms_;
        opt.max_retry = 0;
        if (BrpcSslUtil::ApplyChannel(&opt, ssl))
            LOG_INFO << "BrpcGameDbRepository SSL enabled";
        if (ch->Init(addr.c_str(), &opt) != 0) {
            LOG_ERROR << "BrpcGameDbRepository: Channel Init failed " << addr;
            channels_.clear();
            return false;
        }
        LOG_INFO << "BrpcGameDbRepository channel ready addr=" << addr;
        channels_.push_back(std::move(ch));
    }
    if (channels_.empty())
        return false;
    started_ = true;
    GameDbRepository::Set(this);
    LOG_INFO << "BrpcGameDbRepository ready channels=" << channels_.size();
    return true;
}

brpc::Channel *BrpcGameDbRepository::ChannelForPlayer(uint64_t player_id) {
    if (channels_.empty())
        return nullptr;
    const size_t idx = static_cast<size_t>(player_id % channels_.size());
    return channels_[idx].get();
}

void BrpcGameDbRepository::Start(int) {}

void BrpcGameDbRepository::Stop() {
    channels_.clear();
    started_ = false;
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
    brpc::Channel *ch = ChannelForPlayer(req.player_id);
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
    gdb::GameDbService_Stub stub(ch);
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
