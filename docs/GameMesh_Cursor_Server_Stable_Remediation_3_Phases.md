# GameMesh 服务器稳定版本：Cursor 三阶段实施提示词

> 仓库：`https://github.com/madongxin/webserver`
>
> 实施基线：`main@d631c2f8dcecd70f28b674c27cac09dc63220e31`
>
> 唯一目标：完成一个可重复构建、可自动回归、具备正确分布式语义的服务器稳定版本。
>
> 本文不包含 Unity、C# SDK、客户端 UI、客户端协议封装或任何客户端工程工作。

请将本文整体交给 Cursor。必须严格按“阶段一 → 阶段门禁 → 阶段二 → 阶段门禁 → 阶段三”的顺序执行。每个阶段完成并通过验收后停止，等待用户确认，再进入下一阶段。

---

## 一、当前版本结论

当前版本已经具备较完整的分布式服务器骨架：

- Gateway、Auth/Session、GameLogic、GlobalService、GameDB 多进程拆分。
- 双 Gateway、双 Session、双 GameLogic、双 GameDB 的部署配置。
- Gateway 编排 Login、Reconnect、EnterMap 和跨 GameLogic Transfer。
- `session_id + fence_token + generation` 会话隔离。
- Placement、Owner epoch、route version 和 lease 基础结构。
- Redis Session Lua、Redis Registry 和 Push Replay Store。
- GameLogic 玩家串行队列。
- GameDB 资产事务、幂等表和 Outbox 基础实现。
- 基础 E2E、故障脚本和稳定门禁脚本。

但它目前只能定义为“分布式服务器候选版本”，不能标记为稳定版本。主要阻塞如下：

| 优先级 | 当前缺口 | 稳定性影响 |
| --- | --- | --- |
| P0 | 客户端正式链路仍可调用 `GrantItem` | 玩家可给自己发放资产 |
| P0 | 客户端正式链路仍可调用 `MailDeliver` | 玩家可给自己创建带附件邮件 |
| P0 | Push ACK 不校验最大序号和单调性 | 超前 ACK 可裁剪未处理可靠消息 |
| P0 | FullSnapshot 导出失败仍可能发送成功结构 | 下游可能使用不完整状态覆盖正确状态 |
| P0 | `SaveSnapshot` 忽略 `idempotency_key` | 超时重试无法保证快照写入幂等 |
| P0 | GameDB 写入未知结果没有查询闭环 | RPC 超时后可能重复扣除或发放 |
| P0 | 动态替换 Session/Logic Channel 缺少并发保护 | 服务发现更新时可能数据竞争或悬空引用 |
| P1 | Formal Placement 允许 epoch/route 为 0 时跳过比较 | 地图写 fence 不够严格 |
| P1 | Gateway 部分流程仍使用会静默丢任务的 `Post` | 过载时 Login/EnterMap 只表现为客户端超时 |
| P1 | Kill Session/Logic/GameDB 测试多为进程存活检查 | 没有验证业务连续性和状态正确性 |
| P1 | 动态增加 gl-2 和混沌长跑没有自动化 | 动态扩容能力未被真实证明 |
| P1 | 完整 CI 没有提供 brpc 工具链 | clean clone 无法在 CI 中完成全量构建 |
| P1 | Release、Sanitizer、20×E2E、soak 尚未形成硬门禁 | 不能证明版本可长期稳定运行 |

---

# 二、全局执行约束

你现在位于 GameMesh 项目根目录。不要只输出分析；必须直接修改代码、配置、脚本、测试和服务器文档。

开始每个阶段前：

1. 完整阅读 `AGENTS.md`。
2. 执行：

   ```bash
   git status --short --branch
   git log -1 --oneline
   ```

3. 保留用户已有未提交修改，不覆盖无关文件。
4. 阅读本阶段涉及的代码、proto、CMake、配置、脚本和现有测试。
5. 先还原真实运行调用链，再修改。
6. 未经用户明确要求，不要 commit、tag 或 push。

固定架构边界：

