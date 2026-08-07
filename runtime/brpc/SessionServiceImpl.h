#pragma once

#include "session.pb.h"

class SessionServiceImpl : public sess::SessionService {
public:
    void AcquireSession(::google::protobuf::RpcController *controller,
                        const ::sess::AcquireSessionRequest *request,
                        ::sess::AcquireSessionResponse *response,
                        ::google::protobuf::Closure *done) override;
    void MarkDisconnectedV2(::google::protobuf::RpcController *controller,
                            const ::sess::MarkDisconnectedRequest *request,
                            ::sess::MarkDisconnectedResponse *response,
                            ::google::protobuf::Closure *done) override;
    void ReconnectV2(::google::protobuf::RpcController *controller,
                     const ::sess::ReconnectRequest *request,
                     ::sess::ReconnectResponse *response,
                     ::google::protobuf::Closure *done) override;
    void LogoutV2(::google::protobuf::RpcController *controller,
                  const ::sess::LogoutRequest *request, ::sess::LogoutResponse *response,
                  ::google::protobuf::Closure *done) override;
    void Kick(::google::protobuf::RpcController *controller, const ::sess::KickRequest *request,
              ::sess::KickResponse *response, ::google::protobuf::Closure *done) override;

    void Login(::google::protobuf::RpcController *controller, const ::game::LoginReq *request,
               ::game::LoginRsp *response, ::google::protobuf::Closure *done) override;
    void Reconnect(::google::protobuf::RpcController *controller,
                   const ::game::ReconnectReq *request, ::game::ReconnectRsp *response,
                   ::google::protobuf::Closure *done) override;
    void Logout(::google::protobuf::RpcController *controller, const ::game::LogoutReq *request,
                ::game::LogoutRsp *response, ::google::protobuf::Closure *done) override;
    void ValidateToken(::google::protobuf::RpcController *controller,
                       const ::sess::ValidateTokenReq *request, ::sess::ValidateTokenRsp *response,
                       ::google::protobuf::Closure *done) override;
    void BindConnection(::google::protobuf::RpcController *controller,
                        const ::sess::BindConnectionReq *request,
                        ::sess::BindConnectionRsp *response,
                        ::google::protobuf::Closure *done) override;
    void MarkDisconnected(::google::protobuf::RpcController *controller,
                          const ::sess::MarkDisconnectedReq *request,
                          ::sess::MarkDisconnectedRsp *response,
                          ::google::protobuf::Closure *done) override;
};
