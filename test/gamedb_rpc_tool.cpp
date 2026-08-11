/**
 * 最小 GameDB RPC 工具：mutate / query / inventory（阶段二故障注入）
 * 用法:
 *   gamedb_rpc_tool mutate <addr> <player> <key> <GRANT|CONSUME> <item> <count>
 *   gamedb_rpc_tool query <addr> <player> <key> <op>
 *   gamedb_rpc_tool inventory <addr> <player> [item_id]
 */
#include "gamedb.pb.h"

#include <brpc/channel.h>
#include <brpc/controller.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::printf("usage: %s mutate|query|inventory ...\n", argv[0]);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "mutate") {
        if (argc < 8) {
            std::printf("usage: mutate addr player key GRANT|CONSUME item count\n");
            return 2;
        }
        brpc::Channel ch;
        brpc::ChannelOptions opt;
        opt.timeout_ms = 8000;
        opt.max_retry = 0;
        if (ch.Init(argv[2], &opt) != 0) {
            std::printf("FAIL channel\n");
            return 1;
        }
        gdb::AssetMutationReq req;
        gdb::AssetMutationRsp rsp;
        brpc::Controller cntl;
        req.set_player_id(std::strtoull(argv[3], nullptr, 10));
        req.set_idempotency_key(argv[4]);
        req.set_mutation_type(argv[5]);
        req.set_item_id(static_cast<uint32_t>(std::atoi(argv[6])));
        req.set_count(static_cast<uint32_t>(std::atoi(argv[7])));
        req.set_expected_version(0);
        gdb::GameDbService_Stub stub(&ch);
        stub.ApplyAssetMutation(&cntl, &req, &rsp, nullptr);
        if (cntl.Failed()) {
            std::printf("rpc_failed=1 err=%s\n", cntl.ErrorText().c_str());
            return 1;
        }
        std::printf("ok=%d idempotent=%d ver=%llu remain=%u err=%s msg=%s\n", rsp.ok() ? 1 : 0,
                    rsp.idempotent_hit() ? 1 : 0, (unsigned long long)rsp.asset_version(),
                    rsp.remain_count(), rsp.error_code().c_str(), rsp.message().c_str());
        return rsp.ok() ? 0 : 1;
    }
    if (cmd == "query") {
        if (argc < 6) {
            std::printf("usage: query addr player key op\n");
            return 2;
        }
        brpc::Channel ch;
        brpc::ChannelOptions opt;
        opt.timeout_ms = 3000;
        opt.max_retry = 0;
        if (ch.Init(argv[2], &opt) != 0) {
            std::printf("FAIL channel\n");
            return 1;
        }
        gdb::QueryOperationResultReq req;
        gdb::QueryOperationResultRsp rsp;
        brpc::Controller cntl;
        req.set_player_id(std::strtoull(argv[3], nullptr, 10));
        req.set_idempotency_key(argv[4]);
        req.set_operation_type(argv[5]);
        gdb::GameDbService_Stub stub(&ch);
        stub.QueryOperationResult(&cntl, &req, &rsp, nullptr);
        if (cntl.Failed()) {
            std::printf("rpc_failed=1 err=%s\n", cntl.ErrorText().c_str());
            return 1;
        }
        std::printf("ok=%d found=%d completed_ok=%d status=%s ver=%llu remain=%u err=%s\n",
                    rsp.ok() ? 1 : 0, rsp.found() ? 1 : 0, rsp.completed_ok() ? 1 : 0,
                    rsp.status().c_str(), (unsigned long long)rsp.asset_version(), rsp.remain_count(),
                    rsp.error_code().c_str());
        return rsp.ok() ? 0 : 1;
    }
    if (cmd == "inventory") {
        if (argc < 4) {
            std::printf("usage: inventory addr player [item_id]\n");
            return 2;
        }
        brpc::Channel ch;
        brpc::ChannelOptions opt;
        opt.timeout_ms = 3000;
        opt.max_retry = 0;
        if (ch.Init(argv[2], &opt) != 0) {
            std::printf("FAIL channel\n");
            return 1;
        }
        gdb::LoadInventoryReq req;
        gdb::LoadInventoryRsp rsp;
        brpc::Controller cntl;
        req.set_player_id(std::strtoull(argv[3], nullptr, 10));
        gdb::GameDbService_Stub stub(&ch);
        stub.LoadInventory(&cntl, &req, &rsp, nullptr);
        if (cntl.Failed()) {
            std::printf("rpc_failed=1 err=%s\n", cntl.ErrorText().c_str());
            return 1;
        }
        const uint32_t want_item =
            argc >= 5 ? static_cast<uint32_t>(std::atoi(argv[4])) : 0;
        uint32_t bag_count = 0;
        for (int i = 0; i < rsp.bag_size(); ++i) {
            if (want_item == 0 || rsp.bag(i).item_id() == want_item)
                bag_count += rsp.bag(i).count();
        }
        std::printf("ok=%d ver=%llu bag_count=%u item=%u err=%s\n", rsp.ok() ? 1 : 0,
                    (unsigned long long)rsp.asset_version(), bag_count, want_item,
                    rsp.error_code().c_str());
        return rsp.ok() ? 0 : 1;
    }
    std::printf("unknown cmd\n");
    return 2;
}