- C++17。
- 客户端入口继续是 Reactor TCP + ProtoFraming。
- 内部 RPC 继续使用 brpc，不引入 gRPC。
- 不推倒重写 Reactor。
- 不新增独立 Login 进程；AuthService 与 SessionService 继续同二进制、逻辑分离。
- Client 不能直接连接 Session、GameLogic、GlobalService、GameDB 或注册中心。
- GameLogic 是同构通用节点。
- 同一个运行时 MapInstance 同一时刻只能有一个可写 Owner。
- MySQL 是账号与资产事实源。
- 正式模式中，GameLogic 和 GlobalService 的资产写入必须经 GameDB。
- Redis 当前可以承载 Session、Placement、Registry 和 Push Replay，但文档必须标注其 HA 边界。

本轮明确不做：

- 不创建 Unity 工程。
- 不创建 C# SDK。
- 不生成 Unity Protobuf 文件。
- 不修改客户端 UI 或客户端状态机。
- 不以“方便客户端接入”为理由重构外部协议。
- 不删除现有 proto 字段号；需要封闭危险命令时优先在 Gateway 正式模式策略中拒绝，保持协议兼容。

质量规则：

- 禁止空实现、固定返回成功或删除断言来通过测试。
- 禁止吞掉编译和测试失败码。
- 必需测试缺少二进制、依赖或环境时必须返回非零状态。
- 可选测试必须与稳定门禁分开，不能用 `SKIP` 伪装稳定版本通过。
- Reactor I/O 线程不得执行同步 brpc、Redis 或 MySQL 调用。
- 所有 RPC 必须设置 deadline/timeout。
- 状态修改请求默认 `max_retry=0`。
- 业务重试必须使用稳定幂等键。
- brpc 异步上下文必须覆盖 Controller、request、response 和 callback 生命周期。
- 服务端 `done` 必须恰好调用一次。
- 所有正式模式依赖失败必须 fail-closed。

---

# 阶段一：修复服务器正确性与资产安全阻塞

## 1. 阶段目标

优先消除会造成资产作弊、消息丢失、重复资产操作、错误快照和双写的 P0 问题。本阶段不扩展部署规模，不做长时间压测。

阶段一通过后，服务器应满足：

- 公网客户端无法调用内部发奖和系统邮件投递能力。
- Reliable Push 的分配、回放和 ACK 语义严格正确。
- GameDB 资产变更和快照写入具备稳定幂等语义。
- Formal Map 写入必须携带完整权威 fence。
- 玩家生命周期操作保持同一玩家严格串行。
- 过载时返回明确错误，不静默丢请求。

## 2. 封闭客户端资产管理接口

当前 `GrantItem` 和 `MailDeliver` 可以从客户端 TCP 请求进入正式业务链路。这两个接口必须从公网命令面封闭。

实现统一的服务器端命令策略，例如：

```cpp
enum class CommandTrustLevel {
    PreAuthClient,
    AuthenticatedClient,
    InternalService,
    AdminService,
};

CommandDecision ValidateCommandPolicy(
    GameRequest::BodyCase body,
    CommandTrustLevel trust,
    bool formal_mode);
```

要求：

1. Gateway 正式模式只允许明确的客户端命令 allowlist。
2. 以下命令在正式客户端 TCP 链路中必须返回 `ERR_COMMAND_FORBIDDEN`：
   - `GrantItem`
   - `MailDeliver`
   - 后续新增的 GM、补偿、系统发奖、运营邮件或管理命令
3. 不要只依赖客户端隐藏按钮或文档约定。
4. 不要仅校验 receiver_id 等于当前玩家；玩家仍不能给自己构造系统奖励。
5. 如需保留联调能力，只能通过显式开发配置开启，例如：

   ```text
   allow_unsafe_debug_commands=false
   ```

   默认必须为 false；`GAMEMESH_FORMAL=1` 时即使配置误开也必须拒绝。
6. 系统发奖继续使用 GameDB 内部资产服务或受鉴权的内部 brpc 接口。
7. 系统邮件投递继续使用 `MailService::Deliver` 或独立内部 brpc Service，不能复用公网 Client trust level。
8. 内部接口至少校验服务身份；如 mTLS 尚未在本阶段完成，先限制内部网络、服务 allowlist 和不可伪造的调用来源，并记录后续安全项。
9. 保留现有 protobuf 字段号，避免本阶段引入客户端兼容性破坏。

新增测试：

