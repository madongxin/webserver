# GameMesh 稳定版本剩余闭环——Cursor 分阶段执行提示词

> 仓库：`https://github.com/madongxin/webserver`  
> 审计基线：`main@2e6a8b8c592296446f62a81ea940b5ed749eb30a`  
> 当前结论：`STABLE BLOCKED`  
> 目标：在最小改动范围内形成可供客户端联调和后续业务开发使用的服务器稳定基线。

请把本文件放在 GameMesh 仓库外阅读，或复制全文交给 Cursor。Cursor 必须直接修改工程、补测试并运行门禁，不能只输出方案。

---

## 一、稳定版本支持范围

本次稳定版本只承诺以下固定拓扑：

- Gateway ×2
- Auth/Session ×2
- GameLogic ×2
- GameDB ×2
- World/GlobalService ×1
- Redis ×1
- MySQL ×1
- Client 只连接 Gateway
- Client 与 Gateway 使用 Reactor TCP + ProtoFraming
- 内部服务继续使用 brpc

本次不强制实现：

- Unity 客户端接入
- 运行期动态扩容 `gl-2`
- 实时地图无损迁移
- 分布式 Cell 大世界
- Redis Sentinel/Cluster
- MySQL 主从切换
- 替换 brpc 为 gRPC
- 重写 Reactor

这些能力可以保留为 Experimental，但不得混入本次稳定版的硬承诺。

---

## 二、执行纪律

开始每个阶段前必须：

1. 完整阅读仓库中的 `AGENTS.md`。
2. 执行：

   ```bash
   git status --short --branch
   git rev-parse HEAD
   git log -5 --oneline
   ```

3. 保留已有用户改动，不执行：

   ```text
   git reset --hard
   git checkout -- .
   git clean -fd
   ```

4. 如果 HEAD 已经晚于 `2e6a8b8`，先检查新提交是否已处理本阶段问题，不要机械重复实现。
5. 直接修改代码、测试、脚本和必要文档，不要只生成审计报告。
6. 不提交、不推送、不创建 Tag，除非我另行明确要求。
7. 不删除测试、不降低测试阈值、不用 `sleep` 掩盖竞态、不通过空实现让测试变绿。
8. 一个阶段完成并通过验收后，输出阶段报告并停止；收到我继续执行的指令后再进入下一阶段。

执行顺序固定为：

```text
阶段一：修复同玩家严格顺序和干净构建
→ 阶段二：补齐发现、断线和资产一致性
→ 阶段三：在最终 Commit 上执行完整稳定门禁
```

---

# 阶段一：修复同玩家严格顺序和干净构建

## 1.1 当前确定性失败

`runtime/PlayerSerialQueue.cpp` 当前会把 async 期间暂存的 deferred 命令追加到主队列尾部。

存在以下调度：

```text
玩家 A：A1 开始并进入 async
A2 已经在主队列中，Worker 发现 A 正在 async，将 A2 移入 deferred
玩家 B：B1 在同一 shard 上执行并暂时阻塞
玩家 A：A3 仍留在主队列中
A1 的 completion 在 B1 阻塞期间插入队首
B1 结束
A1 completion 执行，把 deferred 中的 A2 追加到主队列尾部
主队列变成 A3、A2
最终执行顺序为 A1、A3、A2
```

外部复现结果为：

```text
completion=1 external_order=3,2
```

这违反了“同一玩家命令严格按提交顺序执行”的核心游戏服务器约束。

仓库当前新增的 `player_serial_async_test` 能覆盖“预排队命令不能提前执行”，但没有覆盖上述 deferred 与尚未扫描主队列交错的场景。

## 1.2 修改要求

修复 `PlayerSerialQueue`，保证：

1. 同一 `player_id` 的 External 任务严格保持提交顺序。
2. async completion 可以优先恢复该玩家的执行上下文，但不能颠倒任何 External 任务的顺序。
3. async 期间其他玩家可以继续执行，不能让一个玩家阻塞整个 shard。
4. completion 内再次调用 `MarkAsyncInFlight` 的链式异步继续保持串行。
5. `pending_global`、分片容量、Drain 和 Stop 统计保持正确，不能重复计数或出现下溢。
6. DRAINING 拒绝新 External，但已经开始的 completion 仍能回投。
7. STOPPED 后迟到 completion 不得跨线程直接修改玩家状态。

建议选择一种可证明正确的最小实现：

