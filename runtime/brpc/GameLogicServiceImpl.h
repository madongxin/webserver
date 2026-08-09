#pragma once

#include "gamelogic_rpc.pb.h"

#include <cstdint>
#include <string>

class GameLogicServiceImpl : public glrpc::GameLogicService {
public:
    void BindPlayer(::google::protobuf::RpcController *controller,
                    const ::glrpc::BindPlayerRequest *request,
                    ::glrpc::BindPlayerResponse *response,
                    ::google::protobuf::Closure *done) override;
    void Dispatch(::google::protobuf::RpcController *controller,
                  const ::glrpc::ClientCommand *request, ::glrpc::CommandResult *response,
                  ::google::protobuf::Closure *done) override;
    void UnbindPlayer(::google::protobuf::RpcController *controller,
                      const ::glrpc::UnbindPlayerRequest *request,
                      ::glrpc::UnbindPlayerResponse *response,
                      ::google::protobuf::Closure *done) override;
    void FreezePlayer(::google::protobuf::RpcController *controller,
                      const ::glrpc::FreezePlayerRequest *request,
                      ::glrpc::FreezePlayerResponse *response,
                      ::google::protobuf::Closure *done) override;
};

/** Push 按 Bind 时保存的 gateway_instance_id 选目标，非广播。 */
bool GameLogicGetPushTarget(uint64_t player_id, std::string *gateway_instance_id,
                            std::string *session_id);
bool GameLogicGetBoundMeta(uint64_t player_id, std::string *gateway_instance_id,
                           std::string *session_id, std::string *fence_token,
                           uint64_t *generation);
