# GameMesh 服务器稳定版最小闭环：Cursor 三阶段执行提示词

> 适用仓库：`https://github.com/madongxin/webserver`
> 审计基线：`main@e8ab08fc1b2beef59d106fd1ac75bb9e43860ba5`
> 目标版本：`server-stable-v0.1.0`（面向后续客户端联调的服务器稳定基线）
> 当前结论：**STABLE BLOCKED，不得创建稳定 Tag。**

## 1. 稳定版本的范围

本任务只完成服务器稳定闭环，不实现 Unity 客户端，不修改客户端业务协议，不扩展新玩法。

本版本支持的最小拓扑：

```text
Gateway ×2
Auth/Session ×2
GameLogic ×2（测试动态扩容到 gl-2）
GameDB ×2
World/GlobalService ×1
Redis ×1
MySQL ×1
```

本版本的“稳定”含义：

1. 服务器在上述拓扑下可重复构建、启动、停止和测试。
2. 登录、重连、普通命令、进图、可靠 Push、下线链路正确。
3. 单个 Gateway、Session、GameLogic、GameDB 进程异常退出时，不产生双会话、双写资产或错误成功响应。
4. 所有公开资产写入口具备可信身份、幂等、事务和未知结果查询语义。
5. 不在 Reactor I/O 线程或玩家串行执行线程中同步等待 brpc、MySQL、Redis 或 `future.get()`。
6. 完整稳定门禁在同一个 commit 上真实通过，并保存可审计报告。

以下内容不作为本次稳定候选版的阻塞项，但必须在发布说明中明确列为限制：

- Redis Sentinel/Cluster。
- MySQL 主从或托管 HA。
- World/GlobalService 多实例状态复制。
- GameLogic 崩溃后的实时地图无损恢复；当前允许提高 epoch 后让玩家重新进图或从持久化状态恢复。
- Kubernetes、Service Mesh、mTLS、跨机房容灾。
- Unity SDK、Unity Demo 和客户端资源。

因此，本版本可以作为后续客户端联调和功能研发的稳定服务器基线，但不能宣传为生产级全基础设施 HA。

## 2. 当前基线审计结论

`e8ab08f` 已经完成了不少正确整改：

- 正式客户端命令白名单与资产管理命令隔离。
- Push ACK 的 ahead/stale/duplicate 原子判断。
- SaveSnapshot 基础幂等和 `QueryOperationResult`。
- brpc Channel 不可变快照。
- Session peer retry 与 PlacementRecoveryScheduler 框架。
- Debug/Release、Sanitizer、E2E、Load、Soak 脚本框架。

但仍存在以下稳定阻塞：

| 级别 | 问题 | 影响 |
|---|---|---|
| P0 | `MailBatchClaim` 仍通过 `future.get()` 同步等待 | 一个慢 GameDB 请求会阻塞玩家分片 |
| P0 | `PlayerSerialQueue` 完成回投过载时可能在下游回调线程直接修改玩家状态 | 破坏同玩家串行和线程归属 |
| P0 | `ApplyMutation` 的幂等命中未完整校验 `player_id + operation_type + request_hash` | 相同 key 不同玩家/请求可能得到错误幂等结果 |
| P0 | GameDB 资产 SQL 使用自制字符串转义，未正确覆盖反斜杠等输入 | 客户端可控幂等键存在 SQL 注入/语义破坏风险 |
| P0 | FullSnapshot 先保存未含 baseline 的 payload，再修改即时发送 payload | Redis 回放内容与即时下发内容不一致 |
| P0 | FullSnapshot 测试只测 ReplayStore，没有覆盖 Reconnect Orchestrator | 真实错误未被测试发现 |
| P0 | `stable_gate --full` 先执行破坏性 E2E，后续仅凭 PID 文件判断集群存在 | 被杀进程不会重启，完整门禁无法可靠连续执行 |
| P0 | full CI 未安装/提供 brpc，却检测不到就直接失败 | 全新 runner 无法复现完整构建 |
| P1 | Logic 自动恢复测试允许真实进图失败后继续 | 测试可能假绿 |
| P1 | GameDB failover 只验证存活节点 HTTP，不验证故障后的真实资产业务 | 未证明未知结果和业务连续性 |
| P1 | 动态扩容脚本没有真正启动 `gl-2` | 未证明动态扩容 |
| P1 | Registry outage 脚本没有造成真实注册中心故障 | 未证明最后有效 Channel 的运行语义 |
| P1 | 尚未完成 20× E2E、30 分钟负载、2 小时 Soak | 没有稳定性证据 |

