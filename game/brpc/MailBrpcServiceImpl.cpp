/**
 * @file MailBrpcServiceImpl.cpp
 * @brief brpc 适配层：ValidateToken → MailService::Handle* → 抽出 *Rsp
 */

#include "MailBrpcServiceImpl.h"

#include "FormalMode.h"
#include "MailService.h"

#include <brpc/controller.h>

#ifdef WEBSERVER_ENABLE_REDIS
#include "SessionStore.h"
#endif

namespace {

bool RequireToken(uint64_t player_id, const std::string &token, std::string *err) {
#ifdef WEBSERVER_ENABLE_REDIS
    if (!SessionStore::Instance().Available()) {
        // 联调无 Redis 仍放行；正式模式失败关闭，避免会话故障变成越权读信/领奖。
        if (FormalModeEnabled()) {
            if (err)
                *err = "session unavailable";
            return false;
        }
        return true;
    }
    return SessionStore::Instance().ValidateToken(player_id, token, err);
#else
    (void)player_id;
    (void)token;
    if (FormalModeEnabled()) {
        if (err)
            *err = "session unavailable";
        return false;
    }
    (void)err;
    return true;
#endif
}

template <typename AuthedReq, typename RspBody>
bool AuthOrFail(const AuthedReq *authed, uint64_t player_id, RspBody *rsp, std::string *err) {
    if (!authed || !rsp)
        return false;
    if (!RequireToken(player_id, authed->session_token(), err)) {
        rsp->set_ok(false);
        rsp->set_message(err && !err->empty() ? *err : "session invalid");
        rsp->set_error_code("SESSION_INVALID");
        return false;
    }
    return true;
}

}  // namespace

void MailBrpcServiceImpl::Login(::google::protobuf::RpcController *controller,
                                const ::game::LoginReq *request, ::game::LoginRsp *response,
                                ::google::protobuf::Closure *done) {
    brpc::ClosureGuard done_guard(done);
    (void)controller;
    if (!request || !response) {
        if (response) {
            response->set_ok(false);
            response->set_message("null request");
        }
        return;
    }
#ifdef WEBSERVER_ENABLE_REDIS
    if (!SessionStore::Instance().Available()) {
        response->set_ok(false);
        response->set_message("redis session not available");
        return;
    }
    SessionStore::Instance().Login(*request, response);
#else
    response->set_ok(false);
    response->set_message("redis not enabled");
#endif
}

void MailBrpcServiceImpl::MailboxSummary(::google::protobuf::RpcController *controller,
                                         const ::mailrpc::AuthedMailboxSummaryReq *request,
                                         ::game::MailboxSummaryRsp *response,
                                         ::google::protobuf::Closure *done) {
    brpc::ClosureGuard done_guard(done);
    (void)controller;
    if (!request || !response)
        return;
    std::string err;
    if (!AuthOrFail(request, request->req().player_id(), response, &err))
        return;
    game::GameResponse gr;
    MailService::Instance().HandleMailboxSummary(request->req(), &gr);
    if (gr.has_mailbox_summary())
        *response = gr.mailbox_summary();
    else {
        response->set_ok(false);
        response->set_message(gr.message());
    }
}

void MailBrpcServiceImpl::MailList(::google::protobuf::RpcController *controller,
                                   const ::mailrpc::AuthedMailListReq *request,
                                   ::game::MailListRsp *response,
                                   ::google::protobuf::Closure *done) {
    brpc::ClosureGuard done_guard(done);
    (void)controller;
    if (!request || !response)
        return;
    std::string err;
    if (!AuthOrFail(request, request->req().player_id(), response, &err))
        return;
    game::GameResponse gr;
    MailService::Instance().HandleMailList(request->req(), &gr);
    if (gr.has_mail_list())
        *response = gr.mail_list();
    else {
        response->set_ok(false);
        response->set_message(gr.message());
    }
}

void MailBrpcServiceImpl::MailGet(::google::protobuf::RpcController *controller,
                                  const ::mailrpc::AuthedMailGetReq *request,
                                  ::game::MailGetRsp *response,
                                  ::google::protobuf::Closure *done) {
    brpc::ClosureGuard done_guard(done);
    (void)controller;
    if (!request || !response)
        return;
    std::string err;
    if (!AuthOrFail(request, request->req().player_id(), response, &err))
        return;
    game::GameResponse gr;
    MailService::Instance().HandleMailGet(request->req(), &gr);
    if (gr.has_mail_get())
        *response = gr.mail_get();
    else {
        response->set_ok(false);
        response->set_message(gr.message());
    }
}

void MailBrpcServiceImpl::MailRead(::google::protobuf::RpcController *controller,
                                   const ::mailrpc::AuthedMailReadReq *request,
                                   ::game::MailReadRsp *response,
                                   ::google::protobuf::Closure *done) {
    brpc::ClosureGuard done_guard(done);
    (void)controller;
    if (!request || !response)
        return;
    std::string err;
    if (!AuthOrFail(request, request->req().player_id(), response, &err))
        return;
    game::GameResponse gr;
    MailService::Instance().HandleMailRead(request->req(), &gr);
    if (gr.has_mail_read())
        *response = gr.mail_read();
    else {
        response->set_ok(false);
        response->set_message(gr.message());
    }
}

