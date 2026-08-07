#pragma once

#include "forward.pb.h"

/** World 中控 Forward：邮件/全局业务入队 HandleFrame；无地图 epoch 栅栏、无战斗 Tick */
class WorldForwardServiceImpl : public fwd::WorldForward {
public:
    void Forward(::google::protobuf::RpcController *controller, const ::fwd::ForwardReq *request,
                 ::fwd::ForwardRsp *response, ::google::protobuf::Closure *done) override;
};
