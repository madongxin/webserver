# GameMesh 服务器稳定版本最小收口任务书（Cursor 直接执行）

> 仓库：`https://github.com/madongxin/webserver`
>
> 分支：`main`
>
> 本次审计基线：`5b14027868b19870c234547d5bcfbf2ff3c766b1`
>
> 审计日期：2026-08-13
>
> 当前结论：**STABLE BLOCKED**
>
> 目标：以最小改动产出固定拓扑下可供后续客户端联调和业务开发使用的服务器稳定候选版。

---

## 0. 给 Cursor 的总指令

请直接修改当前 GameMesh 工程，不要只输出建议或重新设计架构。

严格按“阶段一 → 阶段二 → 阶段三”执行：

1. 当前阶段所有必需测试未通过时，不得进入下一阶段。
2. 每阶段先阅读实际代码和已有测试，再做小步兼容修改。
3. 每阶段结束必须输出修改文件、真实执行命令、测试结果和遗留风险。
4. 不得删除测试、降低断言、静默 `SKIP`、吞掉失败码或用空实现获得 PASS。
5. 未通过同一最终 commit 的完整门禁前，不得输出 `STABLE PASS`，不得创建稳定 Tag。
6. 不执行 `git reset --hard`、`git checkout --` 等破坏性操作。
7. 保留用户现有未提交改动；不要提交、不要 Push，除非用户另外明确授权。

开始前必须执行：

```bash
git status --short --branch
git rev-parse HEAD
git log -1 --oneline
```

完整阅读：

```text
AGENTS.md
CMakeLists.txt
scripts/stable_gate.sh
scripts/test_unit.sh
scripts/test_integration.sh
scripts/test_final_e2e.sh
docs/release/server-stable-v0.1.0.md
```

如果实际 HEAD 已经晚于 `5b14027`，先逐项核对本文件中的缺口是否已被真实修复。已经修复且有确定性测试的项目不要重复实现；仍存在的项目继续完成。

---

# 一、稳定版本范围

## 1.1 本次必须稳定支持

固定拓扑：

```text
Client
  → L4 LB / VIP
  → Gateway ×2
  → Auth/Session ×2（同一 session 二进制，逻辑分离）
  → GameLogic ×2
  → GameDB ×2
  → MySQL ×1

Session / Placement / Registry
  → Redis ×1

GlobalService ×1
  → 当前代码/二进制可继续使用 world 名称
```

本次稳定候选必须具备：

1. 注册、密码登录、Access Token、Refresh Token。
2. `Auth → Session.AcquireSession → GameLogic.BindPlayer` 登录链路。
3. 同一玩家只有一个有效 Session、fence token 和 generation。
4. Gateway 粘性路由及 `GameLogicService.Dispatch`。
5. 同玩家命令严格串行，不同玩家可并行。
6. `MarkDisconnected != Logout`，支持重连宽限期。
7. gw0 断开或崩溃后，同一个活动 Session 可以通过 gw1 重连。
8. MapInstance 单 Owner，旧 `owner_epoch/route_version` 写入被拒绝。
9. 固定的 gl-0、gl-1 都能承载通用地图实例。
10. GameLogic → 指定 Gateway 的可靠 Push；重连时重放或明确下发全量快照。
11. GameDB 资产版本、幂等、未知结果查询和 Outbox。
12. Redis、MySQL、Session、GameLogic、GameDB 故障时 fail-closed，不产生错误成功响应。
13. 有界队列、背压、超时、可停止和基本可观测性。
14. 同一最终 commit 通过 Debug/Release、单元、集成、双 GW TCP E2E、Sanitizer、负载和 Soak 门禁。

## 1.2 本次明确不做

以下内容属于后续版本，不得扩大本次改动范围：

- Unity 客户端接入。
- 全域大世界、Zone、Cell、Chunk 无缝跨服。
- 运行期动态增加 gl-2。
- MapInstance 无损实时状态迁移。
- Redis Sentinel/Cluster 和 MySQL 主从部署。
- etcd v3 替换当前 Redis Registry。
- Chat、Guild、Friend 等完整业务。
- GlobalService 多活。
- gRPC、独立 Login 进程或 Reactor 全量重写。

稳定版允许的 GameLogic 崩溃语义是：

```text
旧 Owner 失效
→ 旧 epoch 写入被拒绝
→ 玩家通过持久化状态重新登录/重新进图
→ 新 Owner 获得更高 epoch
```