- 为 External 任务增加 shard 内单调递增的 `submit_seq`，释放 deferred 时按序稳定合并；或
- 将每个玩家改成独立 FIFO，由 shard 只调度“当前可运行玩家”；或
- 在 `FinishAsyncLevelLocked` 中把 deferred 插回该玩家尚未执行 External 的正确位置。

不能继续简单使用：

```cpp
shard->q.push_back(deferred_task);
```

除非已经通过序号或队列结构证明不会与尚未扫描的同玩家任务倒置。

## 1.3 必须新增的确定性测试

在 `test/player_serial_async_test.cpp` 或独立测试文件中新增“夹心调度”测试：

```text
1. 单 shard。
2. A1 启动，等待主线程把 A2、B1、A3 按顺序入队。
3. A1 调用 MarkAsyncInFlight 后结束当前执行片段。
4. Worker 把 A2 移到 deferred。
5. B1 开始执行并等待测试信号。
6. B1 阻塞期间投递 A1 completion。
7. 释放 B1。
8. 等待全部任务完成。
9. 断言同玩家 External 顺序严格为 A2、A3，不能是 A3、A2。
```

测试必须使用条件变量、Latch 或原子状态建立确定性顺序，不要依赖碰运气的短 `sleep`。

同时保留并验证：

- 预排队 A2 不在 A1 completion 前执行。
- 链式 async 期间同玩家不并发。
- 不同玩家能够继续推进。
- completion 在外部队列满时仍能完成。
- Drain/Stop 生命周期。

## 1.4 修复 GCC 13 干净编译问题

当前 `CMakeLists.txt` 使用：

```text
-Wall -Werror
```

`log/LogStream.cpp` 中：

```cpp
static_cast<const double>(num)
```

会在 GCC 13 触发 `ignored-qualifiers` 并导致构建失败。修成没有无效顶层 `const` 的写法，并检查本阶段改动在 GCC 13 + C++17 + `-Wall -Werror` 下通过。

不要通过全局关闭 `-Werror` 解决。

## 1.5 阶段一验证

至少执行：

```bash
./scripts/check_deps.sh
./scripts/build.sh Debug
./scripts/test.sh unit
```

如果本机依赖不足：

1. 明确列出缺少的依赖。
2. 不得把未执行写成 PASS。
3. 至少使用仓库相同编译参数单独编译并运行：
   - `player_serial_queue_test`
   - `player_serial_async_test`
   - 新增的确定性顺序测试

阶段一完成标准：

- 新旧玩家串行测试全部通过。
- 确定性夹心测试至少连续执行 100 次均通过。
- GCC 13 `-Wall -Werror` 不再因 `LogStream.cpp` 失败。
- 没有改动网络协议和服务拓扑。

## 1.6 阶段一输出

输出：

1. 修改文件列表。
2. 旧乱序原因和新队列不变量。
3. 确定性测试如何建立竞态窗口。
4. 实际执行的命令和真实结果。
5. 未执行项目及原因。

完成后停止，不要自动进入阶段二。

---

# 阶段二：补齐发现、断线和资产一致性

只有阶段一通过后才能执行。

## 2.1 GameLogic 健康列表必须完整 fail-closed

当前状态：

- `SessionServiceImpl::RefreshHealthyLogicOwners()` 在 Discover 成功且返回零实例时会清空 Session/Placement Owner。
- `ServerBootstrap.cpp` 的周期刷新仍然在 `insts.empty()` 时直接返回，继续保留旧实例。
- `AcquireSession()` 之前没有保证健康列表刚刚按相同规则刷新。

请统一成一个明确的三态语义：

| 发现结果 | 行为 |
|---|---|
| Registry 未启用或尚未 ready | 使用启动期静态配置或最后有效快照 |
| Discover 调用失败 | 保留最后有效快照，记录错误和指标 |
| Discover 成功且非空 | 原子替换健康实例快照 |
| Discover 成功但返回零实例 | 原子清空 Session 和 Placement 健康列表，新登录/新地图分配 fail-closed |

要求：

1. 抽出唯一的健康 GameLogic 快照刷新逻辑，RPC 前刷新和周期刷新不能各写一套不同规则。
2. 成功发现零实例时，同时执行等价操作：

   ```cpp
   SessionStore::SetLogicInstanceIds({});
   PlacementStore::SetLogicOwners({});
   ```

3. `AcquireSession` 在零健康 GameLogic 时返回：

   ```text
   NO_HEALTHY_GAMELOGIC
   ```

   并且不能创建半完成 Redis Session。

