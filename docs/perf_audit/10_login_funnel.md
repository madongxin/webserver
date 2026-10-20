# 10 登录漏斗（run_1339）

## 调用路径（源码）

`GameTcpGateway` → Hello → `GatewayLoginOrchestrator`（AuthLogin → `AcquireSession` → `BindPlayer`）→ 客户端 `EnterMap` → `GatewayEnterMapOrchestrator` / GameLogic `HandleEnterMap` → AOI 快照在 EnterMapRsp。

## 漏斗表（独立玩家，OBSERVED）

| 阶段 | 进入 | 成功 | 业务拒绝 | 超时/超载 | 备注 |
| --- | --- | --- | --- | --- | --- |
| TCP / Hello | 有大量 Hello | — | — | — | hold 期 Hello≫Login |
| AuthLogin | ≥409 | 409 | 0（无失败日志） | 0 | session 日志 Login ok |
| AcquireSession | 409 | 409 | 0 | 0 | |
| BindPlayer | 409 | 409 | 0 | 0 | logic0/1 |
| EnterMap reply | 409 人 / 428 次 | ok=1 全为成功 | 0 NO_LINE 等 | 0 | 无 resolve fail 日志 |
| Admission `ERR_OVERLOADED` | — | — | **0** | — | 正常保护未触发 |

## 延迟

各阶段 p50/p95/p99：**NOT_OBSERVABLE**（无配对 begin/end）。  
仅知 Enter 与 Login 落在同一 10s 桶内占绝大多数 → **DERIVED**：进图未出现分钟级积压。

## 不可观察列

- Auth 密码 CPU、GameDB LoadPlayer SQL 耗时
- Redis Acquire Lua 耗时
- RpcOffloadPool 排队长度时序（仅知 overload 计数累计为 0）
