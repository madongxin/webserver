#include "GameDbServiceImpl.h"

#include "AsyncMysqlGameDbRepository.h"
#include "GameDbAssetStore.h"
#include "IGameDbRepository.h"
#include "PlayerAccountStore.h"

#include <brpc/controller.h>

#include <map>

void GameDbServiceImpl::ClaimMailAttachments(::google::protobuf::RpcController *controller,
                                             const ::gdb::ClaimMailReq *request,
                                             ::gdb::ClaimMailRsp *response,
                                             ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    GameDbMailClaimRequest req;
    req.player_id = request->player_id();
    req.mail_id = request->mail_id();
    req.idempotency_key = request->idempotency_key();
    req.trace_id = request->trace_id();
    req.inventory_soft_cap = request->inventory_soft_cap() > 0 ? request->inventory_soft_cap()
                                                               : 999999;
    for (int i = 0; i < request->bag_snapshot_size(); ++i)
        req.bag_snapshot[request->bag_snapshot(i).item_id()] = request->bag_snapshot(i).count();

    auto result = AsyncMysqlGameDbRepository::Instance().ClaimMailAttachments(std::move(req));
    response->set_ok(result.ok);
    response->set_idempotent_hit(result.idempotent_hit);
    response->set_should_apply_memory(result.should_apply_memory);
    response->set_error_code(result.error_code);
    response->set_message(result.message);
    response->set_attachment_state(result.attachment_state);
    response->set_mail_row_version(result.mail_row_version);
    for (const auto &g : result.grants) {
        auto *out = response->add_grants();
        out->set_asset_id(g.asset_id);
        out->set_count(g.count);
    }
}

void GameDbServiceImpl::LookupAccount(::google::protobuf::RpcController *controller,
                                      const ::gdb::LookupAccountReq *request,
                                      ::gdb::LookupAccountRsp *response,
                                      ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->player_id() == 0) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        response->set_message("player_id required");
        return;
    }
    (void)request->device_id();
#ifdef WEBSERVER_ENABLE_MYSQL
    if (!PlayerAccountStore::Instance().Available())
        PlayerAccountStore::Instance().EnsureTable();
    AccountAuthRow row;
    if (!PlayerAccountStore::Instance().LoadAuth(request->player_id(), &row)) {
        response->set_ok(false);
        response->set_error_code("DB_ERROR");
        response->set_message("load auth failed");
        return;
    }
    response->set_ok(true);
    response->set_exists(row.exists);
    response->set_banned(row.banned);
    response->set_account_id(row.account_id ? row.account_id : request->player_id());
    response->set_player_id(request->player_id());
    response->set_password_hash(row.password_hash);
    response->set_password_salt(row.password_salt);
    response->set_password_iters(row.password_iters);
    response->set_has_password(row.has_password);
    response->set_message(row.exists ? "ok" : "not found");
    if (!row.exists)
        response->set_error_code("ACCOUNT_NOT_FOUND");
#else
    response->set_ok(false);
    response->set_error_code("MYSQL_DISABLED");
    response->set_message("mysql not enabled");
#endif
}

void GameDbServiceImpl::RegisterAccount(::google::protobuf::RpcController *controller,
                                        const ::gdb::RegisterAccountReq *request,
                                        ::gdb::RegisterAccountRsp *response,
                                        ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->device_id().empty() || request->password_hash().empty() ||
        request->password_salt().empty() || request->password_iters() <= 0) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        response->set_message("device_id and password hash fields required");
        return;
    }
#ifdef WEBSERVER_ENABLE_MYSQL
    uint64_t player_id = 0;
    std::string err;
    bool replayed = false;
    if (!PlayerAccountStore::Instance().RegisterWithPasswordIdempotent(
            request->device_id(), request->display_name(), request->password_hash(),
            request->password_salt(), request->password_iters(), request->idempotency_key(),
            &player_id, &err, &replayed)) {
        response->set_ok(false);
        response->set_error_code("REGISTER_FAILED");
        response->set_message(err);
        return;
    }
    response->set_ok(true);
    response->set_message(replayed ? "ok replay" : "ok");
    response->set_player_id(player_id);
    response->set_account_id(player_id);
#else
    response->set_ok(false);
    response->set_error_code("MYSQL_DISABLED");
    response->set_message("mysql not enabled");
#endif
}

void GameDbServiceImpl::LoadPlayer(::google::protobuf::RpcController *controller,
                                   const ::gdb::LoadPlayerReq *request,
                                   ::gdb::LoadPlayerRsp *response,
                                   ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->player_id() == 0) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        response->set_message("player_id required");
        return;
    }
#ifdef WEBSERVER_ENABLE_MYSQL
    uint64_t ver = 0;
    bool exists = false;
    if (!GameDbAssetStore::Instance().LoadMeta(request->player_id(), &ver, &exists)) {
        response->set_ok(false);
        response->set_error_code("DB_UNAVAILABLE");
        response->set_message("load meta failed");
        return;
    }
    response->set_ok(true);
    response->set_player_id(request->player_id());
    response->set_exists(exists);
    response->set_asset_version(exists ? ver : 0);
    response->set_message("ok");