本次不承诺实时地图内存状态无损迁移，但不得丢失已经由 GameDB 确认提交的资产。

---

# 二、对 `main@5b14027` 的重新评估

## 2.1 最新提交实际完成的内容

`3ba6623..5b14027` 只修改了 6 个文件，主要完成：

1. Session 冷启动时，如果 Redis Registry 暂时发现不到 GameLogic，不再立即清空启动静态 Owner。
2. 一旦发现过非空健康集，之后发现空列表仍会 fail-closed。
3. `test_final_e2e.sh` 增加最多 60 秒的注册/登录就绪等待。
4. E2E 客户端改善注册失败信息。
5. 补强健康列表和断线队列测试的部分断言。

这些改动可以保留，不要回退。但它只修复了“冷启动 Owner 被过早清空”问题，没有覆盖下面的稳定性阻塞。

## 2.2 本次审计验证结果

已通过的静态/低层检查：

```text
git show --check HEAD                 PASS
git diff --check origin/main          PASS
bash -n scripts/*.sh                  PASS
手工编译 player_serial_queue_test      PASS
```

当前审计环境缺少 `cmake、protoc、jsoncpp、MySQL Client、hiredis、brpc、shellcheck`，因此不能把完整构建、集成测试或 E2E 标记为通过。

额外停服探针复现：

```text
PlayerSerialQueue 内执行一个 5 秒任务
Stop() 内部声明的 drain deadline 为 3 秒
实际 Stop 返回耗时：4950 ms
```

说明当前 `Stop()` 超时后仍然执行无界 `join()`，停服时长并不受 deadline 约束。

## 2.3 当前稳定性阻塞清单

| 优先级 | 当前缺口 | 代码证据 | 风险 |
|---|---|---|---|
| P0 | 宽限期过期为读取后直接 `DEL` | `game/SessionStore.cpp:700-711` | 可能删除刚刚重连成功的新 Session |
| P0 | Reconnect 先写 ONLINE/新 fence，Rebind 失败不回滚 | `runtime/brpc/GatewayLoginOrchestrator.cpp:231-275` | Redis 显示在线但玩家无法进入 Logic |
| P0 | dirty 资产可被旧/重复响应清除 | `game/GameLogic.cpp:272-319` | 未同步内存重新放行资产写，可能覆盖正确资产 |
| P0 | `GatewayDisconnectAsync` 超时后 detach | `runtime/GatewayDisconnectAsync.cpp:29-60` | 后台线程可能访问已析构单例或 Redis 资源 |
| P0 | `PlayerSerialQueue::Stop()` 超时后仍无界 join | `runtime/PlayerSerialQueue.cpp:138-177` | SIGTERM/发布停服可能无限卡住 |
| P0 | RedisPool Shutdown 未等待活动 Lease | `redis/RedisPool.cpp:43-60,102-112` | 活动线程可能持有已释放的 RedisClient |
| P0 | Session/Auth Token 使用 `mt19937_64` | `game/SessionStore.cpp:26-32`、`runtime/brpc/AuthTokenStore.cpp:21-27` | Session/fence/refresh token 不具备密码学安全性 |
| P0 | Refresh Token 单次消费只靠进程内 mutex | `runtime/brpc/AuthTokenStore.cpp:202-228` | 两个 Session 实例可并发消费同一个 refresh token |
| P0 | 健康 Logic 列表仍是两份顺序写入 | `runtime/HealthyLogicOwners.cpp` | 并发刷新可让 Session 与 Placement 短暂或最终不一致 |
| P0 | readiness 只检查对象初始化/Channel 存在 | `runtime/HealthDeps.cpp:29-82` | 依赖已失效时实例仍可能报告 ready |
| P1 | 登录、重连、部分资产 RPC 为同步 brpc | `runtime/brpc/GatewayAuthClients.cpp`、`db/BrpcGameDbRepository.cpp` | 后端超时会阻塞玩家 shard 中的其他玩家 |
| P1 | 断线 fallback 队列满时直接丢弃 | `game/GameTcpGateway.cpp:246-260` | 断线风暴后 Redis Session 可能长期错误地保持 ONLINE |
| P1 | `SessionService.Kick` 仍是 stub | `runtime/brpc/SessionServiceImpl.cpp:170-178` | 重复登录只能靠 fence 拒绝，旧 TCP 连接不能及时关闭 |
| P1 | kill gw0 场景未验证同一活动 Session 跨 GW 恢复 | `scripts/test_final_e2e.sh` 场景 4 | 当前脚本先完成普通 dual-gw，再 kill gw0，之后只新注册账号 |
| P1 | kill Logic 场景只操作 Placement | `scripts/test_final_e2e.sh` 场景 6 | 未验证真实玩家资产恢复、重新进图和旧 epoch 拒绝 |
| P1 | 若干依赖测试可 `SKIP` 或可选执行 | `scripts/test_unit.sh`、`test/*` | 缺依赖或缺测试二进制时可能得到不真实的 PASS |
| 门禁 | 当前 commit 没有完整稳定门禁报告 | 无 `5b14027` 对应 release manifest | 不能证明当前代码通过长稳验证 |

