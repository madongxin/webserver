# GameMesh 紧急最小稳定版：Cursor 两阶段执行提示词

> 仓库：`https://github.com/madongxin/webserver`
> 审计基线：`main@857d9637dfe5fdc523e424433f4b9694efd63ce6`
> 目标：尽快形成可供后续客户端联调的 `server-stable-v0.1.0-rc1`
> 当前状态：`STABLE BLOCKED`

## 一、严格限定本次稳定范围

本次不继续扩展架构，只形成固定拓扑下的服务器稳定基线。

### 稳定版支持范围

```text
Gateway ×2
Auth/Session ×2
GameLogic ×2
GameDB ×2
World/GlobalService ×1
Redis ×1
MySQL ×1
```

必须稳定支持：

1. Client TCP → 任意 Gateway。
2. Auth → Session → 指定 GameLogic 的登录链路。
3. 普通 Dispatch 的粘性 GameLogic 路由。
4. gw0 断开后通过 gw1 重连。
5. sess-0 失效后 sess-1 可以继续登录和重连。
6. gamedb-0 失效或结果未知时，通过幂等记录在 gamedb-1 查询真实结果。
7. GameLogic 失效时旧 epoch 不得继续写；本版本允许客户端重新登录或重新进图，不承诺实时地图无损恢复。
8. 资产、邮件附件、背包和快照不能部分提交、重复发放或返回伪成功。
9. 可靠 Push 在跨 Gateway 重连时不会静默丢失。
10. 全新 checkout 能在 CI 中构建和测试。

### 本次明确不承诺

- Unity 客户端和 Unity SDK。
- 运行期在线扩容 `gl-2`。
- DRAINING 自动迁移在线玩家。
- GameLogic 实时地图无损接管。
- Redis Sentinel/Cluster。
- MySQL 主从或托管 HA。
- etcd 替换、Kubernetes、Service Mesh、mTLS。

动态扩容、DRAINING、Registry outage 和自动 Placement 恢复代码可以保留，但在本稳定版本中标记为 `experimental`，不得用未通过的实验功能阻塞客户端联调，也不得在发布说明中宣称为稳定能力。

## 二、全局执行规则

下面分两个阶段执行。每次只执行一个阶段，测试通过后停止并报告，等待我确认后再进入下一阶段。

开始每个阶段前：

1. 完整阅读 `AGENTS.md`。
2. 执行：

   ```bash
   git status --short
   git rev-parse HEAD
   ```

3. 保留用户现有修改。
4. 不执行破坏性 Git 操作。
5. 不创建 Tag，不 Push。
6. 继续使用 C++17、Reactor、ProtoFraming 和 brpc。
7. 不引入 gRPC，不新增 Login 进程，不重写 Reactor。
8. 直接修改代码和测试，不要只输出方案。

---

# 阶段一：只修复三个 P0 正确性问题

## 阶段目标

只修改会导致资产部分提交、可靠消息丢失和玩家状态跨线程访问的问题。不要在本阶段修改 E2E、CI、部署拓扑或增加新功能。

## 1. 修复 GameDB 资产事务原子性

重点文件：

```text
db/GameDbAssetStore.cpp
db/GameDbAssetStore.h
db/BrpcGameDbRepository.cpp
runtime/brpc/GameDbServiceImpl.cpp
test/gamedb_mutation_idempotency_test.cpp
```

### 当前严重错误

`ApplyMutation()` 的失败路径会在当前事务中执行：

```text
UPDATE idempotency FAILED
→ COMMIT
```

如果背包、资产版本已经修改，但 Outbox 或幂等 finalize 失败，这会提交部分资产数据。

### 最小安全修复

建立以下强制不变量：

```text
资产修改
+ asset_version
+ SUCCEEDED 幂等结果
+ Outbox
= 同一个 MySQL 事务
```

规则：

1. 事务内任一步失败都必须 `ROLLBACK`。
2. 禁止失败处理函数对包含资产修改的事务执行 `COMMIT`。
3. Outbox 插入失败必须回滚背包、版本和幂等占位。
4. 幂等成功记录 finalize 失败必须回滚全部资产变化。
5. `COMMIT` 返回失败属于结果未知：

   ```text
   不再执行第二次 COMMIT
   不直接标记 FAILED
   → 使用新的数据库连接 QueryOperationResult
   → SUCCEEDED：返回真实成功
   → NOT_FOUND/IN_PROGRESS：返回 UNKNOWN_RESULT，可用同一 key 重试
   ```

