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
    void GetSessionOperation(::google::protobuf::RpcController *controller,
                             const ::sess::GetSessionOperationRequest *request,
                             ::sess::GetSessionOperationResponse *response,
                             ::google::protobuf::Closure *done) override;
    void LogoutV2(::google::protobuf::RpcController *controller,
                  const ::sess::LogoutRequest *request, ::sess::LogoutResponse *response,
                  ::google::protobuf::Closure *done) override;
    void Kick(::google::protobuf::RpcController *controller, const ::sess::KickRequest *request,
              ::sess::KickResponse *response, ::google::protobuf::Closure *done) override;

    void ResolveOrCreateMap(::google::protobuf::RpcController *controller,
                            const ::sess::ResolveOrCreateMapRequest *request,
                            ::sess::ResolveOrCreateMapResponse *response,
                            ::google::protobuf::Closure *done) override;
    void GetPlacement(::google::protobuf::RpcController *controller,
                      const ::sess::GetPlacementRequest *request,
                      ::sess::GetPlacementResponse *response,
                      ::google::protobuf::Closure *done) override;
    void MigrateMap(::google::protobuf::RpcController *controller,
                    const ::sess::MigrateMapRequest *request, ::sess::MigrateMapResponse *response,
                    ::google::protobuf::Closure *done) override;
    void MarkRecovering(::google::protobuf::RpcController *controller,
                        const ::sess::MarkRecoveringRequest *request,
                        ::sess::MarkRecoveringResponse *response,
                        ::google::protobuf::Closure *done) override;
    void HeartbeatOwner(::google::protobuf::RpcController *controller,
                        const ::sess::HeartbeatOwnerRequest *request,
                        ::sess::HeartbeatOwnerResponse *response,
                        ::google::protobuf::Closure *done) override;
    void UpdatePlayerRoute(::google::protobuf::RpcController *controller,
                           const ::sess::UpdatePlayerRouteRequest *request,
                           ::sess::UpdatePlayerRouteResponse *response,
                           ::google::protobuf::Closure *done) override;
    void BeginPlayerTransfer(::google::protobuf::RpcController *controller,
                             const ::sess::BeginPlayerTransferRequest *request,
                             ::sess::BeginPlayerTransferResponse *response,
                             ::google::protobuf::Closure *done) override;
    void CommitPlayerTransfer(::google::protobuf::RpcController *controller,
                              const ::sess::CommitPlayerTransferRequest *request,
                              ::sess::CommitPlayerTransferResponse *response,
                              ::google::protobuf::Closure *done) override;
    void AbortPlayerTransfer(::google::protobuf::RpcController *controller,
                             const ::sess::AbortPlayerTransferRequest *request,
                             ::sess::AbortPlayerTransferResponse *response,
                             ::google::protobuf::Closure *done) override;
    void GetPlayerRoute(::google::protobuf::RpcController *controller,
                        const ::sess::GetPlayerRouteRequest *request,
                        ::sess::GetPlayerRouteResponse *response,
                        ::google::protobuf::Closure *done) override;

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