结论：**架构骨架合理，但当前 commit 不能作为稳定服务器版本。**

---

# 阶段一：修复状态正确性、认证和生命周期 P0

本阶段只处理可能造成错会话、资产错误、认证重放、UAF 或停服失控的问题。

## 1. Session 宽限期过期改为 Redis CAS

当前竞态：

```text
线程 A 读取旧 DISCONNECTED Session，判断已超时
线程 B Reconnect 成功，写入 ONLINE、新 fence、新 generation
线程 A DEL 相同 Redis key
结果：新 Session 被删除
```

修改要求：

1. `ExpireIfGraceElapsed` 不得再执行读取后的无条件 `DEL`。
2. 使用 Redis Lua，在一次原子操作中比较：

   ```text
   state == DISCONNECTED
   session_id
   fence_token
   generation
   disconnect_deadline
   now > disconnect_deadline
   ```

3. 返回值至少区分：

   ```text
   EXPIRED_AND_DELETED
   NOT_DUE
   STALE_SNAPSHOT
   NOT_FOUND
   REDIS_ERROR
   ```

4. `STALE_SNAPSHOT` 时重新读取当前权威记录；不得把新会话报告为 OFFLINE。
5. 所有 `Validate/IsPlayerOnline/GetRoute` 等懒过期入口统一调用 CAS 版本。

确定性测试：在 A 完成读取后暂停，B 执行 Reconnect，再恢复 A；最终新 Session 必须仍为 ONLINE，且新 fence/generation 不变。

## 2. Reconnect 改为 Prepare/Commit/Abort

不要在 Rebind 成功前把权威 Session 直接改成 ONLINE。

实现最小两阶段流程：

```text
Gateway → Session.PrepareReconnect
Session 创建带 TTL 的 pending reconnect operation
Session 返回候选 fence/generation 和当前 Logic/Map 路由
Gateway → 指定 GameLogic.BindPlayer/Rebind
Gateway → Session.CommitReconnect（Redis Lua CAS）
Gateway 更新本地连接绑定
Gateway → Client ReconnectSuccess
```

失败路径：

```text
Bind/Rebind 失败
→ Session.AbortReconnect
→ 删除 pending operation
→ 保留原 DISCONNECTED Session 和原 reconnect_ticket
```

```text
Bind 成功但 Commit CAS 失败
→ 新 Logic 条件 Unbind 候选 fence
→ 不更新 GatewayConnRegistry
→ 返回明确失败
```

```text
Commit 成功但 TCP 已关闭
→ 立即按新 fence/generation 执行 MarkDisconnected
→ 不留下假 ONLINE
```

要求：

- pending key 必须包含 `operation_id`，重复调用返回同一候选结果。
- Commit/Abort 必须比较旧 session、旧 generation、operation_id 和候选 fence。
- pending operation 设置短 TTL，超时由清理逻辑安全回收。
- 禁止用补偿性无条件 Logout 代替 CAS。
- 更新 `proto/session.proto` 及生成文件，保持旧 Reconnect 接口兼容但正式链路只走新流程。

## 3. 修正 asset dirty 语义

`asset_dirty=true` 表示内存状态不可信。在权威重载成功前，它不能被旧响应或重复响应清除。

修改 `ApplyItemRewardsWithVersion`：

1. 只有 `dirty == false && cur == committed` 才允许幂等成功返回。
2. 只有 `dirty == false && cur > committed` 才允许忽略旧响应。
3. `dirty == true` 时，无论版本相等、领先或落后，都必须先从 GameDB 权威重载。
4. 重载失败时保持 dirty，并让后续资产写返回 `STATE_SYNC_REQUIRED`。
5. 重载成功后验证 `loaded_version >= committed_version`；不满足则继续 dirty。
6. 不得在失败或未验证的路径执行 `asset_dirty_.erase()`。