- Formal 模式下，登录玩家调用 GrantItem 被拒绝，资产不变化。
- Formal 模式下，登录玩家调用 MailDeliver 被拒绝，不新增邮件。
- 开发模式只有显式配置开启时才允许测试命令。
- 内部授权路径仍可完成幂等发奖和系统邮件投递。
- 新增管理类请求未登记 trust level 时默认拒绝。

## 3. 修复 Push ACK 原子校验

当前 ACK Lua 只裁剪 `seq <= ack_seq`，没有验证 ACK 是否超前或回退。

修改 `PushReplayStore::Ack` 及其 Lua，使一次原子操作完成：

```text
读取 current_server_seq
读取 last_ack_seq
校验 session replay keys
校验 ack_seq
裁剪 replay list
更新 last_ack_seq 和 TTL
```

必须满足：

1. `ack_seq > current_server_seq`：拒绝，返回 `ERR_ACK_AHEAD`，不裁剪。
2. `ack_seq < last_ack_seq`：拒绝或作为旧 ACK 幂等忽略，但绝不能回退 lastAck。
3. `ack_seq == last_ack_seq`：幂等成功，不重复修改。
4. `last_ack_seq < ack_seq <= current_server_seq`：原子裁剪并更新。
5. ACK 仍须匹配当前：
   - player_id
   - session_id
   - fence_token
   - generation
6. 旧 Session、旧 Gateway 迟到 ACK 不得影响新 Session。
7. Redis 不可用时 Reliable ACK 必须失败，不能只更新进程内状态后返回成功。
8. 增加指标：
   - `push_ack_ok`
   - `push_ack_duplicate`
   - `push_ack_ahead_rejected`
   - `push_ack_stale_rejected`

新增 Redis 集成测试：

- 正常连续 ACK。
- 重复 ACK。
- 回退 ACK。
- 超前 ACK。
- 旧 Session ACK。
- 并发 ACK 乱序到达。
- 非法 ACK 后 Replay 数据仍完整。

## 4. 修复 Replay 与 FullSnapshot 一致性

要求：

1. Replay 必须保存完整记录：

   ```text
   player_id
   session_id
   server_seq
   message_type
   payload
   reliable
   coalescable
   created_at
   ```

2. 重连 Replay 必须保持原始 `server_seq` 和 `message_type`。
3. FullSnapshot 导出失败时：
   - 不得发送 `ok=true` 的空快照。
   - 不得更新 baseline。
   - 不得裁剪旧 Replay。
   - 返回明确服务器错误并允许后续重试。
4. FullSnapshot 的 outer envelope server_seq、inner baseline 和 Replay Store 中保存的数据必须一致。
5. 不允许先把 baseline=0 的 payload 写入 Replay，再只修改网络发送副本。
6. 如需要拆分序号分配和写入，必须通过 Redis Lua/CAS 保证：
   - 序号唯一。
   - 同一个 seq 不会写入两个不同 payload。
   - 写入失败不会被视为已发送成功。
7. 重连发送顺序固定为：

   ```text
   ReconnectResponse
   → Replay 列表或 FullSnapshot
   → 恢复实时 Push
   ```

8. 发送失败不能提前 ACK 或删除 Replay。

新增测试：

- Replay 精确保留 seq、type 和 payload。
- 缓存缺口触发真实 FullSnapshot。
- Snapshot Export 失败时不发送假快照。
- Snapshot Redis 写入失败时不返回成功 baseline。
- Replay FullSnapshot 时 baseline 与 envelope seq 一致。
- 同一 Session 并发 Push 的 server_seq 唯一、连续或具有明确 gap 语义。

## 5. 完成 GameDB 快照幂等

当前 `SaveSnapshot` 接收但忽略 `idempotency_key`。请实现完整幂等结果持久化。

要求：

1. `idempotency_key` 为空时，正式模式拒绝 SaveSnapshot。
2. 同一幂等键首次执行时，在同一 MySQL 事务中完成：
   - 锁定玩家资产版本。
   - 校验 expected_version。
   - 写入 bag snapshot。
   - 提升 asset_version。
   - 保存幂等结果。
   - 写入 Outbox（如该快照需要事件）。
3. 相同幂等键重试返回首次执行结果和同一个 new_version，不重复提升版本。
4. 相同幂等键但请求指纹不同，返回 `IDEMPOTENCY_CONFLICT`。
5. 幂等结果至少保存：

   ```text
   idempotency_key
   operation_type
   player_id
   request_hash
   result_code
   asset_version
   created_at
   ```