6. 为了最小改动，本版本可以只持久化成功幂等结果：

   - 确定业务失败先回滚，再直接返回错误。
   - 不要求把每个业务失败都持久化为 FAILED。
   - 如果继续持久化 FAILED，必须在确认原资产事务已经回滚后，使用独立事务安全写入；不能和资产修改一起提交。

7. 已存在的 FAILED 记录再次请求时直接返回原始失败：

   ```text
   VERSION_CONFLICT
   NOT_ENOUGH
   ...
   ```

   禁止把 FAILED 当成 IN_PROGRESS，最终返回 `IDEMPOTENCY_BUSY`。

8. 幂等命中继续严格比较：

   ```text
   player_id
   operation_type
   request_hash
   ```

9. 旧记录的 `operation_type` 或 `request_hash` 为空时不能默认匹配任意新请求；应返回明确的 legacy conflict，或仅允许经过安全迁移的数据命中。
10. `mysql_real_escape_string` 保留，不退回自制字符串转义。

### 必需测试

通过仅测试构建启用的 failpoint 覆盖：

1. 背包修改后、版本更新前失败 → 全部回滚。
2. 版本更新后、Outbox 前失败 → 全部回滚。
3. Outbox 插入失败 → 背包、版本、幂等、Outbox 都没有部分结果。
4. 幂等 finalize 失败 → 全部回滚。
5. commit 后响应前断开 → 通过另一个 GameDB 查询 SUCCEEDED，不重复写。
6. commit 真实失败且查询不到结果 → 返回 UNKNOWN_RESULT，不伪装成功或失败。
7. 已存 FAILED 重试返回原始失败，不返回 BUSY。
8. 相同 key 并发调用只产生一次资产修改和一条 Outbox。

测试必须同时断言：

```text
bag count
asset_version
idempotency row
outbox row count
```

## 2. 修复 Reliable Push 的序列空洞

重点文件：

```text
game/PushReplayStore.cpp
game/PushReplayStore.h
runtime/brpc/GatewayLoginOrchestrator.cpp
test/push_full_snapshot_test.cpp
```

当前 `ReserveSeq → AppendReserved` 之间可能被其他 Push 插入，或 Append 失败，形成 sequence gap。

### Replay 强制不变量

客户端从 `last_server_seq` 开始回放时，服务端必须验证所有待回放序列连续：

```text
expected = last_server_seq + 1
```

遍历 ReplayEntry：

1. `seq <= last_server_seq`：忽略。
2. `seq == expected`：加入回放，`expected++`。
3. `seq > expected`：立即返回 `NEED_SNAPSHOT`。
4. 遍历完成后，如果 Redis 当前 sequence 仍大于等于 `expected`，说明尾部存在已 Reserve 但未写入的空洞，返回 `NEED_SNAPSHOT`。

这些规则对 `last_server_seq=0` 同样生效，不能特殊跳过首个 gap。

### 其他要求

1. `AppendReserved` 对同一 reserved sequence 必须幂等或拒绝重复，不能写入两个相同 sequence 的条目。
2. FullSnapshot 的三处 sequence 必须一致：

   ```text
   ReplayEntry.server_seq
   ServerPushEnvelope.server_seq
   FullSnapshot.baseline_server_seq
   ```

3. Export、编码或 Redis Store 失败时不得发送伪 FullSnapshot。
4. Reserve 成功但 Append 失败必须留下可检测的 gap，下一次 Reconnect 强制生成快照。
5. ACK 不得接受一个从未成功写入或发送的 reserved sequence。
6. FullSnapshot 的正式测试必须解析真实 Protobuf payload，不能只用字符串模拟 baseline。

### 必需测试

1. Reserve 1 但不 Append，再写 seq 2，`ReplayAfter(0)` 返回 `NEED_SNAPSHOT`。
2. seq 1、3 存在，seq 2 缢失，`ReplayAfter(1)` 返回 `NEED_SNAPSHOT`。
3. Reserve 与普通 Push 并发，不能静默漏消息。
4. 重复 AppendReserved 不产生重复条目。
5. ACK reserved-but-not-stored sequence 被拒绝。
6. 真实 FullSnapshot 即时发送和 Redis Replay 解码后的 baseline 一致。

## 3. 修复 PlayerSerialQueue 停止期线程越界