4. `ResolveOrCreateMap` 同样返回 `NO_HEALTHY_GAMELOGIC`，不能回退到 `gl-0`。
5. `preferred_gamelogic_instance_id` 不在健康列表时不能直接使用。
6. Registry 查询失败与查询成功零实例必须有不同日志/指标。
7. 不允许因为动态发现空列表重新使用已过期静态 GameLogic 地址。

必须测试：

- 启动静态列表可正常分配。
- Discover 失败保留最后快照。
- Discover 成功非空替换快照。
- Discover 成功零实例后 Login 不创建 Session。
- Discover 成功零实例后 Placement 不创建 MapInstance。
- GameLogic TTL 过期后，最多一个刷新周期内停止新分配。
- GameLogic 恢复注册后可以重新接受新分配。

## 2.2 Gateway 断线补偿必须真正异步且停止有界

当前 `GatewayDisconnectAsync` 已经把本地 Redis fallback 移出 Reactor，这是正确方向；但 `Stop(deadline)` 等待 deadline 后仍直接调用阻塞式 `join()`。

确定性复现：

```text
requested_deadline_ms=50
stop_elapsed_ms=1499
```

说明接口声明的“有界停止”并没有实现。

同时，`RedisClient` 当前只设置连接超时，没有为 Redis 命令设置读写超时，`redisCommand` 可能让 worker 长时间阻塞。

修改要求：

1. Reactor disconnect callback 只能做：
   - 快照必要字段；
   - 本地解绑；
   - 异步 brpc 调用；
   - 向有界后台队列入队。
2. 不能在 Reactor EventLoop 中执行同步 Redis、同步 brpc 或同步数据库访问。
3. Redis 建连后使用 hiredis 支持的命令超时，例如可配置的 200～1000ms 读写超时。
4. Redis IO 错误后将连接标为不可用，归还连接池前正确重连或淘汰，不能反复复用损坏 context。
5. `GatewayDisconnectAsync::Stop(deadline)` 必须满足真实有界语义。
6. 不允许简单 `detach` 一个仍捕获 Singleton、栈对象或即将析构成员的线程，避免 UAF。
7. 如果标准 `std::thread` 无法安全超时 join，可以把 worker 状态放入独立 `shared_ptr<State>`：
   - worker 只捕获 State；
   - 超时后旧 State 可安全独立收尾；
   - 新 Start 创建新 State；
   - 不能访问已销毁 Gateway/TcpConnection。
8. 队列满时允许依赖 Session TTL 最终收敛，但必须记录 dropped 指标和限频日志。

必须测试：

- 注入 1.5 秒阻塞 executor，调用 `Stop(50ms)` 后在规定误差范围内返回。
- Stop 返回后不存在对已销毁对象的访问。
- Stop 后可以再次 Start 并正常执行新任务。
- 队列满时 Reactor 投递仍立即返回。
- Redis 不可达、命令超时和连接池耗尽都不会阻塞 Reactor。
- 断线风暴下 EventLoop 延迟不超过仓库约定阈值。

## 2.3 邮件领取后的资产版本必须真正闭环

当前版本已经把 `asset_version` 从 GameDB 贯通到 GameLogic，这是正确改动，但仍有两个失败分支：

1. `MailService::ApplyClaimMemory()` 忽略 `ApplyItemRewardsWithVersion()` 的返回值。
2. 幂等重试返回 `should_apply_memory=false` 时立即退出；如果第一次 DB 已提交但本地内存同步失败，后续相同幂等键重试也不会触发内存恢复。

正确语义必须是：

```text
MySQL 是资产事实源
DB 提交成功
→ GameLogic 按 committed asset_version 对齐内存
→ 对齐成功后才继续允许该玩家执行资产相关命令
```

修改要求：

1. 将 `ApplyClaimMemory` 改为返回明确结果，而不是 `void`。
2. 对成功的 GameDB 响应，无论是否 `should_apply_memory`，都要检查本地 `asset_version`：
   - 内存版本等于提交版本：不重复加道具，视为已对齐。
   - `should_apply_memory=true` 且提交版本正好是内存版本 +1：应用 grants 并更新版本。
   - 内存版本落后、有缺口、回退或幂等命中但未对齐：从 GameDB 加载权威背包快照。
   - 内存版本已经高于该幂等操作版本：不能重复应用旧 grants。
3. 权威重载成功后，验证加载版本至少覆盖本次提交版本。
4. 如果 DB 已提交但内存应用和权威重载都失败：
   - 不得继续向玩家返回普通成功并让旧内存继续运行；
   - 返回明确的 `STATE_SYNC_REQUIRED` 或等价错误；
   - 标记该玩家资产状态 dirty；
   - 在重新加载成功前拒绝资产修改命令，或安全断开要求重连；
   - 重试相同幂等键不得重复发奖。