新增测试：

- 本地应用被阻断后置 dirty，再收到相同 committed version，dirty 仍保持。
- dirty 时收到更旧响应，dirty 仍保持。
- GameDB 重载失败期间 Consume/Grant 都 fail-closed。
- 重载成功后库存和版本等于 GameDB，才恢复写入。

## 4. 收口三个生命周期问题

### 4.1 PlayerSerialQueue

当前 3 秒 drain 超时后仍 join 正在执行的阻塞任务，不是真正有界。

要求：

- `Stop(deadline)` 返回明确结果，例如 `Drained/Cancelled/TimedOut`。
- 禁止 detach worker。
- 将队列 State 改为安全的共享生命周期，Stop 后回调不得访问已析构对象。
- 给任务提供协作取消标志/stop token；本项目投递的任务必须尊重 deadline。
- 下游 RPC 改为异步或具备严格超时，不能在 worker 内无限同步阻塞。
- 超时后拒绝新任务、取消尚未开始的任务，并为被取消任务完成错误回调。
- 进程退出前所有 worker 必须 join；如果仍有不响应取消的内部任务，应使稳定门禁失败，而不是静默 detach。

测试：一个可取消的 5 秒任务，Stop deadline=200ms，应在限定时间内返回且无 UAF；再 Start/Stop 必须正常。

### 4.2 GatewayDisconnectAsync

要求：

- 删除 `worker_.detach()` 路径。
- 默认执行器不能在 Stop 返回后继续调用 `SessionStore::Instance()`。
- Redis 命令必须有 socket timeout，且小于 shutdown budget。
- Stop 时先禁止入队，再处理/取消队列，最后 join worker。
- 被取消的 MarkDisconnected 必须进入可恢复补偿路径或写入指标，不能静默丢失。

### 4.3 RedisPool

要求：

- Pool 进入 `CLOSING` 后拒绝新 Acquire。
- 记录 active leases，Shutdown 等待 Lease 归还或到达 deadline。
- 不得在 active lease 存在时释放其 RedisClient。
- 推荐让 Lease 持有 `shared_ptr<PoolState/RedisClient>` 和 generation，避免归还到已重建的新 Pool。
- Shutdown 后旧 Lease 析构不得把悬空指针重新放回 free list。
- 增加并发 Acquire/Shutdown、Shutdown/Init 新 generation 测试。

## 5. 使用密码学安全随机数并原子轮换 Refresh Token

新增统一 `SecureRandom`：

- Linux/OpenSSL 使用 `RAND_bytes` 或等价 CSPRNG。
- 失败必须 fail-closed，不得回退到 `rand/mt19937/time/pid`。
- Session ID、fence token、reconnect ticket、access token、refresh token、管理会话 ID 全部复用该实现。
- 日志不得输出完整 token。

Refresh Token 轮换使用 Redis Lua 原子完成：

```text
验证旧 refresh digest 与 player/account 匹配
→ 删除旧 refresh
→ 写入新 access digest
→ 写入新 refresh digest
→ 更新 player token index
→ 设置 TTL
```

两个 Session 进程并发提交同一个 refresh token 时，只允许一个成功；另一个必须返回 `TOKEN_ALREADY_ROTATED` 或等价错误。

## 6. 健康 GameLogic 使用单一不可变快照

保留 `5b14027` 的冷启动意图，但不要继续让 SessionStore 和 PlacementStore 分别持有可独立更新的 Owner 列表。

实现一个权威 `HealthyLogicSnapshot`：

```text
version
source = STATIC / REGISTRY
state = BOOTSTRAP / ACTIVE / EMPTY / STALE
instances[{instance_id,address,status,lease_until}]
updated_at
```

要求：

1. 通过 `shared_ptr<const Snapshot>` 原子发布完整快照。
2. Session 分配和 Placement 选 Owner 必须读取同一快照对象/同一 version。
3. 禁止再对两份 vector 顺序写入。
4. 并发 Refresh 的旧 version 不得覆盖新 version。
5. Registry 已启用时：
   - 启动短暂空发现可保留静态 bootstrap 快照；
   - readiness 保持 false 或 `STARTING`，直到发现至少一个实际健康实例；
   - 超过可配置 bootstrap deadline 仍为空则清空可分配集合并 fail-closed；
   - 一旦 ACTIVE，后续空发现立即切 EMPTY/fail-closed。