6. 所有资产写入继续保证 Outbox insert 失败时回滚资产事务。
7. 为 Outbox 和幂等表增加必要索引及迁移脚本。

新增 MySQL 集成测试：

- 相同 SaveSnapshot 并发执行 10 次，只提升一次版本。
- 相同 key、不同 payload 被拒绝。
- Outbox insert 故障注入导致整个资产事务回滚。
- 事务提交后 RPC 模拟丢响应，再次执行返回幂等结果。

## 6. 增加未知结果查询闭环

为 GameDB 内部协议增加操作结果查询接口，例如：

```proto
rpc QueryOperationResult(QueryOperationResultRequest)
    returns (QueryOperationResultResponse);
```

查询键为：

```text
player_id + idempotency_key + operation_type
```

要求：

1. ApplyAssetMutation、SaveSnapshot、ClaimMailAttachments 等状态修改操作都能查询最终结果。
2. RPC 超时或连接断开时，调用方不能立即换节点重复执行。
3. 正确流程：

   ```text
   写 RPC 结果未知
   → QueryOperationResult
   → 已完成：返回已保存结果
   → 明确未执行：允许使用相同幂等键重试
   → 仍未知：返回可重试错误，不修改本地状态
   ```

4. GameLogic 只有确认 GameDB 成功后才能更新内存资产状态。
5. 不能用新的幂等键重试同一业务操作。

## 7. 收紧 Formal Placement fence

修改权威 Placement 校验：

1. Formal 模式下地图写请求必须携带：
   - map_instance_id
   - owner_epoch
   - route_version
   - gamelogic_instance_id
2. epoch 或 route_version 为 0 时返回 `ERR_PLACEMENT_FENCE_REQUIRED`，不得跳过校验。
3. 必须精确验证：
   - state == READY
   - local GameLogic == authoritative owner
   - request epoch == authoritative epoch
   - request route version 与权威版本符合定义
   - lease 未过期
4. 非 Formal 单体兼容逻辑必须通过显式配置隔离，不能影响正式模式。
5. 删除或修改“zero meta skips epoch/route”的单元测试，替换为 Formal 拒绝测试。
6. EnterMap、MapPing、LeaveMap 和后续所有地图写命令使用同一权威验证入口。

## 8. 修复玩家队列过载语义

Gateway 的 Login、Reconnect、EnterMap 等流程不得继续使用可能静默丢任务的 `PlayerSerialQueue::Post`。

要求：

- 全部关键入口使用 `TryPost`。
- 队列满时立即返回 `ERR_OVERLOAD`。
- 不进入同步 fallback。
- 不创建半完成 Session、Transfer 或 Bind。
- 增加队列深度、拒绝数和等待时间指标。
- 队列任务异常必须完成对应回调并返回失败，不能让请求永远等待。

新增测试：

- Login 队列满时返回过载且不创建 Session。
- EnterMap 队列满时不开始 Transfer。
- Freeze/Export/Import/Unbind 队列满时保持原状态并返回明确错误。

## 9. 阶段一门禁

至少执行：

```bash
./scripts/check_deps.sh --full
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
```

必须新增并通过：

```text
command_policy_test
push_ack_redis_test
push_full_snapshot_test
gamedb_snapshot_idempotency_test
gamedb_unknown_result_test
placement_formal_fence_test
gateway_overload_test
```

阶段一完成标准：

- 公网客户端资产管理接口已封闭。
- Push ACK、Replay、FullSnapshot 全部原子正确。
- SaveSnapshot 幂等和未知结果查询闭环完成。
- Formal Placement 不接受缺失 fence。
- 关键队列过载不再静默丢请求。
- Debug 全量构建、单元和真实 Redis/MySQL 集成测试通过。
- 没有必需测试 SKIP。

完成后输出修改文件、调用链变化、测试命令、真实结果和遗留问题，然后停止等待用户确认。

---

# 阶段二：补齐多实例高可用与并发正确性

## 1. 阶段目标

在阶段一正确性基础上，使双 Gateway、双 Session、双 GameLogic、双 GameDB 和动态服务发现具备真实业务连续性，而不是只启动多个进程。

## 2. 修复动态 Channel 并发更新

