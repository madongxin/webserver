# 阶段 1 状态（GameMesh_Cursor_All_Phases）

> 更新：2026-08-08  
> 对照：`docs/GameMesh_Cursor_All_Phases.md` 阶段 1

## 已落地

### 1.1 统一 GameLogic RPC
- 正式业务路径：`BrpcTransport` → 异步 `GameLogicService.Dispatch`（不再走 `GameLogicForward`）
- World 仍走 `WorldForward`（全局服路径）
- `forward.proto` 标记 deprecated；未知 `logic_server_id` fail-closed（不回退首节点）
- `ClientCommand` / `BindPlayerRequest` 携带 `generation`、`route_version`、deadline/trace 字段

### 1.2 Gateway 身份边界
- `GatewayAuthPolicy`：未登录仅白名单（Login/Register/Reconnect/Ping）
- 登录后 sticky 绑定覆盖客户端 `player_id` / logic 路由
- 单元测试：`phase1_gateway_boundary_test`

### 1.3 异步登录状态机
- Login/Logout/Reconnect 投递到 `PlayerSerialQueue`（离开 Reactor）
- Bind 失败调用 `LogoutV2` 补偿，避免假在线会话
- `OrchestrateGatewayReconnect`：ReconnectV2 → BindPlayer

### 1.4–1.5 Fence / 重连
- Session：Acquire/Reconnect 轮换 fence 并递增 generation / route_version
- GameLogic `BindPlayer` 存储并校验 generation；Dispatch 校验 session+fence+generation
- MarkDisconnected 为 token+generation CAS；迟到旧连接忽略
- Reconnect 保留 logic/map 路由并返回权威字段

### 1.6 Session 可水平扩展
- `RedisPool`：有上限连接池 + Ping 健康检查 / 超时租约
- Acquire / Reconnect / MarkDisconnected / Logout / BindConnection 走 Redis **Lua** 原子脚本
- Key 前缀 `key_prefix`（默认 `gamemesh:dev:`），可配 `redis_pool_size`

### 1.7 启动脚本
- `start_formal.sh`：双 Gateway 注入完整 `logic_addrs` + `logic_instance_ids=gl-0,gl-1`
- 依赖就绪改为 `wait_log` 轮询日志，不再固定 sleep 后盲检

## 验证命令

```bash
./scripts/build.sh Debug
./scripts/test_reactor.sh
./scripts/test_unit.sh
./build/test/session_store_test   # 需 Redis
./scripts/run_version.sh
```

## Proto / 接口变更

| 变更 | 兼容 |
|------|------|
| `BindPlayerRequest.generation=12` | 新增可选字段，旧客户端不填则 GameLogic 跳过 gen 严格比较 |
| `ClientCommand.generation/route_version/...` | 同上 |
| Forward 正式路径移除 | 兼容测试若仍链 `GameLogicForward` 需显式开关；计划阶段 2 删除实现 |

## 失败补偿

| 步骤 | 补偿 |
|------|------|
| Auth 失败 | 直接回客户端错误 |
| Acquire 成功、Bind 失败 | `LogoutV2` 释放 Redis 会话 |
| 断线 | `MarkDisconnected`（≠ Logout） |
| 重连超时 | Session Lua 删 key；客户端明确失败 |

## 风险 / 未完成（阶段 1 收尾项）

- §1.8 全量跨进程集成测试（双 GW 重连 E2E、双 Session 进程并发）尚未全部自动化
- 登录编排仍在 worker 内同步 brpc（已离开 Reactor；后续可改为纯异步 callback 链）
- ASan 全链路冒烟未出报告
- 旧 `Forward` 服务实现仍保留在树内供兼容，正式流量已不走
