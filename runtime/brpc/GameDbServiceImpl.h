#pragma once

#include "gamedb.pb.h"

class GameDbServiceImpl : public gdb::GameDbService {
public:
    void ClaimMailAttachments(::google::protobuf::RpcController *controller,
                              const ::gdb::ClaimMailReq *request, ::gdb::ClaimMailRsp *response,
                              ::google::protobuf::Closure *done) override;
    void LookupAccount(::google::protobuf::RpcController *controller,
                       const ::gdb::LookupAccountReq *request, ::gdb::LookupAccountRsp *response,
                       ::google::protobuf::Closure *done) override;
    void RegisterAccount(::google::protobuf::RpcController *controller,
                         const ::gdb::RegisterAccountReq *request,
                         ::gdb::RegisterAccountRsp *response,
                         ::google::protobuf::Closure *done) override;
};