当前 `GatewayAuthClients` 会在发现列表变化时替换 `session_channel_` 和 `logic_channels_`，但请求线程可能同时读取。

采用不可变快照或严格锁保护，例如：

```cpp
struct RpcChannelSnapshot {
    std::shared_ptr<brpc::Channel> session;
    std::unordered_map<std::string, std::shared_ptr<brpc::Channel>> logic;
    uint64_t version = 0;
};
```

要求：

1. 新快照在局部完整构建并验证成功后一次性发布。
2. 在途 RPC 持有旧 `shared_ptr`，直到 callback 完成。
3. 更新失败保留最后有效快照。
4. 读路径不能引用更新线程可能清理的容器元素。
5. `ready()`、peer_count、started 等共享字段不存在数据竞争。
6. `SessionRpcClient`、`GatewayAuthClients`、`BrpcChannelManager`、`BrpcGameDbRepository` 和 GatewayPush Channel 使用一致的快照原则。
7. 不要在锁内执行 `Channel::Init` 或远程调用。

新增测试：

- 100 个线程持续 RPC 路由查询，同时反复更新实例快照。
- 删除旧节点后，在途调用安全完成，新调用不再选择旧节点。
- 空发现和临时 Registry 故障不清空可用 Channel。
- TSan 下无数据竞争。

## 3. Session ×2 业务高可用

双 Session 测试必须验证真实业务，而不是只检查 `/api/version`。

要求：

1. Gateway 使用多地址 Channel 和明确负载策略。
2. Session 二进制保持无状态，权威 Session 数据在 Redis Lua 中原子更新。
3. kill session-0 后必须验证：
   - 新账号可以登录。
   - 已断线玩家可以从另一个 Gateway Reconnect。
   - 旧 fence 不能继续发命令。
   - MarkDisconnected 旧定时器不能误杀新 Session。
4. 状态修改 RPC 不使用 brpc 自动重试；未知结果通过 operation record 查询。
5. Session 恢复上线后可以重新加入服务发现，不覆盖更新版本的数据。

## 4. GameLogic 故障恢复自动化

当前不能依赖测试脚本手工调用 `map_lease_drill` 修改 Placement。

实现最小稳定恢复控制流程：

1. GameLogic 持续续租其拥有的 MapInstance。
2. Session/Placement 定时扫描过期 lease。
3. lease 过期后原子执行：

   ```text
   READY
   → RECOVERING
   → 选择健康新 Owner
   → CAS 提升 owner_epoch 和 route_version
   → RECOVER/RECREATE
   → READY
   ```

4. 旧 Owner 恢复后必须读取权威 epoch，并拒绝旧 epoch 写入。
5. 没有实时地图快照时，明确采用以下一种策略并写入文档：
   - 从持久化快照恢复；或
   - 重建地图实例并要求玩家重新进图。
6. 不得伪装成无损恢复。
7. 故障恢复的每一步必须幂等。
8. 恢复控制器自身多实例运行时使用 CAS/leader lease，不能两个 Session 同时接管同一地图。

## 5. GameDB ×2 业务高可用

双 GameDB 必须验证资产请求，而不是只验证第二个进程存活。

要求：

1. 读请求可在健康 GameDB 之间切换。
2. 写请求以稳定幂等键固定操作身份。
3. 首节点超时后先查询 operation result，再决定是否在另一节点重试。
4. kill 当前 GameDB 后：
   - 已提交但响应丢失的请求不会重复修改资产。
   - 明确未执行的请求可安全在另一实例执行。
   - 无法确认时返回明确未知状态，不错误返回成功。
5. GameDB 两个实例共享同一事实源时，数据库连接池、Outbox claim 和幂等表支持并发实例。
6. Outbox publisher 使用 claim/lock，避免多实例同时发布同一行。

## 6. 消除玩家 shard 的同步下游阻塞

当前部分 GameDB、Session 或 mail repository 调用仍是同步调用，可能阻塞 `PlayerSerialQueue` shard 上的其他玩家。

要求：

