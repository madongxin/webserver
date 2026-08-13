# GameMesh 稳定版本最小修复提示词

请将下面整段任务直接交给 Cursor 执行。

---

你现在位于 `madongxin/webserver` 项目根目录，请基于最新 `main` 继续修改。

当前评估基线：

```text
commit: 03d55303ce58e867e123efd2705d8ab33ab43a94
状态: STABLE BLOCKED
目标: 固定双 Gateway / 双 Session / 双 GameLogic / 双 GameDB 拓扑的稳定服务器版本
```

本轮只修复服务器稳定版最后的最小阻塞项，不做 Unity 客户端，不做大世界 Cell，不做动态扩缩容，不做实时地图无损迁移，不引入 gRPC，不新增独立 Login 进程，不重写 Reactor。

## 一、执行规则

开始前必须：

1. 完整阅读 `AGENTS.md`。
2. 执行 `git status --short --branch`，保留用户已有修改。
3. 确认当前分支和 commit，记录本轮基线。
4. 阅读本任务涉及的实现、proto、CMake、测试和稳定门禁脚本。
5. 直接修改代码并运行测试，不要只输出建议。
6. 不删除现有测试，不使用空断言、WARN、SKIP、`|| true` 隐藏失败。
7. 不提交、不推送、不创建 Tag，除非用户随后明确要求。
8. 不修改无关模块，不执行破坏性 Git 操作。

必须保留：

- C++17。
- Client → Gateway 使用 Reactor TCP + ProtoFraming。
- 内部 RPC 使用 brpc。
- AuthService 与 SessionService 同进程、逻辑分离。
- `MarkDisconnected != Logout`。
- Session Redis Lua、fence、generation 和静态地址降级能力。
- GameDB 是正式资产访问边界。

## 二、当前已经修复的内容

不要回退以下修改：

- 邮件奖励已经写入 `player_asset_bag/player_asset_meta`。
- 邮件状态、正式资产、操作日志和 Outbox 已进入同一 MySQL 事务。
- `LoadInventory` 已移除旧 `player_item` 运行期 fallback。
- PlayerSerialQueue 已使用 async depth 处理链式异步。
- Gateway 断线路径的 Session/Logic brpc 已提供异步接口。
- Push ACK 已增加连续序列检查。
- E2E 客户端已解析真实 `server_seq`。
- 死亡 Owner Resolve reclaim 已增加 `owner_epoch/route_version`。

本轮只补齐下面四项。

---

# 阶段一：彻底修复同玩家异步串行

## 1.1 已确认的问题

当前 `PlayerSerialQueue` 只会把 `MarkAsyncInFlight` 之后新调用 `TryPost` 的任务放入 deferred。

以下场景仍会乱序：

```text
同玩家命令 A 已进入 shard 主队列
同玩家命令 B 紧接着也进入 shard 主队列
A 开始执行
A 调用 MarkAsyncInFlight 并发起异步 GameDB RPC
A 返回 worker
B 已经在主队列，因此继续执行
GameDB callback 稍后才到
```

已经通过最小复现得到：

```text
prequeued_serialization_violated=1
```

这会破坏邮件、背包、交易、客户端序列和 PlayerActor 的同玩家串行保证。

## 1.2 修改要求

推荐采用最小兼容设计：

1. 主队列元素不能再只有 `std::function<void()>`，必须保存：

   ```cpp
   struct TaskEntry {
       uint64_t player_id;
       TaskKind kind; // External / AsyncCompletion
       std::function<void()> fn;
   };
   ```

2. `TryPost` 创建 `External` 类型任务，并保留 `player_id`。
3. `CompleteAsyncInFlight` 创建 `AsyncCompletion` 类型任务。
4. Worker 从主队列取到 `External` 任务时，必须再次检查该玩家的 `async_depth`：

   - 如果 `async_depth > 0`，将该任务转移到该玩家 deferred，不能执行。
   - 这是为了覆盖任务已经在 `MarkAsyncInFlight` 之前进入主队列的情况。
   - 从主队列移到 deferred 不能重复增加或减少 `pending_global`。

