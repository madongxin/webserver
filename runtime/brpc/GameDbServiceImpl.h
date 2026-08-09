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

    void LoadPlayer(::google::protobuf::RpcController *controller,
                    const ::gdb::LoadPlayerReq *request, ::gdb::LoadPlayerRsp *response,
                    ::google::protobuf::Closure *done) override;
    void LoadInventory(::google::protobuf::RpcController *controller,
                       const ::gdb::LoadInventoryReq *request, ::gdb::LoadInventoryRsp *response,
                       ::google::protobuf::Closure *done) override;
    void ApplyAssetMutation(::google::protobuf::RpcController *controller,
                            const ::gdb::AssetMutationReq *request,
                            ::gdb::AssetMutationRsp *response,
                            ::google::protobuf::Closure *done) override;
    void SavePlayerSnapshot(::google::protobuf::RpcController *controller,
                            const ::gdb::SavePlayerSnapshotReq *request,
                            ::gdb::SavePlayerSnapshotRsp *response,
                            ::google::protobuf::Closure *done) override;
    void FlushPlayer(::google::protobuf::RpcController *controller,
                     const ::gdb::FlushPlayerReq *request, ::gdb::FlushPlayerRsp *response,
                     ::google::protobuf::Closure *done) override;
};