1. Reactor I/O 线程保持完全异步。
2. 玩家串行模型仍保证同一玩家一个命令完成后才执行下一命令。
3. 下游 RPC 改成异步后，使用 per-player mailbox 的 in-flight 状态暂停该玩家，而不是阻塞整个 shard worker。
4. RPC callback 回投玩家 mailbox，再提交内存状态和发送响应。
5. 玩家 A 的慢 GameDB 请求不能阻塞同 shard 的玩家 B。
6. `ClaimMailAttachmentsAsync` 必须是真异步实现，不能在函数内同步 RPC 后立即回调。
7. 所有异步操作有 deadline、取消、过载和关闭处理。

新增测试：

- 玩家 A 的 GameDB RPC 延迟 3 秒，玩家 B 请求仍能及时完成。
- 同一玩家连续两个资产请求仍严格有序。
- callback 在 Logout/Unbind 后到达时不修改已失效 Actor。
- shard 停止期间所有在途 callback 安全结束。

## 7. 动态扩容、摘流与注册中心

继续使用当前 Redis Registry 时，至少完成：

- Register、KeepAlive、Discover、DRAINING、Unregister。
- 可路由 advertise address，禁止 `0.0.0.0`。
- 索引残留清理。
- Redis 临时不可用时保留最后有效快照和静态配置降级。
- 新增 gl-2 后无需重启 Session/Gateway 即可获得新 MapInstance。
- gl-2 DRAINING 后不再接收新玩家和新地图。
- GatewayPush 地址能够动态增加、删除和刷新。
- 运行期不逐请求创建 Channel。

文档明确：

- Redis Registry 是当前稳定版本的控制面实现。
- Redis 单点部署不等于生产 HA。
- 如果 Redis 同时承载 Session、Placement 和 Registry，必须给出 Sentinel/Cluster 或托管 Redis 的部署建议。

## 8. 阶段二故障 E2E

新增严格业务测试：

1. kill gw0：原连接断开后通过 gw1 Reconnect，继续发送真实业务命令。
2. kill session-0：session-1 完成新 Login 和旧玩家 Reconnect。
3. kill logic Owner：自动检测 lease，提升 epoch，旧 Owner 写被拒绝，玩家按文档策略恢复。
4. kill gamedb-0：执行真实资产写，最终只变化一次。
5. 动态启动 gl-2：新地图真实分配到 gl-2。
6. 将 gl-2 标记 DRAINING：不再分配新地图，在途玩家按策略完成。
7. Registry 临时不可用：最后有效 Channel 继续工作，恢复后自动收敛。
8. 并发更新服务发现快照：无崩溃、无 use-after-free、无 TSan 报告。

每个测试必须断言：

- 业务响应。
- error_code。
- Session/Placement/资产最终状态。
- fence/epoch/version 是否符合预期。

不能只检查进程或 HTTP 端口存活。

## 9. 阶段二门禁

至少执行：

```bash
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/final_e2e.sh --start-cluster
```

并新增：

```text
scripts/test_session_failover.sh
scripts/test_logic_auto_recovery.sh
scripts/test_gamedb_unknown_result_failover.sh
scripts/test_dynamic_logic_scale.sh
scripts/test_registry_outage.sh
scripts/test_channel_snapshot_race.sh
```

阶段二完成标准：

- 多实例切换验证的是业务连续性，不是进程存活。
- GameLogic 故障恢复不再依赖人工修改 Placement。
- GameDB 未知结果不会造成重复资产变化。
- 动态发现 Channel 快照线程安全。
- 玩家异步下游调用不阻塞无关玩家。
- TSan 并发专项测试通过。
- 阶段二全部必需测试无 SKIP。

完成后输出修改文件、故障状态机、测试命令、真实结果和遗留问题，然后停止等待用户确认。

---

# 阶段三：可重复构建、压测、混沌与稳定发布门禁

## 1. 阶段目标

把阶段一和阶段二的正确实现收敛为可从 clean clone 构建、可持续回归、可观测、可发布和可回滚的服务器稳定版本。

## 2. 提供完整可重复工具链

当前 CI full job 不能依靠 runner 预装 brpc 后直接失败。

必须选择一种可重复方案：

### 方案 A：固定工具链镜像

提供带固定版本的：

- GCC/Clang
- CMake
- Protobuf/protoc
- brpc
- gflags
- leveldb
- jsoncpp
- hiredis
- MySQL client
- OpenSSL

### 方案 B：依赖构建脚本

提供：

```text
scripts/install_deps.sh
```

要求：