5. `AsyncCompletion` 必须允许在 `async_depth > 0` 时执行，否则会死锁。
6. completion 执行完后再减少一层 async depth；如果 completion 内再次 `MarkAsyncInFlight`，仍不能释放 deferred。
7. 只有 async depth 真正归零时，才允许同玩家的 deferred 任务按原顺序回到可执行队列。
8. 保证：

   - 同一个玩家严格串行。
   - 不同玩家仍能通过不同 shard 并行。
   - completion 不受外部队列上限影响，已经发起的异步必须能够收尾。
   - `pending_global` 每个任务只增减一次。
   - DRAINING 允许已开始的 completion 完成。
   - STOPPED 不得 inline 执行业务 completion。
   - Stop/Drain 不得永久等待。

如果现有结构难以安全修补，可以改成“每玩家任务队列 + player active/async 状态”，但不要重写整个业务调度系统。

## 1.3 必须新增的测试

在 `test/player_serial_async_test.cpp` 中增加确定性测试，不要依赖随机 sleep：

### 测试 A：预排队任务

```text
1. A 开始执行，但先等待测试线程确认 B 已经成功入队。
2. B 入队后，A 调用 MarkAsyncInFlight。
3. A 发起一个 100ms 后完成的异步 callback，然后返回。
4. 断言 B 在 callback 完成前绝对不能执行。
5. callback 完成后 B 必须执行一次。
```

测试失败信息必须包含：

```text
prequeued_serialization_violated
```

### 测试 B：混合玩家

- 玩家 A 处于 async-inflight。
- 玩家 B 位于另一个 shard。
- 玩家 B 的命令必须能够及时执行，证明没有退化为全局串行。

### 测试 C：链式异步

保留现有链式异步测试，并确认仍然通过。

## 1.4 阶段一验收

```bash
./scripts/build.sh Debug
./build/test/player_serial_async_test
./scripts/test.sh unit
```

阶段一完成前不要开始阶段二。

---

# 阶段二：同步邮件事务后的 GameLogic 资产版本

## 2.1 已确认的问题

邮件事务调用 `GrantItemsOnConnection` 后，MySQL 中的 `asset_version` 已增加，但新版本没有通过以下链路返回：

```text
AsyncMysqlGameDbRepository
→ GameDbService ClaimMailRsp
→ BrpcGameDbRepository
→ GameDbMailClaimResult
→ MailService
→ GameLogic 内存 asset_version
```

目前 `MailService::ApplyClaimMemory` 只增加内存道具数量，没有更新 `asset_version_`。

可能出现：

```text
登录加载内存版本 10
领取邮件后数据库版本 11
内存仍是 10
立即 ConsumeItem(expected_version=10)
GameDB 返回 VERSION_CONFLICT
```

## 2.2 Proto 和结果对象

修改源 proto，不要只手改生成文件：

```proto
message ClaimMailRsp {
  // 保留现有字段号
  uint64 asset_version = 新字段号;
}
```

然后通过仓库已有 protoc/CMake 流程重新生成对应 `.pb.h/.pb.cc`。

同步增加：

```cpp
struct GameDbMailClaimResult {
    ...
    uint64_t asset_version = 0;
};
```

要求完整贯通：

- `AsyncMysqlGameDbRepository::DoClaimMail`
- `GameDbServiceImpl::ClaimMailAttachments`
- `BrpcGameDbRepository` 同步和异步路径
- `MailService`
- `GameLogic`

## 2.3 数据库事务结果

1. 第一次领取成功时，把 `GrantItemsOnConnection` 返回的 `new_asset_ver` 写入结果。
2. 幂等命中时也必须获得该次操作已提交的准确资产版本。
3. 不要简单猜测 `current_version + 1`。
4. 推荐在邮件操作日志中持久化 `asset_version`，并提供兼容旧表的幂等 schema migration。
5. 如果选择查询 `player_asset_meta`，必须说明并测试并发情况下不会把不对应本次操作的版本误当作结果。
6. MySQL commit 返回失败时属于 UNKNOWN_RESULT，不能直接把资产事务标记为确定失败；必须按幂等键查询最终结果。