下面三个阶段必须严格顺序执行。每次只执行一个阶段；阶段测试全部通过后停止并报告，等我确认后再进入下一阶段。

---

# 阶段一提示词：修复资产幂等、全量快照与玩家线程模型

请直接在 GameMesh 当前仓库中实施本阶段，不要只输出方案。

## 开始前

1. 完整阅读 `AGENTS.md`。
2. 执行 `git status --short` 和 `git rev-parse HEAD`。
3. 保留现有未提交修改，不执行 `git reset --hard`、`git checkout --` 等破坏性操作。
4. 阅读本阶段涉及的实现、proto、测试、CMake 和脚本。
5. 不提交、不创建 Tag、不 Push。
6. 保留 C++17、Reactor、ProtoFraming 和 brpc；不引入 gRPC，不新增 Login 进程，不重写 Reactor。
7. 本阶段不要修改 Unity 或增加新玩法。

## 阶段目标

消除可能导致资产串写、SQL 注入、玩家线程错乱和错误快照的 P0 正确性问题。

## 一、修复所有资产操作的幂等身份

重点检查：

- `db/GameDbAssetStore.cpp`
- `db/BrpcGameDbRepository.cpp`
- `runtime/brpc/GameDbServiceImpl.cpp`
- `proto/gamedb.proto`
- Mail Claim/BatchClaim 到 GameDB 的调用链

要求：

1. 为 `ApplyMutation` 计算稳定的请求摘要，至少包含：

   ```text
   player_id
   operation_type
   expected_version
   item_id
   count
   ```

2. 幂等记录命中时必须同时比较：

   ```text
   player_id
   operation_type
   request_hash
   ```

3. 同一个 `idempotency_key` 被不同玩家、不同操作或不同 payload 复用时，返回：

   ```text
   IDEMPOTENCY_CONFLICT
   ```

   不得返回另一个玩家的资产版本、剩余数量或成功结果。

4. `ApplyMutation` 插入幂等记录时必须写入 `operation_type` 和 `request_hash`，与 SaveSnapshot 使用同一套规则。
5. `QueryOperationResult` 不得把 `IN_PROGRESS` 当成最终失败。建议在保持旧字段兼容的基础上新增状态：

   ```text
   NOT_FOUND
   IN_PROGRESS
   SUCCEEDED
   FAILED
   ```

6. RPC 超时后：

   ```text
   查询幂等结果
   → SUCCEEDED：返回真实成功
   → FAILED：返回真实失败
   → IN_PROGRESS：返回可重试的 UNKNOWN_RESULT/IN_PROGRESS
   → NOT_FOUND：才允许向其他 GameDB 使用同一幂等键重试
   ```

7. 保持资产修改、幂等记录和 Outbox 在同一个 MySQL 事务中提交。
8. schema 变更必须兼容已有表，提供显式迁移或 `EnsureTables` 安全升级；不能删除线上表。

## 二、移除客户端可控 SQL 的手写转义

当前简单替换单引号的 `Escape()` 不能作为 MySQL 安全边界。

要求：

1. 对幂等键、operation type、trace/business key 等进入 SQL 的字符串使用：

   - MySQL prepared statement；或
   - 与当前 `MYSQL*` 连接绑定的 `mysql_real_escape_string` 封装。

2. 禁止用只替换 `'` 的自制转义处理客户端可控数据。
3. 对 idempotency key 设置长度和字符策略，超长输入返回 `INVALID_ARGUMENT`，不得截断后继续执行。
4. 检查 Mail Claim 和 BatchClaim 的客户端幂等键前缀，确保它们不能改变 SQL 语义。