- 不自动 sudo。
- 固定 brpc、protobuf 和关键依赖版本。
- 支持重复执行。
- 校验 checksum。
- 输出清晰的缺失系统包说明。

无论选择哪种方案，都必须保证：

```text
全新 clone
→ bootstrap config
→ check_deps --full
→ Debug build
→ Release build
→ tests
```

可以在 CI 和文档化开发环境中重复执行。

## 3. 修复 Docker 与 CI

要求：

1. Docker build stage 实际安装或继承 brpc，不再假设 `/usr/local/include/brpc` 已存在。
2. 多角色镜像通过 entrypoint 正确启动：
   - gateway
   - session
   - gamelogic
   - world/global
   - gamedb
3. Compose 能启动完整双实例测试拓扑。
4. CI 至少包含：
   - Debug full build。
   - Release full build。
   - Unit。
   - Redis/MySQL integration。
   - 双 GW final E2E。
   - ASan。
   - UBSan。
   - TSan 并发专项。
   - shellcheck。
   - `git diff --check`。
5. 修复文档和生成文件中的 trailing whitespace。
6. CI 不能通过 `bootstrap.sh || true` 隐藏配置失败。

## 4. 重写稳定门禁脚本

`scripts/stable_gate.sh --full` 必须真正覆盖稳定版本要求，而不是运行后再提示“仍然 BLOCKED”。

建议入口：

```bash
./scripts/stable_gate.sh --full
```

必须依次执行：

```text
依赖检查
配置初始化
Debug 全量构建
Release 全量构建
单元测试
Redis/MySQL 集成测试
双 GW/双 Session/双 Logic/双 GameDB E2E
ASan
UBSan
TSan/并发专项
20 轮核心 E2E
基础压力测试
故障演练
soak test 或校验已有同 commit soak 报告
```

任何必需步骤失败或未执行，门禁必须返回非零。

## 5. 强化最终 E2E 场景

替换当前弱断言：

### 场景 1：Login

- 验证 Auth、Session、BindPlayer 全链路。
- 验证唯一有效 Session 和 fence。

### 场景 2：跨 GameLogic 进图

- 验证 Freeze、Export、Import、Commit、Gateway route 更新和旧 Owner Unbind。
- 后续普通命令必须到新 Owner。

### 场景 3：Reliable Push

- 精确断言 message_type、server_seq、payload 和 ACK 结果。
- 不允许 Push/Replay/Snapshot 任意一个出现就算成功。

### 场景 4：Gateway 故障

- 玩家先登录 gw0。
- SIGKILL gw0。
- 同一个玩家从 gw1 Reconnect。
- 重连后继续真实业务命令。
- 旧连接迟到事件不能删除新绑定。

### 场景 5：Session 故障

- kill session-0。
- session-1 完成实际 Login、Reconnect、Disconnect 和 Logout。
- 不接受只检查 `/api/version`。

### 场景 6：GameLogic 故障

- kill 当前 Map Owner。
- 自动发现 lease 过期并接管。
- 新 epoch 增加。
- 旧 epoch 写请求被拒绝。
- 玩家按定义恢复。

### 场景 7：动态扩容

- 运行中启动 gl-2。
- 不重启 Gateway/Session。
- 新 MapInstance 真实分配到 gl-2。

### 场景 8：GameDB 故障

- 在真实资产写入期间 kill gamedb-0。
- 查询 operation result。
- 最终资产只改变一次。
- gamedb-1 继续处理后续请求。

### 场景 9：优雅摘流

- DRAINING 后停止新 Login/Placement。
- 在途请求完成或明确失败。
- 从 Registry 注销后无新流量。

### 场景 10：混沌长跑

- 周期性重启 Gateway、Session、GameLogic、GameDB。
- 持续 Login、Reconnect、EnterMap、资产写和 Push。
- 检查重复资产、双 Session、双 Owner、消息序号和内存增长。

## 6. 压力与 soak 基线

提供不依赖 Unity 的 C++ 或脚本化协议压测客户端。

至少验证：

- 1,000 并发 TCP 连接的基础稳定性；并允许配置提升目标。
- Login 突发流量。
- 普通 Dispatch 持续 QPS。
- 大量跨 Gateway 重连。
- 热点 MapInstance。
- 慢客户端发送队列。
- Redis/MySQL 延迟注入。
- 玩家队列和下游 RPC 过载。