5. BatchClaim 每封邮件的资产版本必须按顺序收敛。
6. GameLogic 内存锁内不能执行无超时的远程调用；如果需要权威重载，设计锁外加载 + 版本校验/串行上下文回投。
7. 保持邮件状态、资产写、op log 和 outbox 处于同一 MySQL 事务。

必须新增/加强测试：

- 首次 Claim：DB 与内存都只增加一次。
- 相同幂等键重试：不重复发奖，内存版本与 DB 对齐。
- DB 提交成功、内存应用失败、权威重载成功：最终状态一致。
- DB 提交成功、内存应用和重载都失败：返回 `STATE_SYNC_REQUIRED`，玩家不能继续修改资产。
- 上述失败后再次用同一幂等键重试：不重复发奖，并能恢复/要求快照。
- BatchClaim 连续多个版本不丢失、不倒序。
- GameLogic 重启后从 GameDB 加载到已领取资产。
- brpc 的 `ClaimMailRsp.asset_version` 编解码和 Repository 映射正确。

把以下测试加入正式门禁，不得只是“存在二进制但脚本不执行”：

```text
gamedb_mail_claim_test
```

在 `scripts/test_integration.sh` 中将它作为必需测试：二进制缺失或测试失败都必须返回非零。

## 2.4 阶段二验证

至少执行：

```bash
./scripts/build.sh Debug
./scripts/test.sh unit
./scripts/test.sh integration
```

并单独执行新增测试至少 100 次或使用仓库已有 repeat 工具：

```text
同玩家夹心顺序测试
GatewayDisconnectAsync 有界停止测试
零健康 GameLogic fail-closed 测试
邮件领取资产版本测试
```

如果涉及共享状态或线程生命周期，运行：

```bash
./scripts/test_sanitizers.sh asan
./scripts/test_sanitizers.sh ubsan
./scripts/test_sanitizers.sh tsan
```

阶段二完成标准：

- 四类阻塞测试均通过。
- Reactor 路径无同步 Redis/brpc/MySQL。
- 资产提交后不存在“DB 成功但旧内存继续可写”的状态。
- 零健康 GameLogic 时不创建 Session 和 MapInstance。
- 有界 Stop 测试与 Sanitizer 通过。

## 2.5 阶段二输出

输出：

1. 修改文件列表。
2. GameLogic 健康快照三态语义。
3. Gateway 断线 worker 生命周期设计。
4. 邮件领取 DB/内存版本收敛方式。
5. 执行过的测试和真实结果。
6. 尚未运行的测试及原因。

完成后停止，不要自动进入阶段三。

---

# 阶段三：形成同一最终 Commit 的稳定版本证据

只有阶段一和阶段二全部通过后执行。本阶段以验证和最小修复为主，不再增加新架构能力。

## 3.1 冻结候选范围

先确认：

```bash
git status --short --branch
git diff --check
git show --check --oneline HEAD
```

要求：

- 所有稳定修复已经进入同一个最终候选 Commit。
- 工作树干净。
- 执行完整门禁后不再修改代码；如果又改代码，所有稳定门禁必须重新执行。
- 旧 Tag `server-stable-v0.1.0-rc1` 对应旧 Commit，不能作为当前 HEAD 的稳定证据。

## 3.2 必须执行的完整门禁

在具备完整依赖的环境执行：

```bash
./scripts/check_deps.sh --full
./scripts/bootstrap_local_config.sh
./scripts/build.sh Debug
./scripts/build.sh Release
./scripts/test.sh unit
./scripts/test.sh integration
./scripts/test_sanitizers.sh all
START_CLUSTER=1 E2E_ROUNDS=20 ./scripts/test_e2e_20x.sh
LOAD_DURATION_SEC=1800 LOAD_CONCURRENCY=32 ./scripts/load_tcp_baseline.sh
SOAK_DURATION_SEC=7200 ./scripts/soak_test.sh
./scripts/stable_gate.sh --full
```

硬要求：

1. E2E 至少 20 轮。
2. Load 至少 1800 秒，不降低既有成功率阈值。
3. Soak 至少 7200 秒。
4. ASan、UBSan、TSan 都通过；第三方抑制必须限定在 brpc/bthread/butil，不能屏蔽项目代码竞态。
5. 报告中的 Commit 必须等于最终候选 Commit。
6. `gamedb_mail_claim_test`、新增串行顺序测试、断线有界停止测试、零健康 Logic 测试必须出现在门禁执行记录中。
7. 缺依赖、缺测试二进制、SKIP 必需测试、时长不足、工作树 dirty 都必须输出：

   ```text
   STABLE BLOCKED
   ```

