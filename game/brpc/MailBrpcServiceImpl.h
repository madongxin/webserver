#pragma once

/**
 * @file MailBrpcServiceImpl.h
 * @brief brpc MailBrpcService 实现：鉴权后转发到 MailService / SessionStore
 */

#include "mail_brpc.pb.h"

class MailBrpcServiceImpl : public mailrpc::MailBrpcService {
public:
    void Login(::google::protobuf::RpcController *controller, const ::game::LoginReq *request,
               ::game::LoginRsp *response, ::google::protobuf::Closure *done) override;

    void MailboxSummary(::google::protobuf::RpcController *controller,
                        const ::mailrpc::AuthedMailboxSummaryReq *request,
                        ::game::MailboxSummaryRsp *response,
                        ::google::protobuf::Closure *done) override;

    void MailList(::google::protobuf::RpcController *controller,
                  const ::mailrpc::AuthedMailListReq *request, ::game::MailListRsp *response,
                  ::google::protobuf::Closure *done) override;

    void MailGet(::google::protobuf::RpcController *controller,
                 const ::mailrpc::AuthedMailGetReq *request, ::game::MailGetRsp *response,
                 ::google::protobuf::Closure *done) override;

    void MailRead(::google::protobuf::RpcController *controller,
                  const ::mailrpc::AuthedMailReadReq *request, ::game::MailReadRsp *response,
                  ::google::protobuf::Closure *done) override;

    void MailClaim(::google::protobuf::RpcController *controller,
                   const ::mailrpc::AuthedMailClaimReq *request, ::game::MailClaimRsp *response,
                   ::google::protobuf::Closure *done) override;

    void MailBatchClaim(::google::protobuf::RpcController *controller,
                        const ::mailrpc::AuthedMailBatchClaimReq *request,
                        ::game::MailBatchClaimRsp *response,
                        ::google::protobuf::Closure *done) override;

    void MailFavorite(::google::protobuf::RpcController *controller,
                      const ::mailrpc::AuthedMailFavoriteReq *request,
                      ::game::MailFavoriteRsp *response,
                      ::google::protobuf::Closure *done) override;

    void MailBatchRead(::google::protobuf::RpcController *controller,
                       const ::mailrpc::AuthedMailBatchReadReq *request,
                       ::game::MailBatchReadRsp *response,
                       ::google::protobuf::Closure *done) override;

    void MailBatchDelete(::google::protobuf::RpcController *controller,
                         const ::mailrpc::AuthedMailBatchDeleteReq *request,
                         ::game::MailBatchDeleteRsp *response,
                         ::google::protobuf::Closure *done) override;

    void MailDeliver(::google::protobuf::RpcController *controller,
                     const ::mailrpc::AuthedMailDeliverReq *request,
                     ::game::MailDeliverRsp *response, ::google::protobuf::Closure *done) override;
};