void MailBrpcServiceImpl::MailClaim(::google::protobuf::RpcController *controller,
                                    const ::mailrpc::AuthedMailClaimReq *request,
                                    ::game::MailClaimRsp *response,
                                    ::google::protobuf::Closure *done) {
    brpc::ClosureGuard done_guard(done);
    (void)controller;
    if (!request || !response)
        return;
    std::string err;
    if (!AuthOrFail(request, request->req().player_id(), response, &err))
        return;
    game::GameResponse gr;
    MailService::Instance().HandleMailClaim(request->req(), &gr);
    if (gr.has_mail_claim())
        *response = gr.mail_claim();
    else {
        response->set_ok(false);
        response->set_message(gr.message());
    }
}

void MailBrpcServiceImpl::MailBatchClaim(::google::protobuf::RpcController *controller,
                                         const ::mailrpc::AuthedMailBatchClaimReq *request,
                                         ::game::MailBatchClaimRsp *response,
                                         ::google::protobuf::Closure *done) {
    brpc::ClosureGuard done_guard(done);
    (void)controller;
    if (!request || !response)
        return;
    std::string err;
    if (!AuthOrFail(request, request->req().player_id(), response, &err))
        return;
    game::GameResponse gr;
    MailService::Instance().HandleMailBatchClaim(request->req(), &gr);
    if (gr.has_mail_batch_claim())
        *response = gr.mail_batch_claim();
    else {
        response->set_ok(false);
        response->set_message(gr.message());
    }
}

void MailBrpcServiceImpl::MailFavorite(::google::protobuf::RpcController *controller,
                                       const ::mailrpc::AuthedMailFavoriteReq *request,
                                       ::game::MailFavoriteRsp *response,
                                       ::google::protobuf::Closure *done) {
    brpc::ClosureGuard done_guard(done);
    (void)controller;
    if (!request || !response)
        return;
    std::string err;
    if (!AuthOrFail(request, request->req().player_id(), response, &err))
        return;
    game::GameResponse gr;
    MailService::Instance().HandleMailFavorite(request->req(), &gr);
    if (gr.has_mail_favorite())
        *response = gr.mail_favorite();
    else {
        response->set_ok(false);
        response->set_message(gr.message());
    }
}

void MailBrpcServiceImpl::MailBatchRead(::google::protobuf::RpcController *controller,
                                        const ::mailrpc::AuthedMailBatchReadReq *request,
                                        ::game::MailBatchReadRsp *response,
                                        ::google::protobuf::Closure *done) {
    brpc::ClosureGuard done_guard(done);
    (void)controller;
    if (!request || !response)
        return;
    std::string err;
    if (!AuthOrFail(request, request->req().player_id(), response, &err))
        return;
    game::GameResponse gr;
    MailService::Instance().HandleMailBatchRead(request->req(), &gr);
    if (gr.has_mail_batch_read())
        *response = gr.mail_batch_read();
    else {
        response->set_ok(false);
        response->set_message(gr.message());
    }
}

void MailBrpcServiceImpl::MailBatchDelete(::google::protobuf::RpcController *controller,
                                          const ::mailrpc::AuthedMailBatchDeleteReq *request,
                                          ::game::MailBatchDeleteRsp *response,
                                          ::google::protobuf::Closure *done) {
    brpc::ClosureGuard done_guard(done);
    (void)controller;
    if (!request || !response)
        return;
    std::string err;
    if (!AuthOrFail(request, request->req().player_id(), response, &err))
        return;
    game::GameResponse gr;
    MailService::Instance().HandleMailBatchDelete(request->req(), &gr);
    if (gr.has_mail_batch_delete())
        *response = gr.mail_batch_delete();
    else {
        response->set_ok(false);
        response->set_message(gr.message());
    }
}

void MailBrpcServiceImpl::MailDeliver(::google::protobuf::RpcController *controller,
                                     const ::mailrpc::AuthedMailDeliverReq *request,
                                     ::game::MailDeliverRsp *response,
                                     ::google::protobuf::Closure *done) {
    brpc::ClosureGuard done_guard(done);
    (void)controller;
    if (!request || !response)
        return;
    // 玩家会话不能给自己投系统奖励。正式环境只允许进程内 MailService::Deliver 或已登录的 GM HTTP。
    if (FormalModeEnabled()) {
        response->set_ok(false);
        response->set_error_code("ERR_COMMAND_FORBIDDEN");
        response->set_message("internal deliver is not a player rpc");
        return;
    }
    std::string err;
    if (!AuthOrFail(request, request->req().receiver_id(), response, &err))
        return;
    game::GameResponse gr;
    MailService::Instance().HandleMailDeliver(request->req(), &gr);
    if (gr.has_mail_deliver())
        *response = gr.mail_deliver();
    else {
        response->set_ok(false);
        response->set_message(gr.message());
    }
}