6. Registry 明确未启用时允许可信静态模式，并在 readiness/detail 中标明 `STATIC_DEGRADED`。

测试至少覆盖并发刷新、冷启动空发现、首次非空、ACTIVE 后变空、旧 version 覆盖防护。

## 阶段一必需测试

新增/更新并真实执行：

```text
session_grace_cas_test
reconnect_transaction_test
asset_dirty_consistency_test
player_serial_shutdown_test
gateway_disconnect_async_test
redis_pool_shutdown_test
secure_random_test
refresh_token_race_test
healthy_logic_refresh_test
```

阶段一结束前运行：

```bash
./scripts/check_deps.sh --full
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
```

任一必需测试缺二进制、SKIP 或失败，都不得进入阶段二。

---

# 阶段二：真实依赖健康、异步隔离和故障 E2E

## 1. Readiness 必须检查真实依赖

当前 `HealthDeps` 主要检查 `Available/isInitialized/channel != nullptr`，不能证明依赖此刻可用。

实现后台周期健康采样，HTTP `/health/ready` 只读取最近快照，不在 HTTP EventLoop 内同步探测。

建议快照字段：

```text
dependency
ok
last_success_at
last_error
consecutive_failures
latency_ms
snapshot_age_ms
```

角色要求：

| 角色 | 必需健康条件 |
|---|---|
| Gateway | TCP 正在接收、Redis 可用、至少一个 Session RPC 可达、至少一个健康 Logic 路由可用 |
| Session/Auth | Redis PING/Lua 可执行、健康 Logic Snapshot 可用；Auth 所需 GameDB 可达 |
| GameLogic | GameDB 可达、Placement/lease 数据源可用、本机 owner/lease 检查正常 |
| GameDB | MySQL `SELECT 1`、连接池可借出连接、RPC Server 已启动 |
| World | GameDB 可达；依赖不可用时不接受写业务 |

规则：

- 连续失败达到阈值后 ready=false，并把注册状态改为 NOT_READY/DRAINING。
- 恢复需连续成功达到阈值，避免抖动。
- 快照过期也必须 ready=false。
- `/health/live` 只表示进程/EventLoop 活着，不因外部依赖失败而变 false。
- 健康检查 RPC 必须轻量、有 deadline、无资产写副作用。

## 2. 下游 RPC 不得阻塞玩家 shard

重点改造：

```text
runtime/brpc/GatewayAuthClients.cpp
runtime/brpc/GatewayLoginOrchestrator.cpp
runtime/brpc/GatewayEnterMapOrchestrator.cpp
db/BrpcGameDbRepository.cpp
game/GameLogic.cpp 的资产写链路
```

要求：

1. Login/Reconnect/EnterMap 使用 brpc 异步 callback 状态机。
2. GameDB Load/ApplyMutation/QueryUnknownResult 使用异步接口。
3. 发起异步前 `MarkAsyncInFlight(player_id)`，完成后通过 `CompleteAsyncInFlight` 回投同玩家队列。
4. callback 不直接跨线程修改 `TcpConnection` 或玩家状态。
5. Controller/request/response/channel/context 生命周期覆盖 RPC 完成。
6. 每个 RPC 设置 deadline；状态修改 `max_retry=0`。
7. 只有具备 operation_id/idempotency_key 的业务层才允许结果查询或重试。
8. 一个依赖超时不能阻塞同 shard 的其他玩家。
9. 异步上下文数量和内存必须有上限，过载返回明确错误。

测试：让一个玩家的 Session/GameDB RPC 延迟 3 秒，同 shard 的其他玩家仍能在目标延迟内完成无关请求。

## 3. 实现 Kick 和断线队列补偿

### Kick

`SessionService.Kick` 不得继续返回 `kick deferred`。

最小流程：

```text
Session CAS 生成更高 generation/使旧 fence 失效
→ 根据旧 gateway_instance_id 调 GatewayKickService
→ Gateway 比较 player_id + session_id + generation
→ 只关闭旧连接
→ GameLogic 旧 fence 继续 fail-closed
```

Kick RPC 失败不影响新 fence 生效，但必须进入有限重试/指标并最终由旧请求 fence 拒绝兜底。

### 断线队列

GatewayDisconnectAsync 队列满时不能只记录日志：