重点文件：

```text
runtime/PlayerSerialQueue.cpp
runtime/PlayerSerialQueue.h
game/MailService.cpp
runtime/brpc/GameLogicServiceImpl.cpp
test/player_serial_async_test.cpp
```

当前队列停止后，`CompleteAsyncInFlight()` 会在 RPC callback 线程直接执行 completion；邮件 completion 会修改玩家状态。

### 最小安全生命周期

1. 将状态拆为：

   ```text
   RUNNING
   DRAINING
   STOPPED
   ```

2. 进入 DRAINING 后：

   - 拒绝新的外部业务任务。
   - 已经开始的异步 completion 仍可以进入内部控制队列。
   - Worker 保持运行，直到已有任务和 async in-flight 清空。

3. `CompleteAsyncInFlight` 不能因为外部过载而失败。
4. STOPPED 后不得 inline 执行会修改玩家状态的 completion。
5. 如果 callback 在 STOPPED 后到达：

   - 不应用背包/邮箱内存变化。
   - 完成 brpc `done` 恰好一次。
   - 返回 `SERVER_STOPPING` 或明确取消结果。

6. Stop 使用可配置的有界 drain deadline；超时时记录未完成玩家和请求数，执行安全取消，不能永久 join。
7. 删除 Mail completion 中 `ClearAsyncInFlight + 直接执行 apply()` 的跨线程 fallback。

### 必需测试

1. 外部队列满时，内部 completion 仍在原 Worker 执行。
2. DRAINING 期间拒绝新任务，但已开始的 Mail Claim 正确完成。
3. STOPPED 后迟到 callback 不修改玩家状态。
4. brpc done 在完成、取消和超时路径都恰好一次。
5. 同玩家顺序不变，不同玩家不被慢 GameDB 阻塞。

## 4. 阶段一验收

实际执行：

```bash
./scripts/check_deps.sh --full
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/test_sanitizers.sh asan
./scripts/test_sanitizers.sh ubsan
./scripts/test_sanitizers.sh tsan
```

完成后输出：

1. 修改文件列表。
2. 资产事务的新原子性说明。
3. Push gap 检测算法。
4. PlayerSerialQueue 停止状态机。
5. 每条测试的真实结果。
6. 尚未解决的问题。

阶段一全部通过后停止，不要自动进入阶段二。

---

# 阶段二：只修复 CI、门禁假绿和发布证据

仅在阶段一全部通过后执行。

## 阶段目标

不再增加服务器功能，只让固定双实例拓扑能够从干净 checkout 构建，并用可信门禁证明稳定。

## 1. 修复 GitHub CI

当前 commit `857d963` 的 GitHub Actions：

```text
lowlevel：FAIL（Debug lowlevel build）
full：FAIL（install brpc）
```

必须修复真实失败原因，不得跳过失败步骤。

要求：

1. `lowlevel` 从全新 checkout 构建并运行低层测试成功。
2. `full` 能安装固定版本 brpc，并完成 Debug/Release 构建。
3. full CI 增加 MySQL 8 Service；Integration 强制 MySQL 时必须提供真实数据库。
4. MySQL/Redis readiness 未就绪时明确等待并失败，不能立即运行测试。
5. 不硬编码可能不存在的 `libasan6/libtsan0`；根据 runner/compiler 安装兼容运行库，或让编译器依赖自动带入。
6. brpc 下载继续校验固定 SHA256。
7. `git show --check HEAD` 删除 `|| true`，手写文件尾随空白必须让 CI 失败。
8. 生成的 `*.pb.*` 可以明确排除，但手写源码、文档和脚本不能排除。
9. lowlevel 和 full 两个 Check 都必须绿色。

## 2. 修复稳定脚本的确定假绿

### Session failover

必须断言：

```text
Redis 中目标玩家恰好一个权威 Session
state == ONLINE
session_id/fence_token/generation 与新连接一致
```

删除异常状态 `WARN but PASS`。

旧连接验证必须发送一条带旧 fence/generation 的真实 Dispatch，并断言 GameLogic 返回 fence reject；不能用再次 Reconnect 后 token 变化代替。

### GameDB failover

必须断言：

1. failpoint 请求确实得到 RPC failure/UNKNOWN_RESULT。
2. gamedb-1 查询得到 SUCCEEDED。
3. 资产数量只增加一次。
4. asset_version 只增加一次。
5. Outbox 恰好一条。
6. gamedb-1 后续业务写成功。