## 三、修复 FullSnapshot 的唯一序列语义

重点检查 `GatewayLoginOrchestrator`、`PushReplayStore`、`GameTcpGateway`。

建立唯一不变量：

```text
Redis ReplayEntry.server_seq
= 外层 ServerPushEnvelope.server_seq
= FullSnapshot.baseline_server_seq
```

同时保证 Redis 中保存的业务 payload 与即时下发、稍后重放所使用的 payload 语义完全一致。

可选择以下实现之一：

- 先安全保留 server sequence，再编码完整 snapshot 并按指定 sequence 原子写入；保留失败产生的 sequence gap，由重连检测触发新快照。
- Redis 中存储无 baseline 的逻辑快照，但即时发送和每次 Replay 时都以 ReplayEntry 的 `server_seq` 重建并覆盖 baseline。

不能继续“先存旧 payload，再只修改内存中的即时 payload”。

失败语义：

1. Export、编码或 Redis 存储失败时，不得下发 `ok=true` 的伪 FullSnapshot。
2. Reconnect 可以完成连接接管，但必须明确返回 `need_full_snapshot=true` 和可重试错误状态。
3. 只有快照真正进入可靠队列后才允许建立 baseline。
4. 旧 session/fence 的快照不得发送给新会话。

## 四、将 MailBatchClaim 改成真正异步

1. 删除 GameLogic/World 玩家执行链中的 `std::promise + future.get()`。
2. BatchClaim 必须是逐封或有界并发的异步状态机。
3. 同一玩家的领取顺序、背包 soft cap、幂等结果和内存应用顺序必须确定。
4. Batch 中一封失败不能造成已经成功的邮件重复领取；每封结果必须准确返回。
5. 下游 callback 必须回投玩家所属 `PlayerSerialQueue` 后才能修改玩家内存状态。
6. 不允许 detached thread，不允许用延长超时掩盖同步阻塞。

## 五、修复 PlayerSerialQueue 的异步完成通道

当前 `CompleteAsyncInFlight` 在队列达到全局上限时可能拒绝内部 completion，调用端随后直接在 RPC callback 线程修改状态。必须修复。

要求：

1. 外部新业务任务可以因过载被拒绝。
2. 已经开始的异步任务，其 completion 必须拥有保留容量或独立内部控制队列，确保能回投原玩家串行执行上下文。
3. 不得在 completion 入队失败时直接执行玩家状态修改。
4. Stop/Drain 时必须明确处理 in-flight：完成、取消并回调错误，或在有界超时后安全退出；不能永久等待。
5. 同玩家后续命令必须在异步 completion 之后执行，不同玩家不能被慢请求阻塞。

## 六、阶段一测试

新增或修改真实测试，禁止只测试局部变量或用 `Expect(true)`：

1. 同 key、同请求并发 20 次，只修改一次资产。
2. 同 key、不同 player 返回 `IDEMPOTENCY_CONFLICT`。
3. 同 key、不同 item/count/operation 返回冲突。
4. 包含单引号、反斜杠和超长内容的 key 不改变 SQL 语义。
5. GameDB 在提交后响应前断开，查询结果返回真实成功且不重复写。
6. FullSnapshot 即时发送、Redis 持久化和跨 Gateway Replay 的 sequence/baseline 完全相同。
7. Snapshot Export/Store 失败时不产生伪成功 payload。
8. BatchClaim 在慢 GameDB 下不阻塞同 shard 的另一个玩家。
9. 同一玩家 BatchClaim 期间的后续命令保持顺序。
10. completion 队列达到外部过载阈值时仍不跨线程修改玩家状态。

增加静态门禁：

```bash
rg -n "future\.get\(|\.wait\(" game runtime apps db
```

允许测试代码或明确的非玩家启动/关闭路径，生产玩家链路中不得命中。

## 七、阶段一验收

实际执行：

```bash
./scripts/check_deps.sh --full
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/test_sanitizers.sh asan
./scripts/test_sanitizers.sh ubsan
```

任何必需依赖或测试缺失都必须失败，不能 SKIP 后返回成功。