- 首选直接走已初始化的异步 Session RPC 通道。
- fallback 任务必须具备幂等字段 `player_id+fence+generation`。
- 提供有界重试或短期补偿日志/Redis Stream；不能阻塞 Reactor。
- 指标至少包括 accepted、dropped、retried、compensated、failed。
- 断线风暴结束后不得残留错误 ONLINE Session。

## 4. 将 E2E 改成真正的业务故障测试

### 4.1 真正的 kill-gw0 同 Session 重连

现有场景不是同一个活动连接被 kill 后再重连。

扩展 E2E client，增加两阶段控制：

```text
客户端连接 gw0
→ Register/Login/EnterMap
→ 保持 TCP 打开并输出 READY_TO_KILL + reconnect_ticket + last_server_seq
→ 测试脚本 SIGKILL gw0
→ 客户端检测 EOF/timeout
→ 使用同一 player/session/ticket 连接 gw1
→ Reconnect
→ 验证新 fence/generation、生效路由、可靠 Push 重放或全量快照
→ 旧 fence 命令被拒绝
```

禁止 kill 前主动关闭 gw0 连接，禁止 kill 后用新账号 Login 代替 Reconnect。

### 4.2 Logic 崩溃后的最小恢复语义

真实玩家流程：

```text
登录并进入 gl-1 Owner 的地图
→ 提交一笔 GameDB 已确认资产变更
→ SIGKILL gl-1
→ 健康快照移除 gl-1
→ 新 Placement 获得更高 epoch/route_version
→ 玩家 Reconnect 或重新 Login/EnterMap 到 gl-0
→ 从 GameDB 恢复已提交资产
→ 使用旧 epoch 向旧/恢复节点写入必须被拒绝
```

本次允许实时地图内存状态丢失，但已确认资产不得回滚或重复。

### 4.3 依赖故障

至少验证：

- kill sess-0 后，新 Login/Reconnect 能通过 sess-1 完成，而不是只检查 `/api/version`。
- kill gamedb-0 后，幂等未知结果由 gamedb-1 查询并得到唯一结果。
- Redis 不可达时 Session/Gateway ready=false，新登录 fail-closed；恢复后可重新 ready。
- MySQL 不可达时 GameDB ready=false，资产写不得返回成功。
- SIGTERM 时实例先 ready=false/摘流，再完成有界 drain 并退出。

## 5. 必需测试禁止 SKIP

整理测试模式：

```text
unit：不依赖外部服务，缺测试二进制立即失败
integration：必须有 Redis/MySQL/brpc，依赖不可达立即失败
e2e：必须启动完整固定拓扑，缺任一进程/端口立即失败
experimental：明确独立，不计入本次稳定结论
```

要求：

- `auth_token_store_test`、Session CAS、Placement、PushReplayStore 等 Redis 测试放入 integration，并禁止 `SKIP → 0`。
- MySQL 资产/Outbox 测试禁止 `SKIP → 0`。
- `test_unit.sh` 中本次稳定所需二进制不得使用 `if [[ -x ... ]]` 可选执行。
- 脚本输出 JSON/JUnit 时必须记录 `required/skipped/exit_code`；required 被 skip 直接失败。
- `stable_gate --full` 中 experimental 项可以 skipped，但不得伪装成 required PASS。

## 阶段二必需命令

```bash
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/final_e2e.sh --start-cluster
./scripts/test_session_failover.sh
./scripts/test_gamedb_unknown_result_failover.sh
./scripts/test_gateway_active_reconnect.sh
./scripts/test_logic_player_recovery.sh
./scripts/network_partition_drill.sh
```

每个脚本失败必须返回非零，并清理自己启动的进程。

---

# 阶段三：同一 commit 的稳定发布门禁

本阶段原则上不再修改业务代码；若门禁暴露问题，回到前一阶段修复后从头执行。

## 1. 可复现工具链

必须保证全新 clone 可以执行：

```bash
./scripts/install_deps.sh --build-brpc
./scripts/check_deps.sh --full
./scripts/bootstrap_local_config.sh
```

要求：

- 锁定 brpc、protobuf/protoc、OpenSSL 等版本和校验和。
- 不依赖开发机未声明的 `/usr/local` 内容。
- Debug 和 Release 使用不同构建目录。
- 配置生成不覆盖已有配置，不写入真实密码或线上地址。
- CI、Docker 和本地脚本使用同一依赖版本。

## 2. 同一最终 commit 的硬门禁