### Soak 采样

修复 Bash 命令替换导致变量修改不回写的问题：

- `rss0/fd0/thr0/proc_exits` 必须在父 Shell 更新。
- 最后一次 `sample_once` 失败不得被 `|| true` 吞掉。
- 报告中的起始指标不能错误地一直为 0。
- 任何关键进程退出都必须让 Soak 失败。

### Stable gate

1. 干净 checkout 上检查提交内容，不是只执行空的 `git diff --check`。
2. 工作树 dirty 时不得输出 `STABLE PASS`。
3. 报告缺失或 hash 不匹配时不得 PASS。
4. `export_release_bundle.sh` 失败不得被 `|| true` 吞掉。
5. 所有报告必须属于当前 commit。

## 3. 收紧稳定功能声明

为了最快获得可信稳定版本：

1. 在正式配置中将以下能力默认关闭或标记实验性：

   ```text
   runtime gl-2 dynamic scale
   DRAINING online migration
   automatic lossless Placement recovery
   Registry outage recovery
   ```

2. 不删除代码，但从 `server-stable-v0.1.0` 的支持能力和硬门禁中移出。
3. GameLogic 崩溃的稳定语义固定为：

   ```text
   旧 owner/epoch 立即失效
   → 玩家收到明确错误或断线
   → 重新登录/重新进图
   → 从 GameDB 持久化状态恢复
   ```

4. 禁止宣称实时地图无损迁移。
5. 如果项目负责人仍决定把动态扩容列为稳定能力，则不能移出门禁，必须修复以下测试后再发布：

   - 不得用 `SEED_OWNERS=gl-0,gl-1` 人工排除 gl-2 来证明 DRAINING。
   - 必须验证 Session/Gateway 的真实 Channel snapshot 不再给 gl-2 分配新地图。
   - Registry 恢复后必须验证消费者发现新版本，不能只用 `redis-cli HGET` 自读自证。

## 4. 最小发布门禁

先执行快速门禁：

```bash
./scripts/check_deps.sh --full
./scripts/build.sh Debug
./scripts/build.sh Release
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/final_e2e.sh --start-cluster
./scripts/test_session_failover.sh
./scripts/test_gamedb_unknown_result_failover.sh
./scripts/test_sanitizers.sh all
```

快速门禁全部通过后，在同一个干净 commit 上执行：

```bash
START_CLUSTER=1 E2E_ROUNDS=20 ./scripts/test_e2e_20x.sh
LOAD_DURATION_SEC=1800 LOAD_CONCURRENCY=32 ./scripts/load_tcp_baseline.sh
SOAK_DURATION_SEC=7200 ./scripts/soak_test.sh
./scripts/stable_gate.sh --full
```

不得通过以下方式获得稳定结论：

- 把并发从失败值降到更低后仍沿用原性能承诺。
- 降低成功率阈值。
- 缩短 30 分钟负载或 2 小时 Soak。
- 删除失败测试。
- 使用旧 commit 的报告。
- 对失败命令添加 `|| true`。

如果 32 并发不是产品真实最低要求，应先在发布文档中明确新的受支持机器配置和负载基线，再由负责人确认；Cursor 不得自行降低。

## 5. 稳定版通过标准

必须同时满足：

```text
GitHub lowlevel = PASS
GitHub full = PASS
本地/发布机 Debug = PASS
Release = PASS
Unit = PASS
Integration = PASS
双 GW/双 Session/双 Logic/双 GameDB E2E = PASS
Session failover = PASS
GameDB unknown-result failover = PASS
ASan/UBSan/TSan = PASS
E2E 20× = PASS
30min load = PASS
2h soak = PASS
working tree clean
全部报告 commit == HEAD
```

只有全部满足后才输出：

```text
STABLE PASS — candidate server-stable-v0.1.0-rc1
```

否则输出：

```text
STABLE BLOCKED
```

## 6. 最终交付

完成阶段二后输出：

1. 最终 commit 和 dirty 状态。
2. GitHub Actions lowlevel/full 链接和结果。
3. 所有门禁 PASS/FAIL 表。
4. Release manifest 路径。
5. 负载和 Soak 报告路径。
6. 当前稳定支持能力。
7. Experimental 能力清单。
8. 是否可以创建 `server-stable-v0.1.0-rc1` Tag。

不要自行创建 Tag 或 Push。