完成后输出：

1. 修改文件清单。
2. 幂等记录的新不变量和 schema 兼容方式。
3. FullSnapshot sequence 的唯一来源。
4. BatchClaim 异步状态机和 completion 线程归属。
5. 实际运行命令和结果。
6. 遗留风险。

完成阶段一后停止，不要自动进入阶段二。

---

# 阶段二提示词：修复稳定门禁并补齐真实进程故障 E2E

仅在阶段一全部通过后执行本阶段。直接修改工程和脚本，不要只写测试计划。

## 开始前

重复执行：阅读 `AGENTS.md`、检查 `git status`、保留已有修改、不提交不 Push。继续保持 C++17 + Reactor + brpc，不涉及 Unity。

## 阶段目标

让所有“HA/故障测试”真实作用于业务，不允许模拟字段变化、存活探测或软失败后判定 PASS。

## 一、重构 E2E 进程清单和场景隔离

1. 启动脚本除兼容 `pids` 外，新增带角色的进程清单，例如：

   ```text
   role instance_id pid rpc_addr http_addr game_addr
   session sess-0 ...
   session sess-1 ...
   gamedb gamedb-0 ...
   gamelogic gl-0 ...
   ```

2. 故障脚本按 `role + instance_id` 查 PID，禁止继续依赖 `PIDS[4]` 之类固定位置。
3. 每个破坏性场景必须：

   ```text
   启动或验证完整前置拓扑
   → 执行业务前置
   → 注入故障
   → 验证业务结果
   → 清理并恢复拓扑
   ```

4. 所有脚本使用 `trap` 清理本次启动的进程和临时配置，不误杀用户其他进程。
5. E2E 使用独立 Redis key prefix、测试账号命名空间和 run 目录。
6. 仅有 PID 文件但关键角色已经死亡时，必须判定集群不健康并重建。

## 二、修复 `stable_gate.sh --full` 的执行顺序

当前 `final_e2e` 会 SIGKILL Gateway、Session、Logic、GameDB，但后续 Phase2 测试只检查 PID 文件是否存在，因此完整门禁无法可靠连续执行。

修改为以下任一可靠方式：

- 每个破坏性场景使用独立集群；或
- 每组破坏性场景结束后 stop + clean restart + readiness 检查。

要求：

1. 不允许因为 `pids` 文件存在就假定集群健康。
2. 不允许在脚本中用 `|| true` 吞掉必需业务断言。
3. 不允许输出 WARN 后继续算 PASS。
4. stable gate 任一步未执行、超时或缺少依赖都返回非零和 `STABLE BLOCKED`。
5. 门禁应生成机器可读摘要，至少记录 commit、开始结束时间、步骤、退出码、报告路径。

## 三、真实 Session 故障切换

测试必须完成：

```text
gw0 登录并建立 Session
→ SIGKILL 当前使用的 sess-0
→ gw1 通过 sess-1 新登录成功
→ 原玩家从 gw0 断开后通过 gw1 Reconnect 成功
→ 旧 fence/旧 Gateway 迟到请求被拒绝
→ Redis 中只有一个 ONLINE fence
```

只检查 sess-1 HTTP 存活不算通过。

## 四、真实 GameLogic 自动恢复

删除 `EnterMap` 失败后继续测试的软失败逻辑。

测试必须完成：

```text
玩家真实进入 gl-0 所属 MapInstance
→ 验证一条普通命令成功
→ SIGKILL gl-0
→ PlacementRecoveryScheduler 自动选取 gl-1
→ owner_epoch 和 route_version 增加
→ 玩家按当前支持语义重新进图/恢复
→ 在 gl-1 再执行一条普通命令成功
→ 旧 epoch 请求被拒绝
```

本版本不要求实时地图无损恢复，但必须证明不会双 Owner 写入，也不能仅用工具直接修改 Redis 后算“自动恢复”。

修复 `placement_recovery_test` 的 timing flake：使用可控时钟、条件变量或明确轮询 deadline，不能靠增加随机 sleep。

## 五、真实 GameDB 未知结果与故障切换

