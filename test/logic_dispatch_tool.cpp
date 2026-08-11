/**
 * 向 GameLogic.Dispatch 发送一条带 fence/generation 的命令（故障演练）。
 * 用法:
 *   logic_dispatch_tool <logic_addr> <player> <session_id> <fence_token> <generation>
 * 输出: ok=... error_code=... message=...
 * 期望旧 fence: error_code=FENCE_REJECT，进程 exit 0（调用方断言 error_code）
 */
#include "gamelogic_rpc.pb.h"

#include <brpc/channel.h>
#include <brpc/controller.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv) {
    if (argc < 6) {
        std::printf("usage: %s logic_addr player session_id fence_token generation\n", argv[0]);
        return 2;
    }
    brpc::Channel ch;
    brpc::ChannelOptions opt;
    opt.timeout_ms = 5000;
    opt.max_retry = 0;
    if (ch.Init(argv[1], &opt) != 0) {
        std::printf("rpc_failed=1 err=channel\n");
        return 1;
    }
    glrpc::ClientCommand req;
    glrpc::CommandResult rsp;
    brpc::Controller cntl;
    req.set_player_id(std::strtoull(argv[2], nullptr, 10));
    req.set_session_id(argv[3]);
    req.set_fence_token(argv[4]);
    req.set_generation(std::strtoull(argv[5], nullptr, 10));
    req.set_message_type("ping");
    req.set_client_seq(1);
    req.set_payload("");
    glrpc::GameLogicService_Stub stub(&ch);
    stub.Dispatch(&cntl, &req, &rsp, nullptr);
    if (cntl.Failed()) {
        std::printf("rpc_failed=1 err=%s\n", cntl.ErrorText().c_str());
        return 1;
    }
    std::printf("ok=%d error_code=%s message=%s\n", rsp.ok() ? 1 : 0, rsp.error_code().c_str(),
                rsp.message().c_str());
    return 0;
}
