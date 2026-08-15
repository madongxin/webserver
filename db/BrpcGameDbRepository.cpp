#include "BrpcGameDbRepository.h"

#include "BrpcSslUtil.h"
#include "GameDbRepository.h"
#include "GameMeshPaths.h"
#include "GatewayConfigPath.h"
#include "IGameDbRepository.h"
#include "Logging.h"
#include "OpsMetrics.h"
#include "gamedb.pb.h"

#include <brpc/callback.h>
#include <brpc/channel.h>
#include <brpc/controller.h>

#include <atomic>
#include <utility>

BrpcGameDbRepository &BrpcGameDbRepository::Instance() {
    static BrpcGameDbRepository g;
    return g;
}

std::shared_ptr<const GameDbChannelSnapshot> BrpcGameDbRepository::Current() const {
    return std::atomic_load_explicit(&snap_, std::memory_order_acquire);
}

void BrpcGameDbRepository::Publish(std::shared_ptr<GameDbChannelSnapshot> next) {
    if (!next)
        return;
    next->version = version_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::atomic_store_explicit(&snap_, std::shared_ptr<const GameDbChannelSnapshot>(std::move(next)),
                               std::memory_order_release);
}

bool BrpcGameDbRepository::started() const {
    return started_.load(std::memory_order_relaxed);
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
    started_.store(true, std::memory_order_relaxed);
    LOG_INFO << "BrpcGameDbRepository ready channels=" << channel_count();
    return true;
}