## 2.4 GameLogic 内存原子更新

新增或调整一个线程安全接口，例如：

```cpp
bool GameLogic::ApplyItemRewardsWithVersion(
    uint64_t player_id,
    const std::vector<GameDbGrantedItem>& grants,
    uint64_t committed_asset_version);
```

要求在同一个 GameLogic 玩家状态临界区内：

1. 应用本次 grants。
2. 更新 `asset_version_` 为数据库提交后的版本。
3. 禁止版本回退。
4. 幂等命中且 `should_apply_memory=false` 时不能重复增加道具。
5. 如果检测到内存版本无法与返回版本衔接，不能静默覆盖；应从 GameDB 重载完整背包和版本，或返回明确的 `ASSET_STATE_STALE` 并触发重新同步。

邮件单领和批量领取都必须使用同一规则。

## 2.5 必须新增的测试

扩充 `test/gamedb_mail_claim_test.cpp`，至少覆盖：

1. 玩家已有正式背包和资产版本。
2. 领取邮件后：
   - DB 道具正确；
   - GameLogic 内存道具正确；
   - DB 和 GameLogic 的 asset_version 一致。
3. 领取邮件后立即 ConsumeItem，不能返回 `VERSION_CONFLICT`。
4. 领取邮件后立即 SaveSnapshot，不能因旧版本失败。
5. 同一个 idempotency_key 重试：
   - 奖励不重复；
   - asset_version 不重复递增；
   - 返回已提交的准确版本。
6. 模拟“事务已提交但 RPC 响应丢失”，重试后内存和 DB 仍一致。
7. 批量领取多封邮件时，每一步版本连续且最终内存版本等于 DB 版本。

## 2.6 阶段二验收

```bash
./scripts/build.sh Debug
./build/test/gamedb_mail_claim_test
./scripts/test.sh unit
./scripts/test.sh integration
```

阶段二完成前不要开始阶段三。

---

# 阶段三：服务发现 Fail-Closed、Reactor 兜底异步化和最终门禁

## 3.1 健康 GameLogic 列表不能保留死亡节点

当前 `RefreshHealthyLogicOwners()` 在 Redis 服务发现成功但返回空列表时直接返回，`PlacementStore` 继续保留旧 owners。

必须区分：

```text
A. 服务发现调用失败/Redis 暂时不可用
B. 服务发现成功，但当前健康 GameLogic 数量为 0
C. 服务发现成功，存在健康 GameLogic
```

要求：

- C：原子替换健康 Owner 列表。
- B：清空动态健康 Owner 列表，并让新的 Placement/Session 分配 fail-closed，返回 `NO_HEALTHY_GAMELOGIC`，不能回退到 `gl-0`。
- A：可以短时间保留最后有效快照或静态 fallback，但必须有明确的过期时间和指标；过期后 fail-closed。
- 静态地址 fallback 只用于注册中心未启用或冷启动配置，不能在“注册中心已明确返回零存活实例”时重新选中死亡节点。

同步修改：

- `PlacementStore::SetLogicOwners` 必须支持显式清空，或增加 `ClearLogicOwners()`。
- `PickOwner()` 在没有健康 Owner 时不能返回硬编码 `gl-0`。
- `ResolveOrCreate` 在新建或 reclaim 时，如果没有健康 Owner，返回明确错误，不能写入空 Owner。
- `PickHealthyOwner()` 没有其他 Owner 时返回空并保持 RECOVERING，不能伪装迁移成功。

新增测试：

1. 成功发现零实例后 Resolve 返回 `NO_HEALTHY_GAMELOGIC`。
2. 死亡 gl-0 被移除、gl-1 存活时，过期 Placement reclaim 到 gl-1 并递增 epoch/route_version。
3. 所有 Logic 死亡时，不续租旧 Owner、不创建新 Placement。
4. 注册中心调用失败时，验证配置的有限快照/静态降级策略。

说明：自动 PlacementRecoveryScheduler 可以继续保持 experimental；本稳定版本至少保证玩家重新登录/进图触发 Resolve 时不会被路由到死亡 Owner。