测试必须把故障注入到真实业务写链路：

```text
发起带 idempotency_key 的资产写
→ 在 commit 后、响应前注入连接/进程故障
→ 客户端或调用方得到未知结果
→ 通过另一个 GameDB 查询同一幂等记录
→ 返回真实最终结果
→ 校验资产版本只增加一次、Outbox 只有一条
→ 后续另一笔资产写仍可通过存活 GameDB 完成
```

只运行本地 store 单测后再杀 GameDB，或只检查 gamedb1 HTTP ready，不算通过。

建议提供仅测试构建启用的 failpoint，正式构建默认关闭。

## 六、真实动态扩容和 DRAINING

`test_dynamic_logic_scale.sh` 必须真正：

1. 启动第三个 GameLogic 进程 `gl-2`，使用独立 RPC/HTTP 端口。
2. 等待注册中心发现 `gl-2` 且 Gateway/Session Channel snapshot 更新。
3. 分配一个新 MapInstance 到 `gl-2`，玩家真实进图并 Dispatch 成功。
4. 将 `gl-2` 标记为 DRAINING 后，不再给它分配新地图。
5. 已在 `gl-2` 的玩家按当前策略继续或迁移，不能突然随机转发。
6. 摘除并停止 gl-2 后，Channel snapshot 不包含悬空实例。

无集群时不得退化成“unit PASS”。稳定门禁模式下必须 fail-closed。

## 七、真实 Registry outage

在隔离测试环境中临时停止注册中心访问或停止专用 Registry Redis：

1. 已建立的 Channel snapshot 保留。
2. 已登录玩家的一条不依赖新 Placement 的普通 Dispatch 仍成功。
3. 新发现结果为空时不能清空有效 Channel。
4. Registry 恢复后可以发现新版本实例。
5. 明确 Session/Placement 与 Registry 共用 Redis 时的限制，不把状态 Redis 故障伪装成只影响发现。

## 八、阶段二测试与验收

至少实际执行：

```bash
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/final_e2e.sh --start-cluster
./scripts/test_session_failover.sh
./scripts/test_logic_auto_recovery.sh
./scripts/test_gamedb_unknown_result_failover.sh
./scripts/test_dynamic_logic_scale.sh
./scripts/test_registry_outage.sh
```

额外要求：

- 每个脚本失败返回非零。
- 不存在 `soft-fail`、`unit only PASS` 或缺场景仍 PASS。
- 测试结束后没有残留测试进程和端口。
- 日志中发现 crash、assert、deadlock、旧 epoch 接受写入时必须失败。

完成后输出每个故障场景的前置业务、注入方式、业务后置断言、真实结果和日志路径。

完成阶段二后停止，不要自动进入阶段三。

---

# 阶段三提示词：可复现 CI、长稳门禁与稳定候选发布

仅在阶段一、二全部通过后执行。本阶段不再增加架构功能，只完成可复现构建和稳定性证明。

## 一、提供可复现完整工具链

当前 GitHub full job 在标准 runner 中没有安装 brpc，却在检测不到 brpc 时直接失败。必须修复。

选择一种可维护方案：

1. 提供固定 digest 的 CI/开发工具链镜像，内含匹配版本的 brpc、protobuf、OpenSSL、MySQL Client、hiredis 和 jsoncpp；推荐。
2. 或在 CI 中使用缓存并通过 `scripts/install_deps.sh --build-brpc` 构建固定 commit 的 brpc。

要求：

- 固定 brpc、protobuf/protoc 和编译器兼容版本。
- Debug 和 Release 从全新 checkout 可构建。
- 不能依赖 runner 预装 `/usr/local/include/brpc`。
- Dockerfile/Compose 与 CI 使用同一兼容依赖来源。
- CI 先验证生成的 protobuf 与 protoc 版本兼容。
- PR 快速门禁与手工/夜间 Release 长稳门禁分离。

## 二、修复源码质量门禁

1. 对提交内容执行：

   ```bash
   git show --check --oneline HEAD
   ```

   手写源码、脚本和文档不得有冲突标记或尾随空白。
