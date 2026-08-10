# Gateway 故障

## 现象

客户端断连；`/health` 无响应；另一 Gateway 仍 ready。

## 处置

1. 确认玩家可从另一 Gateway Reconnect（fence/generation）。
2. SIGKILL/崩溃节点：不要手工清 Redis Session；以权威 Session 为准。
3. 旧连接迟到的 Forget/Unbind 不得删新绑定（见 GatewayConnRegistry 世代校验）。
4. 拉起实例并 Register；观察 `gamemesh_reconnect_*` 与非法帧指标。

演练：`scripts/kill_gateway_drill.sh`、`scripts/test_dual_gw_e2e.sh`。