至少执行：

- 核心 E2E 连续 20 轮。
- 30 分钟基础负载测试。
- 2 小时 soak test。

记录：

```text
成功率
p50/p95/p99 延迟
当前连接数
RSS/堆内存趋势
线程数和 FD 数
队列深度
Redis/MySQL 延迟
RPC 超时率
Push Replay 积压
Session/Placement 冲突数
资产幂等命中与冲突数
```

## 7. 可观测性与故障诊断

Prometheus/监控至少覆盖：

- Gateway 当前连接、连接/断开速率、非法帧和慢客户端断开。
- 登录各阶段成功率和失败码。
- brpc QPS、延迟、超时和错误码。
- Player mailbox 深度、等待时间、in-flight 和拒绝数。
- 在线玩家数。
- 每个 GameLogic 的玩家数和 MapInstance 数。
- Placement READY/RECOVERING、lease 过期和迁移数。
- Session Redis 延迟、Lua 冲突和 fence 拒绝。
- GameDB 队列、MySQL 延迟、版本冲突和未知结果查询。
- Outbox backlog、claim、publish 成功和重试。
- Push sent/replay/full_snapshot/ack/reject/backlog。

日志必须包含 request_id/trace_id，但不能输出完整密码、Token、session secret 或证书。

## 8. 发布与回滚

稳定版本交付必须包含：

```text
docs/release/server-stable-v0.1.0.md
docs/runbook/deploy.md
docs/runbook/rollback.md
docs/runbook/gateway-failure.md
docs/runbook/session-failure.md
docs/runbook/logic-recovery.md
docs/runbook/gamedb-failure.md
```

Release Notes 至少记录：

- commit ID。
- 构建工具链和依赖版本。
- 数据库迁移版本。
- Redis key/schema 版本。
- 配置变更。
- 已通过测试和报告位置。
- 已知限制和仍存在的单点。
- 向前升级和回滚步骤。

升级要求：

- 数据库迁移尽量向后兼容。
- Redis key/schema 变更带版本前缀或迁移策略。
- 先部署兼容读，再部署新写。
- 支持单实例摘流和滚动升级。
- 回滚不会让旧二进制误读新状态。

## 9. 最终稳定版本判定

只有同时满足以下条件，才能输出 `STABLE PASS`：

- clean clone 全量 Debug 和 Release 构建成功。
- Unit、Redis/MySQL Integration 和 E2E 零失败。
- 必需测试零 SKIP。
- ASan、UBSan、TSan 无未解释错误。
- 资产管理命令不暴露公网客户端。
- Reliable Push ACK、Replay、Snapshot 正确。
- 资产变更和快照写具备幂等与未知结果查询闭环。
- Session、Logic、GameDB 故障测试验证真实业务连续性。
- 动态新增/摘除 GameLogic 通过自动化测试。
- 核心 E2E 连续 20 轮通过。
- 30 分钟负载测试通过。
- 2 小时 soak 无持续内存、线程、FD 或队列泄漏。
- 没有未处理的 Critical/High 正确性问题。
- 发布文档、部署 Runbook 和回滚方案完成。

建议候选版本号：

```text
server-stable-v0.1.0-rc1
```

全部门禁通过并经过人工审核后，才建议：

```text
server-stable-v0.1.0
```

未经用户明确授权，不要创建 tag、commit 或 push。

如果任何门禁未执行或失败，必须输出：

```text
STABLE BLOCKED
```

并列出阻塞项、复现命令、日志路径和下一步修复建议。不得降低门禁或改写历史结果。

---

# 三、每阶段统一交付格式

每个阶段完成后必须输出：

1. 当前 commit 和 `git status`。
2. 修改文件清单。
3. 修复的问题及代码位置。
4. 服务器调用链和状态机变化。
5. 新增/修改的配置项及默认值。
6. 数据库/Redis schema 变化与迁移方式。
7. 实际执行的构建、测试和故障演练命令。
8. 每个命令的真实结果和日志位置。
9. 未执行测试及其原因。
10. 本阶段门禁结果：`PASS` 或 `BLOCKED`。
11. 遗留风险和下一阶段入口条件。
12. 回滚方式。

不要只汇报“代码已实现”；必须用可复现的构建、业务断言、故障测试和稳定性数据证明结果。