2. 生成的 protobuf 文件如果工具固定生成空白，可排除，但必须在文档说明；不能让 `git diff --check` 在 clean checkout 上什么也没检查却宣称通过。
3. Shell 脚本执行 `bash -n` 和 ShellCheck。
4. 测试二进制缺失、依赖缺失、报告缺失一律失败。

## 三、完整稳定门禁

在同一个最终 commit 上真实执行，不允许缩短时长后宣称稳定：

```bash
./scripts/check_deps.sh --full
./scripts/bootstrap_local_config.sh
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

TSan 如需 suppression：

- 只能抑制已确认来自未使用 TSan 重编的第三方 brpc/bthread 噪声。
- 不得抑制项目自身函数。
- 最好在 Release 工具链中提供 TSan 重编的 brpc。

## 四、负载与 Soak 报告最低内容

不能只记录成功率。至少记录：

```text
commit
工具链/机器配置
持续时间
并发数
总请求、成功、失败、超时
登录与 Dispatch 的 p50/p95/p99
各错误码计数
各进程起止 RSS
FD/线程数起止和峰值
Gateway 连接数
PlayerSerialQueue 深度和拒绝数
brpc 超时数
Redis/MySQL 延迟和错误数
进程异常退出数
```

最低判定：

- 20 轮核心 E2E 全通过。
- 30 分钟负载无进程退出、无数据一致性错误，成功率阈值由报告明确给出。
- 2 小时 Soak 无持续 RSS/FD/线程增长趋势，无死锁，无队列永久堆积。
- Sanitizer 无项目代码错误。
- 所有故障场景恢复后仍能执行真实业务。

## 五、稳定候选发布规则

`stable_gate.sh --full` 成功后生成不可手工伪造的报告目录，例如：

```text
run/release/<commit>/
  manifest.json
  build-debug.log
  build-release.log
  unit.log
  integration.log
  e2e-20x.log
  failover/*.log
  load-report.json
  soak-report.json
  sanitizers/*.log
```

`manifest.json` 至少包含 commit、dirty 状态、工具链版本、每项 exit code、报告哈希和最终结论。

只有满足以下全部条件才允许输出：

```text
STABLE PASS — candidate server-stable-v0.1.0-rc1
```

否则必须输出：

```text
STABLE BLOCKED
```

并列出未通过项。Cursor 不得自行创建 Tag 或 Push；由我审核报告后决定。

## 六、更新文档

更新：

- `docs/release/server-stable-v0.1.0.md`
- 部署、停止、回滚和故障演练 Runbook
- 当前拓扑和支持范围
- 已知限制

发布说明必须明确：

1. 本版是客户端联调与后续业务开发的稳定服务器基线。
2. Redis/MySQL 当前仍是基础设施单点。
3. GameLogic 故障恢复当前不是实时地图无损迁移。
4. Unity 接入属于下一版本，本阶段没有实现或验证 Unity。

## 七、阶段三最终输出

完成后只基于真实结果输出：

1. 最终 commit 和 dirty 状态。
2. 稳定门禁每项 PASS/FAIL。
3. Release 报告目录。
4. 已修复问题。
5. 已知限制。
6. 是否达到 `server-stable-v0.1.0-rc1` 候选标准。
7. 若未达到，剩余阻塞项。

不得用“理论上通过”“本机没条件所以视为通过”替代真实测试。

---

# 3. 不要在本轮扩大的范围

三个阶段中都不要擅自执行以下工作：

- Unity 客户端、Unity SDK 或客户端 UI。
- 替换 brpc 为 gRPC。
- 推倒重写 Reactor。
- 新增独立 Login 进程。
- 实现 Kubernetes/Service Mesh。
- 实现 Redis Cluster/MySQL HA。
- 实现实时战斗地图无损快照迁移。
- 大规模重命名 World/GlobalService target。
- 新玩法、新协议业务字段或与稳定性无关的重构。

如发现范围外问题，只记录到 `docs/release/server-stable-v0.1.0.md` 的已知限制，不要阻塞当前最小稳定闭环，除非它会造成崩溃、越权、资产错误、双写或测试假绿。