#else
    response->set_ok(false);
    response->set_error_code("MYSQL_DISABLED");
    response->set_message("mysql not enabled");
#endif
}

void GameDbServiceImpl::LoadInventory(::google::protobuf::RpcController *controller,
                                      const ::gdb::LoadInventoryReq *request,
                                      ::gdb::LoadInventoryRsp *response,
                                      ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->player_id() == 0) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        response->set_message("player_id required");
        return;
    }
#ifdef WEBSERVER_ENABLE_MYSQL
    std::map<uint32_t, uint32_t> bag;
    uint64_t ver = 0;
    if (!GameDbAssetStore::Instance().LoadInventory(request->player_id(), &bag, &ver)) {
        response->set_ok(false);
        response->set_error_code("DB_UNAVAILABLE");
        response->set_message("load inventory failed");
        return;
    }
    response->set_ok(true);
    response->set_player_id(request->player_id());
    response->set_asset_version(ver);
    response->set_message("ok");
    for (const auto &kv : bag) {
        auto *e = response->add_bag();
        e->set_item_id(kv.first);
        e->set_count(kv.second);
    }
#else
    response->set_ok(false);
    response->set_error_code("MYSQL_DISABLED");
    response->set_message("mysql not enabled");
#endif
}

void GameDbServiceImpl::ApplyAssetMutation(::google::protobuf::RpcController *controller,
                                           const ::gdb::AssetMutationReq *request,
                                           ::gdb::AssetMutationRsp *response,
                                           ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        return;
    }
#ifdef WEBSERVER_ENABLE_MYSQL
    (void)request->trace_id();
    GameDbAssetStore::MutationResult r;
    if (!GameDbAssetStore::Instance().ApplyMutation(
            request->player_id(), request->idempotency_key(), request->expected_version(),
            request->mutation_type(), request->item_id(), request->count(), &r)) {
        response->set_ok(false);
        response->set_idempotent_hit(r.idempotent_hit);
        response->set_error_code(r.error_code.empty() ? "MUTATION_FAILED" : r.error_code);
        response->set_message(r.message);
        response->set_asset_version(r.asset_version);
        response->set_remain_count(r.remain_count);
        return;
    }
    response->set_ok(true);
    response->set_idempotent_hit(r.idempotent_hit);
    response->set_message(r.message);
    response->set_asset_version(r.asset_version);
    response->set_remain_count(r.remain_count);
#else
    response->set_ok(false);
    response->set_error_code("MYSQL_DISABLED");
    response->set_message("mysql not enabled");
#endif
}

void GameDbServiceImpl::SavePlayerSnapshot(::google::protobuf::RpcController *controller,
                                           const ::gdb::SavePlayerSnapshotReq *request,
                                           ::gdb::SavePlayerSnapshotRsp *response,
                                           ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->player_id() == 0) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        return;
    }
#ifdef WEBSERVER_ENABLE_MYSQL
    std::map<uint32_t, uint32_t> bag;
    for (int i = 0; i < request->bag_size(); ++i)
        bag[request->bag(i).item_id()] = request->bag(i).count();
    uint64_t nv = 0;
    std::string err;
    if (!GameDbAssetStore::Instance().SaveSnapshot(request->player_id(), request->expected_version(),
                                                   bag, request->idempotency_key(), &nv, &err)) {
        response->set_ok(false);
        response->set_error_code(err.empty() ? "SAVE_FAILED" : err);
        response->set_message(err);
        return;
    }
    response->set_ok(true);
    response->set_asset_version(nv);
    response->set_message("ok");
#else
    response->set_ok(false);
    response->set_error_code("MYSQL_DISABLED");
    response->set_message("mysql not enabled");
#endif
}

void GameDbServiceImpl::FlushPlayer(::google::protobuf::RpcController *controller,
                                    const ::gdb::FlushPlayerReq *request,
                                    ::gdb::FlushPlayerRsp *response,
                                    ::google::protobuf::Closure *done) {
    (void)controller;
    brpc::ClosureGuard done_guard(done);
    response->Clear();
    if (!request || request->player_id() == 0) {
        response->set_ok(false);
        response->set_error_code("INVALID_ARG");
        return;
    }
#ifdef WEBSERVER_ENABLE_MYSQL
    (void)request->reason();
    uint64_t ver = 0;
    bool exists = false;
    if (!GameDbAssetStore::Instance().LoadMeta(request->player_id(), &ver, &exists)) {
        response->set_ok(false);
        response->set_error_code("DB_UNAVAILABLE");
        response->set_message("flush meta failed");
        return;
    }
    response->set_ok(true);
    response->set_asset_version(exists ? ver : 0);
    response->set_message("flushed");
#else
    response->set_ok(false);
    response->set_error_code("MYSQL_DISABLED");
    response->set_message("mysql not enabled");
#endif
}