bool BrpcGameDbRepository::ApplySnapshot(const std::vector<std::string> &addrs) {
    if (addrs.empty()) {
        LOG_WARN << "BrpcGameDbRepository: ignore empty ApplySnapshot";
        return started();
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

    auto prev = Current();
    auto next = std::make_shared<GameDbChannelSnapshot>();
    next->channels.reserve(addrs.size());
    next->addrs.reserve(addrs.size());
    for (const auto &addr : addrs) {
        if (addr.empty())
            continue;
        std::shared_ptr<brpc::Channel> ch;
        if (prev) {
            for (size_t i = 0; i < prev->addrs.size(); ++i) {
                if (prev->addrs[i] == addr && i < prev->channels.size() && prev->channels[i]) {
                    ch = prev->channels[i];
                    break;
                }
            }
        }
        if (!ch) {
            ch = std::make_shared<brpc::Channel>();
            brpc::ChannelOptions opt;
            opt.protocol = "baidu_std";
            opt.timeout_ms = timeout_ms_;
            opt.max_retry = 0;  // 非幂等写禁止框架重试
            if (BrpcSslUtil::ApplyChannel(&opt, ssl))
                LOG_INFO << "BrpcGameDbRepository SSL enabled";
            if (ch->Init(addr.c_str(), &opt) != 0) {
                LOG_ERROR << "BrpcGameDbRepository: Channel Init failed " << addr
                          << " (abort publish)";
                return false;
            }
            LOG_INFO << "BrpcGameDbRepository channel ready addr=" << addr;
        }
        next->channels.push_back(std::move(ch));
        next->addrs.push_back(addr);
    }
    if (next->channels.empty())
        return false;
    Publish(std::move(next));
    started_.store(true, std::memory_order_relaxed);
    return true;
}

size_t BrpcGameDbRepository::channel_count() const {
    auto s = Current();
    return s ? s->channels.size() : 0;
}

std::shared_ptr<brpc::Channel> BrpcGameDbRepository::ChannelForPlayer(uint64_t player_id) {
    auto s = Current();
    if (!s || s->channels.empty())
        return nullptr;
    const size_t idx = static_cast<size_t>(player_id % s->channels.size());
    return s->channels[idx];
}

std::shared_ptr<brpc::Channel> BrpcGameDbRepository::ChannelAt(size_t idx) {
    auto s = Current();
    if (!s || idx >= s->channels.size())
        return nullptr;
    return s->channels[idx];
}

bool BrpcGameDbRepository::Ping(int timeout_ms) {
    auto ch = ChannelAt(0);
    if (!ch)
        return false;
    gdb::LoadPlayerReq req;
    gdb::LoadPlayerRsp rsp;
    brpc::Controller cntl;
    cntl.set_timeout_ms(timeout_ms > 0 ? timeout_ms : 800);
    req.set_player_id(0);
    gdb::GameDbService_Stub stub(ch.get());
    stub.LoadPlayer(&cntl, &req, &rsp, nullptr);
    return !cntl.Failed();
}

void BrpcGameDbRepository::Start(int) {}

void BrpcGameDbRepository::Stop() {
    Publish(std::make_shared<GameDbChannelSnapshot>());
    started_.store(false, std::memory_order_relaxed);
    if (GameDbRepository::Get() == this)
        GameDbRepository::Set(nullptr);
}

namespace {

struct ClaimMailAsyncCtx {
    std::shared_ptr<brpc::Channel> ch;
    IGameDbRepository::MailClaimDone done;
    brpc::Controller cntl;
    gdb::ClaimMailReq rpc_req;
    gdb::ClaimMailRsp rpc_rsp;
};

void OnClaimMailDone(ClaimMailAsyncCtx *ctx) {
    GameDbMailClaimResult out;
    if (!ctx) {
        return;
    }
    if (ctx->cntl.Failed()) {
        out.error_code = "INTERNAL_ERROR";
        out.message = std::string("rpc_failed: ") + ctx->cntl.ErrorText();
    } else {
        out.ok = ctx->rpc_rsp.ok();
        out.idempotent_hit = ctx->rpc_rsp.idempotent_hit();
        out.should_apply_memory = ctx->rpc_rsp.should_apply_memory();
        out.error_code = ctx->rpc_rsp.error_code();
        out.message = ctx->rpc_rsp.message();
        out.attachment_state = ctx->rpc_rsp.attachment_state();
        out.mail_row_version = ctx->rpc_rsp.mail_row_version();
        out.asset_version = ctx->rpc_rsp.asset_version();
        for (int i = 0; i < ctx->rpc_rsp.grants_size(); ++i) {
            GameDbGrantedItem g;
            g.asset_id = ctx->rpc_rsp.grants(i).asset_id();
            g.count = ctx->rpc_rsp.grants(i).count();
            out.grants.push_back(g);
        }
    }
    if (ctx->done)
        ctx->done(std::move(out));
    delete ctx;
}

}  // namespace

void BrpcGameDbRepository::ClaimMailAttachmentsAsync(GameDbMailClaimRequest req,
                                                     MailClaimDone done) {
    auto ch = ChannelForPlayer(req.player_id);
    if (!ch) {
        GameDbMailClaimResult out;
        out.error_code = "INTERNAL_ERROR";
        out.message = "gamedb channel missing";
        if (done)
            done(std::move(out));
        return;
    }
    auto *ctx = new ClaimMailAsyncCtx();
    ctx->ch = std::move(ch);
    ctx->done = std::move(done);
    ctx->rpc_req.set_player_id(req.player_id);
    ctx->rpc_req.set_mail_id(req.mail_id);
    ctx->rpc_req.set_idempotency_key(req.idempotency_key);
    ctx->rpc_req.set_trace_id(req.trace_id);
    ctx->rpc_req.set_inventory_soft_cap(req.inventory_soft_cap);
    for (const auto &kv : req.bag_snapshot) {
        auto *e = ctx->rpc_req.add_bag_snapshot();
        e->set_item_id(kv.first);
        e->set_count(kv.second);
    }
    gdb::GameDbService_Stub stub(ctx->ch.get());
    stub.ClaimMailAttachments(&ctx->cntl, &ctx->rpc_req, &ctx->rpc_rsp,
                              brpc::NewCallback(&OnClaimMailDone, ctx));
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
    out.asset_version = rpc_rsp.asset_version();
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

namespace {

void GdbProfileToGame(const gdb::PlayerProfile &in, game::PlayerAttributes *out) {
    if (!out)
        return;
    out->set_player_id(in.player_id());
    out->set_player_name(in.player_name());
    out->set_hp(in.hp());
    out->set_max_hp(in.max_hp());
    out->set_mp(in.mp());
    out->set_max_mp(in.max_mp());
    out->set_attack(in.attack());
    out->set_spell_power(in.spell_power());
    out->set_defense(in.defense());
    out->set_magic_resistance(in.magic_resistance());
    out->set_crit_chance(in.crit_chance());
    out->set_crit_damage(in.crit_damage());
    out->set_move_speed(in.move_speed());
    out->set_attack_speed(in.attack_speed());
    out->set_stats_version(in.stats_version());
}

}  // namespace

bool BrpcGameDbRepository::LoadPlayerProfile(uint64_t player_id, bool ensure_default,
                                             game::PlayerAttributes *out, std::string *err) {
    if (!out)
        return false;
    const size_t n = channel_count();
    for (size_t i = 0; i < n; ++i) {
        auto ch = ChannelAt((static_cast<size_t>(player_id) + i) % n);
        if (!ch)
            continue;
        gdb::LoadPlayerProfileReq req;
        gdb::LoadPlayerProfileRsp rsp;
        brpc::Controller cntl;
        req.set_player_id(player_id);
        req.set_ensure_default(ensure_default);
        gdb::GameDbService_Stub stub(ch.get());
        stub.LoadPlayerProfile(&cntl, &req, &rsp, nullptr);
        if (cntl.Failed()) {
            if (err)
                *err = cntl.ErrorText();
            continue;
        }
        if (!rsp.ok()) {
            if (err)
                *err = rsp.message().empty() ? rsp.error_code() : rsp.message();
            return false;
        }
        if (!rsp.exists()) {
            if (err)
                *err = "ERR_PROFILE_NOT_FOUND";
            return false;
        }
        GdbProfileToGame(rsp.profile(), out);
        return true;
    }
    if (err && err->empty())
        *err = "gamedb unavailable";
    return false;
}

bool BrpcGameDbRepository::QueryOperationResult(uint64_t player_id,
                                                const std::string &idempotency_key,
                                                const std::string &operation_type, bool *found,
                                                bool *completed_ok, bool *idempotent_hit,
                                                uint64_t *asset_version, uint32_t *remain_count,
                                                std::string *err, std::string *status) {
    const size_t n = channel_count();
    for (size_t i = 0; i < n; ++i) {
        auto ch = ChannelAt((static_cast<size_t>(player_id) + i) % n);
        if (!ch)
            continue;
        gdb::QueryOperationResultReq req;
        gdb::QueryOperationResultRsp rsp;
        brpc::Controller cntl;
        req.set_player_id(player_id);
        req.set_idempotency_key(idempotency_key);
        req.set_operation_type(operation_type);
        gdb::GameDbService_Stub stub(ch.get());
        stub.QueryOperationResult(&cntl, &req, &rsp, nullptr);
        if (cntl.Failed()) {
            if (err)
                *err = cntl.ErrorText();
            continue;
        }
        if (!rsp.ok()) {
            if (err)
                *err = rsp.message();
            continue;
        }
        if (found)
            *found = rsp.found();
        if (completed_ok)
            *completed_ok = rsp.completed_ok();
        if (idempotent_hit)
            *idempotent_hit = rsp.idempotent_hit();
        if (asset_version)
            *asset_version = rsp.asset_version();
        if (remain_count)
            *remain_count = rsp.remain_count();
        if (status) {
            if (!rsp.status().empty())
                *status = rsp.status();
            else if (!rsp.found())
                *status = "NOT_FOUND";
            else if (rsp.completed_ok())
                *status = "SUCCEEDED";
            else if (rsp.error_code() == "IN_PROGRESS")
                *status = "IN_PROGRESS";
            else
                *status = "FAILED";
        }
        return true;
    }
    if (err && err->empty())
        *err = "gamedb query unavailable";
    return false;
}

bool BrpcGameDbRepository::FillMutationFromQuery(uint64_t player_id,
                                                 const std::string &idempotency_key,
                                                 const std::string &mutation_type,
                                                 AssetMutationResult *out) {
    bool found = false;
    bool completed_ok = false;
    bool idempotent_hit = false;
    uint64_t ver = 0;
    uint32_t remain = 0;
    std::string err;
    std::string status;
    if (!QueryOperationResult(player_id, idempotency_key, mutation_type, &found, &completed_ok,
                              &idempotent_hit, &ver, &remain, &err, &status))
        return false;
    if (!found || status == "NOT_FOUND")
        return false;
    if (status == "IN_PROGRESS") {
        out->ok = false;
        out->idempotent_hit = true;
        out->unknown_result = true;
        out->error_code = "IN_PROGRESS";
        out->message = "operation still in progress";
        out->asset_version = ver;
        out->remain_count = remain;
        return true;
    }
    out->ok = completed_ok || status == "SUCCEEDED";
    out->idempotent_hit = idempotent_hit || out->ok;
    out->unknown_result = false;
    out->asset_version = ver;
    out->remain_count = remain;
    out->error_code = out->ok ? "" : (err.empty() ? "OPERATION_FAILED" : err);
    out->message = out->ok ? "recovered_from_query" : "operation recorded as failed";
    return true;
}

bool BrpcGameDbRepository::ApplyAssetMutation(uint64_t player_id, const std::string &idempotency_key,
                                              uint64_t expected_version,
                                              const std::string &mutation_type, uint32_t item_id,
                                              uint32_t count, const std::string &trace_id,
                                              AssetMutationResult *out) {
    if (!out)
        return false;
    *out = AssetMutationResult{};
    const size_t n = channel_count();
    if (n == 0) {
        out->error_code = "GAMEDB_UNAVAILABLE";
        out->message = "gamedb channel missing";
        return false;
    }

    gdb::AssetMutationReq req;
    req.set_player_id(player_id);
    req.set_idempotency_key(idempotency_key);
    req.set_expected_version(expected_version);
    req.set_mutation_type(mutation_type);
    req.set_item_id(item_id);
    req.set_count(count);
    req.set_trace_id(trace_id);

    bool saw_rpc_fail = false;
    std::string last_rpc_err;
    for (size_t i = 0; i < n; ++i) {
        auto ch = ChannelAt((static_cast<size_t>(player_id) + i) % n);
        if (!ch)
            continue;
        gdb::AssetMutationRsp rsp;
        brpc::Controller cntl;
        gdb::GameDbService_Stub stub(ch.get());
        stub.ApplyAssetMutation(&cntl, &req, &rsp, nullptr);
        if (cntl.Failed()) {
            saw_rpc_fail = true;
            last_rpc_err = cntl.ErrorText();
            // 未知结果：先查幂等记录再决定是否换节点重试
            if (FillMutationFromQuery(player_id, idempotency_key, mutation_type, out)) {
                if (out->unknown_result) {
                    OpsMetrics::Instance().IncGamedbUnknownResult();
                    return false;
                }
                return out->ok;
            }
            continue;
        }
        out->ok = rsp.ok();
        out->idempotent_hit = rsp.idempotent_hit();
        out->error_code = rsp.error_code();
        out->message = rsp.message();
        out->asset_version = rsp.asset_version();
        out->remain_count = rsp.remain_count();
        return out->ok;
    }

    if (saw_rpc_fail && FillMutationFromQuery(player_id, idempotency_key, mutation_type, out)) {
        if (out->unknown_result) {
            OpsMetrics::Instance().IncGamedbUnknownResult();
            return false;
        }
        return out->ok;
    }

    out->unknown_result = saw_rpc_fail;
    out->error_code = saw_rpc_fail ? "UNKNOWN_RESULT" : "GAMEDB_UNAVAILABLE";
    out->message = saw_rpc_fail ? last_rpc_err : "gamedb unavailable";
    if (saw_rpc_fail)
        OpsMetrics::Instance().IncGamedbUnknownResult();
    return false;
}

bool BrpcGameDbRepository::SavePlayerSnapshot(uint64_t player_id, uint64_t expected_version,
                                              const std::map<uint32_t, uint32_t> &bag,
                                              const std::string &idempotency_key,
                                              uint64_t *new_version, std::string *err) {
    const size_t n = channel_count();
    if (n == 0) {
        if (err)
            *err = "gamedb channel missing";
        return false;
    }
    gdb::SavePlayerSnapshotReq req;
    req.set_player_id(player_id);
    req.set_expected_version(expected_version);
    req.set_idempotency_key(idempotency_key);
    for (const auto &kv : bag) {
        auto *e = req.add_bag();
        e->set_item_id(kv.first);
        e->set_count(kv.second);
    }

    bool saw_rpc_fail = false;
    std::string last_err;
    for (size_t i = 0; i < n; ++i) {
        auto ch = ChannelAt((static_cast<size_t>(player_id) + i) % n);
        if (!ch)
            continue;
        gdb::SavePlayerSnapshotRsp rsp;
        brpc::Controller cntl;
        gdb::GameDbService_Stub stub(ch.get());
        stub.SavePlayerSnapshot(&cntl, &req, &rsp, nullptr);
        if (cntl.Failed()) {
            saw_rpc_fail = true;
            last_err = cntl.ErrorText();
            bool found = false, cok = false, ih = false;
            uint64_t ver = 0;
            uint32_t remain = 0;
            std::string qerr;
            if (QueryOperationResult(player_id, idempotency_key, "SAVE_SNAPSHOT", &found, &cok, &ih,
                                     &ver, &remain, &qerr) &&
                found) {
                if (cok && new_version)
                    *new_version = ver;
                return cok;
            }
            continue;
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
    if (err)
        *err = saw_rpc_fail ? ("UNKNOWN_RESULT: " + last_err) : "gamedb unavailable";
    return false;
}

bool BrpcGameDbRepository::FlushPlayer(uint64_t player_id, const std::string &reason,
                                       uint64_t *version, std::string *err) {
    const size_t n = channel_count();
    for (size_t i = 0; i < n; ++i) {
        auto ch = ChannelAt((static_cast<size_t>(player_id) + i) % n);
        if (!ch)
            continue;
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
            continue;
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
    if (err && err->empty())
        *err = "gamedb unavailable";
    return false;
}

bool BrpcGameDbRepository::HandleGameFrame(uint64_t player_id, const std::string &request_payload,
                                           std::string *response_frame, std::string *err) {
    if (!response_frame)
        return false;
    const size_t n = channel_count();
    for (size_t i = 0; i < n; ++i) {
        auto ch = ChannelAt((static_cast<size_t>(player_id) + i) % n);
        if (!ch)
            continue;
        gdb::HandleGameFrameReq req;
        gdb::HandleGameFrameRsp rsp;
        brpc::Controller cntl;
        req.set_player_id(player_id);
        req.set_request_payload(request_payload);
        gdb::GameDbService_Stub stub(ch.get());
        stub.HandleGameFrame(&cntl, &req, &rsp, nullptr);
        if (cntl.Failed()) {
            if (err)
                *err = cntl.ErrorText();
            continue;
        }
        if (!rsp.ok()) {
            if (err)
                *err = rsp.message().empty() ? "handle_frame_failed" : rsp.message();
            if (!rsp.response_frame().empty())
                *response_frame = rsp.response_frame();
            return false;
        }
        *response_frame = rsp.response_frame();
        return true;
    }
    if (err && err->empty())
        *err = "gamedb unavailable";
    return false;
}