## 3.2 Gateway 断线的本地 Redis fallback 不能阻塞 Reactor

当前 brpc Client 未 ready 时，Gateway 断线回调仍可能同步执行：

```cpp
SessionStore::Instance().MarkDisconnected(...)
```

正式模式下禁止在 EventLoop/Reactor 线程同步访问 Redis。

最小实现：

1. 断线回调只完成：
   - 复制 bind/registry 快照；
   - 清理连接索引；
   - 投递异步断线任务；
   - 返回 EventLoop。
2. brpc ready 时继续使用异步 RPC。
3. brpc not ready 时，把本地 Redis 补偿放入有界后台队列或专用 worker，不能在 Reactor 内执行。
4. 队列满时：记录指标和错误日志，并依靠 Session TTL/补偿扫描最终收敛；不能阻塞网络线程。
5. 后台任务不能保存或访问 `TcpConnection`，只能使用复制出的 player/session/fence/generation。
6. Stop/Drain 时有界等待，不能永久退出不了。

扩充 `gateway_disconnect_async_test`：

- Session brpc 不可用。
- 注入一个会阻塞的本地 Redis fallback 或测试 hook。
- 连续触发至少 64 个断线任务。
- EventLoop 侧投递总耗时必须低于明确阈值，例如 100ms。
- 验证后台补偿最终执行或按设计进入可观测的丢弃/TTL 收敛路径。

## 3.3 完整稳定门禁

修复完成后，先确保：

```bash
git diff --check
git show --check --oneline HEAD
bash -n scripts/*.sh deploy/docker-entrypoint.sh
```

然后在依赖完整的 Linux 环境执行：

```bash
./scripts/check_deps.sh --full
./scripts/build.sh Debug
./scripts/build.sh Release
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/final_e2e.sh --start-cluster
./scripts/stable_gate.sh --full
```

完整门禁必须对应包含所有修复的同一个最终 commit。

不得使用旧 commit `b0fecdc` 的历史门禁报告证明新版本通过。

必须验证：

1. 双 GW 登录和跨 GW 重连。
2. 双 Session 故障切换。
3. 双 GameDB unknown-result/idempotency 恢复。
4. kill 当前 GameLogic 后，玩家重新登录/进图获得存活 Owner。
5. 旧 owner_epoch 请求被拒绝。
6. 邮件领取后立即资产操作不发生 VERSION_CONFLICT。
7. 同玩家预排队异步测试通过。
8. Push replay 或 full snapshot 不能静默丢失。
9. 30 分钟负载测试达到项目门槛。
10. 2 小时 soak 无崩溃、无持续内存增长、无资产不一致。

如果完整门禁没有真实执行，最终结论必须写：

```text
STABLE BLOCKED: full stable gate not executed
```

不能写 STABLE PASS。

---

# 四、禁止范围

本轮不要做：

- Unity 客户端接入。
- 客户端协议大改。
- gRPC。
- 独立 Login 进程。
- Reactor 重写。
- Redis Cluster/MySQL 主从建设。
- GameLogic 动态扩容 gl-2。
- 无损实时地图迁移。
- 大世界 Zone/Cell/Chunk 架构。
- 与稳定阻塞无关的重命名和目录重构。

# 五、最终输出要求

直接完成代码、proto、测试和脚本修改。结束时输出：

1. 当前最终 commit 或工作区基线。
2. 修改文件列表。
3. PlayerSerialQueue 如何阻止“预排队同玩家任务”提前执行。
4. 邮件领取后的 asset_version 如何从 MySQL 贯通到 GameLogic。
5. 服务发现成功但零 Logic 时如何 fail-closed。
6. Gateway 本地 Redis fallback 如何避免阻塞 Reactor。
7. 实际执行的构建和测试命令。
8. 每项测试的真实结果和退出码。
9. `stable_gate --full` 报告路径及报告中的 commit。
10. 尚未完成的事项。

最终只有在同一个最终 commit 的完整门禁全部通过时，才允许输出：

```text
STABLE PASS — fixed-topology server candidate
```

否则必须输出：

```text
STABLE BLOCKED
```

并列出准确阻塞原因。