8. 只有所有硬门禁一次性通过，才能输出：

   ```text
   STABLE PASS — candidate server-stable-v0.1.0
   ```

## 3.3 最终 E2E 必须覆盖

- 双 Gateway TCP 登录。
- Auth → Session → 指定 GameLogic.BindPlayer。
- 普通命令使用 Dispatch，并按玩家绑定路由。
- 从 gw0 断线后经 gw1 重连。
- 旧 fence、旧 generation 和旧 Gateway 迟到消息被拒绝。
- Session 主实例失败后另一实例接管。
- GameDB 请求出现 unknown result 后按幂等键查询最终结果。
- GameLogic Push 只发送到玩家绑定 Gateway。
- 可靠 Push 重连补发或触发全量快照。
- GameLogic 无健康实例时 Login/Placement fail-closed。
- 邮件领取后内存背包、DB 背包和 asset_version 一致。
- 慢客户端、非法帧、连接风暴不会拖垮 Reactor。
- 优雅停服不会因断线 Redis worker 无限等待。

## 3.4 发布材料

更新稳定说明和报告，明确：

### 本版稳定支持

- 固定双 GW、双 Session、双 Logic、双 GameDB 拓扑。
- 登录、粘性 Dispatch、断线宽限、跨 GW 重连。
- Session fence/generation。
- Map owner epoch 拒绝旧写。
- GameDB 资产幂等与邮件领取资产版本闭环。
- 指定 Gateway 的 Push 和可靠消息补发/快照语义。

### 本版明确不支持

- 动态扩容 gl-2 的生产承诺。
- 实时地图无损迁移。
- 分布式 Cell 无缝大世界。
- Redis/MySQL 自动高可用。
- Unity 客户端兼容性承诺。

报告至少包含：

```text
commit
dirty
compiler/cmake/protoc/brpc/protobuf 版本
Debug/Release 构建结果
unit/integration 结果
sanitizer 结果
E2E 轮数和成功率
load 时长、并发、成功率、延迟分位
soak 时长、RSS/FD/线程增长
每个步骤 exit_code
报告文件 SHA256
```

## 3.5 阶段三输出

输出：

1. 最终 Commit ID。
2. 工作树是否干净。
3. 每个稳定门禁的命令、时长和结果。
4. Release manifest 路径。
5. 稳定支持范围和已知限制。
6. 唯一结论之一：

   ```text
   STABLE PASS — candidate server-stable-v0.1.0
   ```

   或：

   ```text
   STABLE BLOCKED
   ```

7. 如果 BLOCKED，列出最小剩余阻塞，不要宣布稳定。

未经我明确授权，不要创建或推送 Git Tag。

---

# 四、禁止事项

整个任务期间禁止：

- 引入 gRPC。
- 新增独立 Login 进程。
- 推倒重写 Reactor。
- 客户端直连 GameLogic。
- GameLogic 接收账号密码。
- TCP 断开立即 Logout。
- 在 Reactor EventLoop 中同步调用 brpc、Redis 或 MySQL。
- Redis 成为背包、货币、邮件附件的事实源。
- GameLogic 以任意随机负载均衡方式处理已绑定玩家。
- 同一 MapInstance 同时存在两个可写 Owner。
- 通过增大 `sleep` 掩盖并发错误。
- 删除失败测试、降低阈值、把必需测试改成可选。
- 用旧 Commit 的稳定报告证明新 Commit 稳定。
- 在未通过完整门禁时输出 `STABLE PASS`。

---

# 五、最终验收定义

只有同时满足以下条件，当前服务器才能作为稳定版本：

1. 同一玩家 External 命令在所有 async 调度下严格有序。
2. 不同玩家不会被单一慢玩家阻塞。
3. 零健康 GameLogic 时新登录和新地图分配 fail-closed。
4. Gateway 断线路径不阻塞 Reactor，后台 worker 停止真正有界。
5. 邮件领取的 DB 资产版本与 GameLogic 内存版本最终一致，失败时禁止旧内存继续写。
6. Debug、Release、Unit、Integration、ASan、UBSan、TSan 全部通过。
7. 双 GW E2E、故障测试、30 分钟负载和 2 小时 Soak 在同一最终 Commit 上通过。
8. 工作树干净并生成对应 Commit 的 Release manifest。

否则结论必须保持：

```text
STABLE BLOCKED
```