在干净工作树上执行：

```bash
./scripts/check_deps.sh --full
./scripts/build.sh Debug
./scripts/build.sh Release
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/test_sanitizers.sh asan
./scripts/test_sanitizers.sh ubsan
./scripts/test_sanitizers.sh tsan
START_CLUSTER=1 E2E_ROUNDS=20 ./scripts/test_e2e_20x.sh
LOAD_DURATION_SEC=1800 LOAD_CONCURRENCY=32 ./scripts/load_tcp_baseline.sh
SOAK_DURATION_SEC=7200 ./scripts/soak_test.sh
./scripts/stable_gate.sh --full
```

不得降低时长、轮数、并发或成功率阈值后仍宣称正式稳定。

## 3. Stable gate 必须核验

`stable_gate.sh --full` 必须检查：

1. 工作树干净。
2. Debug/Release 都是当前 HEAD 构建。
3. Unit/Integration 没有 required SKIP。
4. 双 GW 活动 Session kill/reconnect 场景通过。
5. Session 和 GameDB failover 业务场景通过。
6. Logic 玩家持久化恢复场景通过。
7. ASan/UBSan/TSan 必需测试通过。
8. E2E、Load、Soak 报告的 commit 等于当前 HEAD。
9. 进程结束后无遗留 PID、端口和临时配置污染。
10. 生成 `run/release/<commit>/manifest.json`，包含：

    ```text
    commit
    build_type
    dependency_versions
    test_steps
    exit_codes
    required_skips
    report_paths
    report_sha256
    start/end time
    dirty
    verdict
    ```

只有全部通过，才允许输出：

```text
STABLE PASS — candidate server-stable-v0.1.0-rc1
```

任一步未执行、失败、超时、报告不属于当前 commit 或 required skip，必须输出：

```text
STABLE BLOCKED
```

## 4. 发布文档必须明确限制

更新 `docs/release/server-stable-v0.1.0.md`：

- 这是固定双 GW/双 Session/双 Logic/双 GameDB 的客户端联调稳定基线。
- Redis 和 MySQL 仍为基础设施单点，不宣称生产 HA。
- Logic 崩溃通过重新登录/重新进图和 GameDB 恢复，不承诺实时地图无损。
- 动态 gl-2、自动 Placement 无损迁移、Registry outage 自愈仍为 Experimental。
- Unity 和 Cell/Zone 大世界属于后续版本。

不要在本任务中创建或 Push Tag；由用户审核 release manifest 后决定。

---

# 四、最终交付格式

Cursor 完成每个阶段后必须输出：

```text
阶段：
实际 HEAD：
修改文件：
修复项：
新增测试：
执行命令：
真实结果：
required skip 数量：
遗留风险：
是否允许进入下一阶段：YES/NO
```

阶段三结束额外输出：

```text
最终 commit：
stable_gate verdict：
release manifest：
E2E 报告：
Load 报告：
Soak 报告：
Sanitizer 报告：
仍存在的基础设施单点：
建议 Tag（只建议，不创建）：
```

---

# 五、禁止事项

- 不引入 gRPC。
- 不新增独立 Login 进程。
- 不重写 Reactor。
- 不让客户端直连 GameLogic。
- 不让 GameLogic 接收账号密码或原始凭证。
- 不在 Reactor I/O 线程同步调用 brpc/Redis/MySQL。
- 不用 detached thread 逃避生命周期问题。
- 不用 `mt19937/rand/time/pid` 生成安全 token。
- 不把 Redis 当成玩家资产事实源。
- 不让正式 GameLogic/World/Auth 绕过 GameDB 直接写 MySQL。
- 不通过 `SKIP/WARN/|| true/删除断言` 获得稳定结论。
- 不把 Experimental 场景标记成稳定能力。
- 不在未通过同 commit 完整门禁时输出 STABLE PASS。

---

# 六、最终验收结论规则

只有同时满足以下条件，当前工程才可作为本次定义的服务器稳定候选版本：

1. 阶段一全部数据正确性和生命周期测试通过。
2. 阶段二真实依赖、真实 kill 和同 Session E2E 通过。
3. 阶段三完整门禁在同一最终 commit 上通过。
4. required skip 为 0。
5. Release manifest 与当前 HEAD、构建产物和报告一致。
6. 文档明确 Redis/MySQL 单点和非无损 Logic 恢复边界。

否则统一判定：

```text
STABLE BLOCKED
```
