/**
 * 阶段二：断线 RPC 异步化 — MarkDisconnectedAsync / UnbindPlayerAsync 不得阻塞调用线程。
 * 指向不可达地址 + 长 timeout，同步调用会卡数秒；异步应立刻返回。
 */
#include "GatewayAuthClients.h"
#include "Logging.h"
#include "SessionRpcClient.h"
#include "gamelogic_rpc.pb.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    Logger::setLogLevel(Logger::WARN);

    const int timeout_ms = 3000;
    if (!SessionRpcClient::Instance().Init("127.0.0.1:1", timeout_ms)) {
        std::printf("FAIL SessionRpcClient Init\n");
        return 1;
    }
    // Logic channel：同样不可达
    if (!GatewayAuthClients::Instance().InitAuthSession("127.0.0.1:1", timeout_ms) ||
        !GatewayAuthClients::Instance().InitLogicChannels({"127.0.0.1:1"}, {"gl-dead"},
                                                          timeout_ms)) {
        std::printf("FAIL GatewayAuthClients Init\n");
        return 1;
    }

    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kN = 64;
    for (int i = 0; i < kN; ++i) {
        SessionRpcClient::Instance().MarkDisconnectedAsync(1000 + static_cast<uint64_t>(i),
                                                           "tok", 1);
        glrpc::UnbindPlayerRequest ureq;
        ureq.set_player_id(1000 + static_cast<uint64_t>(i));
        ureq.set_session_id("s");
        ureq.set_fence_token("tok");
        ureq.set_reason("storm_test");
        GatewayAuthClients::Instance().UnbindPlayerAsync("gl-dead", ureq);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    // 同步路径会接近 kN * timeout；异步必须远小于单次 timeout
    if (ms > 500) {
        std::printf("FAIL disconnect storm blocked ms=%lld (limit 500)\n",
                    static_cast<long long>(ms));
        return 1;
    }

    // 给 brpc 一点时间回收 callback，避免进程退出时泄漏噪音
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    SessionRpcClient::Instance().Shutdown();
    std::printf("PASS gateway_disconnect_async_test storm_ms=%lld n=%d\n",
                static_cast<long long>(ms), kN);
    return 0;
}
