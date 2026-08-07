#include "GameDbServiceImpl.h"

#include "AsyncMysqlGameDbRepository.h"
#include "IGameDbRepository.h"
#include "PlayerAccountStore.h"

#include <brpc/controller.h>

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
    const bool exists = PlayerAccountStore::Instance().Exists(request->player_id());
    response->set_ok(true);
    response->set_exists(exists);
    response->set_banned(false);
    response->set_account_id(request->player_id());
    response->set_player_id(request->player_id());
    response->set_message(exists ? "ok" : "not found");
    if (!exists)
        response->set_error_code("ACCOUNT_NOT_FOUND");
#else
    response->set_ok(false);
    response->set_error_code("MYSQL_DISABLED");
    response->set_message("mysql not enabled");
#endif
}
